/**
 * @file test_backtest_risk.c
 * @brief Unit tests for backtest and risk management functions.
 */

#include "fin.h"
#include "tinytest.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>


#define EPSILON 0.001

suite("Backtest and Risk") {
  group("Simple Backtest") {
    it("should backtest a simple long strategy") {
      double open[] = {100, 101, 102, 103, 104};
      double close[] = {101, 102, 103, 104, 105};
      double signal[] = {1, 1, 1, 1, 0}; // Long then flat
      size_t n = 5;
      double equity[5];
      double trades[5];

      size_t num_trades =
          exprtk_bt_backtest(open, close, signal, n, 10000.0, 0.001, equity, trades);

      check(num_trades > 0);
      check(equity[0] > 0.0);
      check(equity[n - 1] > 0.0);
      // Should make profit in uptrend
      check(equity[n - 1] > equity[0]);
    }

    it("should handle flat signal (no trading)") {
      double open[] = {100, 101, 102};
      double close[] = {101, 102, 103};
      double signal[] = {0, 0, 0};
      size_t n = 3;
      double equity[3];
      double trades[3];

      size_t num_trades =
          exprtk_bt_backtest(open, close, signal, n, 10000.0, 0.001, equity, trades);

      check_int_eq(num_trades, 0);
      // Equity should remain constant
      check_float_eq(equity[0], 10000.0, EPSILON);
      check_float_eq(equity[n - 1], 10000.0, EPSILON);
    }

    it("should handle signal changes") {
      double open[] = {100, 101, 102, 103, 104};
      double close[] = {101, 102, 103, 104, 105};
      double signal[] = {1, 1, -1, -1, 0}; // Long, then short, then flat
      size_t n = 5;
      double equity[5];
      double trades[5];

      size_t num_trades =
          exprtk_bt_backtest(open, close, signal, n, 10000.0, 0.001, equity, trades);

      check(num_trades >= 2); // At least 2 trades (long->short, short->flat)
    }
  }

  group("Backtest Statistics") {
    it("should calculate backtest stats") {
      double equity[] = {10000, 10500, 11000, 10800, 11500, 12000};
      double trades[] = {0, 500, 500, -200, 700, 500};
      size_t n = 6;
      double stats[10];

      size_t count = exprtk_bt_stats(equity, trades, n, 5, 252.0, stats);

      check(count >= 4);
      // Total return should be positive
      check(stats[0] > 0.0);
      // Annualized return
      check(stats[1] != 0.0);
      // Sharpe ratio
      check(stats[2] == stats[2]); // Not NaN
      // Max drawdown should be negative
      check(stats[3] <= 0.0);
    }

    it("should calculate trade statistics") {
      double equity[] = {10000, 10500, 10300, 10800, 10600};
      double trades[] = {0, 500, -200, 500, -200};
      size_t n = 5;
      double stats[10];

      size_t count = exprtk_bt_stats(equity, trades, n, 4, 252.0, stats);

      check_int_eq(count, 10);
      // Win rate should be 0.5 (2 wins, 2 losses)
      check_float_eq(stats[4], 0.5, 0.1);
      // Profit factor should be > 1 (more wins than losses)
      check(stats[5] > 1.0);
    }

    it("should handle no trades") {
      double equity[] = {10000, 10000, 10000};
      double trades[] = {0, 0, 0};
      size_t n = 3;
      double stats[10];

      size_t count = exprtk_bt_stats(equity, trades, n, 0, 252.0, stats);

      check(count >= 4);
      // Total return should be 0
      check_float_eq(stats[0], 0.0, EPSILON);
    }
  }

  group("Trading Costs") {
    it("should calculate slippage") {
      double price = 100.0;
      double size = 1000.0;
      double vol = 10000.0;
      double avg_vol = 10000.0;
      double lambda = 0.1;

      double slippage = exprtk_bt_slippage(price, size, vol, avg_vol, lambda);

      check(slippage >= 0.0);
      // Larger orders should have more slippage
      double large_slip = exprtk_bt_slippage(price, size * 2, vol, avg_vol, lambda);
      check(large_slip > slippage);
    }

    it("should calculate total cost") {
      double price = 100.0;
      double size = 100.0;
      double commission = 0.001; // 0.1%
      double tax = 0.001;        // 0.1%
      double slippage = 0.05;    // $0.05 per share

      double cost = exprtk_bt_cost(price, size, commission, tax, slippage);

      check(cost > 0.0);
      // Cost should include commission + tax + slippage
      double expected = 100 * 100 * 0.001 + 100 * 100 * 0.001 + 100 * 0.05;
      check_float_eq(cost, expected, 1.0);
    }
  }

  group("Position Sizing") {
    it("should calculate fixed fractional size") {
      double equity = 10000.0;
      double risk_pct = 0.02;  // Risk 2%
      double stop_dist = 0.05; // 5% stop

      double size = exprtk_fixed_frac(equity, risk_pct, stop_dist);

      check(size > 0.0);
      check(size <= equity);
      // Size should be 200 / 0.05 = 4000
      check_float_eq(size, 4000.0, 100.0);
    }

    it("should cap size at equity") {
      double equity = 10000.0;
      double risk_pct = 0.5;   // Risk 50%
      double stop_dist = 0.01; // 1% stop (very tight)

      double size = exprtk_fixed_frac(equity, risk_pct, stop_dist);

      // Should cap at equity
      check_float_eq(size, equity, EPSILON);
    }

    it("should handle zero stop distance") {
      double equity = 10000.0;
      double risk_pct = 0.02;
      double stop_dist = 0.0;

      double size = exprtk_fixed_frac(equity, risk_pct, stop_dist);

      check_float_eq(size, 0.0, EPSILON);
    }
  }

  group("Optimal F") {
    it("should calculate optimal f for winning trades") {
      double trades[] = {100, -50, 150, -30, 80, -40, 120, -20};
      size_t n = 8;
      double out[2];

      double opt_f = exprtk_optimal_f(trades, n, out);

      check(opt_f > 0.0);
      check(opt_f <= 1.0);
      check(out[0] == opt_f);
      check(out[1] > 1.0); // TWR should be > 1 for profitable system
    }

    it("should handle all winning trades") {
      double trades[] = {100, 150, 80, 120};
      size_t n = 4;
      double out[2];

      double opt_f = exprtk_optimal_f(trades, n, out);

      // No losses - can't calculate optimal f
      check_float_eq(opt_f, 0.0, EPSILON);
    }

    it("should handle losing system") {
      double trades[] = {-100, 50, -150, 30, -80};
      size_t n = 5;
      double out[2];

      double opt_f = exprtk_optimal_f(trades, n, out);

      // Should return a value (might be small)
      check(opt_f >= 0.0);
      check(opt_f <= 1.0);
    }
  }

  group("Monte Carlo Simulation") {
    it("should simulate price paths") {
      double s0 = 100.0;
      double mu = 0.10;        // 10% drift
      double sigma = 0.20;     // 20% volatility
      double dt = 1.0 / 252.0; // Daily
      size_t steps = 10;
      size_t paths = 5;
      double out[50]; // 5 paths × 10 steps
      turbo_pool_t arena;
      turbo_pool_init(&arena, 4096);

      srand(42); // Fixed seed for reproducibility
      size_t result = exprtk_mc_simulate(s0, mu, sigma, dt, steps, paths, out, &arena);

      check_int_eq(result, paths);
      // All paths should start at s0
      for (size_t p = 0; p < paths; p++) {
        check_float_eq(out[p * steps], s0, EPSILON);
      }
      // Prices should be positive
      for (size_t i = 0; i < paths * steps; i++) {
        check(out[i] > 0.0);
      }

      turbo_pool_free(&arena);
    }

    it("should generate different paths") {
      double s0 = 100.0;
      double mu = 0.10;
      double sigma = 0.20;
      double dt = 1.0 / 252.0;
      size_t steps = 10;
      size_t paths = 3;
      double out[30];
      turbo_pool_t arena;
      turbo_pool_init(&arena, 4096);

      srand(123);
      exprtk_mc_simulate(s0, mu, sigma, dt, steps, paths, out, &arena);

      // Final prices should be different across paths
      double final1 = out[1 * steps - 1];
      double final2 = out[2 * steps - 1];
      double final3 = out[3 * steps - 1];

      check(fabs(final1 - final2) > 0.1 || fabs(final2 - final3) > 0.1);

      turbo_pool_free(&arena);
    }
  }
}
