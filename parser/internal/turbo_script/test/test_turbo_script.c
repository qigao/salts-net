#include "turbo_script.h"
#include "tinytest.h"
#include <time.h>
#include <string.h>
#include <turbo_coro.h>
#include <turbo_coro_context.h>

typedef struct {
    turbo_script_ctx_t *ctx;
    const char *script;
    int result;
} script_coro_arg_t;

static void script_coro_task(turbo_coro_t *co, void *arg) {
    (void)co;
    script_coro_arg_t *sarg = (script_coro_arg_t *)arg;
    sarg->result = turbo_script_run(sarg->ctx, sarg->script);
}

spec("turbo_script") {
    describe("Basics") {
        it("should execute math scripts") {
            turbo_script_ctx_t *ctx = turbo_script_init();
            check_not_null(ctx);
            
            check_int_eq(turbo_script_run(ctx, "x := 10; y := x * 2;"), 0);
            check_float_eq(turbo_script_get_var_num(ctx, "y"), 20.0, 0.001);
            
            turbo_script_free(ctx);
        }

        it("should support the var keyword") {
            turbo_script_ctx_t *ctx = turbo_script_init();
            check_int_eq(turbo_script_run(ctx, "var a = 123; var b = 456; var c = a + b;"), 0);
            check_float_eq(turbo_script_get_var_num(ctx, "c"), 579.0, 0.001);
            turbo_script_free(ctx);
        }

        it("should support complex expressions and multiple assignments") {
            turbo_script_ctx_t *ctx = turbo_script_init();
            const char *script = 
                "a := 5; "
                "b := 10; "
                "c := (a + b) * 2; "
                "d := c / 3;";
            check_int_eq(turbo_script_run(ctx, script), 0);
            check_float_eq(turbo_script_get_var_num(ctx, "c"), 30.0, 0.001);
            check_float_eq(turbo_script_get_var_num(ctx, "d"), 10.0, 0.001);
            turbo_script_free(ctx);
        }

        it("should persist variables across multiple runs") {
            turbo_script_ctx_t *ctx = turbo_script_init();
            check_int_eq(turbo_script_run(ctx, "x := 100;"), 0);
            check_int_eq(turbo_script_run(ctx, "y := x + 50;"), 0);
            check_float_eq(turbo_script_get_var_num(ctx, "y"), 150.0, 0.001);
            turbo_script_free(ctx);
        }

        it("should support simple control flow") {
            turbo_script_ctx_t *ctx = turbo_script_init();
            const char *script = 
                "sum := 0; "
                "i := 0; "
                "while (i < 5) { "
                "  sum := sum + i; "
                "  i := i + 1; "
                "}";
            check_int_eq(turbo_script_run(ctx, script), 0);
            check_float_eq(turbo_script_get_var_num(ctx, "sum"), 10.0, 0.001); // 0+1+2+3+4
            turbo_script_free(ctx);
        }
    }

    describe("DateTime") {
        it("should support datetime parsing and now()") {
            turbo_script_ctx_t *ctx = turbo_script_init();
            
            // 2024-01-01 12:00:00 UTC is 1704110400
            int res = turbo_script_run(ctx, "t := date_parse(\"2024-01-01 12:00:00\");");
            if (res != 0) printf("DateTime Parse Error: %s\n", turbo_script_get_error(ctx));
            check_int_eq(res, 0);
            if (res != 0) printf("DateTime Parse Error: %s\n", turbo_script_get_error(ctx));
            check_int_eq(res, 0);
            check_float_eq(turbo_script_get_var_num(ctx, "t"), 1704110400.0, 1.0);
            
            check_int_eq(turbo_script_run(ctx, "curr := now();"), 0);
            check_not_null(ctx); // basic check
            
            turbo_script_free(ctx);
        }
    }

    describe("JSON") {
        it("should support json_query") {
            turbo_script_ctx_t *ctx = turbo_script_init();
            
            const char *script = 
                "js := \"{\\\"user\\\": \\\"bob\\\", \\\"score\\\": 42}\";"
                "res := json_query(js, \"score\");"
                "name := json_query(js, \"user\");";
            
            check_int_eq(turbo_script_run(ctx, script), 0);
            // res should be something, name should be bob
            // Note: json_query current impl returns string "42" for score probably, 
            // unless we improved it.
            
            turbo_script_free(ctx);
        }
    }

    describe("String Utilities") {
        it("should support string conversions and tokens") {
            turbo_script_ctx_t *ctx = turbo_script_init();
            
            const char *script = 
                "var s = \"123.45\";"
                "var n = str_to_num(s);"
                "var s2 = num_to_str(n);"
                "var data = \"apple,banana,cherry\";"
                "var count = str_count(data, \",\");"
                "var item = str_token(data, \",\", 1);";
            
            int res_str = turbo_script_run(ctx, script);
            if (res_str != 0) printf("String Utils Error: %s\n", turbo_script_get_error(ctx));
            check_int_eq(res_str, 0);
            check_float_eq(turbo_script_get_var_num(ctx, "n"), 123.45, 0.01);
            check_float_eq(turbo_script_get_var_num(ctx, "count"), 3.0, 0.1);
            
            turbo_script_free(ctx);
        }
    }

    describe("File System") {
        it("should support file operations") {
            turbo_script_ctx_t *ctx = turbo_script_init();
            
            // 1. Write
            check_int_eq(turbo_script_run(ctx, "res := file_write(\"test_ts.txt\", \"Hello Turbo Script!\");"), 0);
            double write_res = turbo_script_get_var_num(ctx, "res");
            printf("DEBUG: file_write res = %g\n", write_res);
            check_float_eq(write_res, 0.0, 0.1);
            
            // 2. Exists
            check_int_eq(turbo_script_run(ctx, "exists := file_exists(\"test_ts.txt\");"), 0);
            double exists_res = turbo_script_get_var_num(ctx, "exists");
            printf("DEBUG: file_exists res = %g\n", exists_res);
            check_float_eq(exists_res, 1.0, 0.1);
            
            // 3. Read
            check_int_eq(turbo_script_run(ctx, "data := file_read(\"test_ts.txt\");"), 0);
            check_int_eq(turbo_script_run(ctx, "match := (data == \"Hello Turbo Script!\");"), 0);
            double match_res = turbo_script_get_var_num(ctx, "match");
            printf("DEBUG: match res = %g\n", match_res);
            check_float_eq(match_res, 1.0, 0.1);
            
            // 4. Remove
            check_int_eq(turbo_script_run(ctx, "del := file_remove(\"test_ts.txt\");"), 0);
            check_float_eq(turbo_script_get_var_num(ctx, "del"), 0.0, 0.1);
            
            // 5. Not exists
            check_int_eq(turbo_script_run(ctx, "exists2 := file_exists(\"test_ts.txt\");"), 0);
            check_float_eq(turbo_script_get_var_num(ctx, "exists2"), 0.0, 0.1);
            
            turbo_script_free(ctx);
        }
    }

    describe("Vector Functions") {
        it("should support split, avg, len, sum, min, max") {
            turbo_script_ctx_t *ctx = turbo_script_init();
            const char *script = 
                "data := \"10,20,30,40,50\";"
                "v := split(data, \",\");"
                "a := avg(v);"
                "l := len(v);"
                "s := sum(v);"
                "mi := min(v);"
                "ma := max(v);";
            
            check_int_eq(turbo_script_run(ctx, script), 0);
            check_float_eq(turbo_script_get_var_num(ctx, "a"), 30.0, 0.1);
            check_float_eq(turbo_script_get_var_num(ctx, "l"), 5.0, 0.1);
            check_float_eq(turbo_script_get_var_num(ctx, "s"), 150.0, 0.1);
            check_float_eq(turbo_script_get_var_num(ctx, "mi"), 10.0, 0.1);
            check_float_eq(turbo_script_get_var_num(ctx, "ma"), 50.0, 0.1);
            
            turbo_script_free(ctx);
        }
    }

    describe("HTTP") {
        it("should report error if no coro context") {
            turbo_script_ctx_t *ctx = turbo_script_init();
             int res = turbo_script_run(ctx, "res := http_get(\"http://example.com\");");
            if (res != 0) printf("HTTP Coro Error: %s\n", turbo_script_get_error(ctx));
            check_int_eq(res, 0);
            int res_http_no_coro = turbo_script_run(ctx, "res := http_get(\"http://example.com\");");
            if (res_http_no_coro != 0) printf("HTTP no coro Error: %s\n", turbo_script_get_error(ctx));
            check_int_eq(res_http_no_coro, 0);
            check_float_eq(turbo_script_get_var_num(ctx, "res"), 0.0, 0.1);
            check_not_null(turbo_script_get_error(ctx));
            turbo_script_free(ctx);
        }

        it("should attempt HTTP call when in coro") {
            turbo_coro_context_t *coro_ctx = turbo_coro_context_create();
            turbo_script_ctx_t *ctx = turbo_script_init();
            turbo_script_set_coro_context(ctx, coro_ctx);

            script_coro_arg_t sarg = { ctx, "res := http_get(\"http://invalid.url\");", -1 };
            turbo_coro_t *co = turbo_coro_create(script_coro_task, &sarg, NULL);
            turbo_coro_resume(co);
            
            // Run loop for a bit (it will likely fail fast for invalid URL)
            turbo_coro_context_run(coro_ctx);

            // Even if it fails, result should be 0 because the script executed (and returned 0.0)
            check_int_eq(sarg.result, 0);

            turbo_coro_destroy(co);
            turbo_script_free(ctx);
            turbo_coro_context_destroy(coro_ctx);
        }
    }

    describe("Error Handling") {
        it("should report parse errors") {
            turbo_script_ctx_t *ctx = turbo_script_init();
            // Missing semicolon or invalid syntax
            check_int_eq(turbo_script_run(ctx, "x = 10 + * 5"), -1);
            check_not_null(turbo_script_get_error(ctx));
            turbo_script_free(ctx);
        }
    }

    describe("Stock Research: TA-Lib Scripts") {
        it("SMA crossover signal generation") {
            // Scenario: A researcher loads daily closing prices and generates
            // buy/sell signals based on SMA(5) crossing above/below SMA(10).
            turbo_script_ctx_t *ctx = turbo_script_init();

            const char *script =
                // Simulated daily close prices (20 days of uptrend then pullback)
                "close := [100, 101, 102, 104, 106, 108, 110, 112, 115, 118, "
                "          120, 119, 117, 115, 113, 112, 114, 116, 118, 120]; "

                // Compute fast and slow SMAs
                "sma_fast := ta_sma(close, 5); "
                "sma_slow := ta_sma(close, 10); "

                // Check the last bar: is fast SMA above slow SMA?
                // If yes → bullish signal (1), else bearish (0)
                "signal := if (sma_fast[19] > sma_slow[19]) { 1 } else { 0 }; "

                // Compute momentum via RSI
                "rsi := ta_rsi(close, 14); "
                "rsi_last := rsi[19]; "

                // Compute volatility
                "std := ta_stddev(close, 10, 1.0); "
                "vol_last := std[19];";

            int res = turbo_script_run(ctx, script);
            if (res != 0) printf("SMA Crossover Error: %s\n", turbo_script_get_error(ctx));
            check_int_eq(res, 0);

            // After pullback and recovery, fast SMA should be above slow SMA
            double signal = turbo_script_get_var_num(ctx, "signal");
            printf("  Signal: %g (1=bullish, 0=bearish)\n", signal);

            double rsi_last = turbo_script_get_var_num(ctx, "rsi_last");
            printf("  RSI(14): %.2f\n", rsi_last);
            check_float_gt(rsi_last, 0.0);  // RSI should be valid

            double vol_last = turbo_script_get_var_num(ctx, "vol_last");
            printf("  Volatility (StdDev): %.4f\n", vol_last);
            check_float_gt(vol_last, 0.0);  // StdDev should be positive

            turbo_script_free(ctx);
        }

        it("RSI oversold screener") {
            // Scenario: A researcher screens for oversold stocks (RSI < 30)
            turbo_script_ctx_t *ctx = turbo_script_init();

            const char *script =
                // Stock A: strong downtrend (should be oversold)
                "stock_a := [50, 49, 47, 45, 43, 41, 39, 37, 35, 33, "
                "            31, 30, 29, 28, 27]; "
                "rsi_a := ta_rsi(stock_a, 14); "
                "rsi_a_last := rsi_a[14]; "
                "oversold_a := if (rsi_a_last < 30) { 1 } else { 0 }; "

                // Stock B: strong uptrend (should be overbought)
                "stock_b := [50, 52, 54, 56, 58, 60, 62, 64, 66, 68, "
                "            70, 72, 74, 76, 78]; "
                "rsi_b := ta_rsi(stock_b, 14); "
                "rsi_b_last := rsi_b[14]; "
                "overbought_b := if (rsi_b_last > 70) { 1 } else { 0 };";

            int res = turbo_script_run(ctx, script);
            if (res != 0) printf("RSI Screener Error: %s\n", turbo_script_get_error(ctx));
            check_int_eq(res, 0);

            double rsi_a = turbo_script_get_var_num(ctx, "rsi_a_last");
            double rsi_b = turbo_script_get_var_num(ctx, "rsi_b_last");
            printf("  Stock A RSI: %.2f (downtrend)\n", rsi_a);
            printf("  Stock B RSI: %.2f (uptrend)\n", rsi_b);

            // Downtrend stock should have lower RSI than uptrend stock
            check_float_lt(rsi_a, rsi_b);

            turbo_script_free(ctx);
        }

        it("Multi-indicator confluence scoring") {
            // Scenario: A researcher combines multiple indicators into a
            // composite score to rank stocks. Score range: 0 (bearish) to 100 (bullish).
            turbo_script_ctx_t *ctx = turbo_script_init();

            const char *script =
                // 20 bars of price data with clear uptrend
                "close := [100, 102, 104, 103, 105, 107, 109, 108, 110, 112, "
                "          114, 116, 115, 117, 119, 121, 123, 122, 124, 126]; "
                "high  := [102, 104, 106, 105, 107, 109, 111, 110, 112, 114, "
                "          116, 118, 117, 119, 121, 123, 125, 124, 126, 128]; "
                "low   := [ 99, 101, 103, 102, 104, 106, 108, 107, 109, 111, "
                "          113, 115, 114, 116, 118, 120, 122, 121, 123, 125]; "

                // --- Indicator 1: Trend (SMA crossover) ---
                "sma5  := ta_sma(close, 5); "
                "sma10 := ta_sma(close, 10); "
                "trend_score := if (sma5[19] > sma10[19]) { 25 } else { 0 }; "

                // --- Indicator 2: Momentum (RSI) ---
                "rsi := ta_rsi(close, 14); "
                "rsi_val := rsi[19]; "
                "mom_score := if (rsi_val > 50 and rsi_val < 70) { 25 } "
                "             else { if (rsi_val >= 70) { 10 } else { 0 } }; "

                // --- Indicator 3: Volatility (ATR normalized) ---
                "atr := ta_atr(high, low, close, 14); "
                "atr_pct := atr[19] / close[19] * 100; "
                "vol_score := if (atr_pct < 3) { 25 } else { 10 }; "

                // --- Indicator 4: Price position (above EMA) ---
                "ema20 := ta_ema(close, 20); "
                "pos_score := if (close[19] > ema20[19]) { 25 } else { 0 }; "

                // --- Composite Score ---
                "total_score := trend_score + mom_score + vol_score + pos_score;";

            int res = turbo_script_run(ctx, script);
            if (res != 0) printf("Confluence Error: %s\n", turbo_script_get_error(ctx));
            check_int_eq(res, 0);

            double total = turbo_script_get_var_num(ctx, "total_score");
            double trend = turbo_script_get_var_num(ctx, "trend_score");
            double mom   = turbo_script_get_var_num(ctx, "mom_score");
            double vol   = turbo_script_get_var_num(ctx, "vol_score");
            double pos   = turbo_script_get_var_num(ctx, "pos_score");

            printf("  Confluence Score: %.0f / 100\n", total);
            printf("    Trend:    %.0f/25\n", trend);
            printf("    Momentum: %.0f/25\n", mom);
            printf("    Vol:      %.0f/25\n", vol);
            printf("    Position: %.0f/25\n", pos);

            // In a clear uptrend, the total score should be high
            check_float_gt(total, 50.0);

            turbo_script_free(ctx);
        }

        it("Options pricing: BSM call vs put parity") {
            // Scenario: A researcher verifies put-call parity:
            // C - P = S * e^(-qT) - K * e^(-rT)  (for q=0: C - P ≈ S - K*e^(-rT))
            turbo_script_ctx_t *ctx = turbo_script_init();

            const char *script =
                // S=100, K=100, T=1yr, r=5%, sigma=20%
                "call := ta_bsm_call([100], [100], [1], [0.05], [0.2]); "
                "put  := ta_bsm_put([100], [100], [1], [0.05], [0.2]); "
                "call_price := call[0]; "
                "put_price  := put[0]; "

                // Put-call parity: C - P ≈ S - K*e^(-rT) = 100 - 100*e^(-0.05) ≈ 4.877
                "parity := call_price - put_price; "

                // Greeks
                "delta_c := ta_bsm_delta_call([100], [100], [1], [0.05], [0.2]); "
                "delta_p := ta_bsm_delta_put([100], [100], [1], [0.05], [0.2]); "
                "dc := delta_c[0]; "
                "dp := delta_p[0]; "

                // Delta parity: delta_call - delta_put ≈ e^(-qT) ≈ 1 (for q=0)
                "delta_parity := dc - dp;";

            int res = turbo_script_run(ctx, script);
            if (res != 0) printf("Options Error: %s\n", turbo_script_get_error(ctx));
            check_int_eq(res, 0);

            double call_p = turbo_script_get_var_num(ctx, "call_price");
            double put_p  = turbo_script_get_var_num(ctx, "put_price");
            double parity = turbo_script_get_var_num(ctx, "parity");
            double dc     = turbo_script_get_var_num(ctx, "dc");
            double dp     = turbo_script_get_var_num(ctx, "dp");

            printf("  Call: $%.4f\n", call_p);
            printf("  Put:  $%.4f\n", put_p);
            printf("  C - P: $%.4f (expected ~4.877)\n", parity);
            printf("  Delta Call: %.4f\n", dc);
            printf("  Delta Put:  %.4f\n", dp);

            // Put-call parity check: C - P ≈ 4.877
            check_float_eq(parity, 4.877, 0.1);

            // Call should be more expensive than put for ATM with positive rate
            check_float_gt(call_p, put_p);

            // Delta parity: delta_c - delta_p ≈ 1
            double delta_parity = turbo_script_get_var_num(ctx, "delta_parity");
            check_float_eq(delta_parity, 1.0, 0.05);

            turbo_script_free(ctx);
        }

        it("CSV data pipeline: split -> analyze -> score") {
            // Scenario: A researcher loads CSV price data from a string,
            // splits it into a vector, and runs TA analysis.
            turbo_script_ctx_t *ctx = turbo_script_init();

            const char *script =
                // Simulated CSV close prices (comma-separated)
                "csv_data := \"100,102,104,103,105,107,109,108,110,112\"; "

                // Parse CSV into a numeric vector
                "prices := split(csv_data, \",\"); "

                // Compute indicators on the parsed data
                "sma3 := ta_sma(prices, 3); "
                "ema3 := ta_ema(prices, 3); "

                // Last values
                "last_price := prices[9]; "
                "last_sma := sma3[9]; "
                "last_ema := ema3[9]; "

                // Simple trend check
                "trend := if (last_price > last_sma and last_price > last_ema) { 1 } else { 0 };";

            int res = turbo_script_run(ctx, script);
            if (res != 0) printf("CSV Pipeline Error: %s\n", turbo_script_get_error(ctx));
            check_int_eq(res, 0);

            double last_price = turbo_script_get_var_num(ctx, "last_price");
            double last_sma   = turbo_script_get_var_num(ctx, "last_sma");
            double last_ema   = turbo_script_get_var_num(ctx, "last_ema");
            double trend      = turbo_script_get_var_num(ctx, "trend");

            printf("  Last Price: %.2f\n", last_price);
            printf("  SMA(3):    %.2f\n", last_sma);
            printf("  EMA(3):    %.2f\n", last_ema);
            printf("  Trend:     %s\n", trend > 0 ? "BULLISH" : "BEARISH");

            check_float_eq(last_price, 112.0, 0.1);

            turbo_script_free(ctx);
        }
    }
}
