/**
 * @file test_universe.c
 * @brief Unit tests for universe management and SIMD-accelerated
 *        cross-sectional operations.
 */

#include "universe.h"
#include "tinytest.h"
#include <math.h>
#include <string.h>
#include <float.h>

#define EPSILON 0.0001

/* =========================================================================
 * Helper: build a small test universe
 * ========================================================================= */

static universe_t *make_test_universe(turbo_pool_t *arena) {
    universe_t *u = universe_create(arena);
    if (!u) return NULL;

    /* 5 assets: AAPL(0), MSFT(1), GOOG(2), AMZN(3), TSLA(4) */
    universe_asset_t assets[] = {
        { .id = 0, .ticker = "AAPL",  .exchange = "NYSE", .asset_type = UNIVERSE_ASSET_EQUITY,
          .start_date = 100, .end_date = UNIVERSE_DATE_NONE, .lot_size = 1, .tick_size = 0.01 },
        { .id = 1, .ticker = "MSFT",  .exchange = "NYSE", .asset_type = UNIVERSE_ASSET_EQUITY,
          .start_date = 100, .end_date = UNIVERSE_DATE_NONE, .lot_size = 1, .tick_size = 0.01 },
        { .id = 2, .ticker = "GOOG",  .exchange = "NYSE", .asset_type = UNIVERSE_ASSET_EQUITY,
          .start_date = 150, .end_date = UNIVERSE_DATE_NONE, .lot_size = 1, .tick_size = 0.01 },
        { .id = 3, .ticker = "AMZN",  .exchange = "NYSE", .asset_type = UNIVERSE_ASSET_EQUITY,
          .start_date = 100, .end_date = 300, .lot_size = 1, .tick_size = 0.01 },
        { .id = 4, .ticker = "TSLA",  .exchange = "NYSE", .asset_type = UNIVERSE_ASSET_EQUITY,
          .start_date = 200, .end_date = UNIVERSE_DATE_NONE, .lot_size = 1, .tick_size = 0.01 },
    };

    for (size_t i = 0; i < 5; i++) {
        universe_add_asset(u, &assets[i]);
    }

    universe_finalize(u);
    return u;
}

/* =========================================================================
 * Tests
 * ========================================================================= */

suite("Universe Management") {

  /* -------------------------------------------------------------------
   * Lifecycle
   * ------------------------------------------------------------------- */
  group("Lifecycle") {
    it("should create and destroy a universe") {
      turbo_pool_t arena;
      turbo_pool_init(&arena, 8192);

      universe_t *u = universe_create(&arena);
      check_not_null(u);
      check_int_eq((int)u->num_assets, 0);

      universe_free(u);
      turbo_pool_free(&arena);
    }

    it("should add assets and finalize") {
      turbo_pool_t arena;
      turbo_pool_init(&arena, 8192);

      universe_t *u = make_test_universe(&arena);
      check_not_null(u);
      check_int_eq((int)u->num_assets, 5);
      check_not_null(u->active_mask);
      check_not_null(u->cum_adj_cache);

      universe_free(u);
      turbo_pool_free(&arena);
    }
  }

  /* -------------------------------------------------------------------
   * Active Mask & Advance
   * ------------------------------------------------------------------- */
  group("Active Mask") {
    it("should activate assets based on start_date") {
      turbo_pool_t arena;
      turbo_pool_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      /* date=120: AAPL(100), MSFT(100), AMZN(100) active; GOOG(150), TSLA(200) not yet */
      universe_advance(u, 120);
      check_int_eq((int)universe_active_count(u), 3);
      check(universe_is_active(u, 0));   /* AAPL */
      check(universe_is_active(u, 1));   /* MSFT */
      check(!universe_is_active(u, 2));  /* GOOG not yet */
      check(universe_is_active(u, 3));   /* AMZN */
      check(!universe_is_active(u, 4));  /* TSLA not yet */

      universe_free(u);
      turbo_pool_free(&arena);
    }

    it("should activate later assets as date progresses") {
      turbo_pool_t arena;
      turbo_pool_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      /* date=200: all 5 assets are active (AMZN end_date=300 > 200) */
      universe_advance(u, 200);
      check_int_eq((int)universe_active_count(u), 5);

      universe_free(u);
      turbo_pool_free(&arena);
    }

    it("should deactivate delisted assets") {
      turbo_pool_t arena;
      turbo_pool_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 250);
      check(universe_is_active(u, 3)); /* AMZN still active (end_date=300 > 250) */

      universe_advance(u, 300);
      check(!universe_is_active(u, 3)); /* AMZN delisted (end_date=300 <= 300) */
      check_int_eq((int)universe_active_count(u), 4); /* 5 minus AMZN */

      universe_free(u);
      turbo_pool_free(&arena);
    }

    it("should report delisted_today on exact delist date") {
      turbo_pool_t arena;
      turbo_pool_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 250);
      universe_advance(u, 300);

      uint32_t delisted[8];
      size_t n = universe_delisted_today(u, delisted, 8);
      check_int_eq((int)n, 1);
      check_int_eq((int)delisted[0], 3); /* AMZN id=3 */

      universe_free(u);
      turbo_pool_free(&arena);
    }

    it("should fill active_ids correctly") {
      turbo_pool_t arena;
      turbo_pool_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 120);
      uint32_t ids[8];
      size_t n = universe_active_ids(u, ids, 8);
      check_int_eq((int)n, 3);
      check_int_eq((int)ids[0], 0); /* AAPL */
      check_int_eq((int)ids[1], 1); /* MSFT */
      check_int_eq((int)ids[2], 3); /* AMZN */

      universe_free(u);
      turbo_pool_free(&arena);
    }
  }

  /* -------------------------------------------------------------------
   * Adjustments
   * ------------------------------------------------------------------- */
  group("Price Adjustments") {
    it("should apply split adjustment factor") {
      turbo_pool_t arena;
      turbo_pool_init(&arena, 8192);
      universe_t *u = universe_create(&arena);

      universe_asset_t asset = {
        .id = 0, .ticker = "AAPL", .exchange = "NYSE",
        .asset_type = UNIVERSE_ASSET_EQUITY,
        .start_date = 100, .end_date = UNIVERSE_DATE_NONE,
        .lot_size = 1, .tick_size = 0.01
      };
      universe_add_asset(u, &asset);

      /* 4:1 split on day 200 */
      universe_adj_t adj = {
        .asset_id = 0, .date = 200,
        .type = UNIVERSE_ADJ_SPLIT, .factor = 4.0
      };
      universe_add_adjustment(u, &adj);

      universe_finalize(u);

      /* Before the split date: factor should be 1/4 = 0.25 */
      universe_advance(u, 250);
      double factor = universe_adj_factor(u, 0);
      check_float_eq(factor, 0.25, EPSILON);

      /* Adjusted price: 400 * 0.25 = 100 */
      double adjusted = universe_adjust_price(u, 0, 400.0);
      check_float_eq(adjusted, 100.0, EPSILON);

      universe_free(u);
      turbo_pool_free(&arena);
    }

    it("should handle multiple splits correctly") {
      turbo_pool_t arena;
      turbo_pool_init(&arena, 8192);
      universe_t *u = universe_create(&arena);

      universe_asset_t asset = {
        .id = 0, .ticker = "TEST", .exchange = "X",
        .asset_type = UNIVERSE_ASSET_EQUITY,
        .start_date = 100, .end_date = UNIVERSE_DATE_NONE,
        .lot_size = 1, .tick_size = 0.01
      };
      universe_add_asset(u, &asset);

      /* 2:1 split on day 200, then 3:1 split on day 300 */
      universe_adj_t adj1 = { .asset_id = 0, .date = 200, .type = UNIVERSE_ADJ_SPLIT, .factor = 2.0 };
      universe_adj_t adj2 = { .asset_id = 0, .date = 300, .type = UNIVERSE_ADJ_SPLIT, .factor = 3.0 };
      universe_add_adjustment(u, &adj1);
      universe_add_adjustment(u, &adj2);

      universe_finalize(u);

      /* After both splits: cum_adj = 1/(2*3) = 1/6 */
      universe_advance(u, 350);
      double factor = universe_adj_factor(u, 0);
      check_float_eq(factor, 1.0 / 6.0, EPSILON);

      universe_free(u);
      turbo_pool_free(&arena);
    }

    it("should bulk-adjust prices with SIMD") {
      turbo_pool_t arena;
      turbo_pool_init(&arena, 8192);
      universe_t *u = universe_create(&arena);

      universe_asset_t assets[] = {
        { .id = 0, .ticker = "A", .exchange = "X", .asset_type = UNIVERSE_ASSET_EQUITY,
          .start_date = 100, .end_date = UNIVERSE_DATE_NONE, .lot_size = 1, .tick_size = 0.01 },
        { .id = 1, .ticker = "B", .exchange = "X", .asset_type = UNIVERSE_ASSET_EQUITY,
          .start_date = 100, .end_date = UNIVERSE_DATE_NONE, .lot_size = 1, .tick_size = 0.01 },
      };
      universe_add_asset(u, &assets[0]);
      universe_add_asset(u, &assets[1]);

      /* 2:1 split for asset 0 */
      universe_adj_t adj = { .asset_id = 0, .date = 200, .type = UNIVERSE_ADJ_SPLIT, .factor = 2.0 };
      universe_add_adjustment(u, &adj);

      universe_finalize(u);
      universe_advance(u, 250);

      double raw[] = { 200.0, 100.0 };
      double adjusted[2];
      universe_adjust_prices(u, raw, adjusted, 2);

      /* Asset 0: 200 * 0.5 = 100, Asset 1: 100 * 1.0 = 100 */
      check_float_eq(adjusted[0], 100.0, EPSILON);
      check_float_eq(adjusted[1], 100.0, EPSILON);

      universe_free(u);
      turbo_pool_free(&arena);
    }
  }

  /* -------------------------------------------------------------------
   * Cross-Sectional: Rank
   * ------------------------------------------------------------------- */
  group("Cross-Sectional Rank") {
    it("should rank active assets correctly") {
      turbo_pool_t arena;
      turbo_pool_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 200); /* All 5 active */

      double values[] = { 50.0, 10.0, 30.0, 40.0, 20.0 };
      double out[5];
      universe_rank(u, values, 5, out);

      /* Sorted order: MSFT(10)=0, TSLA(20)=1, GOOG(30)=2, AMZN(40)=3, AAPL(50)=4 */
      /* Rank = position / (n-1) = position / 4 */
      check_float_eq(out[0], 4.0 / 4.0, EPSILON); /* AAPL=50 → rank 4/4 = 1.0 */
      check_float_eq(out[1], 0.0 / 4.0, EPSILON); /* MSFT=10 → rank 0/4 = 0.0 */
      check_float_eq(out[2], 2.0 / 4.0, EPSILON); /* GOOG=30 → rank 2/4 = 0.5 */
      check_float_eq(out[3], 3.0 / 4.0, EPSILON); /* AMZN=40 → rank 3/4 = 0.75 */
      check_float_eq(out[4], 1.0 / 4.0, EPSILON); /* TSLA=20 → rank 1/4 = 0.25 */

      universe_free(u);
      turbo_pool_free(&arena);
    }

    it("should set inactive asset ranks to zero") {
      turbo_pool_t arena;
      turbo_pool_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 120); /* Only AAPL, MSFT, AMZN active */

      double values[] = { 30.0, 10.0, 99.0, 20.0, 99.0 };
      double out[5];
      universe_rank(u, values, 5, out);

      /* GOOG(idx=2) and TSLA(idx=4) are inactive → 0.0 */
      check_float_eq(out[2], 0.0, EPSILON);
      check_float_eq(out[4], 0.0, EPSILON);

      /* Active: MSFT(10)=rank0, AMZN(20)=rank1, AAPL(30)=rank2 */
      check_float_eq(out[1], 0.0 / 2.0, EPSILON); /* MSFT → 0.0 */
      check_float_eq(out[3], 1.0 / 2.0, EPSILON); /* AMZN → 0.5 */
      check_float_eq(out[0], 2.0 / 2.0, EPSILON); /* AAPL → 1.0 */

      universe_free(u);
      turbo_pool_free(&arena);
    }
  }

  /* -------------------------------------------------------------------
   * Cross-Sectional: Top-N
   * ------------------------------------------------------------------- */
  group("Cross-Sectional Top-N") {
    it("should return top-k asset IDs by value") {
      turbo_pool_t arena;
      turbo_pool_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 200);

      double values[] = { 50.0, 10.0, 30.0, 40.0, 20.0 };
      uint32_t top_ids[3];
      size_t n = universe_top_n(u, values, 5, 3, top_ids);

      check_int_eq((int)n, 3);
      check_int_eq((int)top_ids[0], 0); /* AAPL=50 (highest) */
      check_int_eq((int)top_ids[1], 3); /* AMZN=40 */
      check_int_eq((int)top_ids[2], 2); /* GOOG=30 */

      universe_free(u);
      turbo_pool_free(&arena);
    }

    it("should clamp k to active count") {
      turbo_pool_t arena;
      turbo_pool_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 120); /* 3 active */

      double values[] = { 30.0, 10.0, 0.0, 20.0, 0.0 };
      uint32_t top_ids[10];
      size_t n = universe_top_n(u, values, 5, 10, top_ids);

      check_int_eq((int)n, 3); /* only 3 active */

      universe_free(u);
      turbo_pool_free(&arena);
    }
  }

  /* -------------------------------------------------------------------
   * Cross-Sectional: Filter
   * ------------------------------------------------------------------- */
  group("Cross-Sectional Filter") {
    it("should filter assets above threshold") {
      turbo_pool_t arena;
      turbo_pool_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 200);

      double values[] = { 50.0, 10.0, 30.0, 40.0, 20.0 };
      uint8_t mask[5];
      size_t count = universe_filter_gt(u, values, 5, 25.0, mask);

      check_int_eq((int)count, 3); /* AAPL(50), GOOG(30), AMZN(40) > 25 */
      check_int_eq(mask[0], 1); /* AAPL */
      check_int_eq(mask[1], 0); /* MSFT=10 */
      check_int_eq(mask[2], 1); /* GOOG=30 */
      check_int_eq(mask[3], 1); /* AMZN=40 */
      check_int_eq(mask[4], 0); /* TSLA=20 */

      universe_free(u);
      turbo_pool_free(&arena);
    }
  }

  /* -------------------------------------------------------------------
   * SIMD Cross-Sectional: Z-Score
   * ------------------------------------------------------------------- */
  group("SIMD Cross-Sectional Z-Score") {
    it("should z-score normalize active assets") {
      turbo_pool_t arena;
      turbo_pool_init(&arena, 16384);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 200); /* All 5 active */

      double values[] = { 50.0, 10.0, 30.0, 40.0, 20.0 };
      double out[5];
      universe_zscore(u, values, 5, out);

      /* Mean of actives = (50+10+30+40+20)/5 = 30
       * Var = ((20^2 + 20^2 + 0 + 10^2 + 10^2) / 4) = 250  (sample var)
       * SD = sqrt(250) ≈ 15.811
       * z(AAPL) = (50-30)/15.811 ≈ 1.265
       * z(MSFT) = (10-30)/15.811 ≈ -1.265
       * z(GOOG) = (30-30)/15.811 ≈ 0.0 */
      check_float_eq(out[2], 0.0, 0.01);           /* GOOG is at the mean */
      check(out[0] > 0);                            /* AAPL above mean */
      check(out[1] < 0);                            /* MSFT below mean */
      check_float_eq(out[0], -out[1], 0.01);        /* Symmetric around mean */

      universe_free(u);
      turbo_pool_free(&arena);
    }

    it("should set inactive slots to zero in zscore") {
      turbo_pool_t arena;
      turbo_pool_init(&arena, 16384);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 120); /* AAPL, MSFT, AMZN active */

      double values[] = { 30.0, 10.0, 99.0, 20.0, 99.0 };
      double out[5];
      universe_zscore(u, values, 5, out);

      check_float_eq(out[2], 0.0, EPSILON); /* GOOG inactive */
      check_float_eq(out[4], 0.0, EPSILON); /* TSLA inactive */

      universe_free(u);
      turbo_pool_free(&arena);
    }
  }

  /* -------------------------------------------------------------------
   * SIMD Cross-Sectional: Demean
   * ------------------------------------------------------------------- */
  group("SIMD Cross-Sectional Demean") {
    it("should subtract cross-sectional mean from active assets") {
      turbo_pool_t arena;
      turbo_pool_init(&arena, 16384);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 200);

      double values[] = { 50.0, 10.0, 30.0, 40.0, 20.0 };
      double out[5];
      universe_demean(u, values, 5, out);

      /* Mean = 30.0 */
      check_float_eq(out[0],  20.0, EPSILON); /* 50 - 30 */
      check_float_eq(out[1], -20.0, EPSILON); /* 10 - 30 */
      check_float_eq(out[2],   0.0, EPSILON); /* 30 - 30 */
      check_float_eq(out[3],  10.0, EPSILON); /* 40 - 30 */
      check_float_eq(out[4], -10.0, EPSILON); /* 20 - 30 */

      universe_free(u);
      turbo_pool_free(&arena);
    }

    it("should set inactive slots to zero in demean") {
      turbo_pool_t arena;
      turbo_pool_init(&arena, 16384);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 120);

      double values[] = { 30.0, 10.0, 99.0, 20.0, 99.0 };
      double out[5];
      universe_demean(u, values, 5, out);

      check_float_eq(out[2], 0.0, EPSILON);
      check_float_eq(out[4], 0.0, EPSILON);

      universe_free(u);
      turbo_pool_free(&arena);
    }
  }

  /* -------------------------------------------------------------------
   * SIMD Cross-Sectional: Clip
   * ------------------------------------------------------------------- */
  group("SIMD Cross-Sectional Clip") {
    it("should clamp values to range") {
      turbo_pool_t arena;
      turbo_pool_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 200);

      double values[] = { 50.0, 10.0, 30.0, 40.0, 20.0 };
      double out[5];
      universe_clip(u, values, 5, 15.0, 45.0, out);

      check_float_eq(out[0], 45.0, EPSILON); /* 50 clamped to 45 */
      check_float_eq(out[1], 15.0, EPSILON); /* 10 clamped to 15 */
      check_float_eq(out[2], 30.0, EPSILON); /* 30 unchanged */
      check_float_eq(out[3], 40.0, EPSILON); /* 40 unchanged */
      check_float_eq(out[4], 20.0, EPSILON); /* 20 unchanged */

      universe_free(u);
      turbo_pool_free(&arena);
    }
  }

  /* -------------------------------------------------------------------
   * SIMD Cross-Sectional: Cross Sum
   * ------------------------------------------------------------------- */
  group("SIMD Cross-Sectional Sum") {
    it("should sum only active asset values") {
      turbo_pool_t arena;
      turbo_pool_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 120); /* AAPL, MSFT, AMZN active */

      double values[] = { 30.0, 10.0, 99.0, 20.0, 99.0 };
      double sum = universe_cross_sum(u, values, 5);

      /* Only active: 30 + 10 + 20 = 60 (ignores GOOG and TSLA) */
      check_float_eq(sum, 60.0, EPSILON);

      universe_free(u);
      turbo_pool_free(&arena);
    }

    it("should sum all when all active") {
      turbo_pool_t arena;
      turbo_pool_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 200);

      double values[] = { 50.0, 10.0, 30.0, 40.0, 20.0 };
      double sum = universe_cross_sum(u, values, 5);

      check_float_eq(sum, 150.0, EPSILON);

      universe_free(u);
      turbo_pool_free(&arena);
    }
  }

  /* -------------------------------------------------------------------
   * Lookup Helpers
   * ------------------------------------------------------------------- */
  group("Lookup") {
    it("should find assets by ID") {
      turbo_pool_t arena;
      turbo_pool_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      check_int_eq((int)universe_find_asset(u, 0), 0);
      check_int_eq((int)universe_find_asset(u, 3), 3);
      check(universe_find_asset(u, 99) == SIZE_MAX);

      universe_free(u);
      turbo_pool_free(&arena);
    }

    it("should find assets by ticker") {
      turbo_pool_t arena;
      turbo_pool_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      const universe_asset_t *a = universe_find_by_ticker(u, "GOOG");
      check_not_null(a);
      check_int_eq((int)a->id, 2);

      check(universe_find_by_ticker(u, "NOPE") == NULL);

      universe_free(u);
      turbo_pool_free(&arena);
    }
  }

  /* -------------------------------------------------------------------
   * Edge Cases
   * ------------------------------------------------------------------- */
  group("Edge Cases") {
    it("should handle empty universe") {
      turbo_pool_t arena;
      turbo_pool_init(&arena, 8192);
      universe_t *u = universe_create(&arena);
      universe_finalize(u);

      check_int_eq((int)universe_active_count(u), 0);

      double values[] = { 1.0 };
      double out[1];
      universe_rank(u, values, 0, out); /* n=0 should not crash */

      universe_free(u);
      turbo_pool_free(&arena);
    }

    it("should handle single asset universe") {
      turbo_pool_t arena;
      turbo_pool_init(&arena, 8192);
      universe_t *u = universe_create(&arena);

      universe_asset_t asset = {
        .id = 0, .ticker = "SOLO", .exchange = "X",
        .asset_type = UNIVERSE_ASSET_EQUITY,
        .start_date = 100, .end_date = UNIVERSE_DATE_NONE,
        .lot_size = 1, .tick_size = 0.01
      };
      universe_add_asset(u, &asset);
      universe_finalize(u);

      universe_advance(u, 200);
      check_int_eq((int)universe_active_count(u), 1);

      double values[] = { 42.0 };
      double out[1];
      universe_rank(u, values, 1, out);
      check_float_eq(out[0], 0.0, EPSILON); /* single asset rank = 0/1 = 0 */

      double sum = universe_cross_sum(u, values, 1);
      check_float_eq(sum, 42.0, EPSILON);

      universe_free(u);
      turbo_pool_free(&arena);
    }

    it("should handle NULL inputs gracefully") {
      turbo_pool_t arena;
      turbo_pool_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);
      universe_advance(u, 200);

      /* These should not crash */
      universe_rank(NULL, NULL, 0, NULL);
      universe_zscore(NULL, NULL, 0, NULL);
      universe_demean(NULL, NULL, 0, NULL);
      universe_clip(NULL, NULL, 0, 0, 0, NULL);
      check_float_eq(universe_cross_sum(NULL, NULL, 0), 0.0, EPSILON);

      universe_free(u);
      turbo_pool_free(&arena);
    }
  }
}
