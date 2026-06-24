/**
 * @file jsonpath_types.h
 * @brief JSONPath parser internal AST types
 */

#ifndef JSONPATH_TYPES_H
#define JSONPATH_TYPES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jsonpath_opcode_s {
  int type;
  struct jsonpath_opcode_s *next;
  struct jsonpath_opcode_s *down;
  struct jsonpath_opcode_s *sibling;
  char *str;
  int num;
  double number;
} jsonpath_opcode_t;

typedef struct jsonpath_parse_ctx_s {
  jsonpath_opcode_t *pool;
  jsonpath_opcode_t *path;
  int error_pos;
  int error_code;
  int off;
} jsonpath_parse_ctx_t;

jsonpath_opcode_t *jsonpath_append_op(jsonpath_opcode_t *a, jsonpath_opcode_t *b);
jsonpath_opcode_t *jsonpath_alloc_op(jsonpath_parse_ctx_t *ctx, int type, int num,
                                      double number, const char *str, ...);

#ifdef __cplusplus
}
#endif

#endif /* JSONPATH_TYPES_H */
