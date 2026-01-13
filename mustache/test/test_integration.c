/**
 * @file test_integration.c
 * @brief Integration tests for the complete mustache lexer + parser pipeline
 */

#include "acutest.h"
#include "mustache_types.h"
#include "mustache_parser.h"
#include "mustache_lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_full_pipeline_simple(void) {
    const char *template_str = "Hello {{name}}!";
    
    // Test tokenization
    mustache_token_t *tokens;
    size_t token_count;
    int lex_result = mustache_tokenize(template_str, strlen(template_str), &tokens, &token_count);
    TEST_CHECK(lex_result == 0);
    TEST_CHECK(token_count == 6); // TEXT + VARIABLE + TEXT + CLOSE + TEXT + EOF
    
    // Test parsing
    mustache_parse_ctx_t ctx;
    int parse_result = mustache_parse_template(template_str, strlen(template_str), &ctx);
    TEST_CHECK(parse_result == 0);
    TEST_CHECK(ctx.root != NULL);
    TEST_CHECK(ctx.root->type == MUSTACHE_NODE_TEMPLATE);
    TEST_CHECK(ctx.root->children_count == 3); // "Hello ", {{name}}, "!"
    
    if (ctx.root && ctx.root->children_count >= 3) {
        TEST_CHECK(ctx.root->children[0]->type == MUSTACHE_NODE_TEXT);
        TEST_CHECK(strcmp(ctx.root->children[0]->text, "Hello ") == 0);
        
        TEST_CHECK(ctx.root->children[1]->type == MUSTACHE_NODE_VARIABLE);
        TEST_CHECK(strcmp(ctx.root->children[1]->name, "name") == 0);
        
        TEST_CHECK(ctx.root->children[2]->type == MUSTACHE_NODE_TEXT);
        TEST_CHECK(strcmp(ctx.root->children[2]->text, "!") == 0);
    }
    
    free(tokens);
    mustache_ast_free(ctx.root);
}

static void test_full_pipeline_sections(void) {
    const char *template_str = "{{#items}}Item: {{name}}{{/items}}";
    
    mustache_parse_ctx_t ctx;
    int result = mustache_parse_template(template_str, strlen(template_str), &ctx);
    
    TEST_CHECK(result == 0);
    TEST_CHECK(ctx.root != NULL);
    TEST_CHECK(ctx.root->children_count == 1);
    
    if (ctx.root && ctx.root->children_count > 0) {
        mustache_ast_node_t *section = ctx.root->children[0];
        TEST_CHECK(section->type == MUSTACHE_NODE_SECTION);
        TEST_CHECK(strcmp(section->name, "items") == 0);
        TEST_CHECK(section->children_count == 2); // "Item: " + {{name}}
        
        if (section->children_count >= 2) {
            TEST_CHECK(section->children[0]->type == MUSTACHE_NODE_TEXT);
            TEST_CHECK(strcmp(section->children[0]->text, "Item: ") == 0);
            
            TEST_CHECK(section->children[1]->type == MUSTACHE_NODE_VARIABLE);
            TEST_CHECK(strcmp(section->children[1]->name, "name") == 0);
        }
    }
    
    mustache_ast_free(ctx.root);
}

static void test_full_pipeline_nested(void) {
    const char *template_str = "{{#outer}}{{#inner}}{{value}}{{/inner}}{{/outer}}";
    
    mustache_parse_ctx_t ctx;
    int result = mustache_parse_template(template_str, strlen(template_str), &ctx);
    
    TEST_CHECK(result == 0);
    TEST_CHECK(ctx.root != NULL);
    
    if (ctx.root && ctx.root->children_count > 0) {
        mustache_ast_node_t *outer = ctx.root->children[0];
        TEST_CHECK(outer->type == MUSTACHE_NODE_SECTION);
        TEST_CHECK(strcmp(outer->name, "outer") == 0);
        
        if (outer->children_count > 0) {
            mustache_ast_node_t *inner = outer->children[0];
            TEST_CHECK(inner->type == MUSTACHE_NODE_SECTION);
            TEST_CHECK(strcmp(inner->name, "inner") == 0);
            
            if (inner->children_count > 0) {
                TEST_CHECK(inner->children[0]->type == MUSTACHE_NODE_VARIABLE);
                TEST_CHECK(strcmp(inner->children[0]->name, "value") == 0);
            }
        }
    }
    
    mustache_ast_free(ctx.root);
}

static void test_full_pipeline_all_features(void) {
    const char *template_str = 
        "{{! Header comment }}\n"
        "Hello {{name}}!\n"
        "{{#items}}\n"
        "  - {{title}}: {{{description}}}\n"
        "{{/items}}\n"
        "{{^empty}}No items{{/empty}}\n"
        "{{>footer}}";
    
    mustache_parse_ctx_t ctx;
    int result = mustache_parse_template(template_str, strlen(template_str), &ctx);
    
    TEST_CHECK(result == 0);
    TEST_CHECK(ctx.root != NULL);
    TEST_CHECK(ctx.root->children_count > 5); // Multiple elements
    
    printf("Full pipeline AST:\n");
    mustache_ast_print(ctx.root, 0);
    
    // Verify we have different node types
    int has_comment = 0, has_variable = 0, has_section = 0, has_inverted = 0, has_unescaped = 0, has_partial = 0;
    
    for (size_t i = 0; i < ctx.root->children_count; i++) {
        mustache_ast_node_t *child = ctx.root->children[i];
        switch (child->type) {
            case MUSTACHE_NODE_COMMENT: has_comment = 1; break;
            case MUSTACHE_NODE_VARIABLE: has_variable = 1; break;
            case MUSTACHE_NODE_SECTION: has_section = 1; break;
            case MUSTACHE_NODE_INVERTED: has_inverted = 1; break;
            case MUSTACHE_NODE_UNESCAPED: has_unescaped = 1; break;
            case MUSTACHE_NODE_PARTIAL: has_partial = 1; break;
            default: break;
        }
        
        // Check nested content in sections
        if (child->type == MUSTACHE_NODE_SECTION && child->children_count > 0) {
            for (size_t j = 0; j < child->children_count; j++) {
                if (child->children[j]->type == MUSTACHE_NODE_UNESCAPED) {
                    has_unescaped = 1;
                }
            }
        }
    }
    
    TEST_CHECK(has_comment);
    TEST_CHECK(has_variable);
    TEST_CHECK(has_section);
    TEST_CHECK(has_inverted);
    TEST_CHECK(has_unescaped);
    TEST_CHECK(has_partial);
    
    mustache_ast_free(ctx.root);
}

static void test_error_handling(void) {
    // Test section name mismatch
    const char *bad_template = "{{#users}}content{{/items}}";
    
    mustache_parse_ctx_t ctx;
    int result = mustache_parse_template(bad_template, strlen(bad_template), &ctx);
    
    TEST_CHECK(result != 0);
    TEST_CHECK(ctx.error == 1);
    
    printf("Expected error message: %s\n", ctx.error_message);
    
    if (ctx.root) {
        mustache_ast_free(ctx.root);
    }
}

static void test_performance_large_template(void) {
    // Create a large template with many variables and sections
    char *large_template = malloc(10000);
    strcpy(large_template, "{{! Large template test }}\n");
    
    for (int i = 0; i < 100; i++) {
        char section[100];
        snprintf(section, sizeof(section), "{{#section%d}}Variable {{var%d}} in section %d{{/section%d}}\n", i, i, i, i);
        strcat(large_template, section);
    }
    
    mustache_parse_ctx_t ctx;
    int result = mustache_parse_template(large_template, strlen(large_template), &ctx);
    
    TEST_CHECK(result == 0);
    TEST_CHECK(ctx.root != NULL);
    TEST_CHECK(ctx.root->children_count > 100); // Should have many elements
    
    printf("Large template parsed successfully with %zu top-level elements\n", 
           ctx.root ? ctx.root->children_count : 0);
    
    mustache_ast_free(ctx.root);
    free(large_template);
}

TEST_LIST = {
    { "full_pipeline_simple", test_full_pipeline_simple },
    { "full_pipeline_sections", test_full_pipeline_sections },
    { "full_pipeline_nested", test_full_pipeline_nested },
    { "full_pipeline_all_features", test_full_pipeline_all_features },
    { "error_handling", test_error_handling },
    { "performance_large_template", test_performance_large_template },
    { 0 }
};