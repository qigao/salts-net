/**
 * @file test_fin.c
 * @brief Unit tests for the fin module (TA, finance, timeseries)
 */
#include "fin.h"
#include "exprtk.h"
#include "tinytest.h"
#include <string.h>
#include <math.h>

spec("fin") {

    describe("Per-env Module Mounting") {
        it("should not resolve ta.sma without module") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            const char *input = "v = [1, 2, 3, 4, 5]; ta.sma(v, 3)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_NUMBER);
            check_float_eq(res.data.number, 0.0, 1e-9);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should resolve ta.sma after mounting ta module") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            const char *input = "v = [1, 2, 3, 4, 5]; ta.sma(v, 3)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_float_eq(res.data.vector.data[2], 2.0, 1e-6);
            check_float_eq(res.data.vector.data[4], 4.0, 1e-6);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should mount all three fin modules independently") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            exprtk_env_add_module(&env, exprtk_module_finance());
            exprtk_env_add_module(&env, exprtk_module_timeseries());
            check_int_eq((int)env.module_count, 3);
            exprtk_env_free(&env);
        }
    }

    describe("TA Overlap Indicators") {
        it("should compute SMA") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            const char *input = "v = [1, 2, 3, 4, 5]; ta.sma(v, 3)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_int_eq((int)res.data.vector.size, 5);
            check_float_eq(res.data.vector.data[2], 2.0, 1e-6);
            check_float_eq(res.data.vector.data[3], 3.0, 1e-6);
            check_float_eq(res.data.vector.data[4], 4.0, 1e-6);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute EMA") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            const char *input = "v = [1, 2, 3, 4, 5]; ta.ema(v, 3)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_float_eq(res.data.vector.data[4], 4.0625, 1e-4);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute WMA") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            const char *input = "v = [1, 2, 3, 4, 5]; ta.wma(v, 3)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_int_eq((int)res.data.vector.size, 5);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute Bollinger Bands") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            const char *input = "v = [20, 21, 22, 23, 24, 25, 26, 27, 28, 29]; ta.bbands(v, 5, 2.0)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute Bollinger Bands (alias boll)") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            const char *input = "v = [20, 21, 22, 23, 24, 25, 26, 27, 28, 29]; ta.boll(v, 5, 2.0)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute BBI") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            /* BBI requires 24 periods */
            const char *input = "v = [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30]; ta.bbi(v)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_int_eq((int)res.data.vector.size, 30);
            check(res.data.vector.data[23] > 0);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute HMA") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            const char *input = "v = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]; ta.hma(v, 4)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_int_eq((int)res.data.vector.size, 10);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute ZLEMA") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            const char *input = "v = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]; ta.zlema(v, 5)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_int_eq((int)res.data.vector.size, 10);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute ALMA") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            const char *input = "v = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]; ta.alma(v, 9, 0.85, 6.0)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_int_eq((int)res.data.vector.size, 10);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute VIDYA") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            const char *input = "v = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]; ta.vidya(v, 5, 5)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_int_eq((int)res.data.vector.size, 10);
            exprtk_free(root);
            exprtk_env_free(&env);
        }


        it("should compute SuperTrend") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            const char *input = "h = [10, 11, 12, 13, 14, 15, 16, 17, 18, 19]; l = [9, 10, 11, 12, 13, 14, 15, 16, 17, 18]; c = [10, 11, 12, 13, 14, 15, 16, 17, 18, 19]; ta.supertrend(h, l, c, 5, 3.0)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute RMA") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            const char *input = "v = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]; ta.rma(v, 5)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_int_eq((int)res.data.vector.size, 10);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute VWAP") {

            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            const char *input = "h = [10, 11]; l = [9, 10]; c = [10, 11]; v = [100, 200]; ta.vwap(h, l, c, v)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute Donchian Channels") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            const char *input = "h = [10, 11, 12, 13, 14]; l = [9, 10, 11, 12, 13]; ta.donchian(h, l, 3)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_float_eq(res.data.vector.data[4], 12.5, 1e-6);
            exprtk_free(root);

            /* Test individual bands */
            root = exprtk_parse("h = [10, 11, 12, 13, 14]; l = [9, 10, 11, 12, 13]; ta.donchian_up(h, l, 3)", 0);
            res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_float_eq(res.data.vector.data[4], 14.0, 1e-6);
            exprtk_free(root);

            root = exprtk_parse("h = [10, 11, 12, 13, 14]; l = [9, 10, 11, 12, 13]; ta.donchian_dn(h, l, 3)", 0);
            res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_float_eq(res.data.vector.data[4], 11.0, 1e-6);
            exprtk_free(root);

            exprtk_env_free(&env);
        }
    }

    describe("TA Momentum Indicators") {
        it("should compute RSI") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            const char *input = "v = [44.34, 44.09, 44.15, 43.61, 44.33, 44.83, 45.10, 45.42, 45.84, 46.08, 45.89, 46.03, 45.61, 46.28, 46.28]; ta.rsi(v, 14)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_int_eq((int)res.data.vector.size, 15);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute MACD") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            const char *input = "v = [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26]; ta.macd(v, 12, 26, 9)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            exprtk_free(root);
            exprtk_env_free(&env);
        }
    }

    describe("TA Volatility Indicators") {
        it("should compute ATR") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            const char *input = "h = [10, 11, 12]; l = [9, 10, 11]; c = [10, 11, 12]; ta.atr(h, l, c, 2)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_int_eq((int)res.data.vector.size, 3);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute RVI") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            const char *input = "v = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]; ta.rvi(v, 5, 5)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_int_eq((int)res.data.vector.size, 10);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute VHF") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            const char *input = "v = [1, 2, 1, 2, 1, 2, 1, 2, 1, 2]; ta.vhf(v, 5)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_int_eq((int)res.data.vector.size, 10);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute Volatility Ratio") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            const char *input = "h = [10, 11, 12, 13, 14]; l = [9, 10, 11, 12, 13]; c = [10, 11, 12, 13, 14]; ta.volatility_ratio(h, l, c, 3)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_int_eq((int)res.data.vector.size, 5);
            exprtk_free(root);
            exprtk_env_free(&env);
        }


        it("should compute StdDev") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            const char *input = "v = [1, 2, 3, 4, 5]; ta.stddev(v, 3, 1.0)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_float_eq(res.data.vector.data[2], 0.816497, 1e-5);
            exprtk_free(root);
            exprtk_env_free(&env);
        }
    }

    describe("TA Price Transforms") {
        it("should compute avgprice") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            const char *input = "o = [10, 11]; h = [12, 13]; l = [9, 10]; c = [11, 12]; ta.avgprice(o, h, l, c)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_float_eq(res.data.vector.data[0], 10.5, 1e-6);
            exprtk_free(root);
            exprtk_env_free(&env);
        }
    }

    describe("Options Pricing") {
        it("should compute BSM call price") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            const char *input = "ta.bsm_call([100], [100], [1], [0.05], [0.2])";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_float_eq(res.data.vector.data[0], 10.45058, 1e-4);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute BSM put price") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            const char *input = "ta.bsm_put([100], [100], [1], [0.05], [0.2])";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_float_eq(res.data.vector.data[0], 5.57352, 1e-4);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute BSM delta") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            const char *input = "ta.bsm_delta_call([100], [100], [1], [0.05], [0.2])";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check_float_eq(res.data.vector.data[0], 0.6368, 1e-3);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute binomial option pricing") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_ta());
            const char *input = "ta.opt_binomial(100, 100, 1, 0.05, 0.2, 100, 1)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_NUMBER);
            check_float_eq(res.data.number, 10.45058, 0.1);
            exprtk_free(root);
            exprtk_env_free(&env);
        }
    }

    describe("Finance Module") {
        it("should compute kelly criterion") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_finance());
            const char *input = "kelly(0.6, 2.0, 1.0)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_NUMBER);
            check_float_eq(res.data.number, 0.4, 1e-6);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute crossover signals") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_finance());
            const char *input = "f = [1, 3, 2, 4]; s = [2, 2, 3, 3]; crossover(f, s)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_int_eq((int)res.data.vector.size, 4);
            check_float_eq(res.data.vector.data[1], 1.0, 1e-9);
            check_float_eq(res.data.vector.data[3], 1.0, 1e-9);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute drawdown") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_finance());
            const char *input = "eq = [100, 110, 105, 120, 115]; drawdown(eq)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_int_eq((int)res.data.vector.size, 5);
            check_float_eq(res.data.vector.data[0], 0.0, 1e-9);
            check_float_eq(res.data.vector.data[2], (110.0 - 105.0) / 110.0, 1e-6);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute Efficiency Ratio") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_finance());
            const char *input = "v = [1, 2, 3, 2, 1, 2, 3]; efficiency_ratio(v, 5)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_int_eq((int)res.data.vector.size, 7);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute Bias") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_finance());
            const char *input = "v = [1, 2, 3, 4, 5]; bias(v, 3)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_int_eq((int)res.data.vector.size, 5);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute Psy") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_finance());
            const char *input = "v = [1, 2, 3, 4, 5]; psy(v, 4)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_int_eq((int)res.data.vector.size, 5);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute Pressure") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_finance());
            const char *input = "h=[10,11]; l=[9,10]; c=[10,11]; v=[100,200]; pressure(h,l,c,v)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_int_eq((int)res.data.vector.size, 4); // 2 inputs * 2 outputs
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute KVO") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_finance());
            const char *input = "h=[10,11,12]; l=[9,10,11]; c=[10,11,12]; v=[100,200,300]; kvo(h,l,c,v,2,3,2)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_int_eq((int)res.data.vector.size, 6); // 3 inputs * 2 outputs
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute Candle Metrics") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_finance());
            const char *input = "o=[10,11]; h=[12,13]; l=[9,10]; c=[11,12]; candle_body(o,h,l,c)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_float_eq(res.data.vector.data[0], 1.0, 1e-6);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute Fuzzy Candle") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_finance());
            const char *input = "o=[10,11]; h=[12,13]; l=[9,10]; c=[11,12]; candle_fuzzy_bull(o,h,l,c)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_float_eq(res.data.vector.data[0], 0.666666, 1e-4);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute Pivots") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_finance());
            const char *input = "h=[10, 12, 11, 13, 11, 14, 10]; pivot_high(h, 1, 1)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_float_eq(res.data.vector.data[1], 12.0, 1e-6);
            check_float_eq(res.data.vector.data[3], 13.0, 1e-6);
            exprtk_free(root);
            exprtk_env_free(&env);
        }


    }

    describe("Timeseries Module") {
        it("should compute ts_diff") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_timeseries());
            const char *input = "v = [1, 3, 6, 10, 15]; ts_diff(v, 1)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_int_eq((int)res.data.vector.size, 5);
            check_float_eq(res.data.vector.data[1], 2.0, 1e-6);
            check_float_eq(res.data.vector.data[2], 3.0, 1e-6);
            check_float_eq(res.data.vector.data[3], 4.0, 1e-6);
            check_float_eq(res.data.vector.data[4], 5.0, 1e-6);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute ts_autocorr") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_timeseries());
            const char *input = "v = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]; ts_autocorr(v, 3)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_int_eq((int)res.data.vector.size, 4);
            check_float_eq(res.data.vector.data[0], 1.0, 1e-6);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute ts_garch") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_timeseries());
            const char *input = "r = [0.01, -0.02, 0.03, -0.01, 0.02, -0.03, 0.01, 0.02]; ts_garch(r, 0.1, 0.8)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_int_eq((int)res.data.vector.size, 8);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute ts_match") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_timeseries());
            // Distance between [1,2,3] and [1,2,3] is 0
            // Distance between [2,3,4] and [1,2,3] is 3 * (1^2) = 3
            const char *input = "v = [1, 2, 3, 4, 5]; p = [1, 2, 3]; ts_match(v, p)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_int_eq((int)res.data.vector.size, 5);
            check_float_eq(res.data.vector.data[0], 0.0, 1e-6);
            check_float_eq(res.data.vector.data[1], 3.0, 1e-6);
            check_float_eq(res.data.vector.data[2], 12.0, 1e-6);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute ts_match_cosine") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_timeseries());
            // Cosine distance of identical vectors is 0
            const char *input = "v = [1, 2, 3, 4, 5]; p = [1, 2, 3]; ts_match_cosine(v, p)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_float_eq(res.data.vector.data[0], 0.0, 1e-6);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute ts_match_normalized") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_timeseries());
            // Normalized match handles scale/offset. [2,4,6] vs [1,2,3] should be 0 dist.
            const char *input = "v = [10, 20, 30, 40, 50]; p = [1, 2, 3]; ts_match_normalized(v, p)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_float_eq(res.data.vector.data[0], 0.0, 1e-4);
            check_float_eq(res.data.vector.data[1], 0.0, 1e-4);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute ts_match_candle") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_timeseries());
            // Comparing identical candle sequences should be 0 dist
            const char *input = 
                "O=[10,11]; H=[12,13]; L=[9,10]; C=[11,12]; "
                "pO=[10]; pH=[12]; pL=[9]; pC=[11]; "
                "ts_match_candle(O, H, L, C, pO, pH, pL, pC)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_float_eq(res.data.vector.data[0], 0.0, 1e-6);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute ts_match_dtw") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_timeseries());
            // DTW should handle slight shifts. [1,2,3] vs [1,2,3] is 0.
            const char *input = "v = [1, 2, 3, 4, 5]; p = [1, 2, 3]; ts_match_dtw(v, p)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_float_eq(res.data.vector.data[0], 0.0, 1e-6);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute ts_match_correl") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_timeseries());
            // Perfect correlation should be 1.0
            const char *input = "v = [1, 2, 3, 4, 5]; p = [1, 2, 3]; ts_match_correl(v, p)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_float_eq(res.data.vector.data[0], 1.0, 1e-6);
            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should compute ts_match_returns") {
            exprtk_env_t env;
            exprtk_env_init(&env);
            exprtk_env_add_module(&env, exprtk_module_timeseries());
            // Distance of identical return profile is 0
            const char *input = "v = [1, 2, 4, 8, 16]; p = [1, 2, 4]; ts_match_returns(v, p)";
            exprtk_node_t *root = exprtk_parse(input, 0);
            exprtk_value_t res = exprtk_eval(root, &env);
            check(res.type == exprtk_VAL_VECTOR);
            check_float_eq(res.data.vector.data[0], 0.0, 1e-4);
            exprtk_free(root);
            exprtk_env_free(&env);
        }
    }

    describe("C-Level API") {
        it("should compute SMA via C API") {
            double in[] = {1, 2, 3, 4, 5};
            double out[5] = {0};
            size_t r = exprtk_ta_sma(in, 5, 3, out);
            check_int_eq((int)r, 5);
            check_float_eq(out[2], 2.0, 1e-6);
            check_float_eq(out[3], 3.0, 1e-6);
            check_float_eq(out[4], 4.0, 1e-6);
        }

        it("should compute EMA via C API") {
            double in[] = {1, 2, 3, 4, 5};
            double out[5] = {0};
            size_t r = exprtk_ta_ema(in, 5, 3, out);
            check_int_eq((int)r, 5);
            check_float_eq(out[4], 4.0625, 1e-4);
        }

        it("should compute RSI via C API") {
            double in[] = {44.34, 44.09, 44.15, 43.61, 44.33, 44.83, 45.10, 45.42, 45.84, 46.08, 45.89, 46.03, 45.61, 46.28, 46.28};
            double out[15] = {0};
            turbo_arena_t arena;
            turbo_arena_init(&arena, 1024);
            size_t r = exprtk_ta_rsi(in, 15, 14, out, &arena);
            turbo_arena_free(&arena);

            check_int_eq((int)r, 15);
            check(out[14] > 0.0);
        }

        it("should compute BSM call via C API") {
            double S[] = {100}, K[] = {100}, T[] = {1}, r[] = {0.05}, sigma[] = {0.2};
            double out[1] = {0};
            exprtk_ta_bsm_call(S, K, T, r, sigma, 1, out);
            check_float_eq(out[0], 10.45058, 1e-4);
        }

        it("should compute crossover via C API") {
            double fast[] = {1, 3, 2, 4};
            double slow[] = {2, 2, 3, 3};
            double out[4] = {0};
            size_t count = exprtk_crossover(fast, slow, 4, out);
            check_int_eq((int)count, 2);
            check_float_eq(out[1], 1.0, 1e-9);
            check_float_eq(out[3], 1.0, 1e-9);
        }

        it("should compute kelly via C API") {
            double k = exprtk_kelly(0.6, 2.0, 1.0);
            check_float_eq(k, 0.4, 1e-6);
        }
    }
}
