/**
 * @file jsonpath.c
 * @brief JSONPath parser and matcher for json_value_t trees
 */

#include "json_parser.h"
#include "json_types.h"
#include "jsonpath_grammar_gen.h"
#include "jsonpath_types.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSONPATH_ERROR_LEN 256

struct json_path_result_s {
  json_value_t **items;
  size_t count;
  size_t capacity;
  int error;
};

static char g_jsonpath_error[JSONPATH_ERROR_LEN] = {0};

void *JsonPathParseAlloc(void *(*mallocProc)(size_t));
void JsonPathParseFree(void *parser, void (*freeProc)(void *));
void JsonPathParse(void *parser, int tokenType, jsonpath_opcode_t *token,
                   jsonpath_parse_ctx_t *ctx);

static char *jsonpath_strdup_len(const char *str, size_t len) {
  char *out = (char *)malloc(len + 1);
  if (!out)
    return NULL;
  memcpy(out, str, len);
  out[len] = '\0';
  return out;
}

jsonpath_opcode_t *jsonpath_append_op(jsonpath_opcode_t *a, jsonpath_opcode_t *b) {
  jsonpath_opcode_t *tail = a;

  if (!a)
    return b;

  while (tail->sibling)
    tail = tail->sibling;

  tail->sibling = b;
  return a;
}

jsonpath_opcode_t *jsonpath_alloc_op(jsonpath_parse_ctx_t *ctx, int type, int num,
                                      double number, const char *str, ...) {
  jsonpath_opcode_t *child;
  jsonpath_opcode_t *op = (jsonpath_opcode_t *)calloc(1, sizeof(*op));
  if (!op) {
    if (ctx)
      ctx->error_code = -2;
    return NULL;
  }

  op->type = type;
  op->num = num;
  op->number = number;

  if (str) {
    op->str = jsonpath_strdup_len(str, strlen(str));
    if (!op->str) {
      free(op);
      if (ctx)
        ctx->error_code = -2;
      return NULL;
    }
  }

  va_list ap;
  va_start(ap, str);
  while ((child = va_arg(ap, jsonpath_opcode_t *)) != NULL) {
    if (!op->down)
      op->down = child;
    else
      jsonpath_append_op(op->down, child);
  }
  va_end(ap);

  op->next = ctx->pool;
  ctx->pool = op;
  return op;
}

static void jsonpath_free_ctx(jsonpath_parse_ctx_t *ctx) {
  jsonpath_opcode_t *op = ctx ? ctx->pool : NULL;
  while (op) {
    jsonpath_opcode_t *next = op->next;
    free(op->str);
    free(op);
    op = next;
  }
  free(ctx);
}

static int jsonpath_hex(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

static int jsonpath_append_utf8(char *out, size_t *pos, int code) {
  if (code <= 0)
    return 0;
  if (code <= 0x7F) {
    out[(*pos)++] = (char)code;
  } else if (code <= 0x7FF) {
    out[(*pos)++] = (char)(0xC0 | (code >> 6));
    out[(*pos)++] = (char)(0x80 | (code & 0x3F));
  } else if (code <= 0xFFFF) {
    out[(*pos)++] = (char)(0xE0 | (code >> 12));
    out[(*pos)++] = (char)(0x80 | ((code >> 6) & 0x3F));
    out[(*pos)++] = (char)(0x80 | (code & 0x3F));
  } else if (code <= 0x10FFFF) {
    out[(*pos)++] = (char)(0xF0 | (code >> 18));
    out[(*pos)++] = (char)(0x80 | ((code >> 12) & 0x3F));
    out[(*pos)++] = (char)(0x80 | ((code >> 6) & 0x3F));
    out[(*pos)++] = (char)(0x80 | (code & 0x3F));
  }
  return 0;
}

static int jsonpath_parse_string(const char *buf, char quote, char **out_str,
                                 jsonpath_parse_ctx_t *ctx) {
  size_t cap = strlen(buf) + 1;
  char *out = (char *)malloc(cap);
  size_t pos = 0;
  size_t i = 1;

  if (!out)
    return -2;

  while (buf[i]) {
    unsigned char c = (unsigned char)buf[i++];

    if (c == (unsigned char)quote) {
      out[pos] = '\0';
      *out_str = out;
      return (int)i;
    }

    if (c != '\\') {
      out[pos++] = (char)c;
      continue;
    }

    c = (unsigned char)buf[i++];
    if (!c) {
      free(out);
      return -1;
    }

    switch (c) {
    case '"':
    case '\'':
    case '\\':
    case '/':
      out[pos++] = (char)c;
      break;
    case 'b':
      out[pos++] = '\b';
      break;
    case 'f':
      out[pos++] = '\f';
      break;
    case 'n':
      out[pos++] = '\n';
      break;
    case 'r':
      out[pos++] = '\r';
      break;
    case 't':
      out[pos++] = '\t';
      break;
    case 'u': {
      int h0 = jsonpath_hex(buf[i]);
      int h1 = jsonpath_hex(buf[i + 1]);
      int h2 = jsonpath_hex(buf[i + 2]);
      int h3 = jsonpath_hex(buf[i + 3]);
      if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) {
        free(out);
        ctx->error_pos = ctx->off + (int)i;
        return -3;
      }
      jsonpath_append_utf8(out, &pos, (h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
      i += 4;
      break;
    }
    case 'x': {
      int h0 = jsonpath_hex(buf[i]);
      int h1 = jsonpath_hex(buf[i + 1]);
      if (h0 < 0 || h1 < 0) {
        free(out);
        ctx->error_pos = ctx->off + (int)i;
        return -3;
      }
      jsonpath_append_utf8(out, &pos, (h0 << 4) | h1);
      i += 2;
      break;
    }
    default:
      if (quote == '/') {
        out[pos++] = '\\';
      }
      out[pos++] = (char)c;
      break;
    }
  }

  free(out);
  return -1;
}

static int jsonpath_lex(const char *input, jsonpath_parse_ctx_t *ctx, jsonpath_opcode_t **out_op) {
  jsonpath_opcode_t *op = NULL;
  char *str = NULL;
  char *end = NULL;
  int type = 0;
  int consumed = 1;

  *out_op = NULL;

  switch (*input) {
  case ' ':
  case '\t':
  case '\r':
  case '\n':
    return 1;
  case '&':
    if (input[1] == '&') {
      type = JSONPATH_TOKEN_AND;
      consumed = 2;
    }
    break;
  case '|':
    if (input[1] == '|') {
      type = JSONPATH_TOKEN_OR;
      consumed = 2;
    }
    break;
  case '<':
    type = input[1] == '=' ? JSONPATH_TOKEN_LE : JSONPATH_TOKEN_LT;
    consumed = input[1] == '=' ? 2 : 1;
    break;
  case '>':
    type = input[1] == '=' ? JSONPATH_TOKEN_GE : JSONPATH_TOKEN_GT;
    consumed = input[1] == '=' ? 2 : 1;
    break;
  case '!':
    type = input[1] == '=' ? JSONPATH_TOKEN_NE : JSONPATH_TOKEN_NOT;
    consumed = input[1] == '=' ? 2 : 1;
    break;
  case '=':
    type = JSONPATH_TOKEN_EQ;
    break;
  case '~':
    type = JSONPATH_TOKEN_MATCH;
    break;
  case ',':
    type = JSONPATH_TOKEN_UNION;
    break;
  case '.':
    type = JSONPATH_TOKEN_DOT;
    break;
  case '[':
    type = JSONPATH_TOKEN_BROPEN;
    break;
  case ']':
    type = JSONPATH_TOKEN_BRCLOSE;
    break;
  case '(':
    type = JSONPATH_TOKEN_POPEN;
    break;
  case ')':
    type = JSONPATH_TOKEN_PCLOSE;
    break;
  case '$':
    type = JSONPATH_TOKEN_ROOT;
    break;
  case '@':
    type = JSONPATH_TOKEN_THIS;
    break;
  case '*':
    type = JSONPATH_TOKEN_WILDCARD;
    break;
  case '\'':
  case '"':
    consumed = jsonpath_parse_string(input, *input, &str, ctx);
    if (consumed < 0)
      return consumed;
    type = JSONPATH_TOKEN_STRING;
    break;
  case '/':
    consumed = jsonpath_parse_string(input, '/', &str, ctx);
    if (consumed < 0)
      return consumed;
    while (isalpha((unsigned char)input[consumed]))
      consumed++;
    type = JSONPATH_TOKEN_REGEXP;
    break;
  default:
    if (*input == '-' || isdigit((unsigned char)*input)) {
      double number = strtod(input, &end);
      if (end == input)
        return -3;
      consumed = (int)(end - input);
      op = jsonpath_alloc_op(ctx, JSONPATH_TOKEN_NUMBER, (int)number, number, NULL, NULL);
      if (!op)
        return -2;
      *out_op = op;
      return consumed;
    }

    if (*input == '_' || isalpha((unsigned char)*input)) {
      const char *start = input;
      while (*input == '_' || isalnum((unsigned char)*input))
        input++;

      consumed = (int)(input - start);
      if (consumed == 4 && memcmp(start, "true", 4) == 0) {
        op = jsonpath_alloc_op(ctx, JSONPATH_TOKEN_BOOL, 1, 1.0, NULL, NULL);
      } else if (consumed == 5 && memcmp(start, "false", 5) == 0) {
        op = jsonpath_alloc_op(ctx, JSONPATH_TOKEN_BOOL, 0, 0.0, NULL, NULL);
      } else {
        str = jsonpath_strdup_len(start, (size_t)consumed);
        if (!str)
          return -2;
        op = jsonpath_alloc_op(ctx, JSONPATH_TOKEN_LABEL, 0, 0.0, str, NULL);
        free(str);
      }
      if (!op)
        return -2;
      *out_op = op;
      return consumed;
    }
    break;
  }

  if (!type)
    return -4;

  op = jsonpath_alloc_op(ctx, type, 0, 0.0, str, NULL);
  free(str);
  if (!op)
    return -2;

  *out_op = op;
  return consumed;
}

static jsonpath_parse_ctx_t *jsonpath_parse(const char *expr) {
  jsonpath_parse_ctx_t *ctx;
  void *parser;
  const char *ptr;
  int consumed;

  if (!expr || !*expr) {
    snprintf(g_jsonpath_error, sizeof(g_jsonpath_error), "Empty JSONPath expression");
    return NULL;
  }

  ctx = (jsonpath_parse_ctx_t *)calloc(1, sizeof(*ctx));
  if (!ctx) {
    snprintf(g_jsonpath_error, sizeof(g_jsonpath_error), "Out of memory");
    return NULL;
  }

  parser = JsonPathParseAlloc(malloc);
  if (!parser) {
    jsonpath_free_ctx(ctx);
    snprintf(g_jsonpath_error, sizeof(g_jsonpath_error), "Out of memory");
    return NULL;
  }

  ptr = expr;
  while (*ptr && !ctx->error_code) {
    jsonpath_opcode_t *op = NULL;
    consumed = jsonpath_lex(ptr, ctx, &op);
    if (consumed < 0) {
      ctx->error_code = consumed;
      ctx->error_pos = ctx->error_pos ? ctx->error_pos : ctx->off;
      break;
    }

    if (op)
      JsonPathParse(parser, op->type, op, ctx);

    ptr += consumed;
    ctx->off += consumed;
  }

  if (!ctx->error_code)
    JsonPathParse(parser, 0, NULL, ctx);

  JsonPathParseFree(parser, free);

  if (ctx->error_code || !ctx->path) {
    snprintf(g_jsonpath_error, sizeof(g_jsonpath_error),
             "Invalid JSONPath expression at offset %d", ctx->error_pos);
    jsonpath_free_ctx(ctx);
    return NULL;
  }

  return ctx;
}

static int jsonpath_result_push(json_path_result_t *result, const json_value_t *value) {
  json_value_t **items;
  size_t new_capacity;

  if (!result || !value)
    return 1;

  if (result->count == result->capacity) {
    new_capacity = result->capacity ? result->capacity * 2 : 8;
    items = (json_value_t **)realloc(result->items, new_capacity * sizeof(*items));
    if (!items) {
      result->error = 1;
      return 0;
    }
    result->items = items;
    result->capacity = new_capacity;
  }

  result->items[result->count++] = (json_value_t *)value;
  return 1;
}

static const json_value_t *jsonpath_match_next(jsonpath_opcode_t *ptr, const json_value_t *root,
                                               const json_value_t *cur,
                                               json_path_result_t *result, int first_only);

static int jsonpath_value_to_op(const json_value_t *value, jsonpath_opcode_t *op) {
  if (!value || !op)
    return 0;

  memset(op, 0, sizeof(*op));

  switch (json_type(value)) {
  case JSON_BOOL:
    op->type = JSONPATH_TOKEN_BOOL;
    op->num = json_bool(value) ? 1 : 0;
    op->number = (double)op->num;
    return 1;
  case JSON_NUMBER:
    op->type = JSONPATH_TOKEN_NUMBER;
    op->number = json_number(value);
    op->num = (int)op->number;
    return 1;
  case JSON_STRING:
    op->type = JSONPATH_TOKEN_STRING;
    op->str = (char *)json_string(value);
    return 1;
  default:
    return 0;
  }
}

static const json_value_t *jsonpath_eval_path(jsonpath_opcode_t *op, const json_value_t *root,
                                              const json_value_t *cur) {
  if (!op)
    return NULL;

  if (op->type == JSONPATH_TOKEN_ROOT)
    return jsonpath_match_next(op->down, root, root, NULL, 1);

  if (op->type == JSONPATH_TOKEN_THIS)
    return jsonpath_match_next(op->down, root, cur, NULL, 1);

  return NULL;
}

static int jsonpath_resolve(jsonpath_opcode_t *op, const json_value_t *root,
                            const json_value_t *cur, jsonpath_opcode_t *out) {
  const json_value_t *value;

  if (!op || !out)
    return 0;

  if (op->type == JSONPATH_TOKEN_ROOT || op->type == JSONPATH_TOKEN_THIS) {
    value = jsonpath_eval_path(op, root, cur);
    return jsonpath_value_to_op(value, out);
  }

  *out = *op;
  return 1;
}

static int jsonpath_compare(jsonpath_opcode_t *op, const json_value_t *root,
                            const json_value_t *cur) {
  jsonpath_opcode_t left;
  jsonpath_opcode_t right;
  int cmp = 0;

  if (!jsonpath_resolve(op->down, root, cur, &left) ||
      !jsonpath_resolve(op->down->sibling, root, cur, &right) || left.type != right.type) {
    return 0;
  }

  switch (left.type) {
  case JSONPATH_TOKEN_BOOL:
    cmp = left.num - right.num;
    break;
  case JSONPATH_TOKEN_NUMBER:
    if (left.number < right.number)
      cmp = -1;
    else if (left.number > right.number)
      cmp = 1;
    break;
  case JSONPATH_TOKEN_STRING:
    cmp = strcmp(left.str ? left.str : "", right.str ? right.str : "");
    break;
  default:
    return 0;
  }

  switch (op->type) {
  case JSONPATH_TOKEN_EQ:
    return cmp == 0;
  case JSONPATH_TOKEN_NE:
    return cmp != 0;
  case JSONPATH_TOKEN_LT:
    return cmp < 0;
  case JSONPATH_TOKEN_LE:
    return cmp <= 0;
  case JSONPATH_TOKEN_GT:
    return cmp > 0;
  case JSONPATH_TOKEN_GE:
    return cmp >= 0;
  default:
    return 0;
  }
}

static void jsonpath_op_to_string(jsonpath_opcode_t *op, char *buf, size_t len) {
  if (!op || !buf || len == 0)
    return;

  switch (op->type) {
  case JSONPATH_TOKEN_BOOL:
    snprintf(buf, len, "%s", op->num ? "true" : "false");
    break;
  case JSONPATH_TOKEN_NUMBER:
    snprintf(buf, len, "%.17g", op->number);
    break;
  case JSONPATH_TOKEN_STRING:
  case JSONPATH_TOKEN_REGEXP:
    snprintf(buf, len, "%s", op->str ? op->str : "");
    break;
  default:
    buf[0] = '\0';
    break;
  }
}

static int jsonpath_match_like(jsonpath_opcode_t *op, const json_value_t *root,
                               const json_value_t *cur) {
  jsonpath_opcode_t left;
  jsonpath_opcode_t right;
  char left_buf[64];
  char right_buf[64];
  const char *haystack;
  const char *needle;

  if (!jsonpath_resolve(op->down, root, cur, &left) ||
      !jsonpath_resolve(op->down->sibling, root, cur, &right)) {
    return 0;
  }

  jsonpath_op_to_string(&left, left_buf, sizeof(left_buf));
  jsonpath_op_to_string(&right, right_buf, sizeof(right_buf));

  haystack = left.str && left.type == JSONPATH_TOKEN_STRING ? left.str : left_buf;
  needle = right.str && (right.type == JSONPATH_TOKEN_STRING || right.type == JSONPATH_TOKEN_REGEXP)
               ? right.str
               : right_buf;

  if (left.type == JSONPATH_TOKEN_REGEXP) {
    haystack = right.str && right.type == JSONPATH_TOKEN_STRING ? right.str : right_buf;
    needle = left.str ? left.str : "";
  }

  return strstr(haystack ? haystack : "", needle ? needle : "") != NULL;
}

static int jsonpath_expr(jsonpath_opcode_t *op, const json_value_t *root, const json_value_t *cur,
                         int index, const char *key) {
  jsonpath_opcode_t *child;

  if (!op)
    return 0;

  switch (op->type) {
  case JSONPATH_TOKEN_WILDCARD:
    return 1;
  case JSONPATH_TOKEN_EQ:
  case JSONPATH_TOKEN_NE:
  case JSONPATH_TOKEN_LT:
  case JSONPATH_TOKEN_LE:
  case JSONPATH_TOKEN_GT:
  case JSONPATH_TOKEN_GE:
    return jsonpath_compare(op, root, cur);
  case JSONPATH_TOKEN_MATCH:
    return jsonpath_match_like(op, root, cur);
  case JSONPATH_TOKEN_ROOT:
  case JSONPATH_TOKEN_THIS:
    return jsonpath_eval_path(op, root, cur) != NULL;
  case JSONPATH_TOKEN_NOT:
    return !jsonpath_expr(op->down, root, cur, index, key);
  case JSONPATH_TOKEN_AND:
    for (child = op->down; child; child = child->sibling) {
      if (!jsonpath_expr(child, root, cur, index, key))
        return 0;
    }
    return 1;
  case JSONPATH_TOKEN_OR:
  case JSONPATH_TOKEN_UNION:
    for (child = op->down; child; child = child->sibling) {
      if (jsonpath_expr(child, root, cur, index, key))
        return 1;
    }
    return 0;
  case JSONPATH_TOKEN_STRING:
    return key && op->str && strcmp(op->str, key) == 0;
  case JSONPATH_TOKEN_NUMBER:
    return index == op->num;
  default:
    return 0;
  }
}

static const json_value_t *jsonpath_match_expr(jsonpath_opcode_t *ptr, const json_value_t *root,
                                               const json_value_t *cur,
                                               json_path_result_t *result, int first_only) {
  const json_value_t *matched = NULL;

  if (!ptr || !cur)
    return NULL;

  if (json_type(cur) == JSON_OBJECT) {
    size_t count = json_object_size(cur);
    for (size_t i = 0; i < count; i++) {
      const char *key = json_object_key(cur, i);
      const json_value_t *value = json_object_value(cur, i);
      if (jsonpath_expr(ptr, root, value, -1, key)) {
        matched = jsonpath_match_next(ptr->sibling, root, value, result, first_only);
        if (matched && first_only)
          return matched;
      }
    }
  } else if (json_type(cur) == JSON_ARRAY) {
    size_t count = json_array_size(cur);
    for (size_t i = 0; i < count; i++) {
      const json_value_t *value = json_array_get(cur, i);
      if (jsonpath_expr(ptr, root, value, (int)i, NULL)) {
        matched = jsonpath_match_next(ptr->sibling, root, value, result, first_only);
        if (matched && first_only)
          return matched;
      }
    }
  }

  return matched;
}

static const json_value_t *jsonpath_match_next(jsonpath_opcode_t *ptr, const json_value_t *root,
                                               const json_value_t *cur,
                                               json_path_result_t *result, int first_only) {
  const json_value_t *next;
  int index;
  size_t len;

  if (!cur)
    return NULL;

  if (!ptr) {
    jsonpath_result_push(result, cur);
    return cur;
  }

  switch (ptr->type) {
  case JSONPATH_TOKEN_STRING:
  case JSONPATH_TOKEN_LABEL:
    next = json_object_get(cur, ptr->str);
    return next ? jsonpath_match_next(ptr->sibling, root, next, result, first_only) : NULL;
  case JSONPATH_TOKEN_NUMBER:
    if (json_type(cur) != JSON_ARRAY)
      return NULL;
    index = ptr->num;
    len = json_array_size(cur);
    if (index < 0)
      index += (int)len;
    if (index < 0 || (size_t)index >= len)
      return NULL;
    next = json_array_get(cur, (size_t)index);
    return jsonpath_match_next(ptr->sibling, root, next, result, first_only);
  default:
    return jsonpath_match_expr(ptr, root, cur, result, first_only);
  }
}

json_path_result_t *json_path_query(const json_value_t *root, const char *expr) {
  jsonpath_parse_ctx_t *ctx;
  json_path_result_t *result;

  if (!root || !expr) {
    snprintf(g_jsonpath_error, sizeof(g_jsonpath_error), "Invalid arguments");
    return NULL;
  }

  ctx = jsonpath_parse(expr);
  if (!ctx)
    return NULL;

  result = (json_path_result_t *)calloc(1, sizeof(*result));
  if (!result) {
    jsonpath_free_ctx(ctx);
    snprintf(g_jsonpath_error, sizeof(g_jsonpath_error), "Out of memory");
    return NULL;
  }

  if (ctx->path->type == JSONPATH_TOKEN_LABEL)
    jsonpath_match_next(ctx->path->down, root, root, result, 0);
  else
    jsonpath_match_next(ctx->path->down, root, root, result, 0);

  jsonpath_free_ctx(ctx);

  if (result->error) {
    json_path_result_free(result);
    snprintf(g_jsonpath_error, sizeof(g_jsonpath_error), "Out of memory");
    return NULL;
  }

  g_jsonpath_error[0] = '\0';
  return result;
}

json_value_t *json_path_get(const json_value_t *root, const char *expr) {
  json_path_result_t *result = json_path_query(root, expr);
  json_value_t *value = NULL;

  if (!result)
    return NULL;

  if (result->count > 0)
    value = result->items[0];

  json_path_result_free(result);
  return value;
}

size_t json_path_result_size(const json_path_result_t *result) {
  return result ? result->count : 0;
}

json_value_t *json_path_result_get(const json_path_result_t *result, size_t index) {
  if (!result || index >= result->count)
    return NULL;
  return result->items[index];
}

void json_path_result_free(json_path_result_t *result) {
  if (!result)
    return;
  free(result->items);
  free(result);
}

const char *json_path_get_error(void) {
  return g_jsonpath_error[0] ? g_jsonpath_error : NULL;
}
