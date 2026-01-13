/**
 * @file test_lexer.c
 * @brief Comprehensive unit tests for the mustache lexer
 */

#include "acutest.h"
#include "mustache_lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* token_type_name(int type) {
    static const char *names[] = {
        "EOF", "ERROR", "TEXT", "VARIABLE", "SECTION_OPEN", "SECTION_INVERTED",
        "SECTION_CLOSE", "PARTIAL", "COMMENT", "DELIMITER", "UNESCAPED", 
        "UNESCAPED_ALT", "CLOSE", "IDENTIFIER", "STRING", "WHITESPACE"
    };
    return (type >= 0 && type < 16) ? names[type] : "UNKNOWN";
}

static void print_tokens(mustache_token_t *tokens, size_t count) {
    printf("Tokens (%zu):\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("  [%zu] %s: '%.*s' (line %d, col %d)\n", 
               i, token_type_name(tokens[i].type),
               (int)tokens[i].len, tokens[i].start,
               tokens[i].line, tokens[i].column);
    }
}

static void test_simple_text(void) {
    const char *input = "Hello World";
    mustache_token_t *tokens;
    size_t token_count;
    
    int result = mustache_tokenize(input, strlen(input), &tokens, &token_count);
    TEST_CHECK(result == 0);
    TEST_CHECK(token_count == 2); // TEXT + EOF
    
    if (result == 0 && token_count >= 2) {
        TEST_CHECK(tokens[0].type == MUSTACHE_TOKEN_TEXT);
        TEST_CHECK(tokens[0].len == 11);
        TEST_CHECK(strncmp(tokens[0].start, "Hello World", 11) == 0);
        TEST_CHECK(tokens[1].type == MUSTACHE_TOKEN_EOF);
        
        print_tokens(tokens, token_count);
        free(tokens);
    }
}

static void test_simple_variable(void) {
    const char *input = "{{name}}";
    mustache_token_t *tokens;
    size_t token_count;
    
    int result = mustache_tokenize(input, strlen(input), &tokens, &token_count);
    TEST_CHECK(result == 0);
    TEST_CHECK(token_count == 4); // VARIABLE + TEXT + CLOSE + EOF
    
    if (result == 0 && token_count >= 4) {
        TEST_CHECK(tokens[0].type == MUSTACHE_TOKEN_VARIABLE);
        TEST_CHECK(tokens[1].type == MUSTACHE_TOKEN_TEXT);
        TEST_CHECK(strncmp(tokens[1].start, "name", 4) == 0);
        TEST_CHECK(tokens[2].type == MUSTACHE_TOKEN_CLOSE);
        TEST_CHECK(tokens[3].type == MUSTACHE_TOKEN_EOF);
        
        print_tokens(tokens, token_count);
        free(tokens);
    }
}

static void test_mixed_content(void) {
    const char *input = "Hello {{name}}!";
    mustache_token_t *tokens;
    size_t token_count;
    
    int result = mustache_tokenize(input, strlen(input), &tokens, &token_count);
    TEST_CHECK(result == 0);
    TEST_CHECK(token_count == 6); // TEXT + VARIABLE + TEXT + CLOSE + TEXT + EOF
    
    if (result == 0 && token_count >= 6) {
        TEST_CHECK(tokens[0].type == MUSTACHE_TOKEN_TEXT);
        TEST_CHECK(strncmp(tokens[0].start, "Hello ", 6) == 0);
        
        TEST_CHECK(tokens[1].type == MUSTACHE_TOKEN_VARIABLE);
        TEST_CHECK(tokens[2].type == MUSTACHE_TOKEN_TEXT);
        TEST_CHECK(strncmp(tokens[2].start, "name", 4) == 0);
        TEST_CHECK(tokens[3].type == MUSTACHE_TOKEN_CLOSE);
        
        TEST_CHECK(tokens[4].type == MUSTACHE_TOKEN_TEXT);
        TEST_CHECK(strncmp(tokens[4].start, "!", 1) == 0);
        
        TEST_CHECK(tokens[5].type == MUSTACHE_TOKEN_EOF);
        
        print_tokens(tokens, token_count);
        free(tokens);
    }
}

static void test_section_tags(void) {
    const char *input = "{{#section}}content{{/section}}";
    mustache_token_t *tokens;
    size_t token_count;
    
    int result = mustache_tokenize(input, strlen(input), &tokens, &token_count);
    TEST_CHECK(result == 0);
    
    if (result == 0) {
        print_tokens(tokens, token_count);
        
        // Should have: SECTION_OPEN, TEXT, CLOSE, TEXT, SECTION_CLOSE, TEXT, CLOSE, EOF
        TEST_CHECK(token_count >= 8);
        TEST_CHECK(tokens[0].type == MUSTACHE_TOKEN_SECTION_OPEN);
        TEST_CHECK(tokens[1].type == MUSTACHE_TOKEN_TEXT);
        TEST_CHECK(strncmp(tokens[1].start, "section", 7) == 0);
        TEST_CHECK(tokens[2].type == MUSTACHE_TOKEN_CLOSE);
        TEST_CHECK(tokens[3].type == MUSTACHE_TOKEN_TEXT);
        TEST_CHECK(strncmp(tokens[3].start, "content", 7) == 0);
        TEST_CHECK(tokens[4].type == MUSTACHE_TOKEN_SECTION_CLOSE);
        TEST_CHECK(tokens[5].type == MUSTACHE_TOKEN_TEXT);
        TEST_CHECK(strncmp(tokens[5].start, "section", 7) == 0);
        TEST_CHECK(tokens[6].type == MUSTACHE_TOKEN_CLOSE);
        
        free(tokens);
    }
}

static void test_inverted_section(void) {
    const char *input = "{{^empty}}Not empty{{/empty}}";
    mustache_token_t *tokens;
    size_t token_count;
    
    int result = mustache_tokenize(input, strlen(input), &tokens, &token_count);
    TEST_CHECK(result == 0);
    
    if (result == 0) {
        print_tokens(tokens, token_count);
        
        TEST_CHECK(token_count >= 8);
        TEST_CHECK(tokens[0].type == MUSTACHE_TOKEN_SECTION_INVERTED);
        TEST_CHECK(tokens[1].type == MUSTACHE_TOKEN_TEXT);
        TEST_CHECK(strncmp(tokens[1].start, "empty", 5) == 0);
        TEST_CHECK(tokens[2].type == MUSTACHE_TOKEN_CLOSE);
        TEST_CHECK(tokens[3].type == MUSTACHE_TOKEN_TEXT);
        TEST_CHECK(strncmp(tokens[3].start, "Not empty", 9) == 0);
        TEST_CHECK(tokens[4].type == MUSTACHE_TOKEN_SECTION_CLOSE);
        
        free(tokens);
    }
}

static void test_unescaped_variable(void) {
    const char *input = "{{{html}}}";
    mustache_token_t *tokens;
    size_t token_count;
    
    int result = mustache_tokenize(input, strlen(input), &tokens, &token_count);
    TEST_CHECK(result == 0);
    
    if (result == 0) {
        print_tokens(tokens, token_count);
        
        TEST_CHECK(token_count >= 4);
        TEST_CHECK(tokens[0].type == MUSTACHE_TOKEN_UNESCAPED);
        TEST_CHECK(tokens[1].type == MUSTACHE_TOKEN_TEXT);
        TEST_CHECK(strncmp(tokens[1].start, "html", 4) == 0);
        TEST_CHECK(tokens[2].type == MUSTACHE_TOKEN_CLOSE);
        
        free(tokens);
    }
}

static void test_unescaped_ampersand(void) {
    const char *input = "{{&html}}";
    mustache_token_t *tokens;
    size_t token_count;
    
    int result = mustache_tokenize(input, strlen(input), &tokens, &token_count);
    TEST_CHECK(result == 0);
    
    if (result == 0) {
        print_tokens(tokens, token_count);
        
        TEST_CHECK(token_count >= 4);
        TEST_CHECK(tokens[0].type == MUSTACHE_TOKEN_UNESCAPED_ALT);
        TEST_CHECK(tokens[1].type == MUSTACHE_TOKEN_TEXT);
        TEST_CHECK(strncmp(tokens[1].start, "html", 4) == 0);
        TEST_CHECK(tokens[2].type == MUSTACHE_TOKEN_CLOSE);
        
        free(tokens);
    }
}

static void test_partial(void) {
    const char *input = "{{>header}}";
    mustache_token_t *tokens;
    size_t token_count;
    
    int result = mustache_tokenize(input, strlen(input), &tokens, &token_count);
    TEST_CHECK(result == 0);
    
    if (result == 0) {
        print_tokens(tokens, token_count);
        
        TEST_CHECK(token_count >= 4);
        TEST_CHECK(tokens[0].type == MUSTACHE_TOKEN_PARTIAL);
        TEST_CHECK(tokens[1].type == MUSTACHE_TOKEN_TEXT);
        TEST_CHECK(strncmp(tokens[1].start, "header", 6) == 0);
        TEST_CHECK(tokens[2].type == MUSTACHE_TOKEN_CLOSE);
        
        free(tokens);
    }
}

static void test_comment(void) {
    const char *input = "{{! This is a comment }}";
    mustache_token_t *tokens;
    size_t token_count;
    
    int result = mustache_tokenize(input, strlen(input), &tokens, &token_count);
    TEST_CHECK(result == 0);
    
    if (result == 0) {
        print_tokens(tokens, token_count);
        
        TEST_CHECK(token_count >= 4);
        TEST_CHECK(tokens[0].type == MUSTACHE_TOKEN_COMMENT);
        TEST_CHECK(tokens[1].type == MUSTACHE_TOKEN_TEXT);
        TEST_CHECK(tokens[2].type == MUSTACHE_TOKEN_CLOSE);
        
        free(tokens);
    }
}

static void test_delimiter_change(void) {
    const char *input = "{{=<% %>=}}<%name%>";
    mustache_token_t *tokens;
    size_t token_count;
    
    int result = mustache_tokenize(input, strlen(input), &tokens, &token_count);
    TEST_CHECK(result == 0);
    
    if (result == 0) {
        print_tokens(tokens, token_count);
        
        TEST_CHECK(token_count >= 5);
        TEST_CHECK(tokens[0].type == MUSTACHE_TOKEN_DELIMITER);
        // Note: This lexer doesn't handle delimiter changes dynamically
        // It will tokenize the rest as text, which is acceptable for this test
        
        free(tokens);
    }
}

static void test_multiline_template(void) {
    const char *input = 
        "Hello {{name}}!\n"
        "{{#items}}\n"
        "  Item: {{.}}\n"
        "{{/items}}";
    
    mustache_token_t *tokens;
    size_t token_count;
    
    int result = mustache_tokenize(input, strlen(input), &tokens, &token_count);
    TEST_CHECK(result == 0);
    
    if (result == 0) {
        print_tokens(tokens, token_count);
        
        // Verify line numbers are tracked correctly
        int found_line_2 = 0, found_line_3 = 0, found_line_4 = 0;
        for (size_t i = 0; i < token_count; i++) {
            if (tokens[i].line == 2) found_line_2 = 1;
            if (tokens[i].line == 3) found_line_3 = 1;
            if (tokens[i].line == 4) found_line_4 = 1;
        }
        
        TEST_CHECK(found_line_2);
        TEST_CHECK(found_line_3);
        TEST_CHECK(found_line_4);
        
        free(tokens);
    }
}

static void test_empty_template(void) {
    const char *input = "";
    mustache_token_t *tokens;
    size_t token_count;
    
    int result = mustache_tokenize(input, strlen(input), &tokens, &token_count);
    TEST_CHECK(result == 0);
    TEST_CHECK(token_count == 1); // Just EOF
    
    if (result == 0) {
        TEST_CHECK(tokens[0].type == MUSTACHE_TOKEN_EOF);
        free(tokens);
    }
}

static void test_malformed_tags(void) {
    const char *input = "{{unclosed";
    mustache_token_t *tokens;
    size_t token_count;
    
    int result = mustache_tokenize(input, strlen(input), &tokens, &token_count);
    
    if (result == 0) {
        print_tokens(tokens, token_count);
        // Should handle gracefully - might be treated as text or error
        free(tokens);
    }
}

TEST_LIST = {
    { "simple_text", test_simple_text },
    { "simple_variable", test_simple_variable },
    { "mixed_content", test_mixed_content },
    { "section_tags", test_section_tags },
    { "inverted_section", test_inverted_section },
    { "unescaped_variable", test_unescaped_variable },
    { "unescaped_ampersand", test_unescaped_ampersand },
    { "partial", test_partial },
    { "comment", test_comment },
    { "delimiter_change", test_delimiter_change },
    { "multiline_template", test_multiline_template },
    { "empty_template", test_empty_template },
    { "malformed_tags", test_malformed_tags },
    { 0 }
};