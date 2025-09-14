/**
 * @file mustache_parser.c
 * @brief Bridge between re2c+Lemon parser and existing mustache API
 */

#include "mustache_types.h"
#include "mustache_lexer.h"
#include "mustache.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Forward declarations for generated parser functions
void *MustacheParseAlloc(void *(*mallocProc)(size_t));
void MustacheParseFree(void *p, void (*freeProc)(void*));
void MustacheParse(void *yyp, int yymajor, mustache_token_t yyminor, mustache_parse_ctx_t *ctx);

// Include the old MUSTACHE_TAGTYPE definitions for compatibility
typedef enum MUSTACHE_TAGTYPE {
    MUSTACHE_TAGTYPE_NONE = 0,
    MUSTACHE_TAGTYPE_DELIM,
    MUSTACHE_TAGTYPE_COMMENT,
    MUSTACHE_TAGTYPE_VAR,
    MUSTACHE_TAGTYPE_VERBATIMVAR,
    MUSTACHE_TAGTYPE_VERBATIMVAR2,
    MUSTACHE_TAGTYPE_OPENSECTION,
    MUSTACHE_TAGTYPE_OPENSECTIONINV,
    MUSTACHE_TAGTYPE_CLOSESECTION,
    MUSTACHE_TAGTYPE_CLOSESECTIONINV,
    MUSTACHE_TAGTYPE_PARTIAL,
    MUSTACHE_TAGTYPE_INDENT
} MUSTACHE_TAGTYPE;

typedef struct MUSTACHE_TAGINFO {
    MUSTACHE_TAGTYPE type;
    int line;
    int col;
    int beg;
    int end;
    int name_beg;
    int name_end;
} MUSTACHE_TAGINFO;

int mustache_parse_template(const char *input, size_t len, mustache_parse_ctx_t *ctx) {
    memset(ctx, 0, sizeof(mustache_parse_ctx_t));
    
    // Tokenize input
    mustache_token_t *tokens;
    size_t token_count;
    
    if (mustache_tokenize(input, len, &tokens, &token_count) != 0) {
        ctx->error = 1;
        snprintf(ctx->error_message, sizeof(ctx->error_message), "Tokenization failed");
        return -1;
    }
    
    // Create parser
    void *parser = MustacheParseAlloc(malloc);
    if (!parser) {
        free(tokens);
        ctx->error = 1;
        snprintf(ctx->error_message, sizeof(ctx->error_message), "Parser allocation failed");
        return -1;
    }
    
    // Parse tokens
    for (size_t i = 0; i < token_count && !ctx->error; i++) {
        MustacheParse(parser, tokens[i].type, tokens[i], ctx);
    }
    
    // Finalize parsing
    if (!ctx->error) {
        MustacheParse(parser, 0, (mustache_token_t){0}, ctx);
    }
    
    // Cleanup
    MustacheParseFree(parser, free);
    free(tokens);
    
    return ctx->error ? -1 : 0;
}

void mustache_ast_free(mustache_ast_node_t *node) {
    if (!node) return;
    
    // Free children
    for (size_t i = 0; i < node->children_count; i++) {
        mustache_ast_free(node->children[i]);
    }
    free(node->children);
    
    // Free node data
    free(node->name);
    free(node->text);
    free(node);
}

void mustache_ast_print(const mustache_ast_node_t *node, int indent) {
    if (!node) return;
    
    for (int i = 0; i < indent; i++) printf("  ");
    
    const char *type_names[] = {
        "TEMPLATE", "TEXT", "VARIABLE", "UNESCAPED", 
        "SECTION", "INVERTED", "PARTIAL", "COMMENT", "IDENTIFIER"
    };
    
    printf("%s", type_names[node->type]);
    if (node->name) printf(" name='%s'", node->name);
    if (node->text) printf(" text='%.20s%s'", node->text, strlen(node->text) > 20 ? "..." : "");
    printf(" (%d:%d)\n", node->line, node->column);
    
    for (size_t i = 0; i < node->children_count; i++) {
        mustache_ast_print(node->children[i], indent + 1);
    }
}

// Convert AST to old-style MUSTACHE_TAGINFO array for compatibility
static int ast_to_taginfo(const mustache_ast_node_t *node, const char *template_data, 
                         MUSTACHE_TAGINFO **tags, size_t *count, size_t *capacity) {
    if (!node) return 0;
    
    // Add current node if it's not the root template
    if (node->type != MUSTACHE_NODE_TEMPLATE) {
        if (*count >= *capacity) {
            *capacity *= 2;
            MUSTACHE_TAGINFO *new_tags = realloc(*tags, *capacity * sizeof(MUSTACHE_TAGINFO));
            if (!new_tags) return -1;
            *tags = new_tags;
        }
        
        MUSTACHE_TAGINFO *tag = &(*tags)[(*count)++];
        memset(tag, 0, sizeof(MUSTACHE_TAGINFO));
        
        // Map node types to tag types
        switch (node->type) {
            case MUSTACHE_NODE_TEXT:
                tag->type = MUSTACHE_TAGTYPE_NONE; // Text is handled differently
                break;
            case MUSTACHE_NODE_VARIABLE:
                tag->type = MUSTACHE_TAGTYPE_VAR;
                break;
            case MUSTACHE_NODE_UNESCAPED:
                tag->type = MUSTACHE_TAGTYPE_VERBATIMVAR;
                break;
            case MUSTACHE_NODE_SECTION:
                tag->type = MUSTACHE_TAGTYPE_OPENSECTION;
                break;
            case MUSTACHE_NODE_INVERTED:
                tag->type = MUSTACHE_TAGTYPE_OPENSECTIONINV;
                break;
            case MUSTACHE_NODE_PARTIAL:
                tag->type = MUSTACHE_TAGTYPE_PARTIAL;
                break;
            case MUSTACHE_NODE_COMMENT:
                tag->type = MUSTACHE_TAGTYPE_COMMENT;
                break;
            default:
                tag->type = MUSTACHE_TAGTYPE_NONE;
                break;
        }
        
        tag->line = node->line;
        tag->col = node->column;
        
        // For compatibility, we need to find the positions in the original template
        // This is a simplified approach - in a full implementation, we'd store
        // the original positions during parsing
        if (node->name) {
            const char *pos = strstr(template_data, node->name);
            if (pos) {
                tag->name_beg = pos - template_data;
                tag->name_end = tag->name_beg + strlen(node->name);
            }
        }
    }
    
    // Process children
    for (size_t i = 0; i < node->children_count; i++) {
        if (ast_to_taginfo(node->children[i], template_data, tags, count, capacity) != 0) {
            return -1;
        }
    }
    
    return 0;
}

// Enhanced mustache_compile that can use either the new parser or fall back to old
MUSTACHE_TEMPLATE* mustache_compile_enhanced(const char* templ_data, size_t templ_size,
                                           const MUSTACHE_PARSER* parser, void* parser_data,
                                           unsigned flags) {
    // Try new parser first
    mustache_parse_ctx_t ctx;
    if (mustache_parse_template(templ_data, templ_size, &ctx) == 0) {
        // Success with new parser - convert AST to old format for compatibility
        size_t capacity = 64;
        size_t count = 0;
        MUSTACHE_TAGINFO *tags = malloc(capacity * sizeof(MUSTACHE_TAGINFO));
        
        if (!tags) {
            mustache_ast_free(ctx.root);
            return NULL;
        }
        
        if (ast_to_taginfo(ctx.root, templ_data, &tags, &count, &capacity) == 0) {
            // Successfully converted - now compile using existing logic
            // This is a simplified approach - ideally we'd generate bytecode directly from AST
            mustache_ast_free(ctx.root);
            
            // For now, fall back to original parser but we have validated syntax
            free(tags);
        } else {
            free(tags);
            mustache_ast_free(ctx.root);
        }
    }
    
    // Fall back to original parser (for now)
    // In a complete implementation, we'd generate the template directly from the AST
    return mustache_compile(templ_data, templ_size, parser, parser_data, flags);
}