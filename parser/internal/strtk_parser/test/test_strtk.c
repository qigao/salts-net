#include "strtk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinytest.h"

// Helper macro to check token content
// Since tinytest defines check functions, we use them directly inside 'it' blocks.
// I will rewrite checks using tinytest macros.

spec("strtk_parser") {

    describe("Tokenization") {
        it("should tokenize basic string") {
            const char *input = "a,b,c";
            size_t count = 0;
            char **tokens = strtk_tokenize(input, 0, ",", false, &count);
            check_not_null(tokens);
            check_int_eq(count, 3);
            check_str_eq(tokens[0], "a");
            check_str_eq(tokens[1], "b");
            check_str_eq(tokens[2], "c");
            strtk_free_tokens(tokens, count);
        }

        it("should tokenize with quotes") {
            const char *input = "a,\"b,c\",d";
            size_t count = 0;
            char **tokens = strtk_tokenize(input, 0, ",", false, &count);
            check_not_null(tokens);
            check_int_eq(count, 3);
            check_str_eq(tokens[0], "a");
            check_str_eq(tokens[1], "\"b,c\"");
            check_str_eq(tokens[2], "d");
            strtk_free_tokens(tokens, count);
        }

        it("should ignore empty tokens if requested") {
            const char *input = "a,,c";
            size_t count = 0;
            char **tokens = strtk_tokenize(input, 0, ",", true, &count);
            check_not_null(tokens);
            check_int_eq(count, 2);
            check_str_eq(tokens[0], "a");
            check_str_eq(tokens[1], "c");
            strtk_free_tokens(tokens, count);
        }

        it("should include empty tokens if requested") {
            const char *input = "a,,c";
            size_t count = 0;
            char **tokens = strtk_tokenize(input, 0, ",", false, &count);
            check_not_null(tokens);
            check_int_eq(count, 3);
            check_str_eq(tokens[0], "a");
            // Empty string check
            check_str_eq(tokens[1], "");
            check_str_eq(tokens[2], "c");
            strtk_free_tokens(tokens, count);
        }
    }

    describe("Conversion") {
        it("should convert strings to types") {
            long long i;
            check(strtk_to_int("123", &i));
            check_int_eq(i, 123);
            
            check(strtk_to_int("0xFF", &i));
            check_int_eq(i, 255);
            
            double d;
            check(strtk_to_double("3.14", &d));
            check_float_eq(d, 3.14, 0.01);
            
            bool b;
            check(strtk_to_bool("true", &b));
            check(b);
            check(strtk_to_bool("0", &b));
            check(!b);
        }
    }

    describe("Splitting") {
        it("should split string by single delimiter") {
            const char *input = "apple|banana|cherry";
            size_t count = 0;
            char **parts = strtk_split(input, '|', &count);
            check_int_eq(count, 3);
            check_str_eq(parts[1], "banana");
            strtk_free_tokens(parts, count);
        }
    }
}
