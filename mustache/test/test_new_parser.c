/**
 * @file test_new_parser.c
 * @brief Test the new re2c + recursive descent parser using acutest framework
 */

#include "acutest.h"
#include "mustache_types.h"
#include "mustache_parser.h"
#include "mustache_lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_simple_template(void) {
    const char *template_str = "Hello {{name}}!";
    
    mustache_parse_ctx_t ctx;
    int result = mustache_parse_template(template_str, strlen(template_str), &ctx);
    
    if (result != 0) {
        TEST_MSG("Parse error: %s", ctx.error_message);
    }
    
    TEST_CHECK(result == 0);
    TEST_CHECK(ctx.root != NULL);
    
    if (ctx.root) {
        TEST_CHECK(ctx.root->type == MUSTACHE_NODE_TEMPLATE);
        
        // Print AST for debugging
        printf("AST for 'Hello {{name}}!':\n");
        mustache_ast_print(ctx.root, 0);
        
        mustache_ast_free(ctx.root);
    }
}

static void test_section_template(void) {
    const char *template_str = "{{#users}}User: {{name}}{{/users}}";
    
    mustache_parse_ctx_t ctx;
    int result = mustache_parse_template(template_str, strlen(template_str), &ctx);
    
    if (result != 0) {
        TEST_MSG("Parse error: %s", ctx.error_message);
    }
    
    TEST_CHECK(result == 0);
    TEST_CHECK(ctx.root != NULL);
    
    if (ctx.root) {
        TEST_CHECK(ctx.root->type == MUSTACHE_NODE_TEMPLATE);
        
        // Print AST for debugging
        printf("AST for '{{#users}}User: {{name}}{{/users}}':\n");
        mustache_ast_print(ctx.root, 0);
        
        mustache_ast_free(ctx.root);
    }
}

static void test_tokenizer(void) {
    const char *template_str = "Hello {{name}}!";
    
    mustache_token_t *tokens;
    size_t token_count;
    
    int result = mustache_tokenize(template_str, strlen(template_str), &tokens, &token_count);
    TEST_CHECK(result == 0);
    TEST_CHECK(token_count > 0);
    
    if (result == 0) {
        printf("Tokenized 'Hello {{name}}!' into %zu tokens:\n", token_count);
        
        // Print tokens for debugging
        for (size_t i = 0; i < token_count; i++) {
            const char *type_names[] = {
                "EOF", "ERROR", "TEXT", "VARIABLE", "SECTION_OPEN", "SECTION_INVERTED",
                "SECTION_CLOSE", "PARTIAL", "COMMENT", "DELIMITER", "UNESCAPED", 
                "UNESCAPED_ALT", "CLOSE", "IDENTIFIER", "STRING", "WHITESPACE"
            };
            
            const char *type_name = (tokens[i].type < 16) ? type_names[tokens[i].type] : "UNKNOWN";
            printf("  Token %zu: %s, text='%.*s', line=%d, col=%d\n", 
                   i, type_name, (int)tokens[i].len, tokens[i].start,
                   tokens[i].line, tokens[i].column);
        }
        
        free(tokens);
    }
}

static void test_complex_template(void) {
    const char *template_str = 
        "{{! This is a comment }}\n"
        "Hello {{name}}!\n"
        "{{#items}}\n"
        "  - {{title}}: {{&description}}\n"
        "{{/items}}\n"
        "{{^empty}}No items{{/empty}}\n"
        "{{>footer}}";
    
    mustache_parse_ctx_t ctx;
    int result = mustache_parse_template(template_str, strlen(template_str), &ctx);
    
    if (result != 0) {
        TEST_MSG("Parse error: %s", ctx.error_message);
    }
    
    TEST_CHECK(result == 0);
    TEST_CHECK(ctx.root != NULL);
    
    if (ctx.root) {
        printf("AST for complex template:\n");
        mustache_ast_print(ctx.root, 0);
        
        mustache_ast_free(ctx.root);
    }
}

TEST_LIST = {
    { "tokenizer", test_tokenizer },
    { "simple_template", test_simple_template },
    { "section_template", test_section_template },
    { "complex_template", test_complex_template },
    { 0 }
};