/**
 * @file mustache_parser.c
 * @brief Recursive descent parser for mustache templates
 */

#include "mustache_lexer.h"
#include "mustache_types.h"
#include "tlog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct {
  mustache_token_t *tokens;
  size_t token_count;
  size_t current;
  mustache_parse_ctx_t *ctx;
} parser_state_t;

static mustache_ast_node_t *create_node(mustache_node_type_t type) {
  mustache_ast_node_t *node = malloc(sizeof(mustache_ast_node_t));
  if (!node)
    return NULL;

  memset(node, 0, sizeof(mustache_ast_node_t));
  node->type = type;
  return node;
}

static void add_child(mustache_ast_node_t *parent, mustache_ast_node_t *child) {
  if (!parent || !child)
    return;

  if (!parent->children) {
    parent->children = malloc(sizeof(mustache_ast_node_t *) * 4);
    parent->children_capacity = 4;
    parent->children_count = 0;
  }

  if (parent->children_count >= parent->children_capacity) {
    parent->children_capacity *= 2;
    parent->children =
        realloc(parent->children, sizeof(mustache_ast_node_t *) * parent->children_capacity);
  }

  parent->children[parent->children_count++] = child;
  child->parent = parent;
}

static char *copy_token_text(const mustache_token_t *token) {
  if (!token || token->len == 0)
    return NULL;

  char *text = malloc(token->len + 1);
  if (!text)
    return NULL;

  memcpy(text, token->start, token->len);
  text[token->len] = '\0';

  return text;
}

static char *copy_token_text_trimmed(const mustache_token_t *token) {
  if (!token || token->len == 0)
    return NULL;

  char *text = malloc(token->len + 1);
  if (!text)
    return NULL;

  memcpy(text, token->start, token->len);
  text[token->len] = '\0';

  // Trim whitespace for identifiers
  char *start = text;
  char *end = text + token->len - 1;

  while (*start && (*start == ' ' || *start == '\t'))
    start++;
  while (end > start && (*end == ' ' || *end == '\t'))
    end--;

  *(end + 1) = '\0';

  if (start != text) {
    memmove(text, start, strlen(start) + 1);
  }

  return text;
}

static mustache_token_t *current_token(parser_state_t *state) {
  if (state->current >= state->token_count)
    return NULL;
  return &state->tokens[state->current];
}

static mustache_token_t *peek_token(parser_state_t *state) { return current_token(state); }

static mustache_token_t *consume_token(parser_state_t *state) {
  mustache_token_t *token = current_token(state);
  if (token)
    state->current++;
  return token;
}

static int expect_token(parser_state_t *state, int expected_type) {
  mustache_token_t *token = current_token(state);
  if (!token || token->type != expected_type) {
    state->ctx->error = 1;
    snprintf(state->ctx->error_message, sizeof(state->ctx->error_message),
             "Expected token type %d, got %d at line %d", expected_type, token ? token->type : -1,
             token ? token->line : 0);
    return 0;
  }
  state->current++;
  return 1;
}

static mustache_ast_node_t *parse_template(parser_state_t *state);

static mustache_ast_node_t *parse_variable(parser_state_t *state, int var_type) {
  mustache_ast_node_t *node =
      create_node(var_type == MUSTACHE_TOKEN_VARIABLE        ? MUSTACHE_NODE_VARIABLE
                  : var_type == MUSTACHE_TOKEN_UNESCAPED     ? MUSTACHE_NODE_UNESCAPED
                  : var_type == MUSTACHE_TOKEN_UNESCAPED_ALT ? MUSTACHE_NODE_UNESCAPED
                                                             : MUSTACHE_NODE_VARIABLE);

  if (!node)
    return NULL;

  // Expect variable name (TEXT token)
  mustache_token_t *name_token = peek_token(state);
  if (name_token && name_token->type == MUSTACHE_TOKEN_TEXT) {
    node->name = copy_token_text_trimmed(name_token);
    node->line = name_token->line;
    node->column = name_token->column;
    consume_token(state);
  }

  // Expect closing }}
  if (!expect_token(state, MUSTACHE_TOKEN_CLOSE)) {
    free(node->name);
    free(node);
    return NULL;
  }

  return node;
}

static mustache_ast_node_t *parse_section(parser_state_t *state, int section_type) {
  mustache_ast_node_t *node = create_node(
      section_type == MUSTACHE_TOKEN_SECTION_OPEN ? MUSTACHE_NODE_SECTION : MUSTACHE_NODE_INVERTED);

  if (!node)
    return NULL;

  // Expect section name (TEXT token)
  mustache_token_t *name_token = peek_token(state);
  if (name_token && name_token->type == MUSTACHE_TOKEN_TEXT) {
    node->name = copy_token_text_trimmed(name_token);
    node->line = name_token->line;
    node->column = name_token->column;
    consume_token(state);
  }

  // Expect closing }}
  if (!expect_token(state, MUSTACHE_TOKEN_CLOSE)) {
    free(node->name);
    free(node);
    return NULL;
  }

  // Parse section content until we find the closing tag
  while (peek_token(state) && peek_token(state)->type != MUSTACHE_TOKEN_SECTION_CLOSE) {
    mustache_ast_node_t *child = parse_template(state);
    if (child) {
      add_child(node, child);
    }
    if (state->ctx->error)
      break;
  }

  // Expect section close
  if (!expect_token(state, MUSTACHE_TOKEN_SECTION_CLOSE)) {
    mustache_ast_free(node);
    return NULL;
  }

  // Expect closing section name
  mustache_token_t *close_name_token = peek_token(state);
  if (close_name_token && close_name_token->type == MUSTACHE_TOKEN_TEXT) {
    char *close_name = copy_token_text(close_name_token);
    if (node->name && close_name && strcmp(node->name, close_name) != 0) {
      state->ctx->error = 1;
      snprintf(state->ctx->error_message, sizeof(state->ctx->error_message),
               "Section name mismatch: '%s' vs '%s'", node->name, close_name);
      free(close_name);
      mustache_ast_free(node);
      return NULL;
    }
    free(close_name);
    consume_token(state);
  }

  // Expect final closing }}
  if (!expect_token(state, MUSTACHE_TOKEN_CLOSE)) {
    mustache_ast_free(node);
    return NULL;
  }

  return node;
}

static mustache_ast_node_t *parse_partial(parser_state_t *state) {
  mustache_ast_node_t *node = create_node(MUSTACHE_NODE_PARTIAL);
  if (!node)
    return NULL;

  // Expect partial name (TEXT token)
  mustache_token_t *name_token = peek_token(state);
  if (name_token && name_token->type == MUSTACHE_TOKEN_TEXT) {
    node->name = copy_token_text(name_token);
    node->line = name_token->line;
    node->column = name_token->column;
    consume_token(state);
  }

  // Expect closing }}
  if (!expect_token(state, MUSTACHE_TOKEN_CLOSE)) {
    free(node->name);
    free(node);
    return NULL;
  }

  return node;
}

static mustache_ast_node_t *parse_comment(parser_state_t *state) {
  mustache_ast_node_t *node = create_node(MUSTACHE_NODE_COMMENT);
  if (!node)
    return NULL;

  // Expect comment text (TEXT token)
  mustache_token_t *text_token = peek_token(state);
  if (text_token && text_token->type == MUSTACHE_TOKEN_TEXT) {
    node->text = copy_token_text(text_token);
    node->line = text_token->line;
    node->column = text_token->column;
    consume_token(state);
  }

  // Expect closing }}
  if (!expect_token(state, MUSTACHE_TOKEN_CLOSE)) {
    free(node->text);
    free(node);
    return NULL;
  }

  return node;
}

static mustache_ast_node_t *parse_template(parser_state_t *state) {
  mustache_token_t *token = peek_token(state);
  if (!token || token->type == MUSTACHE_TOKEN_EOF) {
    return NULL;
  }

  switch (token->type) {
  case MUSTACHE_TOKEN_TEXT: {
    mustache_ast_node_t *node = create_node(MUSTACHE_NODE_TEXT);
    if (node) {
      node->text = copy_token_text(token);
      node->line = token->line;
      node->column = token->column;
    }
    consume_token(state);
    return node;
  }

  case MUSTACHE_TOKEN_VARIABLE:
  case MUSTACHE_TOKEN_UNESCAPED:
  case MUSTACHE_TOKEN_UNESCAPED_ALT:
    consume_token(state);
    return parse_variable(state, token->type);

  case MUSTACHE_TOKEN_SECTION_OPEN:
  case MUSTACHE_TOKEN_SECTION_INVERTED:
    consume_token(state);
    return parse_section(state, token->type);

  case MUSTACHE_TOKEN_PARTIAL:
    consume_token(state);
    return parse_partial(state);

  case MUSTACHE_TOKEN_COMMENT:
    consume_token(state);
    return parse_comment(state);

  case MUSTACHE_TOKEN_DELIMITER:
    // Skip delimiter changes for now
    consume_token(state);
    if (peek_token(state) && peek_token(state)->type == MUSTACHE_TOKEN_TEXT) {
      consume_token(state); // Skip delimiter content
    }
    if (peek_token(state) && peek_token(state)->type == MUSTACHE_TOKEN_CLOSE) {
      consume_token(state); // Skip closing }}
    }
    return parse_template(state); // Continue parsing

  default:
    // Skip unknown tokens
    consume_token(state);
    return parse_template(state);
  }
}

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

  // Create parser state
  parser_state_t state = {.tokens = tokens, .token_count = token_count, .current = 0, .ctx = ctx};

  // Create root template node
  ctx->root = create_node(MUSTACHE_NODE_TEMPLATE);
  if (!ctx->root) {
    free(tokens);
    ctx->error = 1;
    snprintf(ctx->error_message, sizeof(ctx->error_message), "Memory allocation failed");
    return -1;
  }

  // Parse all elements
  while (peek_token(&state) && peek_token(&state)->type != MUSTACHE_TOKEN_EOF && !ctx->error) {
    mustache_ast_node_t *element = parse_template(&state);
    if (element) {
      add_child(ctx->root, element);
    }
  }

  free(tokens);
  return ctx->error ? -1 : 0;
}

void mustache_ast_free(mustache_ast_node_t *node) {
  if (!node)
    return;

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
  if (!node)
    return;

  const char *type_names[] = {"TEMPLATE", "TEXT",    "VARIABLE", "UNESCAPED", "SECTION",
                              "INVERTED", "PARTIAL", "COMMENT",  "IDENTIFIER"};

  if (node->name && node->text) {
    TLOG_DEBUG("{:*>{}} {} name='{}' text='{:.20s}{}' ({}:{})", "", indent * 2,
               type_names[node->type], node->name, node->text, strlen(node->text) > 20 ? "..." : "",
               node->line, node->column);
  } else if (node->name) {
    TLOG_DEBUG("{:*>{}} {} name='{}' ({}:{})", "", indent * 2, type_names[node->type], node->name,
               node->line, node->column);
  } else if (node->text) {
    TLOG_DEBUG("{:*>{}} {} text='{:.20s}{}' ({}:{})", "", indent * 2, type_names[node->type],
               node->text, strlen(node->text) > 20 ? "..." : "", node->line, node->column);
  } else {
    TLOG_DEBUG("{:*>{}} {} ({}:{})", "", indent * 2, type_names[node->type], node->line,
               node->column);
  }

  for (size_t i = 0; i < node->children_count; i++) {
    mustache_ast_print(node->children[i], indent + 1);
  }
}