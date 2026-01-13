/**
 * @file mustache_types.h
 * @brief Mustache Parser Types
 */

#ifndef MUSTACHE_TYPES_H
#define MUSTACHE_TYPES_H

#include <stddef.h>
#include "common/platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MUSTACHE_NODE_TEMPLATE,
    MUSTACHE_NODE_TEXT,
    MUSTACHE_NODE_VARIABLE,
    MUSTACHE_NODE_UNESCAPED,
    MUSTACHE_NODE_SECTION,
    MUSTACHE_NODE_INVERTED,
    MUSTACHE_NODE_PARTIAL,
    MUSTACHE_NODE_COMMENT,
    MUSTACHE_NODE_IDENTIFIER
} mustache_node_type_t;

typedef struct mustache_ast_node {
    mustache_node_type_t type;
    char *name;           // Variable/section name
    char *text;           // Text content
    int line;
    int column;
    
    struct mustache_ast_node *parent;
    struct mustache_ast_node **children;
    size_t children_count;
    size_t children_capacity;
} mustache_ast_node_t;

typedef struct {
    mustache_ast_node_t *root;
    int error;
    int error_line;
    int error_column;
    char error_message[256];
} mustache_parse_ctx_t;

/**
 * Parse a mustache template into an AST
 * @param input Template string
 * @param len Length of template
 * @param ctx Parse context (output)
 * @return 0 on success, -1 on error
 */
CXX_C_API int mustache_parse_template(const char *input, size_t len, mustache_parse_ctx_t *ctx);

/**
 * Free an AST node and all its children
 * @param node Node to free
 */
CXX_C_API void mustache_ast_free(mustache_ast_node_t *node);

/**
 * Print AST for debugging
 * @param node Root node
 * @param indent Indentation level
 */
CXX_C_API void mustache_ast_print(const mustache_ast_node_t *node, int indent);

#ifdef __cplusplus
}
#endif

#endif /* MUSTACHE_TYPES_H */