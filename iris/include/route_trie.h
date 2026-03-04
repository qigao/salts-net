#ifndef ROUTE_TRIE_H
#define ROUTE_TRIE_H

#include <stddef.h>
#include <stdbool.h>

#include "router.h"
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

// Path segment structure for pre-tokenized paths
typedef struct
{
    const char *start;
    size_t len;
    bool is_param;    // true if this segment is :param
    bool is_wildcard; // true if this segment is *
} path_segment_t;

// Tokenized path to avoid re-parsing
typedef struct
{
    path_segment_t *segments;
    int count;
    int capacity;
} tokenized_path_t;

typedef struct trie_node
{
    struct trie_node *children[128];  // ASCII characters
    struct trie_node *param_child;    // For :param segments
    struct trie_node *wildcard_child; // For * wildcard
    char *param_name;                 // Name of parameter if this is a param node
    bool is_end;                      // Marks end of a route
    RequestHandler handlers[8];       // Handlers for different HTTP methods
    void *middleware_ctx[8];          // Middleware context for each method
} trie_node_t;

typedef struct
{
    trie_node_t *root;
    size_t route_count;
    turbo_rwlock_t lock;
    void *node_pool;
    turbo_pool_t param_arena;
} route_trie_t;

typedef struct
{
    const char *data;
    size_t len;
} string_view_t;

typedef struct
{
    string_view_t key;
    string_view_t value;
} param_match_t;

typedef struct
{
    RequestHandler handler;
    void *middleware_ctx;
    param_match_t params[32];
    int param_count;
} route_match_t;

typedef enum
{
    METHOD_GET = 0,
    METHOD_POST = 1,
    METHOD_PUT = 2,
    METHOD_DELETE = 3,
    METHOD_PATCH = 4,
    METHOD_HEAD = 5,
    METHOD_OPTIONS = 6,
    METHOD_UNKNOWN = 7
} http_method_t;

// Path tokenization functions
/* Phase IRIS-1: Updated to use turbo_pool_t */
CXX_C_API int tokenize_path(turbo_pool_t *arena, const char *path, tokenized_path_t *result);

// re2c-based route parsing functions
CXX_C_API http_method_t parse_http_method_re2c(const char *method, size_t len);
CXX_C_API int tokenize_path_re2c(turbo_pool_t *arena, const char *path, tokenized_path_t *result);
CXX_C_API int count_path_segments_re2c(const char *path, size_t len);
CXX_C_API bool extract_path_segment_re2c(const char **cursor, const char *limit, path_segment_t *segment);
CXX_C_API bool is_valid_path_char_re2c(unsigned char c);
CXX_C_API bool match_path_segment_re2c(const char *pattern, size_t pattern_len,
                             const char *path, size_t path_len);

// Now takes tokenized path instead of raw string
CXX_C_API bool route_trie_match(route_trie_t *trie,
                      const char *method,
                      const tokenized_path_t *tokenized_path,
                      route_match_t *match);

// Existing functions remain same
CXX_C_API route_trie_t *route_trie_create(void);
CXX_C_API int route_trie_add(route_trie_t *trie, const char *method, const char *path,
                   RequestHandler handler, void *middleware_ctx);
CXX_C_API void route_trie_free(route_trie_t *trie);
CXX_C_API http_method_t get_method_index(const char *method);

#ifdef __cplusplus
}
#endif

#endif
