#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "route_trie.h"
#include "compat.h"
#include "middleware.h"
#include "turbo_buffer.h"

// Forward declarations for re2c functions
extern http_method_t parse_http_method_re2c(const char *method, size_t len);
extern int tokenize_path_re2c(turbo_pool_t *arena, const char *path, tokenized_path_t *result);

#define TRIE_NODE_BLOCK_CAPACITY 256

typedef struct trie_node_block_s {
    trie_node_t *nodes;
    size_t used;
    struct trie_node_block_s *next;
} trie_node_block_t;

typedef struct {
    trie_node_block_t *blocks;
} trie_node_pool_t;

static trie_node_pool_t *trie_node_pool_create(void)
{
    trie_node_pool_t *pool = (trie_node_pool_t *)calloc(1, sizeof(trie_node_pool_t));
    return pool;
}

static void trie_node_pool_free(trie_node_pool_t *pool)
{
    if (!pool)
        return;

    trie_node_block_t *block = pool->blocks;
    while (block)
    {
        trie_node_block_t *next = block->next;
        free(block->nodes);
        free(block);
        block = next;
    }

    free(pool);
}

static trie_node_t *trie_node_alloc(route_trie_t *trie)
{
    trie_node_pool_t *pool = (trie_node_pool_t *)trie->node_pool;
    if (!pool)
        return NULL;

    trie_node_block_t *block = pool->blocks;
    if (!block || block->used >= TRIE_NODE_BLOCK_CAPACITY)
    {
        trie_node_block_t *new_block = (trie_node_block_t *)calloc(1, sizeof(trie_node_block_t));
        if (!new_block)
            return NULL;
        new_block->nodes = (trie_node_t *)calloc(TRIE_NODE_BLOCK_CAPACITY, sizeof(trie_node_t));
        if (!new_block->nodes)
        {
            free(new_block);
            return NULL;
        }
        new_block->used = 0;
        new_block->next = pool->blocks;
        pool->blocks = new_block;
        block = new_block;
    }

    return &block->nodes[block->used++];
}

// Splits a path into segments (/users/123/posts -> ["users", "123", "posts"])
/* Phase IRIS-1: Updated to use turbo_pool_t */
int tokenize_path(turbo_pool_t *arena, const char *path, tokenized_path_t *result)
{
    // Use re2c-based tokenizer for better performance
    return tokenize_path_re2c(arena, path, result);
}

static trie_node_t *match_segments(trie_node_t *node,
                                   const tokenized_path_t *path,
                                   int segment_idx,
                                   route_match_t *match,
                                   int depth)
{
    if (!node || depth > 100)
        return NULL;

    // All segments processed
    if (segment_idx >= path->count)
    {
        return node->is_end ? node : NULL;
    }

    const path_segment_t *segment = &path->segments[segment_idx];

    // Try exact match first (only for non-param segments)
    if (!segment->is_param && !segment->is_wildcard)
    {
        trie_node_t *current = node;

        // Match character by character
        for (size_t i = 0; i < segment->len && current; i++)
        {
            unsigned char c = (unsigned char)segment->start[i];
            current = current->children[c];
        }

        // If exact match succeeded
        if (current)
        {
            if (segment_idx + 1 >= path->count)
            {
                // Last segment
                if (current->is_end)
                    return current;
            }
            else
            {
                // More segments - need slash separator
                unsigned char sep = '/';
                if (current->children[sep])
                {
                    trie_node_t *result = match_segments(
                        current->children[sep], path, segment_idx + 1, match, depth + 1);
                    if (result)
                        return result;
                }
            }
        }
    }

    // Try parameter match
    if (node->param_child)
    {
        // Store parameter value
        if (match && match->param_count < 32)
        {
            match->params[match->param_count].key.data = node->param_child->param_name;
            match->params[match->param_count].key.len = strlen(node->param_child->param_name);
            match->params[match->param_count].value.data = segment->start;
            match->params[match->param_count].value.len = segment->len;
            match->param_count++;
        }

        // Continue matching
        if (segment_idx + 1 >= path->count)
        {
            if (node->param_child->is_end)
                return node->param_child;
        }
        else
        {
            unsigned char sep = '/';
            if (node->param_child->children[sep])
            {
                trie_node_t *result = match_segments(
                    node->param_child->children[sep], path, segment_idx + 1, match, depth + 1);
                if (result)
                    return result;
            }
        }

        // Backtrack parameter
        if (match && match->param_count > 0)
        {
            match->param_count--;
        }
    }

    // Try wildcard match
    if (node->wildcard_child && node->wildcard_child->is_end)
    {
        return node->wildcard_child;
    }

    return NULL;
}

// Find a matching route
bool route_trie_match(route_trie_t *trie,
                      const char *method,
                      const tokenized_path_t *tokenized_path,
                      route_match_t *match)
{
    if (!trie || !method || !tokenized_path || !match)
        return false;

    http_method_t method_idx = get_method_index(method);
    if (method_idx == METHOD_UNKNOWN)
        return false;

    turbo_rwlock_rdlock(&trie->lock);

    // Initialize match result
    match->handler = NULL;
    match->middleware_ctx = NULL;
    match->param_count = 0;

    trie_node_t *matched_node = NULL;

    // Handle root path
    if (tokenized_path->count == 0)
    {
        if (trie->root->is_end)
        {
            matched_node = trie->root;
        }
    }
    else
    {
        // Start from root/slash
        trie_node_t *start_node = trie->root;
        unsigned char sep = '/';
        if (start_node->children[sep])
        {
            start_node = start_node->children[sep];
        }

        matched_node = match_segments(start_node, tokenized_path, 0, match, 0);
    }

    // Extract handler if found
    if (matched_node && matched_node->handlers[method_idx])
    {
        match->handler = matched_node->handlers[method_idx];
        match->middleware_ctx = matched_node->middleware_ctx[method_idx];
        turbo_rwlock_rdunlock(&trie->lock);
        return true;
    }

    turbo_rwlock_rdunlock(&trie->lock);
    return false;
}

// Create a new trie node
static trie_node_t *trie_node_create(route_trie_t *trie)
{
    trie_node_t *node = trie_node_alloc(trie);
    if (!node)
        return NULL;

    node->is_end = false;
    return node;
}

// Free a trie node and its children recursively
static void trie_node_free(trie_node_t *node)
{
    if (!node)
        return;

    // Free all children
    for (int i = 0; i < 128; i++)
    {
        if (node->children[i])
        {
            trie_node_free(node->children[i]);
        }
    }

    // Free param and wildcard children
    if (node->param_child)
    {
        trie_node_free(node->param_child);
    }

    if (node->wildcard_child)
    {
        trie_node_free(node->wildcard_child);
    }

    // Free middleware contexts
    if (node->is_end)
    {
        for (int i = 0; i < 8; i++)
        {
            if (node->middleware_ctx[i])
            {
                MiddlewareInfo *middleware_info = (MiddlewareInfo *)node->middleware_ctx[i];
                if (middleware_info)
                {
                    free_middleware_info(middleware_info);
                }
            }
        }
    }

    // Free param name
    if (node->param_name)
    {
        node->param_name = NULL;
    }
}

// Get HTTP method index from string
http_method_t get_method_index(const char *method)
{
    if (!method)
        return METHOD_UNKNOWN;

    // Use re2c-based parser for better performance
    return parse_http_method_re2c(method, strlen(method));
}

// Create a new route trie
route_trie_t *route_trie_create(void)
{
    route_trie_t *trie = calloc(1, sizeof(route_trie_t));
    if (!trie)
        return NULL;

    trie->node_pool = trie_node_pool_create();
    if (!trie->node_pool)
    {
        free(trie);
        return NULL;
    }

    if (turbo_pool_init(&trie->param_arena, 4096) != 0)
    {
        trie_node_pool_free((trie_node_pool_t *)trie->node_pool);
        free(trie);
        return NULL;
    }

    trie->root = trie_node_create(trie);
    if (!trie->root)
    {
        turbo_pool_free(&trie->param_arena);
        trie_node_pool_free((trie_node_pool_t *)trie->node_pool);
        free(trie);
        return NULL;
    }

    // Initialize read-write lock for thread safety
    if (turbo_rwlock_init(&trie->lock) != 0)
    {
        trie_node_free(trie->root);
        turbo_pool_free(&trie->param_arena);
        trie_node_pool_free((trie_node_pool_t *)trie->node_pool);
        free(trie);
        return NULL;
    }

    return trie;
}

// Add a route to the trie
int route_trie_add(route_trie_t *trie, const char *method, const char *path,
                   RequestHandler handler, void *middleware_ctx)
{
    if (!trie || !method || !path || !handler)
        return -1;

    http_method_t method_idx = get_method_index(method);
    if (method_idx == METHOD_UNKNOWN)
        return -1;

    // Write lock for thread safety
    turbo_rwlock_wrlock(&trie->lock);

    trie_node_t *current = trie->root;
    const char *p = path;

    // Skip leading slash
    if (*p == '/')
        p++;

    while (*p)
    {
        // Handle parameter segments (:param)
        if (*p == ':')
        {
            p++; // Skip ':'

            // Extract parameter name
            const char *param_start = p;
            while (*p && *p != '/')
                p++;

            size_t param_len = p - param_start;

            // Create or navigate to param child
            if (!current->param_child)
            {
                current->param_child = trie_node_create(trie);
                if (!current->param_child)
                {
                    turbo_rwlock_wrunlock(&trie->lock);
                    return -1;
                }

                // Store parameter name
                current->param_child->param_name =
                    (char *)turbo_pool_alloc(&trie->param_arena, param_len + 1);
                if (!current->param_child->param_name)
                {
                    turbo_rwlock_wrunlock(&trie->lock);
                    return -1;
                }

                memcpy(current->param_child->param_name, param_start, param_len);
                current->param_child->param_name[param_len] = '\0';
            }

            current = current->param_child;
        }
        // Handle wildcard segments (*)
        else if (*p == '*')
        {
                if (!current->wildcard_child)
                {
                    current->wildcard_child = trie_node_create(trie);
                    if (!current->wildcard_child)
                    {
                    turbo_rwlock_wrunlock(&trie->lock);
                    return -1;
                }
            }

            current = current->wildcard_child;
            break; // Wildcard matches everything after
        }
        // Handle regular segments
        else
        {
            // Process until next segment or end
            while (*p && *p != '/')
            {
                unsigned char c = (unsigned char)*p;

                // Create child if doesn't exist
                if (!current->children[c])
                {
                    current->children[c] = trie_node_create(trie);
                    if (!current->children[c])
                    {
                        turbo_rwlock_wrunlock(&trie->lock);
                        return -1;
                    }
                }

                current = current->children[c];
                p++;
            }
        }

        // Skip segment separator
        if (*p == '/')
        {
            unsigned char c = (unsigned char)'/';
            if (!current->children[c])
            {
                current->children[c] = trie_node_create(trie);
                if (!current->children[c])
                {
                    turbo_rwlock_wrunlock(&trie->lock);
                    return -1;
                }
            }
            current = current->children[c];
            p++;
        }
    }

    // Mark as end node and store handler
    current->is_end = true;
    current->handlers[method_idx] = handler;
    current->middleware_ctx[method_idx] = middleware_ctx;
    trie->route_count++;

    turbo_rwlock_wrunlock(&trie->lock);
    return 0;
}

// Free the route trie
void route_trie_free(route_trie_t *trie)
{
    if (!trie)
        return;

    turbo_rwlock_wrlock(&trie->lock);
    trie_node_free(trie->root);
    trie->root = NULL;
    turbo_rwlock_wrunlock(&trie->lock);

    turbo_rwlock_destroy(&trie->lock);
    turbo_pool_free(&trie->param_arena);
    trie_node_pool_free((trie_node_pool_t *)trie->node_pool);
    free(trie);
}
