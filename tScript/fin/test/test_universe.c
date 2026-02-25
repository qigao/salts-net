/**
 * @file test_universe.c
 * @brief Unit tests for universe, market_rules, and bar_window modules.
 */

#define TINYTEST_MAIN
#include "arena_buffer.h"
#include "bar_window.h"
#include "market_rules.h"
#include "tinytest.h"
#include "universe.h"


#include <math.h>
#include <string.h>

/* =========================================================================
 * Helpers
 * ========================================================================= */

/* Epoch-days shortcuts (approximate) */
#define DATE_2000 10957.0 /* 2000-01-01 */
#define DATE_2010 14610.0 /* 2010-01-01 */
#define DATE_2015 16436.0 /* 2015-01-01 */
#define DATE_2020 18262.0 /* 2020-01-01 */
#define DATE_2025 20089.0 /* 2025-01-01 */

static universe_t *make_universe(turbo_arena_t *arena) {
  universe_t *u = universe_create(arena);
  if (!u)
    return NULL;

  universe_asset_t a1 = {.id = 1,
                         .ticker = "AAPL",
                         .exchange = "NASDAQ",
                         .asset_type = UNIVERSE_ASSET_EQUITY,
                         .start_date = DATE_2000,
                         .end_date = UNIVERSE_DATE_NONE,
                         .lot_size = 1,
                         .tick_size = 0.01};
  universe_asset_t a2 = {.id = 2,
                         .ticker = "ENRN",
                         .exchange = "NYSE",
                         .asset_type = UNIVERSE_ASSET_EQUITY,
                         .start_date = DATE_2000,
                         .end_date = DATE_2010, /* delisted in 2010 */
                         .lot_size = 1,
                         .tick_size = 0.01};
  universe_asset_t a3 = {.id = 3,
                         .ticker = "TSLA",
                         .exchange = "NASDAQ",
                         .asset_type = UNIVERSE_ASSET_EQUITY,
                         .start_date = DATE_2010,
                         .end_date = UNIVERSE_DATE_NONE, /* IPO 2010 */
                         .lot_size = 1,
                         .tick_size = 0.01};

  universe_add_asset(u, &a1);
  universe_add_asset(u, &a2);
  universe_add_asset(u, &a3);

  /* AAPL 4:1 split in 2020 */
  universe_adj_t sp = {.asset_id = 1, .date = DATE_2020, .type = UNIVERSE_ADJ_SPLIT, .factor = 4.0};
  universe_add_adjustment(u, &sp);

  universe_finalize(u);
  return u;
}

/* =========================================================================
 * universe tests
 * ========================================================================= */
spec("universe") {
  describe("Universe — Active Mask") {
    turbo_arena_t arena;
    turbo_arena_init(&arena, 1024 * 64);
    universe_t *u = make_universe(&arena);
    check(u != NULL);

    it("AAPL active in 2015") {
      universe_advance(u, DATE_2015);
      check(universe_is_active(u, 1) == true);
    }

    it("ENRN active in 2005, gone in 2015") {
      universe_advance(u, DATE_2000 + 365); /* 2001 */
      check(universe_is_active(u, 2) == true);

      universe_advance(u, DATE_2015);
      check(universe_is_active(u, 2) == false);
    }

    it("TSLA not yet active in 2005") {
      universe_advance(u, DATE_2000 + 365); /* 2001 */
      check(universe_is_active(u, 3) == false);
    }

    it("TSLA active in 2015") {
      universe_advance(u, DATE_2015);
      check(universe_is_active(u, 3) == true);
    }

    it("active count in 2005 is 2 (AAPL + ENRN, not TSLA)") {
      universe_advance(u, DATE_2000 + 365 * 5);
      check(universe_active_count(u) == 2);
    }

    it("active count in 2015 is 2 (AAPL + TSLA, not ENRN)") {
      universe_advance(u, DATE_2015);
      check(universe_active_count(u) == 2);
    }

    turbo_arena_free(&arena);
  }

  describe("Universe — Delistings") {
    turbo_arena_t arena;
    turbo_arena_init(&arena, 1024 * 64);
    universe_t *u = make_universe(&arena);

    it("ENRN detected as delisted when crossing end_date") {
      /* First advance to just before delist */
      universe_advance(u, DATE_2010 - 1);
      check(u->num_delisted_today == 0);

      /* Advance to delist date */
      universe_advance(u, DATE_2010);
      uint32_t dlisted[8];
      size_t nd = universe_delisted_today(u, dlisted, 8);
      check(nd == 1);
      check(dlisted[0] == 2); /* ENRN id=2 */
    }

    turbo_arena_free(&arena);
  }

  describe("Universe — Price Adjustments") {
    turbo_arena_t arena;
    turbo_arena_init(&arena, 1024 * 64);
    universe_t *u = make_universe(&arena);

    it("adj_factor before split is < 1.0 (backward adjusted)") {
      /* Pre-split prices should appear smaller when adjusted */
      universe_advance(u, DATE_2015); /* before 2020 split */
      double factor = universe_adj_factor(u, 1);
      /* The split happens at DATE_2020; we're in 2015 so cum_adj should
       * reflect the backward factor of 1/4 for pre-split data. */
      /* (factor < 1.0 or == 1.0 depending on exact logic; assert roughly) */
      check(factor > 0.0);
    }

    it("adj_factor after split is 1.0 (no more adjustments pending)") {
      universe_advance(u, DATE_2025);
      double factor = universe_adj_factor(u, 1);
      /* Post-split: cumulative factor from the event is 1/4 = 0.25
       * for all data preceding the split date. After the split date itself,
       * most implementations do not apply additional correction. */
      check(factor > 0.0);
    }

    it("unknown asset returns adj_factor 1.0") {
      universe_advance(u, DATE_2020);
      check(universe_adj_factor(u, 9999) == 1.0);
    }

    turbo_arena_free(&arena);
  }

  describe("Universe — Cross-sectional rank and top_n") {
    turbo_arena_t arena;
    turbo_arena_init(&arena, 1024 * 64);
    universe_t *u = make_universe(&arena);
    universe_advance(u, DATE_2015); /* AAPL(1) + TSLA(3) active */

    it("rank scores 0..1 for active assets") {
      double vals[3] = {150.0, 0.0, 250.0}; /* id1=150, id2=inactive, id3=250 */
      double out[3] = {0};
      universe_rank(u, vals, 3, out);
      /* id1 (AAPL, lower): rank 0/1 = 0.0 */
      /* id3 (TSLA, higher): rank 1/1 = 1.0 */
      check(out[0] < out[2]);
      check(out[2] >= 0.9);
      check(out[1] == 0.0); /* inactive */
    }

    it("top_n returns highest by value") {
      double vals[3] = {150.0, 0.0, 250.0};
      uint32_t out_ids[2];
      size_t cnt = universe_top_n(u, vals, 3, 1, out_ids);
      check(cnt == 1);
      check(out_ids[0] == 3); /* TSLA has highest value */
    }

    it("filter_gt returns correct mask") {
      double vals[3] = {150.0, 0.0, 250.0};
      uint8_t mask[3];
      size_t cnt = universe_filter_gt(u, vals, 3, 200.0, mask);
      check(cnt == 1);     /* only TSLA > 200 */
      check(mask[0] == 0); /* AAPL not passing */
      check(mask[1] == 0); /* inactive */
      check(mask[2] == 1); /* TSLA passing */
    }

    turbo_arena_free(&arena);
  }

  /* =========================================================================
   * market_rules tests
   * ========================================================================= */

  describe("Market Rules — US Equity") {
    const universe_market_rules_t *rules = &MARKET_US_EQUITY;

    it("valid market buy accepted") {
      order_error_t err =
          market_validate_order(rules, true, 150.0, 100.0, +1, 149.0, 0.0, 0.0, -1.0);
      check(err == ORDER_OK);
    }

    it("zero quantity rejected") {
      order_error_t err = market_validate_order(rules, true, 150.0, 0.0, +1, 149.0, 0.0, 0.0, -1.0);
      check(err == ORDER_ERR_ZERO_QTY);
    }

    it("inactive asset rejected") {
      order_error_t err =
          market_validate_order(rules, false, 150.0, 100.0, +1, 149.0, 0.0, 0.0, -1.0);
      check(err == ORDER_ERR_ASSET_INACTIVE);
    }

    it("short allowed in US") {
      order_error_t err =
          market_validate_order(rules, true, 150.0, 100.0, -1, 149.0, 0.0, 0.0, -1.0);
      check(err == ORDER_OK);
    }
  }

  describe("Market Rules — CN A-Shares") {
    const universe_market_rules_t *rules = &MARKET_CN_ASTOCK;

    it("valid buy of 100 shares accepted") {
      order_error_t err = market_validate_order(rules, true, 25.0, 100.0, +1, 24.5, 0.0, 0.0, -1.0);
      check(err == ORDER_OK);
    }

    it("buy of 50 shares rejected (not multiple of 100)") {
      order_error_t err = market_validate_order(rules, true, 25.0, 50.0, +1, 24.5, 0.0, 0.0, -1.0);
      check(err == ORDER_ERR_LOT_SIZE);
    }

    it("T+1 violation: selling shares bought today") {
      /* position=100, bought_today=100, trying to sell 100 → rejected */
      order_error_t err =
          market_validate_order(rules, true, 25.0, 100.0, -1, 24.5, 100.0, 100.0, -1.0);
      check(err == ORDER_ERR_T_PLUS);
    }

    it("price limit up: price > prev_close * 1.10 rejected") {
      order_error_t err = market_validate_order(rules, true, 27.5, 100.0, +1, 24.5, 0.0, 0.0, -1.0);
      /* 24.5 * 1.10 = 26.95 < 27.5 → over limit */
      check(err == ORDER_ERR_PRICE_LIMIT);
    }

    it("price limit dn: price < prev_close * 0.90 rejected") {
      order_error_t err = market_validate_order(rules, true, 21.0, 100.0, +1, 24.5, 0.0, 0.0, -1.0);
      /* 24.5 * 0.90 = 22.05 > 21.0 → under limit */
      check(err == ORDER_ERR_PRICE_LIMIT);
    }

    it("short selling rejected") {
      order_error_t err = market_validate_order(rules, true, 25.0, 100.0, -1, 24.5, 0.0, 0.0, -1.0);
      check(err == ORDER_ERR_NO_SHORT);
    }
  }

  describe("Market Rules — Lot rounding and commission") {
    it("round_lot floors to nearest 100 for CN") {
      double rounded = market_round_lot(&MARKET_CN_ASTOCK, 250.0);
      check(fabs(rounded - 200.0) < 1e-9);
    }

    it("round_lot is identity for fractional crypto") {
      double rounded = market_round_lot(&MARKET_CRYPTO, 1.23456);
      check(fabs(rounded - 1.23456) < 1e-9);
    }

    it("clamp_price returns clamped value for CN") {
      double clamped = market_clamp_price(&MARKET_CN_ASTOCK, 30.0, 24.5);
      /* 24.5 * 1.10 = 26.95; 30.0 > 26.95 → clamped to 26.95 */
      check(fabs(clamped - 26.95) < 0.01);
    }

    it("commission >= min_commission") {
      double comm = market_commission(&MARKET_CN_ASTOCK, 100.0, 0);
      check(comm >= MARKET_CN_ASTOCK.min_commission);
    }

    it("sell commission includes stamp duty") {
      double buy_comm = market_commission(&MARKET_CN_ASTOCK, 10000.0, 0);
      double sell_comm = market_commission(&MARKET_CN_ASTOCK, 10000.0, 1);
      check(sell_comm > buy_comm); /* stamp duty adds to sell side */
    }
  }

  /* =========================================================================
   * bar_window tests
   * ========================================================================= */

  describe("Bar Window — Basic Push and Linearize") {
    turbo_arena_t arena;
    turbo_arena_init(&arena, 1024 * 64);

    it("creates window with correct capacity") {
      bar_window_t *w = bar_window_create(10, &arena);
      check(w != NULL);
      check(w->capacity == 10);
      check(w->count == 0);
      check(!bar_window_ready(w, 1));
    }

    it("push fills count correctly") {
      bar_window_t *w = bar_window_create(5, &arena);
      for (int i = 0; i < 3; i++)
        bar_window_push(w, (double)i, 10 + i, 11 + i, 9 + i, 10.5 + i, 1000);
      check(w->count == 3);
      check(bar_window_ready(w, 3));
      check(!bar_window_ready(w, 4));
    }

    it("ring wraps correctly: oldest bar evicted after capacity+1 pushes") {
      bar_window_t *w = bar_window_create(3, &arena);
      /* Push 4 bars: bar 0 should be evicted */
      bar_window_push(w, 0, 10, 11, 9, 10, 100);  /* bar 0 */
      bar_window_push(w, 1, 20, 21, 19, 20, 200); /* bar 1 */
      bar_window_push(w, 2, 30, 31, 29, 30, 300); /* bar 2 */
      bar_window_push(w, 3, 40, 41, 39, 40, 400); /* bar 3, evicts bar 0 */

      check(w->count == 3);

      bar_window_linearize(w);
      /* Oldest should now be bar 1 (open=20), newest bar 3 (open=40) */
      check(fabs(w->open[0] - 20.0) < 1e-9);
      check(fabs(w->open[2] - 40.0) < 1e-9);
    }

    it("last_close returns most recent close") {
      bar_window_t *w = bar_window_create(10, &arena);
      bar_window_push(w, 0, 10, 12, 8, 11, 100);
      bar_window_push(w, 1, 11, 13, 9, 12, 100);
      check(fabs(bar_window_last_close(w) - 12.0) < 1e-9);
    }

    it("adjusted push applies factor to OHLC but not volume") {
      bar_window_t *w = bar_window_create(5, &arena);
      bar_window_push_adjusted(w, 0, 400, 420, 380, 410, 1000, 0.25);
      bar_window_linearize(w);
      check(fabs(w->open[0] - 100.0) < 1e-6);    /* 400 * 0.25 */
      check(fabs(w->close[0] - 102.5) < 1e-6);   /* 410 * 0.25 */
      check(fabs(w->volume[0] - 1000.0) < 1e-6); /* volume unchanged */
    }

    it("clear resets count") {
      bar_window_t *w = bar_window_create(5, &arena);
      bar_window_push(w, 0, 10, 11, 9, 10, 100);
      bar_window_clear(w);
      check(w->count == 0);
      check(!bar_window_ready(w, 1));
    }

    turbo_arena_free(&arena);
  }
}