#include "exprtk_types.h"
#include "tinytest.h"
#include "turbo_script.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  turbo_script_ctx_t *ctx;
  const char *script;
  int result;
} script_coro_arg_t;

static exprtk_value_t test_triple_fn(size_t argc, exprtk_value_t *args, void *user_data) {
  (void)user_data;
  if (argc != 1 || args[0].type != exprtk_VAL_NUMBER) {
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = 0.0};
  }
  return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = args[0].data.number * 3.0};
}

spec("turbo_script") {
  describe("Basics") {
    it("should execute math scripts") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      check_not_null(ctx);

      check_int_eq(turbo_script_run(ctx, " x = 10; y = x * 2;"), 0);
      check_float_eq(get_num(ctx, "y"), 20.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should support the var keyword") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      check_int_eq(turbo_script_run(ctx, " var "
                                         "a = 123; var b = 456; var c = a + b;"),
                   0);
      check_float_eq(get_num(ctx, "c"), 579.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should support complex expressions and multiple assignments") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = " a = 5; "
                           "b = 10; "
                           "c = (a + b) * 2; "
                           "d = c / 3;";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "c"), 30.0, 0.001);
      check_float_eq(get_num(ctx, "d"), 10.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should persist variables across multiple runs") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      check_int_eq(turbo_script_run(ctx, " x = 100;"), 0);
      check_int_eq(turbo_script_run(ctx, " y = x + 50;"), 0);
      check_float_eq(get_num(ctx, "y"), 150.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should support simple control flow") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = " sum = 0; "
                           "i = 0; "
                           "while (i < 5) { "
                           "  sum = sum + i; "
                           "  i = i + 1; "
                           "}";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "sum"), 10.0, 0.001); // 0+1+2+3+4
      turbo_script_free(ctx);
    }
  }

  describe("DateTime") {
    it("should support date parsing and now()") {
      turbo_script_ctx_t *ctx = turbo_script_init();

      // 2024-01-01 12:00:00 UTC is 1704110400
      int res = turbo_script_run(ctx, "t = date(\"2024-01-01 12:00:00\");");
      if (res != 0)
        printf("DateTime Parse Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(get_num(ctx, "t"), 1704110400.0, 1.0);

      check_int_eq(turbo_script_run(ctx, "curr = now();"), 0);
      check_float_gt(get_num(ctx, "curr"), 1700000000.0);

      turbo_script_free(ctx);
    }
  }

  describe("JSON") {
    it("should support json_query") {
      turbo_script_ctx_t *ctx = turbo_script_init();

      const char *script = " js = "
                           "\"{\\\"user\\\": \\\"bob\\\", \\\"score\\\": 42}\";"
                           "res = json.query(js, \"score\");"
                           "name = json.query(js, \"user\");";

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

      const char *script = " var s = \"123.45\";"
                           "var n = str_to_num(s);"
                           "var s2 = num_to_str(n);"
                           "var data = \"apple,banana,cherry\";"
                           "var count = str_count(data, \",\");"
                           "var item = str_token(data, \",\", 1);";

      int res_str = turbo_script_run(ctx, script);
      if (res_str != 0)
        printf("String Utils Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res_str, 0);
      check_float_eq(get_num(ctx, "n"), 123.45, 0.01);
      check_float_eq(get_num(ctx, "count"), 3.0, 0.1);

      turbo_script_free(ctx);
    }
  }

  describe("File System") {
    it("should support file operations") {
      turbo_script_ctx_t *ctx = turbo_script_init();

      // 1. Write
      check_int_eq(turbo_script_run(ctx, "res "
                                         "= write_file(\"test_ts.txt\", \"Hello Turbo Script!\");"),
                   0);
      double write_res = get_num(ctx, "res");
      printf("DEBUG: file_write res = %g\n", write_res);
      check_float_eq(write_res, 0.0, 0.1);

      // 2. Exists
      check_int_eq(turbo_script_run(ctx, ""
                                         "exists = file_exists(\"test_ts.txt\");"),
                   0);
      double exists_res = get_num(ctx, "exists");
      printf("DEBUG: file_exists res = %g\n", exists_res);
      check_float_eq(exists_res, 1.0, 0.1);

      // 3. Read
      check_int_eq(turbo_script_run(ctx, "data "
                                         "= read_file(\"test_ts.txt\");"),
                   0);
      const char *data = get_str(ctx, "data");
      check_not_null(data);
      check_str_eq(data, "Hello Turbo Script!");

      // 4. Remove
      check_int_eq(turbo_script_run(ctx, "del "
                                         "= file_remove(\"test_ts.txt\");"),
                   0);
      check_float_eq(get_num(ctx, "del"), 0.0, 0.1);

      // 5. Not exists
      check_int_eq(turbo_script_run(ctx, ""
                                         "exists2 = file_exists(\"test_ts.txt\");"),
                   0);
      check_float_eq(get_num(ctx, "exists2"), 0.0, 0.1);

      turbo_script_free(ctx);
    }
  }

  describe("Vector Functions") {
    it("should support split, avg, len, sum, min, max") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = " data = \"10,20,30,40,50\";"
                           "v = split(data, \",\");"
                           "a = vec.avg(v);"
                           "l = vec.len(v);"
                           "s = vec.sum(v);"
                           "mi = vec.min(v);"
                           "ma = vec.max(v);";

      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "a"), 30.0, 0.1);
      check_float_eq(get_num(ctx, "l"), 5.0, 0.1);
      check_float_eq(get_num(ctx, "s"), 150.0, 0.1);
      check_float_eq(get_num(ctx, "mi"), 10.0, 0.1);
      check_float_eq(get_num(ctx, "ma"), 50.0, 0.1);

      turbo_script_free(ctx);
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
      turbo_script_load_plugin(ctx, "ta");

      const char *script =
          // Simulated daily close prices (20 days of uptrend then pullback)
          "close = [100, 101, 102, 104, 106, "
          "108, 110, 112, 115, 118, "
          "          120, 119, 117, 115, 113, 112, 114, 116, 118, 120]; "

          // Compute fast and slow SMAs
          "sma_fast = ta.sma(close, 5); "
          "sma_slow = ta.sma(close, 10); "

          // Check the last bar: is fast SMA above slow SMA?
          // If yes → bullish signal (1), else bearish (0)
          "signal = if (sma_fast[19] > sma_slow[19]) { 1 } else { 0 }; "

          // Compute momentum via RSI
          "rsi = ta.rsi(close, 14); "
          "rsi_last = rsi[19]; "

          // Compute volatility
          "std = ta.stddev(close, 10, 1.0); "
          "vol_last = std[19];";

      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("SMA Crossover Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      // After pullback and recovery, fast SMA should be above slow SMA
      double signal = get_num(ctx, "signal");
      printf("  Signal: %g (1=bullish, 0=bearish)\n", signal);

      double rsi_last = get_num(ctx, "rsi_last");
      printf("  RSI(14): %.2f\n", rsi_last);
      check_float_gt(rsi_last, 0.0); // RSI should be valid

      double vol_last = get_num(ctx, "vol_last");
      printf("  Volatility (StdDev): %.4f\n", vol_last);
      check_float_gt(vol_last, 0.0); // StdDev should be positive

      turbo_script_free(ctx);
    }

    it("RSI oversold screener") {
      // Scenario: A researcher screens for oversold stocks (RSI < 30)
      turbo_script_ctx_t *ctx = turbo_script_init();
      turbo_script_load_plugin(ctx, "ta");

      const char *script =
          // Stock A: strong downtrend (should be oversold)
          "stock_a = [50, 49, 47, 45, 43, 41, "
          "39, 37, 35, 33, "
          "            31, 30, 29, 28, 27]; "
          "rsi_a = ta.rsi(stock_a, 14);"
          "rsi_a_last = rsi_a[14]; "
          "oversold_a = if (rsi_a_last < 30) { 1 } else { 0 }; "

          // Stock B: strong uptrend (should be overbought)
          "stock_b = [50, 52, 54, 56, 58, 60, 62, 64, 66, 68, "
          "            70, 72, 74, 76, 78]; "
          "rsi_b = ta.rsi(stock_b, 14);"
          "rsi_b_last = rsi_b[14]; "
          "overbought_b = if (rsi_b_last > 70) { 1 } else { 0 };";

      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("RSI Screener Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      double rsi_a = get_num(ctx, "rsi_a_last");
      double rsi_b = get_num(ctx, "rsi_b_last");
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
      turbo_script_load_plugin(ctx, "ta");

      const char *script =
          // 20 bars of price data with clear uptrend
          "close = [100, 102, 104, 103, 105, "
          "107, 109, 108, 110, 112, "
          "          114, 116, 115, 117, 119, 121, 123, 122, 124, 126]; "
          "high  = [102, 104, 106, 105, 107, 109, 111, 110, 112, 114, "
          "          116, 118, 117, 119, 121, 123, 125, 124, 126, 128]; "
          "low   = [ 99, 101, 103, 102, 104, 106, 108, 107, 109, 111, "
          "          113, 115, 114, 116, 118, 120, 122, 121, 123, 125]; "

          // --- Indicator 1: Trend (SMA crossover) ---
          "sma5  = ta.sma(close, 5); "
          "sma10 = ta.sma(close, 10); "
          "trend_score = if (sma5[19] > sma10[19]) { 25 } else { 0 }; "

          // --- Indicator 2: Momentum (RSI) ---
          "rsi = ta.rsi(close, 14); "
          "rsi_val = rsi[19]; "
          "mom_score = if (rsi_val > 50 and rsi_val < 70) { 25 } "
          "             else { if (rsi_val >= 70) { 10 } else { 0 } }; "

          // --- Indicator 3: Volatility (ATR normalized) ---
          "atr = ta.atr(high, low, close, 14); "
          "atr_pct = atr[19] / close[19] * 100; "
          "vol_score = if (atr_pct < 3) { 25 } else { 10 }; "

          // --- Indicator 4: Price position (above EMA) ---
          "ema20 = ta.ema(close, 20); "
          "pos_score = if (close[19] > ema20[19]) { 25 } else { 0 }; "

          // --- Composite Score ---
          "total_score = trend_score + mom_score + vol_score + pos_score;";

      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("Confluence Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      double total = get_num(ctx, "total_score");
      double trend = get_num(ctx, "trend_score");
      double mom = get_num(ctx, "mom_score");
      double vol = get_num(ctx, "vol_score");
      double pos = get_num(ctx, "pos_score");

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
      turbo_script_load_plugin(ctx, "ta");

      const char *script =
          // S=100, K=100, T=1yr, r=5%, sigma=20%
          "call = ta.bsm_call([100], [100], "
          "[1], [0.05], [0.2]); "
          "put  = ta.bsm_put([100], [100], [1], [0.05], [0.2]); "
          "call_price = call[0]; "
          "put_price  = put[0]; "

          // Put-call parity: C - P ≈ S - K*e^(-rT) = 100 - 100*e^(-0.05) ≈ 4.877
          "parity = call_price - put_price; "

          // Greeks
          "delta_c = ta.bsm_delta_call([100], [100], [1], [0.05], [0.2]); "
          "delta_p = ta.bsm_delta_put([100], [100], [1], [0.05], [0.2]); "
          "dc = delta_c[0]; "
          "dp = delta_p[0]; "

          // Delta parity: delta_call - delta_put ≈ e^(-qT) ≈ 1 (for q=0)
          "delta_parity = dc - dp;";

      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("Options Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      double call_p = get_num(ctx, "call_price");
      double put_p = get_num(ctx, "put_price");
      double parity = get_num(ctx, "parity");
      double dc = get_num(ctx, "dc");
      double dp = get_num(ctx, "dp");

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
      double delta_parity = get_num(ctx, "delta_parity");
      check_float_eq(delta_parity, 1.0, 0.05);

      turbo_script_free(ctx);
    }

    it("CSV data pipeline: split -> analyze -> score") {
      // Scenario: A researcher loads CSV price data from a string,
      // splits it into a vector, and runs TA analysis.
      turbo_script_ctx_t *ctx = turbo_script_init();
      turbo_script_load_plugin(ctx, "ta");

      const char *script =
          // Simulated CSV close prices (comma-separated)
          "csv_data = "
          "\"100,102,104,103,105,107,109,108,110,112\"; "

          // Parse CSV into a numeric vector
          "prices = split(csv_data, \",\"); "

          // Compute indicators on the parsed data
          "sma3 = ta.sma(prices, 3); "
          "ema3 = ta.ema(prices, 3); "

          // Last values
          "last_price = prices[9]; "
          "last_sma = sma3[9]; "
          "last_ema = ema3[9]; "

          // Simple trend check
          "trend = if (last_price > last_sma and last_price > last_ema) { 1 } else { 0 };";

      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("CSV Pipeline Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      double last_price = get_num(ctx, "last_price");
      double last_sma = get_num(ctx, "last_sma");
      double last_ema = get_num(ctx, "last_ema");
      double trend = get_num(ctx, "trend");

      printf("  Last Price: %.2f\n", last_price);
      printf("  SMA(3):    %.2f\n", last_sma);
      printf("  EMA(3):    %.2f\n", last_ema);
      printf("  Trend:     %s\n", trend > 0 ? "BULLISH" : "BEARISH");

      check_float_eq(last_price, 112.0, 0.1);

      turbo_script_free(ctx);
    }
  }

  describe("Function Syntax (func)") {
    it("should allow defining and calling func in script") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = "func double_val(x) { "
                           "  return x * 2; "
                           "}"
                           "res = double_val(21);";

      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "res"), 42.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should support recursion with func") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = "func factorial(n) { "
                           "  if (n <= 1) { return 1; } "
                           "  else { return n * factorial(n - 1); } "
                           "};"
                           "res = factorial(5);";

      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "res"), 120.0, 0.001);
      turbo_script_free(ctx);
    }
  }

  describe("Modules") {
    it("should support importing external scripts") {
      turbo_script_ctx_t *ctx = turbo_script_init();

      // Write it using native file_write (or just setup beforehand? native is easier if we have
      // ctx) But we can use turbo_script_run to write it!
      turbo_script_run(ctx, "write_file(\"utils.ts\", "
                            "\"var MODULE_VERSION = 2.0; func square(x) { return x * x; };\");");

      // Main script
      const char *script = "import(\"utils.ts\"); "
                           "res = square(5); "
                           "ver = MODULE_VERSION;";

      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "res"), 25.0, 0.1);
      check_float_eq(get_num(ctx, "ver"), 2.0, 0.1);

      // Clean up
      turbo_script_run(ctx, "file_remove(\"utils.ts\");");
      turbo_script_free(ctx);
    }
  }

  describe("TA Indicators and JSON Vectors") {
    it("should compute SMA and RSI on mocked Polymarket data") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      turbo_script_load_plugin(ctx, "ta");
      turbo_script_load_plugin(ctx, "feeds");

      // Mocked Polymarket history JSON (array of objects with 'p' field)
      const char *mock_history = "["
                                 "{\"p\":0.51},{\"p\":0.52},{\"p\":0.53},{\"p\":0.54},{\"p\":0.55},"
                                 "{\"p\":0.56},{\"p\":0.57},{\"p\":0.58},{\"p\":0.59},{\"p\":0.60}"
                                 "]";
      bind_str(ctx, "history_json", mock_history);

      const char *script = "prices = "
                           "json.to_vec(history_json, \"p\");"
                           "sma3 = ta.sma(prices, 3);"
                           "rsi5 = ta.rsi(prices, 5);"
                           "last_price = prices[9];"
                           "last_sma = sma3[9];"
                           "last_rsi = rsi5[9];"
                           "res = last_sma;";

      int run_res = turbo_script_run(ctx, script);
      if (run_res != 0)
        printf("TA Test Run Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(run_res, 0);

      double lp = get_num(ctx, "last_price");
      printf("  Last Price: %g\n", lp);
      check_float_eq(lp, 0.60, 0.001);

      double ls = get_num(ctx, "res");
      printf("  SMA(3): %g\n", ls);
      check_float_eq(ls, 0.59, 0.001);

      double rsi_val = get_num(ctx, "last_rsi");
      printf("  RSI(5): %g\n", rsi_val);
      check_float_gt(rsi_val, 0.0);
      check_float_eq(rsi_val, 100.0, 0.1);

      turbo_script_free(ctx);
    }
  }

  describe("Compile/Exec Separation") {
    it("should compile once and exec twice with different x") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      turbo_script_compiled_t *compiled = turbo_script_compile(ctx, "y = x * 2;");
      check_not_null(compiled);

      bind_num(ctx, "x", 5.0);
      check_int_eq(turbo_script_exec(ctx, compiled), 0);
      check_float_eq(get_num(ctx, "y"), 10.0, 0.001);

      bind_num(ctx, "x", 100.0);
      check_int_eq(turbo_script_exec(ctx, compiled), 0);
      check_float_eq(get_num(ctx, "y"), 200.0, 0.001);

      turbo_script_compiled_free(compiled);
      turbo_script_free(ctx);
    }

    it("should return NULL on compile error") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      turbo_script_compiled_t *compiled = turbo_script_compile(ctx, "x = 10 + * 5");
      check_null(compiled);
      turbo_script_free(ctx);
    }
  }

  describe("Vector Data Channel") {
    it("should inject double[] from C and read back from script") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      double data[] = {1.0, 2.0, 3.0, 4.0, 5.0};
      check_int_eq(bind_vec(ctx, "v", data, 5), 0);

      check_int_eq(turbo_script_run(ctx, "s = "
                                         "vec.sum(v); a = vec.avg(v);"),
                   0);
      check_float_eq(get_num(ctx, "s"), 15.0, 0.001);
      check_float_eq(get_num(ctx, "a"), 3.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should extract vector from script back to C") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      check_int_eq(turbo_script_run(ctx, "v = "
                                         "split(\"10,20,30\", \",\");"),
                   0);

      const double *out = NULL;
      size_t out_len = 0;
      check_int_eq(get_vec(ctx, "v", &out, &out_len), 0);
      check_int_eq((int)out_len, 3);
      check_float_eq(out[0], 10.0, 0.001);
      check_float_eq(out[1], 20.0, 0.001);
      check_float_eq(out[2], 30.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should return -1 for not-found vector") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const double *out = NULL;
      size_t out_len = 0;
      check_int_eq(get_vec(ctx, "nonexistent", &out, &out_len), -1);
      turbo_script_free(ctx);
    }
  }

  describe("String Variable Access") {
    it("should roundtrip set_var_str and get_var_str") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      bind_str(ctx, "greeting", "hello world");
      const char *val = get_str(ctx, "greeting");
      check_not_null(val);
      check_int_eq(strcmp(val, "hello world"), 0);
      turbo_script_free(ctx);
    }

    it("should return NULL for wrong type") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      bind_num(ctx, "num", 42.0);
      const char *val = get_str(ctx, "num");
      check_null(val);
      turbo_script_free(ctx);
    }
  }

  describe("User Function Registration") {
    it("should bind C function and call from script") {
      turbo_script_ctx_t *ctx = turbo_script_init();

      bind_func(ctx, "triple", test_triple_fn, NULL);
      check_int_eq(turbo_script_run(ctx, "r = triple(7);"), 0);
      check_float_eq(get_num(ctx, "r"), 21.0, 0.001);

      turbo_script_free(ctx);
    }
  }

  describe("Error Propagation") {
    it("should abort on wrong arg type") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      int res = turbo_script_run(ctx, "r = vec.avg(42);");
      check_int_eq(res, -1);
      turbo_script_free(ctx);
    }

    it("should not abort on runtime failure like missing file") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      int res = turbo_script_run(ctx, "data = "
                                      "read_file(\"nonexistent_file_xyz.txt\");");
      check_int_eq(res, 0);
      turbo_script_free(ctx);
    }
  }

  describe("Grammar: Compound Assignment") {
    it("should support += -= *= /=") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = "x = 10; "
                           "x += 5; " // 15
                           "x -= 3; " // 12
                           "x *= 2; " // 24
                           "x /= 4;"; // 6
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "x"), 6.0, 0.001);
      turbo_script_free(ctx);
    }
  }

  describe("Grammar: For Loop") {
    it("should support for(init; cond; post) { body }") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = "sum = 0; "
                           "for (i = 0; i < 10; i += 1) { "
                           "  sum += i; "
                           "}";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "sum"), 45.0, 0.001); // 0+1+...+9
      turbo_script_free(ctx);
    }
  }

  describe("Grammar: Vector Slicing") {
    it("should support arr[start..end] slicing") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = "v = [10, 20, 30, 40, 50]; "
                           "s = v[1..4]; "
                           "l = vec.len(s); "
                           "total = vec.sum(s);";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "l"), 3.0, 0.001);      // elements at index 1,2,3
      check_float_eq(get_num(ctx, "total"), 90.0, 0.001); // 20+30+40
      turbo_script_free(ctx);
    }
  }

  describe("Quant: Risk Metrics") {
    it("should compute VaR, CVaR, Kelly criterion") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      turbo_script_load_plugin(ctx, "ta");
      const char *script =
          " returns = [-0.02, 0.01, -0.03, "
          "0.02, 0.01, -0.01, 0.03, -0.02, 0.01, -0.04, "
          "            0.02, 0.01, -0.01, 0.03, -0.02, 0.01, -0.03, 0.02, 0.01, -0.01]; "
          "vh = var_hist(returns, 0.95); "
          "vp = var_param(returns, 0.95); "
          "cv = cvar(returns, 0.95); "
          "k = kelly(0.6, 0.02, 0.015);";
      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("Risk Metrics Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      double vh = get_num(ctx, "vh");
      double vp = get_num(ctx, "vp");
      double cv = get_num(ctx, "cv");
      double k = get_num(ctx, "k");
      printf("  VaR(hist): %.4f\n", vh);
      printf("  VaR(param): %.4f\n", vp);
      printf("  CVaR: %.4f\n", cv);
      printf("  Kelly: %.4f\n", k);

      // VaR should be positive (loss magnitude)
      check_float_gt(vh, 0.0);
      // Kelly should be positive for profitable strategy
      check_float_gt(k, 0.0);

      turbo_script_free(ctx);
    }

    it("should compute drawdown stats") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      turbo_script_load_plugin(ctx, "ta");
      const char *script = "equity = [100, "
                           "105, 103, 108, 106, 110, 107, 112, 115, 113]; "
                           "dd = drawdown(equity); "
                           "dd_stats = drawdown_stats(equity);";
      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("Drawdown Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      // dd should be a vector of same length
      const double *dd_data = NULL;
      size_t dd_len = 0;
      check_int_eq(get_vec(ctx, "dd", &dd_data, &dd_len), 0);
      check_int_eq((int)dd_len, 10);

      turbo_script_free(ctx);
    }
  }

  describe("Quant: Signal Detection") {
    it("should detect crossover and crossunder") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      turbo_script_load_plugin(ctx, "ta");
      const char *script = " fast = [1, 3, 5, 4, 2, 4, 6]; "
                           "slow = [2, 2, 4, 5, 3, 3, 5]; "
                           "co = crossover(fast, slow); "
                           "cu = crossunder(fast, slow); "
                           "co_len = vec.len(co); "
                           "cu_len = vec.len(cu);";
      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("Signal Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(get_num(ctx, "co_len"), 7.0, 0.001);
      check_float_eq(get_num(ctx, "cu_len"), 7.0, 0.001);

      turbo_script_free(ctx);
    }
  }

  describe("Quant: Candlestick Patterns") {
    it("should detect doji and hammer patterns") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      turbo_script_load_plugin(ctx, "ta");
      const char *script =
          // Doji: open ≈ close, long shadows
          "o = [100, 100, 100, 100, 100]; "
          "h = [105, 105, 105, 105, 105]; "
          "l = [ 95,  95,  95,  95,  95]; "
          "c = [100.1, 100.2, 99.9, 100, 100.1]; "
          "doji = fin.candle_doji(o, h, l, c, 0.1); "
          "hammer = fin.candle_hammer(o, h, l, c); "
          "doji_len = vec.len(doji); "
          "hammer_len = vec.len(hammer);";
      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("Candle Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(get_num(ctx, "doji_len"), 5.0, 0.001);
      check_float_eq(get_num(ctx, "hammer_len"), 5.0, 0.001);

      turbo_script_free(ctx);
    }
  }

  describe("Quant: Portfolio Optimization") {
    it("should compute minimum variance weights") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      turbo_script_load_plugin(ctx, "ta");
      const char *script =
          // 2x2 covariance matrix (2 assets): [var1, cov12, cov21, var2]
          "cov = [0.04, 0.01, 0.01, 0.09]; "
          "w = pf_min_variance(cov); "
          "w_len = vec.len(w);";
      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("Portfolio Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      const double *w_data = NULL;
      size_t w_len = 0;
      check_int_eq(get_vec(ctx, "w", &w_data, &w_len), 0);
      check_int_eq((int)w_len, 2);

      // Weights should sum to ~1
      double w_sum = w_data[0] + w_data[1];
      printf("  Weights: [%.4f, %.4f] sum=%.4f\n", w_data[0], w_data[1], w_sum);
      check_float_eq(w_sum, 1.0, 0.05);

      turbo_script_free(ctx);
    }
  }

  describe("Scientific: Advanced Statistics") {
    it("should compute median, percentile, skewness, kurtosis") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = "v = [1, 2, 3, 4, "
                           "5, 6, 7, 8, 9, 10]; "
                           "med = median(v); "
                           "p75 = percentile(v, 75); "
                           "sk = skewness(v); "
                           "ku = kurtosis(v); "
                           "gm = geometric_mean(v); "
                           "hm = harmonic_mean(v);";
      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("Stats Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      double med = get_num(ctx, "med");
      double p75 = get_num(ctx, "p75");
      double sk = get_num(ctx, "sk");
      double gm = get_num(ctx, "gm");
      double hm = get_num(ctx, "hm");
      printf("  Median: %.2f\n", med);
      printf("  P75: %.2f\n", p75);
      printf("  Skewness: %.4f\n", sk);
      printf("  Geometric Mean: %.4f\n", gm);
      printf("  Harmonic Mean: %.4f\n", hm);

      check_float_eq(med, 5.5, 0.1);
      check_float_gt(p75, med);
      // Uniform distribution: skewness ≈ 0
      check_float_eq(sk, 0.0, 0.5);
      // GM < AM < nothing, HM < GM
      check_float_lt(hm, gm);

      turbo_script_free(ctx);
    }

    it("should compute cumsum, cumprod, rank, zscore") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = "v = [3, 1, 4, 1, 5]; "
                           "cs = vec.cumsum(v); "
                           "rk = rank(v); "
                           "zs = zscore(v); "
                           "cs_last = cs[4]; "
                           "cs_len = vec.len(cs);";
      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("Stats2 Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(get_num(ctx, "cs_last"), 14.0, 0.001); // 3+1+4+1+5
      check_float_eq(get_num(ctx, "cs_len"), 5.0, 0.001);

      turbo_script_free(ctx);
    }
  }

  describe("Scientific: Time Series") {
    it("should compute diff, autocorrelation, hurst exponent") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      turbo_script_load_plugin(ctx, "ts");
      turbo_script_load_plugin(ctx, "vec");
      const char *script = "v = [100, 102, "
                           "101, 105, 103, 107, 106, 110, 108, 112, "
                           "     111, 115, 113, 117, 116, 120, 118, 122, 121, 125]; "
                           "d = ts.diff(v, 1); "
                           "d_len = vec.len(d); "
                           "ac = ts.autocorr(v, 5); "
                           "ac_len = vec.len(ac); "
                           "h = ts.hurst(v);";
      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("TimeSeries Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      double d_len = get_num(ctx, "d_len");
      double ac_len = get_num(ctx, "ac_len");
      printf("  Diff len: %.0f\n", d_len);
      printf("  Autocorr len: %.0f\n", ac_len);
      check_float_gt(d_len, 0.0);
      check_float_gt(ac_len, 0.0);

      turbo_script_free(ctx);
    }
  }

  describe("Scientific: Calculus") {
    it("should integrate and differentiate script functions") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script =
          // Define f(x) = x^2, integrate from 0 to 3 → should be 9
          "func f(x) { return x * x; }; "
          "area = integrate(\"f\", 0, 3, 1000); "
          // derivative of x^2 at x=2 → should be 4
          "slope = derivative(\"f\", 2);";
      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("Calculus Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      double area = get_num(ctx, "area");
      double slope = get_num(ctx, "slope");
      printf("  Integral of x^2 from 0..3: %.4f (expected 9)\n", area);
      printf("  Derivative of x^2 at x=2: %.4f (expected 4)\n", slope);

      check_float_eq(area, 9.0, 0.01);
      check_float_eq(slope, 4.0, 0.01);

      turbo_script_free(ctx);
    }
  }

  describe("Scientific: Math Builtins") {
    it("should support trig, log, exp, rounding") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = "s = sin(0); "
                           "c = cos(0); "
                           "sq = sqrt(144); "
                           "lg = log(e); "
                           "ex = exp(1); "
                           "cl = ceil(2.3); "
                           "fl = floor(2.7); "
                           "rn = round(2.5); "
                           "ab = abs(-42); "
                           "fib = fibonacci(10); "
                           "g = gcd(12, 8);";
      check_int_eq(turbo_script_run(ctx, script), 0);

      check_float_eq(get_num(ctx, "s"), 0.0, 0.001);
      check_float_eq(get_num(ctx, "c"), 1.0, 0.001);
      check_float_eq(get_num(ctx, "sq"), 12.0, 0.001);
      check_float_eq(get_num(ctx, "lg"), 1.0, 0.001);
      check_float_eq(get_num(ctx, "cl"), 3.0, 0.001);
      check_float_eq(get_num(ctx, "fl"), 2.0, 0.001);
      check_float_eq(get_num(ctx, "rn"), 3.0, 1.0); // platform-dependent rounding
      check_float_eq(get_num(ctx, "ab"), 42.0, 0.001);
      check_float_eq(get_num(ctx, "fib"), 55.0, 0.001);
      check_float_eq(get_num(ctx, "g"), 4.0, 0.001);

      turbo_script_free(ctx);
    }
  }

  describe("Scientific: String Builtins") {
    it("should support lower, upper, trim, contains, substr, replace") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = "lo = lower(\"HELLO\"); "
                           "up = upper(\"hello\"); "
                           "tr = trim(\"  hi  \"); "
                           "ct = contains(\"foobar\", \"bar\"); "
                           "sw = starts_with(\"foobar\", \"foo\"); "
                           "ew = ends_with(\"foobar\", \"bar\"); "
                           "ix = index_of(\"foobar\", \"bar\"); "
                           "ss = substr(\"hello world\", 6, 5); "
                           "rp = replace(\"aabbcc\", \"bb\", \"XX\");";
      check_int_eq(turbo_script_run(ctx, script), 0);

      check_float_eq(get_num(ctx, "ct"), 1.0, 0.001);
      check_float_eq(get_num(ctx, "sw"), 1.0, 0.001);
      check_float_eq(get_num(ctx, "ew"), 1.0, 0.001);
      check_float_eq(get_num(ctx, "ix"), 3.0, 0.001);

      turbo_script_free(ctx);
    }
  }

  describe("Scientific: Matrix Operations") {
    it("should compute determinant, inverse, matmul for 2x2") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script =
          // Matrix A = [[1,2],[3,4]], det = 1*4 - 2*3 = -2
          "A = [1, 2, 3, 4]; "
          "d = matrix.det2(A); "
          "Ainv = matrix.inv2(A); "
          "Ainv_len = vec.len(Ainv); "
          // Identity check: A * Ainv should be ~identity
          "I = matrix.matmul(A, Ainv, 2, 2, 2); "
          "i00 = I[0]; "
          "i11 = I[3];";
      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("Matrix Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(get_num(ctx, "d"), -2.0, 0.001);
      check_float_eq(get_num(ctx, "Ainv_len"), 4.0, 0.001);
      check_float_eq(get_num(ctx, "i00"), 1.0, 0.01);
      check_float_eq(get_num(ctx, "i11"), 1.0, 0.01);

      turbo_script_free(ctx);
    }
  }

  describe("Module Import: dot notation") {
    it("should import fs module and use dot notation") {
      turbo_script_ctx_t *ctx = turbo_script_init_bare();
      check_not_null(ctx);

      const char *script = ""
                           "io.write_file(\"test_dot.txt\", \"dot notation works\"); "
                           "data = io.read_file(\"test_dot.txt\"); "
                           "io.file_remove(\"test_dot.txt\"); "
                           "gone = io.file_exists(\"test_dot.txt\");";

      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("FS dot Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      const char *data_str = get_str(ctx, "data");
      check_not_null(data_str);
      check_str_eq(data_str, "dot notation works");
      check_float_eq(get_num(ctx, "gone"), 0.0, 0.1);

      turbo_script_free(ctx);
    }

    it("should import json module and use dot notation") {
      turbo_script_ctx_t *ctx = turbo_script_init_bare();
      check_not_null(ctx);
      check_int_eq(turbo_script_load_plugin(ctx, "feeds"), 0);

      const char *script = ""
                           "js = \"{\\\"name\\\": \\\"alice\\\", \\\"score\\\": 99}\"; "
                           "val = json.query(js, \"score\");";

      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("JSON dot Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(get_num(ctx, "val"), 99.0, 0.1);

      turbo_script_free(ctx);
    }

    it("should still support file import with init_bare") {
      turbo_script_ctx_t *ctx = turbo_script_init_bare();
      check_not_null(ctx);

      turbo_script_run(ctx, ""
                            "write_file(\"utils_dot.ts\", \"func square(x) { return x * x; };\");");

      const char *script = "import(\"utils_dot.ts\"); "
                           "res = square(7);";

      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "res"), 49.0, 0.1);

      turbo_script_run(ctx, "file_remove(\"utils_dot.ts\");");
      turbo_script_free(ctx);
    }
  }

  describe("CSV Module") {
    it("should count rows and columns") {
      turbo_script_ctx_t *ctx = turbo_script_init_bare();
      check_int_eq(turbo_script_load_plugin(ctx, "feeds"), 0);

      const char *script = "var data = "
                           "\"name_s,age_n,city_s\\nAlice,30,NYC\\nBob,25,LA\\nCharlie,35,SF\";"
                           "var r = csv.rows(data);"
                           "var c = csv.cols(data);";

      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("CSV rows/cols Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(get_num(ctx, "r"), 3.0, 0.1); // data rows only (header separated)
      check_float_eq(get_num(ctx, "c"), 3.0, 0.1);

      turbo_script_free(ctx);
    }

    it("should get cell value as string") {
      turbo_script_ctx_t *ctx = turbo_script_init_bare();
      check_int_eq(turbo_script_load_plugin(ctx, "feeds"), 0);

      const char *script = "var data = "
                           "\"name,score\\nAlice,95\\nBob,87\";"
                           "var val = csv.get(data, 0, 0);";

      check_int_eq(turbo_script_run(ctx, script), 0);
      const char *val = get_str(ctx, "val");
      check_not_null(val);
      check_int_eq(strcmp(val, "Alice"), 0);

      turbo_script_free(ctx);
    }

    it("should get cell value as number") {
      turbo_script_ctx_t *ctx = turbo_script_init_bare();
      check_int_eq(turbo_script_load_plugin(ctx, "feeds"), 0);

      const char *script = "var data = "
                           "\"name,score\\nAlice,95\\nBob,87\";"
                           "var val = csv.get_num(data, 1, 1);";

      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "val"), 87.0, 0.1);

      turbo_script_free(ctx);
    }

    it("should extract column as numeric vector by index") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      check_int_eq(turbo_script_load_plugin(ctx, "feeds"), 0);
      check_int_eq(turbo_script_load_plugin(ctx, "vec"), 0);

      const char *script =
          "var data = "
          "\"ticker,close,volume\\nAAPL,150.5,1000\\nGOOG,2800.0,500\\nMSFT,300.25,750\";"
          "var prices = csv.col(data, 1);"
          "var n = vec.len(prices);"
          "var total = vec.sum(prices);";

      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("CSV col Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(get_num(ctx, "n"), 3.0, 0.1);
      check_float_eq(get_num(ctx, "total"), 3250.75, 0.1);

      turbo_script_free(ctx);
    }

    it("should extract column as numeric vector by name") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      check_int_eq(turbo_script_load_plugin(ctx, "feeds"), 0);
      check_int_eq(turbo_script_load_plugin(ctx, "vec"), 0);

      const char *script =
          "var data = "
          "\"ticker,close,volume\\nAAPL,150.5,1000\\nGOOG,2800.0,500\\nMSFT,300.25,750\";"
          "var prices = csv.col(data, \"close\");"
          "var avg_price = vec.avg(prices);";

      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "avg_price"), 1083.583, 0.1);

      turbo_script_free(ctx);
    }

    it("should run csv.col into TA pipeline") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      check_int_eq(turbo_script_load_plugin(ctx, "ta"), 0);
      check_int_eq(turbo_script_load_plugin(ctx, "feeds"), 0);

      const char *script = "var data = \"date,close\\n"
                           "2024-01-01,100\\n2024-01-02,102\\n2024-01-03,104\\n"
                           "2024-01-04,103\\n2024-01-05,105\\n2024-01-06,107\\n"
                           "2024-01-07,109\\n2024-01-08,108\\n2024-01-09,110\\n"
                           "2024-01-10,112\";"
                           "var close = csv.col(data, \"close\");"
                           "var sma3 = ta.sma(close, 3);"
                           "var last = close[9];"
                           "var last_sma = sma3[9];";

      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("CSV TA Pipeline Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(get_num(ctx, "last"), 112.0, 0.1);
      check_float_gt(get_num(ctx, "last_sma"), 0.0);

      turbo_script_free(ctx);
    }

    it("should filter rows and count matches") {
      turbo_script_ctx_t *ctx = turbo_script_init_bare();
      check_int_eq(turbo_script_load_plugin(ctx, "feeds"), 0);

      const char *script = "var data = "
                           "\"name_s,score_n\\nAlice,95\\nBob,60\\nCharlie,85\\nDave,45\";"
                           "var n = csv.filter_count(data, \"score > 70\");";

      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("CSV filter_count Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(get_num(ctx, "n"), 2.0, 0.1);

      turbo_script_free(ctx);
    }

    it("should filter rows and return matching content") {
      turbo_script_ctx_t *ctx = turbo_script_init_bare();
      check_int_eq(turbo_script_load_plugin(ctx, "feeds"), 0);
      check_int_eq(turbo_script_load_plugin(ctx, "vec"), 0);

      const char *script = "var data = "
                           "\"name_s,score_n\\nAlice,95\\nBob,60\\nCharlie,85\";"
                           "var result = csv.filter(data, \"score > 70\");"
                           "var has_data = (vec.len(result) > 0);";

      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("CSV filter Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(get_num(ctx, "has_data"), 1.0, 0.1);

      turbo_script_free(ctx);
    }

    it("should work via import mechanism") {
      turbo_script_ctx_t *ctx = turbo_script_init();

      const char *script = ""
                           "import(\"feeds\");"
                           "import(\"vec\");"
                           "var data = \"x,y\\n1,2\\n3,4\\n5,6\";"
                           "var xs = csv.col(data, \"x\");"
                           "var total = vec.sum(xs);";

      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "total"), 9.0, 0.1);

      turbo_script_free(ctx);
    }
  }

  describe("DSV Filter via Script API") {
    it("should filter rows based on number column") {
      turbo_script_ctx_t *ctx = turbo_script_init_bare();
      check_int_eq(turbo_script_load_plugin(ctx, "feeds"), 0);

      const char *script =
          "var data = "
          "\"name_s,age_n,role_s\\nAlice,30,dev\\nBob,40,manager\\nCharlie,25,intern\";"
          "var result = csv.filter(data, \"age > 30\");"
          "var n = csv.filter_count(data, \"age > 30\");";

      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("DSV filter Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(get_num(ctx, "n"), 1.0, 0.1);

      const char *result = get_str(ctx, "result");
      check_not_null(result);

      turbo_script_free(ctx);
    }

    it("should filter rows based on string column") {
      turbo_script_ctx_t *ctx = turbo_script_init_bare();
      check_int_eq(turbo_script_load_plugin(ctx, "feeds"), 0);

      const char *script = "var data = "
                           "\"name_s,score_n\\nAlice,10\\nBob,5\";"
                           "var result = csv.filter(data, \"name == \\\"Alice\\\"\");"
                           "var n = csv.filter_count(data, \"name == \\\"Alice\\\"\");";

      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("DSV string filter Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(get_num(ctx, "n"), 1.0, 0.1);

      turbo_script_free(ctx);
    }

    it("should count zero matches correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init_bare();
      check_int_eq(turbo_script_load_plugin(ctx, "feeds"), 0);

      const char *script = "var data = \"a_n,b_n\\n1,2\\n3,4\";"
                           "var n = csv.filter_count(data, \"a + b == 99\");";

      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "n"), 0.0, 0.1);

      turbo_script_free(ctx);
    }
  }

  describe("Vector Sort") {
    it("should sort ascending") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      check_int_eq(turbo_script_load_plugin(ctx, "vec"), 0);
      const char *script = "var v = [3, 1, 4, 1, 5, 9, 2, 6];"
                           "var s = vec.sort(v);"
                           "var first = s[0];"
                           "var last = s[7];";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "first"), 1.0, 0.001);
      check_float_eq(get_num(ctx, "last"), 9.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should sort descending") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      check_int_eq(turbo_script_load_plugin(ctx, "vec"), 0);
      const char *script = "var v = [3, 1, 4, 1, 5];"
                           "var s = vec.sort_desc(v);"
                           "var first = s[0];"
                           "var last = s[4];";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "first"), 5.0, 0.001);
      check_float_eq(get_num(ctx, "last"), 1.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should not modify original vector") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      check_int_eq(turbo_script_load_plugin(ctx, "vec"), 0);
      const char *script = "var v = [3, 1, 2];"
                           "var s = vec.sort(v);"
                           "var orig_first = v[0];";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "orig_first"), 3.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should handle single element") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      check_int_eq(turbo_script_load_plugin(ctx, "vec"), 0);
      const char *script = " var v = [42];"
                           "var s = vec.sort(v);"
                           "var val = s[0];";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "val"), 42.0, 0.001);
      turbo_script_free(ctx);
    }
  }

  describe("Vector Unique") {
    it("should remove duplicates and sort") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      check_int_eq(turbo_script_load_plugin(ctx, "vec"), 0);
      const char *script = "var v = [3, 1, 2, 1, 3, 2];"
                           "var u = vec.unique(v);"
                           "var n = vec.len(u);"
                           "var first = u[0];"
                           "var last = u[2];";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "n"), 3.0, 0.001);
      check_float_eq(get_num(ctx, "first"), 1.0, 0.001);
      check_float_eq(get_num(ctx, "last"), 3.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should handle all same values") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      check_int_eq(turbo_script_load_plugin(ctx, "vec"), 0);
      const char *script = "var v = [5, 5, 5, 5];"
                           "var u = vec.unique(v);"
                           "var n = vec.len(u);";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "n"), 1.0, 0.001);
      turbo_script_free(ctx);
    }
  }

  describe("String Join") {
    it("should join vector with delimiter") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = "var v = [1, 2, 3];"
                           "var s = join(v, \",\");";
      check_int_eq(turbo_script_run(ctx, script), 0);
      const char *result = get_str(ctx, "s");
      check_not_null(result);
      check_str_eq(result, "1,2,3");
      turbo_script_free(ctx);
    }

    it("should handle single element") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = "var v = [42];"
                           "var s = join(v, \"-\");";
      check_int_eq(turbo_script_run(ctx, script), 0);
      const char *result = get_str(ctx, "s");
      check_not_null(result);
      check_str_eq(result, "42");
      turbo_script_free(ctx);
    }
  }

  describe("CSV Write") {
    it("should write and read back CSV file") {
      turbo_script_ctx_t *ctx = turbo_script_init_bare();
      turbo_script_load_plugin(ctx, "feeds");

      int r1 = turbo_script_run(ctx, "var data = \"name,age,role\\nAlice,30,dev\\nBob,40,mgr\";"
                                     "var res = csv.write(\"_test_csv_write.csv\", data);"
                                     "var content = read_file(\"_test_csv_write.csv\");"
                                     "var rows = csv.rows(data);"
                                     "file_remove(\"_test_csv_write.csv\");");
      if (r1 != 0)
        printf("CSV write Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(r1, 0);
      check_float_eq(get_num(ctx, "res"), 0.0, 0.1);
      check_float_eq(get_num(ctx, "rows"), 2.0, 0.1);
      const char *content = get_str(ctx, "content");
      check_not_null(content);

      turbo_script_free(ctx);
    }
  }

  describe("Vector Reverse") {
    it("should reverse a vector") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      check_int_eq(turbo_script_load_plugin(ctx, "vec"), 0);
      const char *script = " var v = [1, 2, 3, 4, 5];"
                           "var r = vec.reverse(v);"
                           "var first = r[0];"
                           "var last = r[4];";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "first"), 5.0, 0.001);
      check_float_eq(get_num(ctx, "last"), 1.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should handle single element") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      check_int_eq(turbo_script_load_plugin(ctx, "vec"), 0);
      const char *script = " var v = [42];"
                           "var r = vec.reverse(v);"
                           "var val = r[0];";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "val"), 42.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should not modify original vector") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      check_int_eq(turbo_script_load_plugin(ctx, "vec"), 0);
      const char *script = " var v = [1, 2, 3];"
                           "var r = vec.reverse(v);"
                           "var orig = v[0];";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "orig"), 1.0, 0.001);
      turbo_script_free(ctx);
    }
  }

  describe("Vector Concat") {
    it("should merge two vectors") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      check_int_eq(turbo_script_load_plugin(ctx, "vec"), 0);
      const char *script = " var a = [1, 2, 3];"
                           "var b = [4, 5];"
                           "var c = vec.concat(a, b);"
                           "var n = vec.len(c);"
                           "var last = c[4];";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "n"), 5.0, 0.001);
      check_float_eq(get_num(ctx, "last"), 5.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should handle one empty vector") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      check_int_eq(turbo_script_load_plugin(ctx, "vec"), 0);
      bind_vec(ctx, "a", (const double[]){1.0, 2.0}, 2);
      const char *script = " var b = vec.range(0);"
                           "var c = vec.concat(a, b);"
                           "var n = vec.len(c);";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "n"), 2.0, 0.001);
      turbo_script_free(ctx);
    }
  }

  describe("Vector Range") {
    it("should generate vec.range(5)") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      check_int_eq(turbo_script_load_plugin(ctx, "vec"), 0);
      const char *script = " var v = vec.range(5);"
                           "var n = vec.len(v);"
                           "var first = v[0];"
                           "var last = v[4];";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "n"), 5.0, 0.001);
      check_float_eq(get_num(ctx, "first"), 0.0, 0.001);
      check_float_eq(get_num(ctx, "last"), 4.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should generate vec.range(2, 5)") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      check_int_eq(turbo_script_load_plugin(ctx, "vec"), 0);
      const char *script = " var v = vec.range(2, 5);"
                           "var n = vec.len(v);"
                           "var first = v[0];"
                           "var last = v[2];";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "n"), 3.0, 0.001);
      check_float_eq(get_num(ctx, "first"), 2.0, 0.001);
      check_float_eq(get_num(ctx, "last"), 4.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should return empty for vec.range(0)") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      check_int_eq(turbo_script_load_plugin(ctx, "vec"), 0);
      const char *script = "var v = vec.range(0);"
                           "var n = vec.len(v);";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "n"), 0.0, 0.001);
      turbo_script_free(ctx);
    }
  }

  describe("Vector Cumsum") {
    it("should compute cumulative sum") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = "var v = [1, 2, 3];"
                           "var cs = vec.cumsum(v);"
                           "var a = cs[0];"
                           "var b = cs[1];"
                           "var c = cs[2];";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "a"), 1.0, 0.001);
      check_float_eq(get_num(ctx, "b"), 3.0, 0.001);
      check_float_eq(get_num(ctx, "c"), 6.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should handle single element") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = "var v = [42];"
                           "var cs = vec.cumsum(v);"
                           "var val = cs[0];";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "val"), 42.0, 0.001);
      turbo_script_free(ctx);
    }
  }

  describe("Vector Diff") {
    it("should compute first-order differences") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      check_int_eq(turbo_script_load_plugin(ctx, "vec"), 0);
      const char *script = " var v = [1, 3, 6];"
                           "var d = vec.diff(v);"
                           "var n = vec.len(d);"
                           "var a = d[0];"
                           "var b = d[1];";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "n"), 2.0, 0.001);
      check_float_eq(get_num(ctx, "a"), 2.0, 0.001);
      check_float_eq(get_num(ctx, "b"), 3.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should return empty for single element") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      check_int_eq(turbo_script_load_plugin(ctx, "vec"), 0);
      const char *script = " var v = [42];"
                           "var d = vec.diff(v);"
                           "var n = vec.len(d);";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "n"), 0.0, 0.001);
      turbo_script_free(ctx);
    }
  }

  describe("Vector Find") {
    it("should return index when found") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      check_int_eq(turbo_script_load_plugin(ctx, "vec"), 0);
      const char *script = " var v = [10, 20, 30, 40];"
                           "var idx = vec.find(v, 30);";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "idx"), 2.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should return -1 when not found") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      check_int_eq(turbo_script_load_plugin(ctx, "vec"), 0);
      const char *script = " var v = [10, 20, 30];"
                           "var idx = vec.find(v, 99);";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "idx"), -1.0, 0.001);
      turbo_script_free(ctx);
    }
  }

  describe("String Format") {
    it("should format with %%s and %%d") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = "var s = "
                           "format(\"%s is %d\", \"age\", 30);";
      check_int_eq(turbo_script_run(ctx, script), 0);
      const char *val = get_str(ctx, "s");
      check_not_null(val);
      check_str_eq(val, "age is 30");
      turbo_script_free(ctx);
    }

    it("should format with %%g") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = "var s = format(\"pi=%g\", 3.14);";
      check_int_eq(turbo_script_run(ctx, script), 0);
      const char *val = get_str(ctx, "s");
      check_not_null(val);
      check_str_eq(val, "pi=3.14");
      turbo_script_free(ctx);
    }

    it("should escape %% as literal percent") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = "var s = format(\"%d%%\", 100);";
      check_int_eq(turbo_script_run(ctx, script), 0);
      const char *val = get_str(ctx, "s");
      check_not_null(val);
      check_str_eq(val, "100%");
      turbo_script_free(ctx);
    }
  }

  describe("Print") {
    it("should not crash and return 0") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = "var r = "
                           "print(\"hello\", 42, [1, 2, 3]);";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "r"), 0.0, 0.001);
      turbo_script_free(ctx);
    }
  }

  describe("IO Module: File Operations") {
    it("should append_file content") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = ""
                           "write_file(\"_test_append.txt\", \"hello\"); "
                           "var r = append_file(\"_test_append.txt\", \" world\"); "
                           "var data = read_file(\"_test_append.txt\"); "
                           "file_remove(\"_test_append.txt\");";
      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("append_file Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(get_num(ctx, "r"), 0.0, 0.1);
      const char *data = get_str(ctx, "data");
      check_not_null(data);
      check_str_eq(data, "hello world");
      turbo_script_free(ctx);
    }

    it("should get file_size") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = ""
                           "write_file(\"_test_size.txt\", \"12345\"); "
                           "var sz = file_size(\"_test_size.txt\"); "
                           "file_remove(\"_test_size.txt\");";
      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("file_size Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(get_num(ctx, "sz"), 5.0, 0.1);
      turbo_script_free(ctx);
    }

    it("should return -1 for file_size on missing file") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = ""
                           "var sz = file_size(\"_nonexistent_xyz.txt\");";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(get_num(ctx, "sz"), -1.0, 0.1);
      turbo_script_free(ctx);
    }

    it("should get file_stat vector [size, mtime, is_file, is_dir]") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = ""
                           "write_file(\"_test_stat.txt\", \"abc\"); "
                           "var st = file_stat(\"_test_stat.txt\"); "
                           "var n = vec.len(st); "
                           "var sz = st[0]; "
                           "var isf = st[2]; "
                           "var isd = st[3]; "
                           "file_remove(\"_test_stat.txt\");";
      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("file_stat Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(get_num(ctx, "n"), 4.0, 0.1);
      check_float_eq(get_num(ctx, "sz"), 3.0, 0.1);
      check_float_eq(get_num(ctx, "isf"), 1.0, 0.1);
      check_float_eq(get_num(ctx, "isd"), 0.0, 0.1);
      turbo_script_free(ctx);
    }

    it("should check is_file and is_dir") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = ""
                           "write_file(\"_test_isfile.txt\", \"x\"); "
                           "var f = is_file(\"_test_isfile.txt\"); "
                           "var d = is_dir(\"_test_isfile.txt\"); "
                           "file_remove(\"_test_isfile.txt\");";
      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("is_file/is_dir Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(get_num(ctx, "f"), 1.0, 0.1);
      check_float_eq(get_num(ctx, "d"), 0.0, 0.1);
      turbo_script_free(ctx);
    }

    it("should rename a file") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = ""
                           "write_file(\"_test_rename_a.txt\", \"renamed\"); "
                           "var r = file_rename(\"_test_rename_a.txt\", \"_test_rename_b.txt\"); "
                           "var data = read_file(\"_test_rename_b.txt\"); "
                           "file_remove(\"_test_rename_b.txt\");";
      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("file_rename Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(get_num(ctx, "r"), 0.0, 0.1);
      const char *data = get_str(ctx, "data");
      check_not_null(data);
      check_str_eq(data, "renamed");
      turbo_script_free(ctx);
    }
  }

  describe("IO Module: Directory Operations") {
    it("should mkdir and rmdir") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = ""
                           "var r1 = mkdir(\"_test_dir_io\"); "
                           "var exists = is_dir(\"_test_dir_io\"); "
                           "var r2 = rmdir(\"_test_dir_io\"); "
                           "var gone = is_dir(\"_test_dir_io\");";
      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("mkdir/rmdir Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(get_num(ctx, "r1"), 0.0, 0.1);
      check_float_eq(get_num(ctx, "exists"), 1.0, 0.1);
      check_float_eq(get_num(ctx, "r2"), 0.0, 0.1);
      check_float_eq(get_num(ctx, "gone"), 0.0, 0.1);
      turbo_script_free(ctx);
    }

    it("should return tmpdir as a string") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = ""
                           "var td = tmpdir(); "
                           "var exists = is_dir(td);";
      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("tmpdir Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      const char *td = get_str(ctx, "td");
      check_not_null(td);
      check_float_eq(get_num(ctx, "exists"), 1.0, 0.1);
      turbo_script_free(ctx);
    }
  }

  describe("IO Module: Path Utilities") {
    it("should join paths") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = ""
                           "var p = path_join(\"foo\", \"bar.txt\");";
      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("path_join Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      const char *p = get_str(ctx, "p");
      check_not_null(p);
      printf("  path_join result: %s\n", p);
      turbo_script_free(ctx);
    }

    it("should extract dirname and basename") {
      turbo_script_ctx_t *ctx = turbo_script_init();
#ifdef _WIN32
      const char *script = ""
                           "var d = path_dirname(\"C:\\\\foo\\\\bar.txt\"); "
                           "var b = path_basename(\"C:\\\\foo\\\\bar.txt\");";
#else
      const char *script = ""
                           "var d = path_dirname(\"/foo/bar.txt\"); "
                           "var b = path_basename(\"/foo/bar.txt\");";
#endif
      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("dirname/basename Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      const char *d = get_str(ctx, "d");
      const char *b = get_str(ctx, "b");
      check_not_null(d);
      check_not_null(b);
      printf("  dirname: %s, basename: %s\n", d, b);
      check_str_eq(b, "bar.txt");
      turbo_script_free(ctx);
    }

    it("should detect absolute vs relative paths") {
      turbo_script_ctx_t *ctx = turbo_script_init();
#ifdef _WIN32
      const char *script = ""
                           "var a = path_is_absolute(\"C:\\\\foo\"); "
                           "var r = path_is_absolute(\"foo\\\\bar\");";
#else
      const char *script = ""
                           "var a = path_is_absolute(\"/foo\"); "
                           "var r = path_is_absolute(\"foo/bar\");";
#endif
      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("path_is_absolute Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(get_num(ctx, "a"), 1.0, 0.1);
      check_float_eq(get_num(ctx, "r"), 0.0, 0.1);
      turbo_script_free(ctx);
    }
  }

  describe("IO Module: Date/Time") {
    it("should return now() as a positive timestamp") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = ""
                           "var t = now();";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_gt(get_num(ctx, "t"), 1700000000.0);
      turbo_script_free(ctx);
    }

    it("should parse date string to timestamp") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = ""
                           "var t = date(\"2024-01-01 00:00:00\");";
      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("date Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_gt(get_num(ctx, "t"), 0.0);
      turbo_script_free(ctx);
    }

    it("should format_date with default and custom format") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = ""
                           "var t = now(); "
                           "var s1 = format_date(t); "
                           "var s2 = format_date(t, \"%Y-%m-%d\");";
      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("format_date Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      const char *s1 = get_str(ctx, "s1");
      const char *s2 = get_str(ctx, "s2");
      check_not_null(s1);
      check_not_null(s2);
      printf("  format_date default: %s\n", s1);
      printf("  format_date custom:  %s\n", s2);
      turbo_script_free(ctx);
    }
  }

  describe("IO Module: Platform Info") {
    it("should return os_name as a known string") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = ""
                           "var os = os_name();";
      check_int_eq(turbo_script_run(ctx, script), 0);
      const char *os = get_str(ctx, "os");
      check_not_null(os);
      printf("  os_name: %s\n", os);
      turbo_script_free(ctx);
    }

    it("should return pid as a positive number") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = ""
                           "var p = pid();";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_gt(get_num(ctx, "p"), 0.0);
      turbo_script_free(ctx);
    }

    it("should return uptime_ms and monotonic_ms as non-negative") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = ""
                           "var u = uptime_ms(); "
                           "var m = monotonic_ms();";
      check_int_eq(turbo_script_run(ctx, script), 0);
      double u = get_num(ctx, "u");
      double m = get_num(ctx, "m");
      printf("  uptime_ms: %g, monotonic_ms: %g\n", u, m);
      check_float_eq(u >= 0.0, 1.0, 0.1);
      check_float_eq(m >= 0.0, 1.0, 0.1);
      turbo_script_free(ctx);
    }
  }

  describe("IO Module: End-to-End Pipeline") {
    it("should write, append, stat, read, rename, remove in sequence") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = ""
                           "write_file(\"_test_e2e.txt\", \"line1\\n\"); "
                           "append_file(\"_test_e2e.txt\", \"line2\\n\"); "
                           "var sz = file_size(\"_test_e2e.txt\"); "
                           "var st = file_stat(\"_test_e2e.txt\"); "
                           "var st_sz = st[0]; "
                           "file_rename(\"_test_e2e.txt\", \"_test_e2e_renamed.txt\"); "
                           "var data = read_file(\"_test_e2e_renamed.txt\"); "
                           "file_remove(\"_test_e2e_renamed.txt\"); "
                           "var gone = file_exists(\"_test_e2e_renamed.txt\");";
      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("E2E IO Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_gt(get_num(ctx, "sz"), 0.0);
      check_float_eq(get_num(ctx, "sz"), get_num(ctx, "st_sz"), 0.1);
      check_float_eq(get_num(ctx, "gone"), 0.0, 0.1);
      const char *data = get_str(ctx, "data");
      check_not_null(data);
      printf("  E2E data: %s\n", data);
      turbo_script_free(ctx);
    }

    it("should use tmpdir + path_join for temp file workflow") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = ""
                           "var td = tmpdir(); "
                           "var p = path_join(td, \"_test_tmpfile.txt\"); "
                           "var wr = write_file(p, \"temp data\"); "
                           "var data = read_file(p); "
                           "file_remove(p);";
      int res = turbo_script_run(ctx, script);
      if (res != 0)
        printf("tmpdir pipeline Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      const char *td = get_str(ctx, "td");
      const char *p = get_str(ctx, "p");
      printf("  tmpdir: %s\n", td ? td : "(null)");
      printf("  path:   %s\n", p ? p : "(null)");
      check_float_eq(get_num(ctx, "wr"), 0.0, 0.1);
      const char *data = get_str(ctx, "data");
      check_not_null(data);
      check_str_eq(data, "temp data");
      turbo_script_free(ctx);
    }
  }
}

