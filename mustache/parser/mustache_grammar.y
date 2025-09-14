/**
 * @file mustache_grammar.y
 * @brief Mustache Template Parser Grammar (Lemon) - Conflict-free version
 *
 * Build: lemon -Tlempar.c mustache_grammar.y
 */

%name MustacheParse
%token_prefix MUSTACHE_TOKEN_
%token_type {mustache_token_t}
%default_type {mustache_ast_node_t*}
%stack_size 512

%extra_argument {mustache_parse_ctx_t *ctx}

%include {
#include "mustache_lexer.h"
#include "mustache_types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static mustache_ast_node_t *create_node(mustache_parse_ctx_t *ctx, mustache_node_type_t type) {
    mustache_ast_node_t *node = malloc(sizeof(mustache_ast_node_t));
    if (!node) return NULL;
    
    memset(node, 0, sizeof(mustache_ast_node_t));
    node->type = type;
    node->line = 0;
    node->column = 0;
    
    return node;
}

static char *copy_token_text(const mustache_token_t *token) {
    if (!token || token->len == 0) return NULL;
    
    char *text = malloc(token->len + 1);
    if (!text) return NULL;
    
    memcpy(text, token->start, token->len);
    text[token->len] = '\0';
    
    // Trim whitespace
    char *start = text;
    char *end = text + token->len - 1;
    
    while (*start && (*start == ' ' || *start == '\t')) start++;
    while (end > start && (*end == ' ' || *end == '\t')) end--;
    
    *(end + 1) = '\0';
    
    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }
    
    return text;
}

static void add_child(mustache_ast_node_t *parent, mustache_ast_node_t *child) {
    if (!parent || !child) return;
    
    if (!parent->children) {
        parent->children = malloc(sizeof(mustache_ast_node_t*) * 4);
        parent->children_capacity = 4;
        parent->children_count = 0;
    }
    
    if (parent->children_count >= parent->children_capacity) {
        parent->children_capacity *= 2;
        parent->children = realloc(parent->children, 
                                 sizeof(mustache_ast_node_t*) * parent->children_capacity);
    }
    
    parent->children[parent->children_count++] = child;
    child->parent = parent;
}

}

%syntax_error {
    ctx->error = 1;
    ctx->error_line = TOKEN.line;
    ctx->error_column = TOKEN.column;
    snprintf(ctx->error_message, sizeof(ctx->error_message), 
             "Syntax error at line %d, column %d", TOKEN.line, TOKEN.column);
}

%parse_failure {
    ctx->error = 1;
    snprintf(ctx->error_message, sizeof(ctx->error_message), "Parse failure");
}

// Define precedence to resolve conflicts
%left TEXT.
%left VARIABLE UNESCAPED UNESCAPED_ALT SECTION_OPEN SECTION_INVERTED PARTIAL COMMENT.

// Start rule
template ::= element_list(A). { 
    ctx->root = A ? A : create_node(ctx, MUSTACHE_NODE_TEMPLATE); 
}

template ::= . { 
    ctx->root = create_node(ctx, MUSTACHE_NODE_TEMPLATE); 
}

// Element list (sequence of elements)
element_list(A) ::= element_list(B) element(C). {
    A = B ? B : create_node(ctx, MUSTACHE_NODE_TEMPLATE);
    if (C) add_child(A, C);
}

element_list(A) ::= element(B). {
    A = create_node(ctx, MUSTACHE_NODE_TEMPLATE);
    if (B) add_child(A, B);
}

// Individual elements
element(A) ::= text(B). { A = B; }
element(A) ::= variable(B). { A = B; }
element(A) ::= section(B). { A = B; }
element(A) ::= inverted_section(B). { A = B; }
element(A) ::= partial(B). { A = B; }
element(A) ::= comment(B). { A = B; }

// Text content
text(A) ::= TEXT(B). {
    A = create_node(ctx, MUSTACHE_NODE_TEXT);
    if (A) {
        A->text = copy_token_text(&B);
        A->line = B.line;
        A->column = B.column;
    }
}

// Variables
variable(A) ::= VARIABLE tag_content(C) CLOSE. {
    A = create_node(ctx, MUSTACHE_NODE_VARIABLE);
    if (A && C) {
        A->name = C->name;
        A->line = C->line;
        A->column = C->column;
        free(C);
    }
}

variable(A) ::= UNESCAPED tag_content(C) CLOSE. {
    A = create_node(ctx, MUSTACHE_NODE_UNESCAPED);
    if (A && C) {
        A->name = C->name;
        A->line = C->line;
        A->column = C->column;
        free(C);
    }
}

variable(A) ::= UNESCAPED_ALT tag_content(C) CLOSE. {
    A = create_node(ctx, MUSTACHE_NODE_UNESCAPED);
    if (A && C) {
        A->name = C->name;
        A->line = C->line;
        A->column = C->column;
        free(C);
    }
}

// Sections
section(A) ::= section_open(B) element_list(C) section_close(D). {
    A = create_node(ctx, MUSTACHE_NODE_SECTION);
    if (A && B && D) {
        // Verify section names match
        if (B->name && D->name && strcmp(B->name, D->name) == 0) {
            A->name = strdup(B->name);
            A->line = B->line;
            A->column = B->column;
            if (C) add_child(A, C);
        } else {
            ctx->error = 1;
            snprintf(ctx->error_message, sizeof(ctx->error_message),
                    "Section name mismatch: '%s' vs '%s'", 
                    B->name ? B->name : "(null)", 
                    D->name ? D->name : "(null)");
        }
        free(B->name); free(B);
        free(D->name); free(D);
    }
}

section(A) ::= section_open(B) section_close(D). {
    A = create_node(ctx, MUSTACHE_NODE_SECTION);
    if (A && B && D) {
        // Verify section names match
        if (B->name && D->name && strcmp(B->name, D->name) == 0) {
            A->name = strdup(B->name);
            A->line = B->line;
            A->column = B->column;
        } else {
            ctx->error = 1;
            snprintf(ctx->error_message, sizeof(ctx->error_message),
                    "Section name mismatch: '%s' vs '%s'", 
                    B->name ? B->name : "(null)", 
                    D->name ? D->name : "(null)");
        }
        free(B->name); free(B);
        free(D->name); free(D);
    }
}

inverted_section(A) ::= inverted_open(B) element_list(C) section_close(D). {
    A = create_node(ctx, MUSTACHE_NODE_INVERTED);
    if (A && B && D) {
        // Verify section names match
        if (B->name && D->name && strcmp(B->name, D->name) == 0) {
            A->name = strdup(B->name);
            A->line = B->line;
            A->column = B->column;
            if (C) add_child(A, C);
        } else {
            ctx->error = 1;
            snprintf(ctx->error_message, sizeof(ctx->error_message),
                    "Inverted section name mismatch: '%s' vs '%s'", 
                    B->name ? B->name : "(null)", 
                    D->name ? D->name : "(null)");
        }
        free(B->name); free(B);
        free(D->name); free(D);
    }
}

inverted_section(A) ::= inverted_open(B) section_close(D). {
    A = create_node(ctx, MUSTACHE_NODE_INVERTED);
    if (A && B && D) {
        // Verify section names match
        if (B->name && D->name && strcmp(B->name, D->name) == 0) {
            A->name = strdup(B->name);
            A->line = B->line;
            A->column = B->column;
        } else {
            ctx->error = 1;
            snprintf(ctx->error_message, sizeof(ctx->error_message),
                    "Inverted section name mismatch: '%s' vs '%s'", 
                    B->name ? B->name : "(null)", 
                    D->name ? D->name : "(null)");
        }
        free(B->name); free(B);
        free(D->name); free(D);
    }
}

// Section openers and closers
section_open(A) ::= SECTION_OPEN tag_content(C) CLOSE. {
    A = C;
}

inverted_open(A) ::= SECTION_INVERTED tag_content(C) CLOSE. {
    A = C;
}

section_close(A) ::= SECTION_CLOSE tag_content(C) CLOSE. {
    A = C;
}

// Partials
partial(A) ::= PARTIAL tag_content(C) CLOSE. {
    A = create_node(ctx, MUSTACHE_NODE_PARTIAL);
    if (A && C) {
        A->name = C->name;
        A->line = C->line;
        A->column = C->column;
        free(C);
    }
}

// Comments
comment(A) ::= COMMENT comment_content(C) CLOSE. {
    A = create_node(ctx, MUSTACHE_NODE_COMMENT);
    if (A && C) {
        A->text = C->text;
        A->line = C->line;
        A->column = C->column;
        free(C);
    }
}

comment(A) ::= COMMENT CLOSE. {
    A = create_node(ctx, MUSTACHE_NODE_COMMENT);
    if (A) {
        A->text = strdup("");
    }
}

// Tag content (identifier inside mustache tags)
tag_content(A) ::= TEXT(B). {
    A = create_node(ctx, MUSTACHE_NODE_IDENTIFIER);
    if (A) {
        A->name = copy_token_text(&B);
        A->line = B.line;
        A->column = B.column;
    }
}

// Comment content
comment_content(A) ::= TEXT(B). {
    A = create_node(ctx, MUSTACHE_NODE_TEXT);
    if (A) {
        A->text = copy_token_text(&B);
        A->line = B.line;
        A->column = B.column;
    }
}