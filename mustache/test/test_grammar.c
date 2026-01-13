/**
 * @file test_grammar.c
 * @brief Comprehensive unit tests for the mustache grammar parser
 */

#include "acutest.h"
#include "mustache_types.h"
#include "mustache_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* node_type_name(mustache_node_type_t type) {
    static const char *names[] = {
        "TEMPLATE", "TEXT", "VARIABLE", "UNESCAPED", "SECTION", 
        "INVERTED", "PARTIAL", "COMMENT", "IDENTIFIER"
    };
    return (type >= 0 && type < 9) ? names[type] : "UNKNOWN";
}

static void print_ast_node(const mustache_ast_node_t *node, int indent) {
    if (!node) return;
    
    for (int i = 0; i < indent; i++) printf("  ");
    
    printf("%s", node_type_name(node->type));
    if (node->name) printf(" name='%s'", node->name);
    if (node->text) printf(" text='%s'", node->text);
    printf(" (line %d, col %d)\n", node->line, node->column);
    
    for (size_t i = 0; i < node->children_count; i++) {
        print_ast_node(node->children[i], indent + 1);
    }
}

static void test_simple_text(void) {
    const char *input = "Hello World";
    mustache_parse_ctx_t ctx;
    
    int result = mustache_parse_template(input, strlen(input), &ctx);
    
    if (result != 0) {
        TEST_MSG("Parse error: %s", ctx.error_message);
    }
    
    TEST_CHECK(result == 0);
    TEST_CHECK(ctx.root != NULL);
    
    if (ctx.root) {
        TEST_CHECK(ctx.root->type == MUSTACHE_NODE_TEMPLATE);
        TEST_CHECK(ctx.root->children_count == 1);
        
        if (ctx.root->children_count > 0) {
            mustache_ast_node_t *text_node = ctx.root->children[0];
            TEST_CHECK(text_node->type == MUSTACHE_NODE_TEXT);
            TEST_CHECK(text_node->text != NULL);
            TEST_CHECK(strcmp(text_node->text, "Hello World") == 0);
        }
        
        printf("AST for 'Hello World':\n");
        print_ast_node(ctx.root, 0);
        
        mustache_ast_free(ctx.root);
    }
}

static void test_simple_variable(void) {
    const char *input = "{{name}}";
    mustache_parse_ctx_t ctx;
    
    int result = mustache_parse_template(input, strlen(input), &ctx);
    
    if (result != 0) {
        TEST_MSG("Parse error: %s", ctx.error_message);
    }
    
    TEST_CHECK(result == 0);
    TEST_CHECK(ctx.root != NULL);
    
    if (ctx.root) {
        TEST_CHECK(ctx.root->type == MUSTACHE_NODE_TEMPLATE);
        TEST_CHECK(ctx.root->children_count == 1);
        
        if (ctx.root->children_count > 0) {
            mustache_ast_node_t *var_node = ctx.root->children[0];
            TEST_CHECK(var_node->type == MUSTACHE_NODE_VARIABLE);
            TEST_CHECK(var_node->name != NULL);
            TEST_CHECK(strcmp(var_node->name, "name") == 0);
        }
        
        printf("AST for '{{name}}':\n");
        print_ast_node(ctx.root, 0);
        
        mustache_ast_free(ctx.root);
    }
}

static void test_mixed_content(void) {
    const char *input = "Hello {{name}}!";
    mustache_parse_ctx_t ctx;
    
    int result = mustache_parse_template(input, strlen(input), &ctx);
    
    if (result != 0) {
        TEST_MSG("Parse error: %s", ctx.error_message);
    }
    
    TEST_CHECK(result == 0);
    TEST_CHECK(ctx.root != NULL);
    
    if (ctx.root) {
        TEST_CHECK(ctx.root->type == MUSTACHE_NODE_TEMPLATE);
        TEST_CHECK(ctx.root->children_count == 3); // "Hello ", {{name}}, "!"
        
        if (ctx.root->children_count >= 3) {
            TEST_CHECK(ctx.root->children[0]->type == MUSTACHE_NODE_TEXT);
            TEST_CHECK(ctx.root->children[1]->type == MUSTACHE_NODE_VARIABLE);
            TEST_CHECK(ctx.root->children[2]->type == MUSTACHE_NODE_TEXT);
            
            TEST_CHECK(strcmp(ctx.root->children[0]->text, "Hello ") == 0);
            TEST_CHECK(strcmp(ctx.root->children[1]->name, "name") == 0);
            TEST_CHECK(strcmp(ctx.root->children[2]->text, "!") == 0);
        }
        
        printf("AST for 'Hello {{name}}!':\n");
        print_ast_node(ctx.root, 0);
        
        mustache_ast_free(ctx.root);
    }
}

static void test_section(void) {
    const char *input = "{{#users}}Hello {{name}}{{/users}}";
    mustache_parse_ctx_t ctx;
    
    int result = mustache_parse_template(input, strlen(input), &ctx);
    
    if (result != 0) {
        TEST_MSG("Parse error: %s", ctx.error_message);
    }
    
    TEST_CHECK(result == 0);
    TEST_CHECK(ctx.root != NULL);
    
    if (ctx.root) {
        TEST_CHECK(ctx.root->type == MUSTACHE_NODE_TEMPLATE);
        TEST_CHECK(ctx.root->children_count == 1);
        
        if (ctx.root->children_count > 0) {
            mustache_ast_node_t *section = ctx.root->children[0];
            TEST_CHECK(section->type == MUSTACHE_NODE_SECTION);
            TEST_CHECK(section->name != NULL);
            TEST_CHECK(strcmp(section->name, "users") == 0);
            
            // Section should have content
            TEST_CHECK(section->children_count > 0);
        }
        
        printf("AST for '{{#users}}Hello {{name}}{{/users}}':\n");
        print_ast_node(ctx.root, 0);
        
        mustache_ast_free(ctx.root);
    }
}

static void test_inverted_section(void) {
    const char *input = "{{^empty}}Not empty{{/empty}}";
    mustache_parse_ctx_t ctx;
    
    int result = mustache_parse_template(input, strlen(input), &ctx);
    
    if (result != 0) {
        TEST_MSG("Parse error: %s", ctx.error_message);
    }
    
    TEST_CHECK(result == 0);
    TEST_CHECK(ctx.root != NULL);
    
    if (ctx.root) {
        TEST_CHECK(ctx.root->type == MUSTACHE_NODE_TEMPLATE);
        TEST_CHECK(ctx.root->children_count == 1);
        
        if (ctx.root->children_count > 0) {
            mustache_ast_node_t *section = ctx.root->children[0];
            TEST_CHECK(section->type == MUSTACHE_NODE_INVERTED);
            TEST_CHECK(section->name != NULL);
            TEST_CHECK(strcmp(section->name, "empty") == 0);
        }
        
        printf("AST for '{{^empty}}Not empty{{/empty}}':\n");
        print_ast_node(ctx.root, 0);
        
        mustache_ast_free(ctx.root);
    }
}

static void test_unescaped_variable(void) {
    const char *input = "{{{html}}}";
    mustache_parse_ctx_t ctx;
    
    int result = mustache_parse_template(input, strlen(input), &ctx);
    
    if (result != 0) {
        TEST_MSG("Parse error: %s", ctx.error_message);
    }
    
    TEST_CHECK(result == 0);
    TEST_CHECK(ctx.root != NULL);
    
    if (ctx.root) {
        TEST_CHECK(ctx.root->children_count == 1);
        
        if (ctx.root->children_count > 0) {
            mustache_ast_node_t *var_node = ctx.root->children[0];
            TEST_CHECK(var_node->type == MUSTACHE_NODE_UNESCAPED);
            TEST_CHECK(var_node->name != NULL);
            TEST_CHECK(strcmp(var_node->name, "html") == 0);
        }
        
        printf("AST for '{{{html}}}':\n");
        print_ast_node(ctx.root, 0);
        
        mustache_ast_free(ctx.root);
    }
}

static void test_unescaped_ampersand(void) {
    const char *input = "{{&html}}";
    mustache_parse_ctx_t ctx;
    
    int result = mustache_parse_template(input, strlen(input), &ctx);
    
    if (result != 0) {
        TEST_MSG("Parse error: %s", ctx.error_message);
    }
    
    TEST_CHECK(result == 0);
    TEST_CHECK(ctx.root != NULL);
    
    if (ctx.root) {
        TEST_CHECK(ctx.root->children_count == 1);
        
        if (ctx.root->children_count > 0) {
            mustache_ast_node_t *var_node = ctx.root->children[0];
            TEST_CHECK(var_node->type == MUSTACHE_NODE_UNESCAPED);
            TEST_CHECK(var_node->name != NULL);
            TEST_CHECK(strcmp(var_node->name, "html") == 0);
        }
        
        printf("AST for '{{&html}}':\n");
        print_ast_node(ctx.root, 0);
        
        mustache_ast_free(ctx.root);
    }
}

static void test_partial(void) {
    const char *input = "{{>header}}";
    mustache_parse_ctx_t ctx;
    
    int result = mustache_parse_template(input, strlen(input), &ctx);
    
    if (result != 0) {
        TEST_MSG("Parse error: %s", ctx.error_message);
    }
    
    TEST_CHECK(result == 0);
    TEST_CHECK(ctx.root != NULL);
    
    if (ctx.root) {
        TEST_CHECK(ctx.root->children_count == 1);
        
        if (ctx.root->children_count > 0) {
            mustache_ast_node_t *partial_node = ctx.root->children[0];
            TEST_CHECK(partial_node->type == MUSTACHE_NODE_PARTIAL);
            TEST_CHECK(partial_node->name != NULL);
            TEST_CHECK(strcmp(partial_node->name, "header") == 0);
        }
        
        printf("AST for '{{>header}}':\n");
        print_ast_node(ctx.root, 0);
        
        mustache_ast_free(ctx.root);
    }
}

static void test_comment(void) {
    const char *input = "{{! This is a comment }}";
    mustache_parse_ctx_t ctx;
    
    int result = mustache_parse_template(input, strlen(input), &ctx);
    
    if (result != 0) {
        TEST_MSG("Parse error: %s", ctx.error_message);
    }
    
    TEST_CHECK(result == 0);
    TEST_CHECK(ctx.root != NULL);
    
    if (ctx.root) {
        TEST_CHECK(ctx.root->children_count == 1);
        
        if (ctx.root->children_count > 0) {
            mustache_ast_node_t *comment_node = ctx.root->children[0];
            TEST_CHECK(comment_node->type == MUSTACHE_NODE_COMMENT);
        }
        
        printf("AST for '{{! This is a comment }}':\n");
        print_ast_node(ctx.root, 0);
        
        mustache_ast_free(ctx.root);
    }
}

static void test_nested_sections(void) {
    const char *input = "{{#outer}}{{#inner}}{{value}}{{/inner}}{{/outer}}";
    mustache_parse_ctx_t ctx;
    
    int result = mustache_parse_template(input, strlen(input), &ctx);
    
    if (result != 0) {
        TEST_MSG("Parse error: %s", ctx.error_message);
    }
    
    TEST_CHECK(result == 0);
    TEST_CHECK(ctx.root != NULL);
    
    if (ctx.root) {
        TEST_CHECK(ctx.root->children_count == 1);
        
        if (ctx.root->children_count > 0) {
            mustache_ast_node_t *outer = ctx.root->children[0];
            TEST_CHECK(outer->type == MUSTACHE_NODE_SECTION);
            TEST_CHECK(strcmp(outer->name, "outer") == 0);
            
            // Outer section should contain inner section
            TEST_CHECK(outer->children_count > 0);
            if (outer->children_count > 0) {
                mustache_ast_node_t *inner = outer->children[0];
                TEST_CHECK(inner->type == MUSTACHE_NODE_SECTION);
                TEST_CHECK(strcmp(inner->name, "inner") == 0);
            }
        }
        
        printf("AST for nested sections:\n");
        print_ast_node(ctx.root, 0);
        
        mustache_ast_free(ctx.root);
    }
}

static void test_complex_template(void) {
    const char *input = 
        "{{! Header comment }}\n"
        "Hello {{name}}!\n"
        "{{#items}}\n"
        "  - {{title}}: {{&description}}\n"
        "{{/items}}\n"
        "{{^empty}}No items{{/empty}}\n"
        "{{>footer}}";
    
    mustache_parse_ctx_t ctx;
    
    int result = mustache_parse_template(input, strlen(input), &ctx);
    
    if (result != 0) {
        TEST_MSG("Parse error: %s", ctx.error_message);
    }
    
    TEST_CHECK(result == 0);
    TEST_CHECK(ctx.root != NULL);
    
    if (ctx.root) {
        TEST_CHECK(ctx.root->type == MUSTACHE_NODE_TEMPLATE);
        TEST_CHECK(ctx.root->children_count > 5); // Multiple elements
        
        printf("AST for complex template:\n");
        print_ast_node(ctx.root, 0);
        
        mustache_ast_free(ctx.root);
    }
}

static void test_empty_template(void) {
    const char *input = "";
    mustache_parse_ctx_t ctx;
    
    int result = mustache_parse_template(input, strlen(input), &ctx);
    
    TEST_CHECK(result == 0);
    TEST_CHECK(ctx.root != NULL);
    
    if (ctx.root) {
        TEST_CHECK(ctx.root->type == MUSTACHE_NODE_TEMPLATE);
        TEST_CHECK(ctx.root->children_count == 0);
        
        mustache_ast_free(ctx.root);
    }
}

static void test_section_mismatch(void) {
    const char *input = "{{#users}}content{{/items}}";
    mustache_parse_ctx_t ctx;
    
    int result = mustache_parse_template(input, strlen(input), &ctx);
    
    // Should fail due to section name mismatch
    TEST_CHECK(result != 0);
    TEST_CHECK(ctx.error == 1);
    
    if (result != 0) {
        printf("Expected error: %s\n", ctx.error_message);
    }
    
    if (ctx.root) {
        mustache_ast_free(ctx.root);
    }
}

TEST_LIST = {
    { "simple_text", test_simple_text },
    { "simple_variable", test_simple_variable },
    { "mixed_content", test_mixed_content },
    { "section", test_section },
    { "inverted_section", test_inverted_section },
    { "unescaped_variable", test_unescaped_variable },
    { "unescaped_ampersand", test_unescaped_ampersand },
    { "partial", test_partial },
    { "comment", test_comment },
    { "nested_sections", test_nested_sections },
    { "complex_template", test_complex_template },
    { "empty_template", test_empty_template },
    { "section_mismatch", test_section_mismatch },
    { 0 }
};