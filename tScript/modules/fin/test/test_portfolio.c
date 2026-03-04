/**
 * @file test_portfolio.c
 * @brief Unit tests for portfolio optimization functions.
 */

#include "fin.h"
#include "tinytest.h"
#include <math.h>
#include <string.h>

#define EPSILON 0.001

suite("Portfolio Optimization") {
  group("Covariance Matrix") {
    it("should calculate covariance matrix") {
      // 3 assets, 5 periods
      double returns[] = {
        0.01, 0.02, -0.01, 0.03, 0.01,  // Asset 1
        0.02, 0.01,  0.00, 0.02, 0.02,  // Asset 2
       -0.01, 0.03,  0.02, 0.01, 0.00   // Asset 3
      };
      double cov[9];
      turbo_pool_t arena;
      turbo_pool_init(&arena, 4096);

      exprtk_pf_cov_matrix(returns, 3, 5, cov, &arena);

      // Diagonal elements (variances) should be positive
      check(cov[0] > 0.0); // Var(Asset 1)
      check(cov[4] > 0.0); // Var(Asset 2)
      check(cov[8] > 0.0); // Var(Asset 3)

      // Matrix should be symmetric
      check_float_eq(cov[1], cov[3], EPSILON);
      check_float_eq(cov[2], cov[6], EPSILON);
      check_float_eq(cov[5], cov[7], EPSILON);

      turbo_pool_free(&arena);
    }

    it("should handle single asset") {
      double returns[] = {0.01, 0.02, -0.01, 0.03};
      double cov[1];
      turbo_pool_t arena;
      turbo_pool_init(&arena, 1024);

      exprtk_pf_cov_matrix(returns, 1, 4, cov, &arena);

      check(cov[0] > 0.0); // Variance should be positive

      turbo_pool_free(&arena);
    }
  }

  group("Minimum Variance Portfolio") {
    it("should find minimum variance weights") {
      // Simple 2-asset case
      double cov[] = {
        0.04, 0.01,
        0.01, 0.09
      };
      double weights[2];
      turbo_pool_t arena;
      turbo_pool_init(&arena, 4096);

      double variance = exprtk_pf_min_variance(cov, 2, weights, &arena);

      // Weights should sum to 1
      double sum = weights[0] + weights[1];
      check_float_eq(sum, 1.0, EPSILON);

      // Weights should be positive (or close to it)
      check(weights[0] >= -EPSILON);
      check(weights[1] >= -EPSILON);

      // Variance should be positive
      check(variance >= 0.0);

      turbo_pool_free(&arena);
    }

    it("should handle equal variance case") {
      double cov[] = {
        0.04, 0.00,
        0.00, 0.04
      };
      double weights[2];
      turbo_pool_t arena;
      turbo_pool_init(&arena, 4096);

      exprtk_pf_min_variance(cov, 2, weights, &arena);

      // With equal variance and zero correlation, should be ~50/50
      check_float_eq(weights[0], 0.5, 0.1);
      check_float_eq(weights[1], 0.5, 0.1);

      turbo_pool_free(&arena);
    }

    it("should handle 3 assets") {
      double cov[] = {
        0.04, 0.01, 0.00,
        0.01, 0.09, 0.02,
        0.00, 0.02, 0.16
      };
      double weights[3];
      turbo_pool_t arena;
      turbo_pool_init(&arena, 4096);

      double variance = exprtk_pf_min_variance(cov, 3, weights, &arena);

      // Weights should sum to 1
      double sum = weights[0] + weights[1] + weights[2];
      check_float_eq(sum, 1.0, EPSILON);

      // Variance should be positive
      check(variance >= 0.0);

      turbo_pool_free(&arena);
    }
  }

  group("Maximum Sharpe Ratio") {
    it("should find max Sharpe weights") {
      double mu[] = {0.10, 0.15};  // Expected returns
      double cov[] = {
        0.04, 0.01,
        0.01, 0.09
      };
      double rf = 0.02;  // Risk-free rate
      double weights[2];
      turbo_pool_t arena;
      turbo_pool_init(&arena, 4096);

      double sharpe = exprtk_pf_max_sharpe(mu, cov, 2, rf, weights, &arena);

      // Weights should sum to 1
      double sum = weights[0] + weights[1];
      check_float_eq(sum, 1.0, EPSILON);

      // Sharpe ratio should be positive
      check(sharpe >= 0.0);

      // Weights should be reasonable (not all in one asset)
      check(weights[0] >= 0.0 && weights[0] <= 1.0);
      check(weights[1] >= 0.0 && weights[1] <= 1.0);

      turbo_pool_free(&arena);
    }

    it("should handle zero risk-free rate") {
      double mu[] = {0.08, 0.12, 0.10};
      double cov[] = {
        0.04, 0.01, 0.00,
        0.01, 0.09, 0.02,
        0.00, 0.02, 0.16
      };
      double weights[3];
      turbo_pool_t arena;
      turbo_pool_init(&arena, 4096);

      double sharpe = exprtk_pf_max_sharpe(mu, cov, 3, 0.0, weights, &arena);

      // Weights should sum to 1
      double sum = weights[0] + weights[1] + weights[2];
      check_float_eq(sum, 1.0, EPSILON);

      // Sharpe should be positive
      check(sharpe >= 0.0);

      turbo_pool_free(&arena);
    }
  }

  group("Markowitz Optimization") {
    it("should find weights for target return") {
      double mu[] = {0.08, 0.12};
      double cov[] = {
        0.04, 0.01,
        0.01, 0.09
      };
      double target = 0.10;  // Target 10% return
      double weights[2];
      turbo_pool_t arena;
      turbo_pool_init(&arena, 4096);

      double variance = exprtk_pf_markowitz(mu, cov, 2, target, weights, &arena);

      // Weights should sum to 1
      double sum = weights[0] + weights[1];
      check_float_eq(sum, 1.0, EPSILON);

      // Portfolio return should be close to target
      double port_ret = weights[0] * mu[0] + weights[1] * mu[1];
      check_float_eq(port_ret, target, 0.05);

      // Variance should be positive
      check(variance >= 0.0);

      turbo_pool_free(&arena);
    }

    it("should handle target at boundary") {
      double mu[] = {0.05, 0.10, 0.15};
      double cov[] = {
        0.04, 0.01, 0.00,
        0.01, 0.09, 0.02,
        0.00, 0.02, 0.16
      };
      double weights[3];
      turbo_pool_t arena;
      turbo_pool_init(&arena, 4096);

      // Target at minimum
      exprtk_pf_markowitz(mu, cov, 3, 0.05, weights, &arena);
      double sum = weights[0] + weights[1] + weights[2];
      check_float_eq(sum, 1.0, EPSILON);

      // Target at maximum
      exprtk_pf_markowitz(mu, cov, 3, 0.15, weights, &arena);
      sum = weights[0] + weights[1] + weights[2];
      check_float_eq(sum, 1.0, EPSILON);

      turbo_pool_free(&arena);
    }
  }

  group("Edge Cases") {
    it("should handle single asset portfolio") {
      double mu[] = {0.10};
      double cov[] = {0.04};
      double weights[1];
      turbo_pool_t arena;
      turbo_pool_init(&arena, 1024);

      // Min variance
      exprtk_pf_min_variance(cov, 1, weights, &arena);
      check_float_eq(weights[0], 1.0, EPSILON);

      // Max Sharpe
      exprtk_pf_max_sharpe(mu, cov, 1, 0.02, weights, &arena);
      check_float_eq(weights[0], 1.0, EPSILON);

      // Markowitz
      exprtk_pf_markowitz(mu, cov, 1, 0.10, weights, &arena);
      check_float_eq(weights[0], 1.0, EPSILON);

      turbo_pool_free(&arena);
    }

    it("should handle perfectly correlated assets") {
      double cov[] = {
        0.04, 0.04,
        0.04, 0.04
      };
      double weights[2];
      turbo_pool_t arena;
      turbo_pool_init(&arena, 4096);

      exprtk_pf_min_variance(cov, 2, weights, &arena);

      // Should still return valid weights
      double sum = weights[0] + weights[1];
      check_float_eq(sum, 1.0, 0.1);

      turbo_pool_free(&arena);
    }
  }
}
 