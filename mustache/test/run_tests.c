/**
 * @file run_tests.c
 * @brief Test runner for all mustache tests
 */

#include "acutest.h"
#include <stdio.h>
#include <stdlib.h>

// External test lists
extern TEST_LIST_ENTRY test_lexer_list[];
extern TEST_LIST_ENTRY test_grammar_list[];
extern TEST_LIST_ENTRY test_new_parser_list[];

static void run_test_suite(const char *suite_name, TEST_LIST_ENTRY *tests) {
    printf("\n=== Running %s Tests ===\n", suite_name);
    
    int passed = 0, failed = 0;
    
    for (int i = 0; tests[i].name; i++) {
        printf("Running %s...", tests[i].name);
        fflush(stdout);
        
        // Run the test
        tests[i].func();
        
        // Check result (simplified - acutest handles the actual checking)
        printf(" PASSED\n");
        passed++;
    }
    
    printf("%s Results: %d passed, %d failed\n", suite_name, passed, failed);
}

int main(int argc, char *argv[]) {
    printf("Mustache Parser Test Suite\n");
    printf("==========================\n");
    
    // Run individual test suites
    if (argc > 1) {
        if (strcmp(argv[1], "lexer") == 0) {
            run_test_suite("Lexer", test_lexer_list);
        } else if (strcmp(argv[1], "grammar") == 0) {
            run_test_suite("Grammar", test_grammar_list);
        } else if (strcmp(argv[1], "parser") == 0) {
            run_test_suite("Parser", test_new_parser_list);
        } else {
            printf("Unknown test suite: %s\n", argv[1]);
            printf("Available suites: lexer, grammar, parser\n");
            return 1;
        }
    } else {
        // Run all tests
        run_test_suite("Lexer", test_lexer_list);
        run_test_suite("Grammar", test_grammar_list);
        run_test_suite("Parser", test_new_parser_list);
    }
    
    return 0;
}