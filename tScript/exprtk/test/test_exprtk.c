/**
 * @file test_exprtk.c
 * @brief Unit tests for exprtk parser
 */

#include "exprtk.h" 
#include "tinytest.h"
#include <string.h>
#include <math.h>
#include "turbo_fs.h"

static exprtk_value_t my_func(size_t argc, exprtk_value_t *args, void *ud) {
    (void)ud;
    if (argc == 1 && args[0].type == exprtk_VAL_NUMBER) {
        exprtk_value_t res;
        res.type = exprtk_VAL_NUMBER;
        res.data.number = args[0].data.number * 2;
        return res;
    }
    exprtk_value_t zero = { exprtk_VAL_NUMBER, {0.0} };
    return zero;
}

spec("exprtk_parser") {
    describe("Arithmetic Expressions") {
        it("should parse and evaluate simple addition") {
            const char *input = "1 + 2";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            check_int_eq(root->type, exprtk_NODE_BLOCK);
            exprtk_value_t res = exprtk_eval(root, NULL);
            double result = res.data.number;
            check_float_eq(result, 3.0, 0.0001);
            exprtk_free(root);
        }

        it("should parse and evaluate operator precedence") {
            const char *input = "1 + 2 * 3"; // Should be 7, not 9
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, NULL);
            double result = res.data.number;
            check_float_eq(result, 7.0, 0.0001);
            exprtk_free(root);
        }

        it("should parse parentheses") {
            const char *input = "(1 + 2) * 3"; // Should be 9
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, NULL);
            double result = res.data.number;
            check_float_eq(result, 9.0, 0.0001);
            exprtk_free(root);
        }

        it("should handle division") {
            const char *input = "10 / 2";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, NULL);
            double result = res.data.number;
            check_float_eq(result, 5.0, 0.0001);
            exprtk_free(root);
        }

        it("should handle mixed operators") {
            const char *input = "10 + 5 * 2 - 4 / 2"; // 10 + 10 - 2 = 18
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, NULL);
            double result = res.data.number;
            check_float_eq(result, 18.0, 0.0001);
            exprtk_free(root);
        }
    }

    describe("Function Calls") {
        it("should evaluate sin(0)") {
            const char *input = "sin(0)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            check_int_eq(root->type, exprtk_NODE_BLOCK);
            check_int_eq(root->data.block.statements[0]->type, exprtk_NODE_FUNCTION_CALL);
            exprtk_value_t res = exprtk_eval(root, NULL);
            double result = res.data.number;
            check_float_eq(result, 0.0, 0.0001);
            exprtk_free(root);
        }

        it("should evaluate cos(0)") {
            const char *input = "cos(0)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, NULL);
            double result = res.data.number;
            check_float_eq(result, 1.0, 0.0001);
            exprtk_free(root);
        }

        it("should evaluate max(1, 4, 2)") {
            const char *input = "max(1, 4, 2)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, NULL);
            double result = res.data.number;
            check_float_eq(result, 4.0, 0.0001);
            exprtk_free(root);
        }

        it("should evaluate avg(2, 4, 6)") {
            const char *input = "avg(2, 4, 6)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, NULL);
            double result = res.data.number;
            check_float_eq(result, 4.0, 0.0001);
            exprtk_free(root);
        }

        it("should evaluate nested functions") {
            const char *input = "max(sin(0), cos(0))"; // max(0, 1) = 1
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, NULL);
            double result = res.data.number;
            check_float_eq(result, 1.0, 0.0001);
            exprtk_free(root);
        }
    }

    describe("Phase 1: Operators & Variables") {
        it("should evaluate mod and power") {
            const char *input = "10 % 3 + 2^3"; // 1 + 8 = 9
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, NULL);
            double result = res.data.number;
            check_float_eq(result, 9.0, 0.0001);
            exprtk_free(root);
        }

        it("should evaluate comparisons") {
            exprtk_node_t *root = exprtk_parse("10 > 5", 0);
            check_float_eq(exprtk_eval(root, NULL).data.number, 1.0, 0.0001);
            exprtk_free(root);

            root = exprtk_parse("10 == 5", 0);
            check_float_eq(exprtk_eval(root, NULL).data.number, 0.0, 0.0001);
            exprtk_free(root);
        }

        it("should evaluate logic") {
            exprtk_node_t *root = exprtk_parse("1 and 0", 0);
            check_float_eq(exprtk_eval(root, NULL).data.number, 0.0, 0.0001);
            exprtk_free(root);

            root = exprtk_parse("1 or 0", 0);
            check_float_eq(exprtk_eval(root, NULL).data.number, 1.0, 0.0001);
            exprtk_free(root);
            
            root = exprtk_parse("not 0", 0);
            check_float_eq(exprtk_eval(root, NULL).data.number, 1.0, 0.0001);
            exprtk_free(root);
        }

        it("should handle assignment and variables") {
            exprtk_env_t env;
            exprtk_env_init(&env);

            const char *input = "x = 10 + 5";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, &env);
            double result = res.data.number;
            check_float_eq(result, 15.0, 0.0001);
            exprtk_free(root);

            // Use 'x' in next expression
            input = "x * 2";
            root = exprtk_parse(input, 0);
            check_not_null(root);
            res = exprtk_eval(root, &env);
            result = res.data.number;
            check_float_eq(result, 30.0, 0.0001);
            exprtk_free(root);

            exprtk_env_free(&env);
        }

        it("should handle compound assignments") {
            exprtk_env_t env;
            exprtk_env_init(&env);

            // x = 10; x += 5; x
            const char *input = "x = 10; x += 5; x";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, &env);
            double result = res.data.number;
            check_float_eq(result, 15.0, 0.0001);
            exprtk_free(root);

            // x *= 2; x
            input = "x *= 2; x";
            root = exprtk_parse(input, 0);
            check_not_null(root);
            res = exprtk_eval(root, &env);
            result = res.data.number;
            check_float_eq(result, 30.0, 0.0001);
            exprtk_free(root);

            exprtk_env_free(&env);
        }
    }

    describe("Phase 2: Control Structures") {
        it("should handle if-else statements") {
            exprtk_env_t env;
            exprtk_env_init(&env);

            // if (1) x = 10 else x = 20
            const char *input = "x = 0; if (1) { x = 10; } else { x = 20; }; x";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, &env);
            double result = res.data.number;
            check_float_eq(result, 10.0, 0.0001);
            exprtk_free(root);

            // if (0) x = 10 else x = 20
            input = "x = 0; if (0) { x = 10; } else { x = 20; }; x";
            root = exprtk_parse(input, 0);
            check_not_null(root);
            res = exprtk_eval(root, &env);
            result = res.data.number;
            check_float_eq(result, 20.0, 0.0001);
            exprtk_free(root);

            exprtk_env_free(&env);
        }

        it("should handle while loops") {
            exprtk_env_t env;
            exprtk_env_init(&env);

            // sum = 0; i = 1; while (i <= 5) { sum = sum + i; i = i + 1; }; sum
            const char *input = "sum = 0; i = 1; while (i <= 5) { sum = sum + i; i = i + 1; }; sum";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, &env);
            double result = res.data.number;
            check_float_eq(result, 15.0, 0.0001); // 1+2+3+4+5 = 15
            exprtk_free(root);

            exprtk_env_free(&env);
        }

        it("should handle for loops") {
            exprtk_env_t env;
            exprtk_env_init(&env);

            // sum = 0; for (i = 1; i <= 5; i = i + 1) { sum = sum + i; }; sum
            const char *input = "sum = 0; for (i = 1; i <= 5; i = i + 1) { sum = sum + i; }; sum";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, &env);
            double result = res.data.number;
            check_float_eq(result, 15.0, 0.0001);
            exprtk_free(root);

            exprtk_env_free(&env);
        }

        it("should handle break and continue") {
            exprtk_env_t env;
            exprtk_env_init(&env);

            // sum = 0; i = 0; while (i < 10) { i = i + 1; if (i == 5) continue; if (i > 7) break; sum = sum + i; }; sum
            // i=1, sum=1
            // i=2, sum=3
            // i=3, sum=6
            // i=4, sum=10
            // i=5, continue
            // i=6, sum=16
            // i=7, sum=23
            // i=8, break
            // Result 23
            const char *input = "sum = 0; i = 0; while (i < 10) { i = i + 1; if (i == 5) continue; if (i > 7) break; sum = sum + i; }; sum";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, &env);
            double result = res.data.number;
            check_float_eq(result, 23.0, 0.0001);
            exprtk_free(root);

            exprtk_env_free(&env);
        }

        it("should handle return") {
            exprtk_env_t env;
            exprtk_env_init(&env);

            const char *input = "x = 10; return x + 5; x = 100";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, &env);
            double result = res.data.number;
            check_float_eq(result, 15.0, 0.0001);
            exprtk_free(root);

            exprtk_env_free(&env);
        }
    }

    describe("Phase 3: Strings") {
        it("should parse and evaluate string literals") {
            const char *input = "\"Hello World\"";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, NULL);
            check_int_eq(res.type, exprtk_VAL_STRING);
            check_int_eq(res.data.string.len, 11);
            check_int_eq(strncmp(res.data.string.data, "Hello World", 11), 0);
            exprtk_free(root);
        }

        it("should handle string concatenation") {
            const char *input = "\"Hello \" + \"World\"";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, NULL);
            check_int_eq(res.type, exprtk_VAL_STRING);
            check_int_eq(res.data.string.len, 11);
            check_int_eq(strncmp(res.data.string.data, "Hello World", 11), 0);
            exprtk_free(root);
        }

        it("should support len() function") {
            const char *input = "len(\"Turbo\")";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, NULL);
            check_int_eq(res.type, exprtk_VAL_NUMBER);
            check_float_eq(res.data.number, 5.0, 0.0001);
            exprtk_free(root);
        }
    }

    describe("Phase 4: Extended String Operations") {
        it("should handle case conversion") {
             exprtk_env_t env;
             exprtk_env_init(&env);
             
             const char *input = "lower(\"HeLLo\")";
             exprtk_node_t *root = exprtk_parse(input, 0);
             check_not_null(root);
             exprtk_value_t res = exprtk_eval(root, &env);
             check_int_eq(res.type, exprtk_VAL_STRING);
             check_int_eq(tstr_v_eq(res.data.string, tstr_v_from_cstr("hello")), 1);
             exprtk_free(root);
             
             input = "upper(\"world\")";
             root = exprtk_parse(input, 0);
             check_not_null(root);
             res = exprtk_eval(root, &env);
             check_int_eq(res.type, exprtk_VAL_STRING);
             check_int_eq(tstr_v_eq(res.data.string, tstr_v_from_cstr("WORLD")), 1);
             exprtk_free(root);
             
             exprtk_env_free(&env);
        }
        
        it("should handle trimming") {
             exprtk_env_t env;
             exprtk_env_init(&env);
             
             const char *input = "trim(\"  hello  \")";
             exprtk_node_t *root = exprtk_parse(input, 0);
             check_not_null(root);
             exprtk_value_t res = exprtk_eval(root, &env);
             check_int_eq(tstr_v_eq(res.data.string, tstr_v_from_cstr("hello")), 1);
             exprtk_free(root);
             
             input = "ltrim(\"  hello\")";
             root = exprtk_parse(input, 0);
             check_not_null(root);
             res = exprtk_eval(root, &env);
             check_int_eq(tstr_v_eq(res.data.string, tstr_v_from_cstr("hello")), 1);
             exprtk_free(root);
             
             input = "rtrim(\"hello  \")";
             root = exprtk_parse(input, 0);
             check_not_null(root);
             res = exprtk_eval(root, &env);
             check_int_eq(tstr_v_eq(res.data.string, tstr_v_from_cstr("hello")), 1);
             exprtk_free(root);
             
             exprtk_env_free(&env);
        }
        
        it("should handle search and predicates") {
             exprtk_env_t env;
             exprtk_env_init(&env);
             
             const char *input = "contains(\"foobar\", \"oo\")";
             exprtk_node_t *root = exprtk_parse(input, 0);
             check_not_null(root);
             exprtk_value_t res = exprtk_eval(root, &env);
             check_float_eq(res.data.number, 1.0, 0.0001);
             exprtk_free(root);
             
             input = "starts_with(\"foobar\", \"foo\")";
             root = exprtk_parse(input, 0);
             res = exprtk_eval(root, &env);
             check_float_eq(res.data.number, 1.0, 0.0001);
             exprtk_free(root);
             
             input = "ends_with(\"foobar\", \"bar\")";
             root = exprtk_parse(input, 0);
             res = exprtk_eval(root, &env);
             check_float_eq(res.data.number, 1.0, 0.0001);
             exprtk_free(root);
             
             input = "index_of(\"abcde\", \"c\")";
             root = exprtk_parse(input, 0);
             res = exprtk_eval(root, &env);
             check_float_eq(res.data.number, 2.0, 0.0001);
             exprtk_free(root);
             
             exprtk_env_free(&env);
        }
        
        it("should handle slicing and transformation") {
             exprtk_env_t env;
             exprtk_env_init(&env);
             
             const char *input = "substr(\"hello world\", 6, 5)";
             exprtk_node_t *root = exprtk_parse(input, 0);
             check_not_null(root);
             exprtk_value_t res = exprtk_eval(root, &env);
             check_int_eq(tstr_v_eq(res.data.string, tstr_v_from_cstr("world")), 1);
             exprtk_free(root);
             
             input = "reverse(\"abc\")";
             root = exprtk_parse(input, 0);
             res = exprtk_eval(root, &env);
             check_int_eq(tstr_v_eq(res.data.string, tstr_v_from_cstr("cba")), 1);
             exprtk_free(root);
             
             input = "replace(\"banana\", \"a\", \"o\")";
             root = exprtk_parse(input, 0);
             res = exprtk_eval(root, &env);
             check_int_eq(tstr_v_eq(res.data.string, tstr_v_from_cstr("bonono")), 1);
             exprtk_free(root);
             
             exprtk_env_free(&env);
        }
    }
    describe("AST Optimizations") {
        it("should fold constant numeric expressions") {
            const char *input = "1 + 2 * 3"; // 7
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            check_int_eq(root->type, exprtk_NODE_BLOCK);
            // The entire expression should be folded into a single NUMBER node
            check_int_eq(root->data.block.statements[0]->type, exprtk_NODE_NUMBER);
            check_float_eq(root->data.block.statements[0]->data.number, 7.0, 0.0001);
            exprtk_free(root);
        }

        it("should fold constant string concatenation") {
            const char *input = "\"hello \" + \"world\"";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            check_int_eq(root->type, exprtk_NODE_BLOCK);
            check_int_eq(root->data.block.statements[0]->type, exprtk_NODE_STRING);
            check_int_eq(root->data.block.statements[0]->data.string.value.len, 11);
            check_int_eq(strncmp(root->data.block.statements[0]->data.string.value.data, "hello world", 11), 0);
            exprtk_free(root);
        }

        it("should perform DCE on constant IF conditions") {
            const char *input = "if (1) { return \"true\"; } else { return \"false\"; }";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            check_int_eq(root->type, exprtk_NODE_BLOCK);
            // Since it's folded, the IF node should be replaced by its true branch (which is a block)
            check_int_eq(root->data.block.statements[0]->type, exprtk_NODE_BLOCK);
            
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_value_t res = exprtk_eval(root, &env);
            check_int_eq(tstr_v_eq(res.data.string, tstr_v_from_cstr("true")), 1);
            exprtk_env_free(&env);
            exprtk_free(root);
        }

        it("should perform DCE on false constant IF conditions") {
            const char *input = "if (0) { return \"true\"; } else { return \"false\"; }";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            check_int_eq(root->type, exprtk_NODE_BLOCK);
            // Replaced by else branch
            check_int_eq(root->data.block.statements[0]->type, exprtk_NODE_BLOCK);
            
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_value_t res = exprtk_eval(root, &env);
            check_int_eq(tstr_v_eq(res.data.string, tstr_v_from_cstr("false")), 1);
            exprtk_env_free(&env);
            exprtk_free(root);
        }
    }

    describe("Vectors") {
        it("should create a vector literal") {
            const char *input = "[1, 2, 3]";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_value_t res = exprtk_eval(root, &env);
            check_int_eq(res.type, exprtk_VAL_VECTOR);
            check_int_eq(res.data.vector.size, 3);
            check_float_eq(res.data.vector.data[0], 1.0, 0.0001);
            check_float_eq(res.data.vector.data[1], 2.0, 0.0001);
            check_float_eq(res.data.vector.data[2], 3.0, 0.0001);
            exprtk_env_free(&env);
            exprtk_free(root);
        }

        it("should support vector indexing") {
            const char *input = "v = [10, 20, 30]; v[1]";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_value_t res = exprtk_eval(root, &env);
            check_int_eq(res.type, exprtk_VAL_NUMBER);
            check_float_eq(res.data.number, 20.0, 0.0001);
            exprtk_env_free(&env);
            exprtk_free(root);
        }

        it("should support element-wise vector addition") {
            const char *input = "[1, 2] + [3, 4]";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_value_t res = exprtk_eval(root, &env);
            check_int_eq(res.type, exprtk_VAL_VECTOR);
            check_int_eq(res.data.vector.size, 2);
            check_float_eq(res.data.vector.data[0], 4.0, 0.0001);
            check_float_eq(res.data.vector.data[1], 6.0, 0.0001);
            exprtk_env_free(&env);
            exprtk_free(root);
        }

        it("should support vector-scalar multiplication") {
            const char *input = "[1, 2, 3] * 10";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_value_t res = exprtk_eval(root, &env);
            check_int_eq(res.type, exprtk_VAL_VECTOR);
            check_int_eq(res.data.vector.size, 3);
            check_float_eq(res.data.vector.data[0], 10.0, 0.0001);
            check_float_eq(res.data.vector.data[1], 20.0, 0.0001);
            check_float_eq(res.data.vector.data[2], 30.0, 0.0001);
            exprtk_env_free(&env);
            exprtk_free(root);
        }

        it("should return vector size") {
            const char *input = "size([1, 2, 3, 4, 5])";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_value_t res = exprtk_eval(root, &env);
            check_int_eq(res.type, exprtk_VAL_NUMBER);
            check_float_eq(res.data.number, 5.0, 0.0001);
            exprtk_env_free(&env);
            exprtk_free(root);
        }

        it("should support string slicing") {
            const char *input = "\"hello\"[1..4]";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_value_t res = exprtk_eval(root, &env);
            check_int_eq(res.type, exprtk_VAL_STRING);
            check_int_eq(tstr_v_eq(res.data.string, tstr_v_from_cstr("ell")), 1);
            exprtk_env_free(&env);
            exprtk_free(root);
        }

        it("should support vector slicing") {
            const char *input = "[1, 2, 3, 4, 5][1..4]";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_value_t res = exprtk_eval(root, &env);
            check_int_eq(res.type, exprtk_VAL_VECTOR);
            check_int_eq(res.data.vector.size, 3);
            check_float_eq(res.data.vector.data[0], 2.0, 0.0001);
            check_float_eq(res.data.vector.data[1], 3.0, 0.0001);
            check_float_eq(res.data.vector.data[2], 4.0, 0.0001);
            exprtk_env_free(&env);
            exprtk_free(root);
        }
    }

    describe("Phase 7: User-defined Symbols & Constants") {
        it("should support built-in constants") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            
            exprtk_node_t *root = exprtk_parse("pi", 0);
            check_not_null(root);
            check_float_eq(exprtk_eval(root, &env).data.number, 3.14159, 0.0001);
            exprtk_free(root);
            
            root = exprtk_parse("e", 0);
            check_not_null(root);
            check_float_eq(exprtk_eval(root, &env).data.number, 2.71828, 0.0001);
            exprtk_free(root);

            root = exprtk_parse("true", 0);
            check_not_null(root);
            check_float_eq(exprtk_eval(root, &env).data.number, 1.0, 0.0001);
            exprtk_free(root);
            
            exprtk_env_free(&env);
        }

        it("should prevent overwriting constants") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            
            const char *input = "pi = 4; pi";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            check_float_eq(exprtk_eval(root, &env).data.number, 3.14159265, 0.0001);
            exprtk_free(root);
            
            exprtk_env_free(&env);
        }


        it("should support native function registration") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_register_func(&env, "double_it", (exprtk_native_fn)my_func, NULL);
            
            const char *input = "double_it(10)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            check_float_eq(exprtk_eval(root, &env).data.number, 20.0, 0.0001);
            exprtk_free(root);
            
            exprtk_env_free(&env);
        }

        it("should support script-defined functions") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            
            const char *input = "f(x) = x * 2; f(10)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            check_float_eq(exprtk_eval(root, &env).data.number, 20.0, 0.0001);
            exprtk_free(root);
            
            exprtk_env_free(&env);
        }

        it("should support multivariate script functions") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            
            const char *input = "area(w, h) = w * h; area(5, 10)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            check_float_eq(exprtk_eval(root, &env).data.number, 50.0, 0.0001);
            exprtk_free(root);
            
            exprtk_env_free(&env);
        }

        it("should support function composition") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            
            const char *input = "f(x) = x + 1; g(x) = x * 2; g(f(10))"; // 11 * 2 = 22
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            check_float_eq(exprtk_eval(root, &env).data.number, 22.0, 0.0001);
            exprtk_free(root);
            
            exprtk_env_free(&env);
        }
    }

    describe("Phase 8: Validation & Safety Checks") {
        it("should abort on exceeded recursion depth") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            env.max_recursion = 10; // Low limit for testing
            
            const char *input = "f(x) = f(x + 1); f(1)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            
            exprtk_value_t res = exprtk_eval(root, &env);
            check_int_eq(env.aborted, 1);
            check_float_eq(res.data.number, 0.0, 0.0001);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should abort on infinite loop") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            env.max_loop_iterations = 50;
            
            const char *input = "x = 0; while(true) { x = x + 1 }";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            
            exprtk_eval(root, &env);
            check_int_eq(env.aborted, 1);
            check_int_eq(env.curr_loop_iterations > 50, 1);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should abort on excessive node count") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            env.max_nodes = 20;
            
            // Use variables to avoid constant folding in parser
            const char *input = "x = 1; x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            
            exprtk_eval(root, &env);
            check_int_eq(env.aborted, 1);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should handle assert() correctly") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            
            const char *input1 = "assert(1 == 1, \"should pass\")";
            exprtk_node_t *root1 = exprtk_parse(input1, 0);
            exprtk_eval(root1, &env);
            check_int_eq(env.aborted, 0);
            exprtk_free(root1);
            
            const char *input2 = "assert(1 == 0, \"should fail\")";
            exprtk_node_t *root2 = exprtk_parse(input2, 0);
            exprtk_eval(root2, &env);
            check_int_eq(env.aborted, 1);
            exprtk_free(root2);
            
            exprtk_env_free(&env);
        }

        it("should report correct node count and depth") {
            const char *input = "x = 1 + 2 * 3";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            
            // "1 + 2 * 3" is folded to "7"
            // Structure: BLOCK -> ASSIGNMENT -> NUMBER
            // Total nodes: BLOCK(1) + assignment(1) + number(1) = 3
            // Depth: BLOCK(1) -> ASSIGNMENT(2) -> NUMBER(3)
            check_int_eq((int)exprtk_node_count(root), 3);
            check_int_eq((int)exprtk_node_depth(root), 3);
            
            exprtk_free(root);
            
            const char *input2 = "y = x + 1";
            exprtk_node_t *root2 = exprtk_parse(input2, 0);
            check_not_null(root2);
            // Structure: BLOCK -> ASSIGNMENT -> BINARY_OP (PLUS) -> [VARIABLE, NUMBER]
            // Count: BLOCK(1) + ASSIGN(1) + PLUS(1) + VAR(1) + NUM(1) = 5
            // Depth: BLOCK(1) -> ASSIGN(2) -> PLUS(3) -> VAR/NUM(4)
            check_int_eq((int)exprtk_node_count(root2), 5);
            check_int_eq((int)exprtk_node_depth(root2), 4);
            exprtk_free(root2);
        }

        it("should support numerical integration") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            
            // f(x) = x^2
            const char *setup = "f(x) = x^2";
            exprtk_node_t *root1 = exprtk_parse(setup, 0);
            exprtk_eval(root1, &env);
            exprtk_free(root1);
            
            // integrate("f", 0, 1) should be ~1/3
            const char *input = "integrate(\"f\", 0, 1)";
            exprtk_node_t *root2 = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root2, &env);
            check_float_eq(res.data.number, 1.0/3.0, 1e-6);
            exprtk_free(root2);
            
            // integrate("sin", 0, pi) should be ~2.0
            const char *input3 = "integrate(\"sin\", 0, pi)";
            exprtk_node_t *root3 = exprtk_parse(input3, 0);
            exprtk_value_t res3 = exprtk_eval(root3, &env);
            check_float_eq(res3.data.number, 2.0, 1e-6);
            exprtk_free(root3);
            
            exprtk_env_free(&env);
        }

        it("should support numerical differentiation") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            
            // f(x) = x^2
            const char *setup = "f(x) = x^2";
            exprtk_node_t *root1 = exprtk_parse(setup, 0);
            exprtk_eval(root1, &env);
            exprtk_free(root1);
            
            // derivative("f", 2) should be ~4.0
            const char *input = "derivative(\"f\", 2)";
            exprtk_node_t *root2 = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root2, &env);
            check_float_eq(res.data.number, 4.0, 1e-6);
            exprtk_free(root2);
            
            // derivative("sin", 0) should be ~1.0
            const char *input3 = "derivative(\"sin\", 0)";
            exprtk_node_t *root3 = exprtk_parse(input3, 0);
            exprtk_value_t res3 = exprtk_eval(root3, &env);
            check_float_eq(res3.data.number, 1.0, 1e-6);
            exprtk_free(root3);
            
            exprtk_env_free(&env);
        }
    }

    describe("Phase 10: External Library Integration") {
        it("should support file system operations") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            
            // 1. Write a file
            const char *write_script = "write_file(\"test_file.txt\", \"Hello world\")";
            exprtk_node_t *root1 = exprtk_parse(write_script, 0);
            exprtk_value_t res1 = exprtk_eval(root1, &env);
            check_int_eq((int)res1.data.number, 0);
            exprtk_free(root1);
            
            // 2. Check existence
            const char *exist_script = "file_exists(\"test_file.txt\")";
            exprtk_node_t *root2 = exprtk_parse(exist_script, 0);
            exprtk_value_t res2 = exprtk_eval(root2, &env);
            check_int_eq((int)res2.data.number, 1);
            exprtk_free(root2);
            
            // 3. Read back
            const char *read_script = "read_file(\"test_file.txt\")";
            exprtk_node_t *root3 = exprtk_parse(read_script, 0);
            exprtk_value_t res3 = exprtk_eval(root3, &env);
            check(res3.type == exprtk_VAL_STRING);
            check_str_eq(res3.data.string.data, "Hello world");
            exprtk_free(root3);
            
            // Cleanup
            turbo_fs_unlink("test_file.txt");
            exprtk_env_free(&env);
        }

        it("should support date time functions") {
            exprtk_env_t env;
            exprtk_env_init(&env);

            // 1. Get now()
            const char *now_script = "now(0)";
            exprtk_node_t *root1 = exprtk_parse(now_script, 0);
            exprtk_value_t res1 = exprtk_eval(root1, &env);
            check(res1.type == exprtk_VAL_NUMBER);
            check(res1.data.number > 1600000000); // Sanity check for recent timestamp
            exprtk_free(root1);

            // 2. Parse date
            const char *date_script = "date(\"2023-10-27T10:00:00Z\")";
            exprtk_node_t *root2 = exprtk_parse(date_script, 0);
            exprtk_value_t res2 = exprtk_eval(root2, &env);
            check(res2.type == exprtk_VAL_NUMBER);
            exprtk_free(root2);

            // 3. Format date
            const char *fmt_script = "format_date(date(\"2023-10-27T10:00:00Z\"))";
            exprtk_node_t *root3 = exprtk_parse(fmt_script, 0);
            exprtk_value_t res3 = exprtk_eval(root3, &env);
            check(res3.type == exprtk_VAL_STRING);
            // RFC 822 format output check or similar
            exprtk_free(root3);

            exprtk_env_free(&env);
        }
    }

    describe("Math Modules") {
        it("should support statistics functions") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            
            // Median
            const char *input = "median([1, 2, 3, 4, 5])";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_float_eq(exprtk_eval(root, &env).data.number, 3.0, 1e-6);
            exprtk_free(root);
            
            // Percentile
            input = "percentile([1, 2, 3, 4, 5], 50)"; // median again
            root = exprtk_parse(input, 0);
            check_float_eq(exprtk_eval(root, &env).data.number, 3.0, 1e-6);
            exprtk_free(root);
            
            // Geometric Mean
            input = "geometric_mean([1, 2, 3])"; // (1*2*3)^(1/3) = 6^(1/3) ~ 1.81712
            root = exprtk_parse(input, 0);
            check_float_eq(exprtk_eval(root, &env).data.number, pow(6.0, 1.0/3.0), 1e-6);
            exprtk_free(root);
            
            // Harmonic Mean
            input = "harmonic_mean([1, 2, 4])"; // 3 / (1/1 + 1/2 + 1/4) = 3 / (1.75) ~ 1.71428
            root = exprtk_parse(input, 0);
            check_float_eq(exprtk_eval(root, &env).data.number, 3.0 / 1.75, 1e-6);
            exprtk_free(root);
            
            // Cumsum
            input = "cumsum([1, 2, 3])";
            root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_float_eq(res.data.vector.data[0], 1.0, 1e-6);
            check_float_eq(res.data.vector.data[1], 3.0, 1e-6);
            check_float_eq(res.data.vector.data[2], 6.0, 1e-6);
            exprtk_free(root);

            exprtk_env_free(&env);
        }

        it("should support numeric functions") {
            exprtk_env_t env;
            exprtk_env_init(&env);

            // Fibonacci
            const char *input = "fibonacci(10)"; // F(0)=0, F(1)=1, F(2)=1, F(3)=2, F(4)=3, F(5)=5, F(6)=8, F(7)=13, F(8)=21, F(9)=34, F(10)=55
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_float_eq(exprtk_eval(root, &env).data.number, 55.0, 1e-6);
            exprtk_free(root);

            // GCD
            input = "gcd(12, 18)";
            root = exprtk_parse(input, 0);
            check_float_eq(exprtk_eval(root, &env).data.number, 6.0, 1e-6);
            exprtk_free(root);

            exprtk_env_free(&env);
        }

        it("should support linear algebra functions") {
            exprtk_env_t env;
            exprtk_env_init(&env);

            // det2
            const char *input = "det2([1, 2, 3, 4])"; // 1*4 - 2*3 = -2
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_float_eq(exprtk_eval(root, &env).data.number, -2.0, 1e-6);
            exprtk_free(root);

            // matmul
            input = "matmul([1, 2, 3, 4], [5, 6, 7, 8], 2, 2, 2)";
            // [1 2; 3 4] * [5 6; 7 8] = [1*5+2*7 1*6+2*8; 3*5+4*7 3*6+4*8] = [19 22; 43 50]
            root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_float_eq(res.data.vector.data[0], 19.0, 1e-6);
            check_float_eq(res.data.vector.data[1], 22.0, 1e-6);
            check_float_eq(res.data.vector.data[2], 43.0, 1e-6);
            check_float_eq(res.data.vector.data[3], 50.0, 1e-6);
            exprtk_free(root);

            // eig3
            input = "v = [0,0,0]; eig3([2,0,0, 0,3,0, 0,0,4], v)"; // eigenvalues of diag(2,3,4) are 4,3,2
            root = exprtk_parse(input, 0);
            exprtk_eval(root, &env);
            exprtk_value_t v_res = exprtk_env_get(&env, "v");
            check(v_res.type == exprtk_VAL_VECTOR);
            check_float_eq(v_res.data.vector.data[0], 4.0, 1e-6);
            check_float_eq(v_res.data.vector.data[1], 3.0, 1e-6);
            check_float_eq(v_res.data.vector.data[2], 2.0, 1e-6);
            exprtk_free(root);

            exprtk_env_free(&env);
        }
    }

    describe("var keyword assignment") {
        it("should support var a=10; var c=5; var sum = a+c") {
            exprtk_env_t env;
            exprtk_env_init(&env);

            // Basic: multiple var declarations, then use in a third
            const char *input = "var a=10; var c=5; var sum = a+c; sum";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, &env);
            check_float_eq(res.data.number, 15.0, 1e-6);  // 10+5 = 15
            exprtk_free(root);

            exprtk_env_free(&env);
        }

        it("should treat var x=expr as identical to x = expr") {
            exprtk_env_t env;
            exprtk_env_init(&env);

            // var and = produce the same variable in the same env
            const char *input = "x = 3; var y = 7; x + y";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, &env);
            check_float_eq(res.data.number, 10.0, 1e-6);
            exprtk_free(root);

            exprtk_env_free(&env);
        }

        it("should default undeclared variables to 0") {
            exprtk_env_t env;
            exprtk_env_init(&env);

            // b is never declared — evaluates to 0.0
            const char *input = "var a=10; var result = a+b; result";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, &env);
            check_float_eq(res.data.number, 10.0, 1e-6);  // a + 0 = 10
            exprtk_free(root);

            exprtk_env_free(&env);
        }

        it("should allow var to overwrite an existing variable") {
            exprtk_env_t env;
            exprtk_env_init(&env);

            const char *input = "var x=1; var x=99; x";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, &env);
            check_float_eq(res.data.number, 99.0, 1e-6);
            exprtk_free(root);

            exprtk_env_free(&env);
        }
    }

    describe("Assignment Syntax") {
        it("should support JS-style assignment with '='") {
            const char *input = "x = 10; y = x * 2; y";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_value_t res = exprtk_eval(root, &env);
            check_float_eq(res.data.number, 20.0, 0.0001);
            exprtk_env_free(&env);
            exprtk_free(root);
        }
        
        it("should support function definition with '='") {
            const char *input = "f(x) = x * x; f(5)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_value_t res = exprtk_eval(root, &env);
            check_float_eq(res.data.number, 25.0, 0.0001);
            exprtk_env_free(&env);
            exprtk_free(root);
        }
    }

    describe("Function Definition (func)") {
        it("should support func f(x) syntax") {
            const char *input = "func f(x) { return x*2; }; f(10)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_value_t res = exprtk_eval(root, &env);
            check_float_eq(res.data.number, 20.0, 0.0001);
            exprtk_env_free(&env);
            exprtk_free(root);
        }

        it("should support func with multiple args") {
             const char *input = "func sum(a, b) { return a + b; }; sum(10, 20)";
             exprtk_node_t *root = exprtk_parse(input, 0);
             check_not_null(root);
             exprtk_env_t env;
             exprtk_env_init(&env);
             exprtk_value_t res = exprtk_eval(root, &env);
             check_float_eq(res.data.number, 30.0, 0.0001);
             exprtk_env_free(&env);
             exprtk_free(root);
        }
    }

    describe("String Module: Tokenization & Conversion") {
        it("should tokenize a string and extract token by index") {
            exprtk_env_t env;
            exprtk_env_init(&env);

            const char *input = "tokenize(\"apple,banana,cherry\", \",\", 1)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, &env);
            check_int_eq(res.type, exprtk_VAL_STRING);
            check_int_eq(tstr_v_eq(res.data.string, tstr_v_from_cstr("banana")), 1);
            exprtk_free(root);

            // First token
            input = "tokenize(\"one:two:three\", \":\", 0)";
            root = exprtk_parse(input, 0);
            res = exprtk_eval(root, &env);
            check_int_eq(res.type, exprtk_VAL_STRING);
            check_int_eq(tstr_v_eq(res.data.string, tstr_v_from_cstr("one")), 1);
            exprtk_free(root);

            // Out-of-bounds index returns 0
            input = "tokenize(\"a,b\", \",\", 5)";
            root = exprtk_parse(input, 0);
            res = exprtk_eval(root, &env);
            check_int_eq(res.type, exprtk_VAL_NUMBER);
            check_float_eq(res.data.number, 0.0, 0.0001);
            exprtk_free(root);

            exprtk_env_free(&env);
        }

        it("should split a string into a numeric vector") {
            exprtk_env_t env;
            exprtk_env_init(&env);

            const char *input = "v = split(\"10,20,30\", \",\"); v";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, &env);
            check_int_eq(res.type, exprtk_VAL_VECTOR);
            check_int_eq((int)res.data.vector.size, 3);
            check_float_eq(res.data.vector.data[0], 10.0, 0.001);
            check_float_eq(res.data.vector.data[1], 20.0, 0.001);
            check_float_eq(res.data.vector.data[2], 30.0, 0.001);
            exprtk_free(root);

            exprtk_env_free(&env);
        }

        it("should count tokens in a delimited string") {
            exprtk_env_t env;
            exprtk_env_init(&env);

            const char *input = "token_count(\"a,b,c,d\", \",\")";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, &env);
            check_float_eq(res.data.number, 4.0, 0.0001);
            exprtk_free(root);

            exprtk_env_free(&env);
        }

        it("should convert string to number with to_num") {
            exprtk_env_t env;
            exprtk_env_init(&env);

            const char *input = "to_num(\"3.14\")";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, &env);
            check_float_eq(res.data.number, 3.14, 0.001);
            exprtk_free(root);

            // Negative number
            input = "to_num(\"-42.5\")";
            root = exprtk_parse(input, 0);
            res = exprtk_eval(root, &env);
            check_float_eq(res.data.number, -42.5, 0.001);
            exprtk_free(root);

            exprtk_env_free(&env);
        }

        it("should convert number to string with to_str") {
            exprtk_env_t env;
            exprtk_env_init(&env);

            const char *input = "to_str(42)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, &env);
            check_int_eq(res.type, exprtk_VAL_STRING);
            check_int_eq(tstr_v_eq(res.data.string, tstr_v_from_cstr("42")), 1);
            exprtk_free(root);

            exprtk_env_free(&env);
        }

        it("should convert string to integer with to_int") {
            exprtk_env_t env;
            exprtk_env_init(&env);

            const char *input = "to_int(\"255\")";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, &env);
            check_float_eq(res.data.number, 255.0, 0.0001);
            exprtk_free(root);

            // Hex
            input = "to_int(\"0xFF\")";
            root = exprtk_parse(input, 0);
            res = exprtk_eval(root, &env);
            check_float_eq(res.data.number, 255.0, 0.0001);
            exprtk_free(root);

            exprtk_env_free(&env);
        }

        it("should convert string to double with to_double") {
            exprtk_env_t env;
            exprtk_env_init(&env);

            const char *input = "to_double(\"1.618\")";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, &env);
            check_float_eq(res.data.number, 1.618, 0.001);
            exprtk_free(root);

            exprtk_env_free(&env);
        }

        it("should convert string to bool with to_bool") {
            exprtk_env_t env;
            exprtk_env_init(&env);

            const char *input = "to_bool(\"true\")";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, &env);
            check_float_eq(res.data.number, 1.0, 0.0001);
            exprtk_free(root);

            input = "to_bool(\"false\")";
            root = exprtk_parse(input, 0);
            res = exprtk_eval(root, &env);
            check_float_eq(res.data.number, 0.0, 0.0001);
            exprtk_free(root);

            input = "to_bool(\"yes\")";
            root = exprtk_parse(input, 0);
            res = exprtk_eval(root, &env);
            check_float_eq(res.data.number, 1.0, 0.0001);
            exprtk_free(root);

            input = "to_bool(\"no\")";
            root = exprtk_parse(input, 0);
            res = exprtk_eval(root, &env);
            check_float_eq(res.data.number, 0.0, 0.0001);
            exprtk_free(root);

            input = "to_bool(\"1\")";
            root = exprtk_parse(input, 0);
            res = exprtk_eval(root, &env);
            check_float_eq(res.data.number, 1.0, 0.0001);
            exprtk_free(root);

            input = "to_bool(\"0\")";
            root = exprtk_parse(input, 0);
            res = exprtk_eval(root, &env);
            check_float_eq(res.data.number, 0.0, 0.0001);
            exprtk_free(root);

            exprtk_env_free(&env);
        }

        it("should handle invalid conversion gracefully") {
            exprtk_env_t env;
            exprtk_env_init(&env);

            // to_num with non-numeric string returns 0
            const char *input = "to_num(\"abc\")";
            exprtk_node_t *root = exprtk_parse(input, 0);
            check_not_null(root);
            exprtk_value_t res = exprtk_eval(root, &env);
            check_float_eq(res.data.number, 0.0, 0.0001);
            exprtk_free(root);

            // to_bool with invalid string returns 0
            input = "to_bool(\"maybe\")";
            root = exprtk_parse(input, 0);
            res = exprtk_eval(root, &env);
            check_float_eq(res.data.number, 0.0, 0.0001);
            exprtk_free(root);

            // Wrong arg type returns 0
            input = "to_num(42)";
            root = exprtk_parse(input, 0);
            res = exprtk_eval(root, &env);
            check_float_eq(res.data.number, 0.0, 0.0001);
            exprtk_free(root);

            input = "to_str(\"already a string\")";
            root = exprtk_parse(input, 0);
            res = exprtk_eval(root, &env);
            check_float_eq(res.data.number, 0.0, 0.0001);
            exprtk_free(root);

            exprtk_env_free(&env);
        }
    }
}

