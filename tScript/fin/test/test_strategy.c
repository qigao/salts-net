/**
 * @file test_strategy.c
 * @brief Tests for order_manager and exprtk_mod_strategy.
 *
 * Coverage:
 *   - order_manager: position tracking, market/limit orders, SL/TP
 *     triggers, mark-to-market, equity curve, trade log
 *   - exprtk_mod_strategy: buy/sell/flat, position queries, vec_rank,
 *     vec_top, vec_filter_gt/lt, kelly_size, risk_size, atr_sl,
 *     last/prev/vec_mean/sum/max/min/std, win_rate/cum_pnl/num_trades
 */

#define TINYTEST_MAIN
#include "exprtk.h"
#include "exprtk_module.h"
#include "fin.h"
#include "market_rules.h"
#include "order_manager.h"
#include "order_executor_loader.h"
#include "tinytest.h"

#include <math.h>
#include <string.h>

/* =========================================================================
 * Shared environment helpers
 * ========================================================================= */

/** Bind a scalar into env. */
static void t_bind_num(exprtk_env_t *env, const char *name, double val) {
    exprtk_value_t v;
    v.type        = exprtk_VAL_NUMBER;
    v.data.number = val;
    exprtk_env_set(env, name, v);
}

/** Bind a vector into env. */
static void t_bind_vec(exprtk_env_t *env, const char *name, double *data, size_t len) {
    exprtk_value_t v;
    v.type             = exprtk_VAL_VECTOR;
    v.data.vector.data = data;
    v.data.vector.size = len;
    exprtk_env_set(env, name, v);
}

/** Read a scalar from env (default 0). */
static double t_read_num(exprtk_env_t *env, const char *name) {
    exprtk_value_t v = exprtk_env_get(env, name);
    return (v.type == exprtk_VAL_NUMBER) ? v.data.number : 0.0;
}

/**
 * Evaluate a tiny script string and return result.
 * Persists results in env arena if they point to short-lived AST arena.
 */
static exprtk_value_t t_eval(const char *src, exprtk_env_t *env) {
    exprtk_node_t   *root = exprtk_parse(src, 0);
    exprtk_value_t   res  = exprtk_eval(root, env);
    
    /* If result is a vector or string, it likely points to the root's arena.
     * Copy it to the env arena so it outlives exprtk_free(root). */
    if (res.type == exprtk_VAL_VECTOR && res.data.vector.data) {
        size_t sz = res.data.vector.size;
        double *out = (double*)turbo_arena_alloc(&env->arena, sz * sizeof(double));
        if (out) {
            memcpy(out, res.data.vector.data, sz * sizeof(double));
            res.data.vector.data = out;
        }
    } else if (res.type == exprtk_VAL_STRING && res.data.string.data) {
        char *out = (char*)turbo_arena_alloc(&env->arena, res.data.string.len + 1);
        if (out) {
            memcpy(out, res.data.string.data, res.data.string.len);
            out[res.data.string.len] = '\0';
            res.data.string.data = out;
        }
    }

    exprtk_free(root);
    return res;
}

/** Create a fresh strategy env with the module mounted and all state vars bound. */
static void t_make_env(exprtk_env_t *env) {
    exprtk_env_init(env);
    exprtk_env_add_module(env, exprtk_module_strategy());

    t_bind_num(env, "signal",      0.0);
    t_bind_num(env, "size",        0.0);
    t_bind_num(env, "stop_loss",   0.0);
    t_bind_num(env, "take_profit", 0.0);
    t_bind_num(env, "position",    0.0);
    t_bind_num(env, "entry_price", 0.0);
    t_bind_num(env, "cash",     1000000.0);
    t_bind_num(env, "equity",   1000000.0);
    t_bind_num(env, "prev_close",  100.0);
    t_bind_num(env, "num_trades",   0.0);
    t_bind_num(env, "num_wins",     0.0);
    t_bind_num(env, "cum_pnl",      0.0);
}

/* =========================================================================
 * Live Order Manager Mock
 * ========================================================================= */

static int g_submits = 0;
static order_error_t mock_submit(order_executor_t *self, order_t *order) {
    (void)self; (void)order;
    g_submits++;
    return ORDER_OK;
}

static void mock_sync(order_executor_t *self, order_manager_t *mgr) {
    (void)self;
    mgr->cash = 1234.5;
    mgr->equity = 5678.9;
}

/* =========================================================================
 * order_manager tests
 * ========================================================================= */

spec("order_manager") {

  describe("Lifecycle") {
    turbo_arena_t arena;
    turbo_arena_init(&arena, 1024 * 64);

    it("creates with correct initial state") {
      order_manager_t *mgr = order_manager_create(&MARKET_US_EQUITY, 100000.0, 32, &arena);
      check(mgr != NULL);
      check(fabs(mgr->cash - 100000.0) < 1e-6);
      check(fabs(mgr->equity - 100000.0) < 1e-6);
      check(mgr->num_trades   == 0);
      check(mgr->num_pending  == 0);
      check(mgr->equity_len   == 0);
      order_manager_free(mgr);
    }

    turbo_arena_free(&arena);
  }

  describe("Market Buy — Long Round-Trip") {
    turbo_arena_t arena;
    turbo_arena_init(&arena, 1024 * 64);
    order_manager_t *mgr = order_manager_create(&MARKET_US_EQUITY, 100000.0, 32, &arena);

    it("performs full lifecycle of a long trade") {
      /* buy */
      order_manager_begin_bar(mgr, 1000.0);
      order_error_t err = order_manager_market(mgr, 1, +1, 0.10, 100.0, 99.0, 0.0, 0.0);
      check(err == ORDER_OK);
      check(order_manager_position(mgr, 1) > 0.0);

      double ep = order_manager_entry_price(mgr, 1);
      check(ep > 99.0 && ep < 102.0);   /* includes 0.05% slippage */
      check(mgr->cash < 100000.0);

      /* close */
      size_t before = mgr->num_trades;
      const trade_record_t *tr = order_manager_close(mgr, 1, 110.0, 1001.0);
      check(mgr->num_trades == before + 1);
      check(tr != NULL);
      check(tr->pnl > 0.0);
      check(fabs(order_manager_position(mgr, 1)) < 1e-9);
    }

    order_manager_free(mgr);
    turbo_arena_free(&arena);
  }

  describe("Market Buy — Invalid Inputs") {
    turbo_arena_t arena;
    turbo_arena_init(&arena, 1024 * 64);
    order_manager_t *mgr = order_manager_create(&MARKET_US_EQUITY, 100000.0, 32, &arena);

    it("rejects invalid fractions and execution prices") {
      order_manager_begin_bar(mgr, 1000.0);
      order_error_t err1 = order_manager_market(mgr, 2, +1, 0.0, 100.0, 99.0, 0.0, 0.0);
      check(err1 != ORDER_OK);

      order_error_t err2 = order_manager_market(mgr, 2, +1, 0.1, 0.0, 99.0, 0.0, 0.0);
      check(err2 != ORDER_OK);
    }

    order_manager_free(mgr);
    turbo_arena_free(&arena);
  }

  describe("Limit Orders") {
    turbo_arena_t arena;
    turbo_arena_init(&arena, 1024 * 64);
    order_manager_t *mgr = order_manager_create(&MARKET_US_EQUITY, 100000.0, 32, &arena);

    it("queues and executes limit order when price triggers") {
      order_manager_begin_bar(mgr, 1000.0);
      order_error_t err = order_manager_limit(mgr, 1, +1, 0.1, 95.0, 0.0, 0.0, -1, NULL);
      check(err == ORDER_OK);
      check(mgr->num_pending == 1);

      /* not triggered */
      order_manager_begin_bar(mgr, 1001.0);
      order_manager_check_triggers(mgr, 1, 100.0, 105.0, 98.0, 102.0, 99.0);
      check(order_manager_position(mgr, 1) == 0.0);
      check(mgr->num_pending == 1);

      /* triggered */
      order_manager_begin_bar(mgr, 1002.0);
      order_manager_check_triggers(mgr, 1, 96.0, 100.0, 93.0, 97.0, 99.0);
      check(order_manager_position(mgr, 1) > 0.0);
      check(mgr->num_pending == 0);
    }

    order_manager_free(mgr);
    turbo_arena_free(&arena);
  }

  describe("Stop-Loss Trigger") {
    turbo_arena_t arena;
    turbo_arena_init(&arena, 1024 * 64);
    order_manager_t *mgr = order_manager_create(&MARKET_US_EQUITY, 100000.0, 32, &arena);

    it("SL triggers when bar low falls below SL price") {
      order_manager_begin_bar(mgr, 1000.0);
      /* Buy with SL at $90 */
      order_manager_market(mgr, 1, +1, 0.1, 100.0, 99.0, 90.0, 0.0);
      check(order_manager_position(mgr, 1) > 0.0);

      order_manager_begin_bar(mgr, 1001.0);
      /* Bar low = 88, below SL of 90 */
      order_manager_check_triggers(mgr, 1, 92.0, 95.0, 88.0, 91.0, 100.0);

      check(fabs(order_manager_position(mgr, 1)) < 1e-9);
      check(mgr->num_trades == 1);
      check(mgr->trades[0].pnl < 0.0);   /* losing trade */
    }

    order_manager_free(mgr);
    turbo_arena_free(&arena);
  }

  describe("Take-Profit Trigger") {
    turbo_arena_t arena;
    turbo_arena_init(&arena, 1024 * 64);
    order_manager_t *mgr = order_manager_create(&MARKET_US_EQUITY, 100000.0, 32, &arena);

    it("TP triggers when bar high exceeds TP price") {
      order_manager_begin_bar(mgr, 1000.0);
      /* Buy with TP at $120 */
      order_manager_market(mgr, 1, +1, 0.1, 100.0, 99.0, 0.0, 120.0);
      check(order_manager_position(mgr, 1) > 0.0);

      order_manager_begin_bar(mgr, 1001.0);
      /* Bar high = 125 > TP of 120 */
      order_manager_check_triggers(mgr, 1, 115.0, 125.0, 113.0, 122.0, 100.0);

      check(fabs(order_manager_position(mgr, 1)) < 1e-9);
      check(mgr->num_trades == 1);
      check(mgr->trades[0].pnl > 0.0);   /* winning trade */
    }

    order_manager_free(mgr);
    turbo_arena_free(&arena);
  }

  describe("Mark-to-Market and Equity Curve") {
    turbo_arena_t arena;
    turbo_arena_init(&arena, 1024 * 64);
    order_manager_t *mgr = order_manager_create(&MARKET_US_EQUITY, 100000.0, 32, &arena);

    it("tracks mark-to-market, equity curve, and returns") {
      order_manager_begin_bar(mgr, 1000.0);
      order_manager_market(mgr, 1, +1, 0.5, 100.0, 99.0, 0.0, 0.0);

      double prices[32] = {0};
      prices[0] = 110.0;   /* price improved */
      order_manager_mark_to_market(mgr, prices, 32);
      check(mgr->equity > 90000.0);   /* roughly 100k + ~5k gain */

      size_t before = mgr->equity_len;
      order_manager_record_equity(mgr);
      check(mgr->equity_len == before + 1);

      order_manager_close(mgr, 1, 110.0, 1001.0);
      double rets[16] = {0};
      size_t n = order_manager_trade_returns(mgr, rets, 16);
      check(n == mgr->num_trades);
      check(n > 0);
      check(rets[0] > 0.0);   /* profitable */
    }

    order_manager_free(mgr);
    turbo_arena_free(&arena);
  }

  describe("Cancel Pending") {
    turbo_arena_t arena;
    turbo_arena_init(&arena, 1024 * 64);
    order_manager_t *mgr = order_manager_create(&MARKET_US_EQUITY, 100000.0, 32, &arena);

    it("correctly cancels pending orders") {
      order_manager_begin_bar(mgr, 1000.0);
      order_manager_limit(mgr, 5, +1, 0.1, 95.0, 0.0, 0.0, -1, NULL);
      order_manager_limit(mgr, 5, +1, 0.1, 94.0, 0.0, 0.0, -1, NULL);
      order_manager_limit(mgr, 7, +1, 0.05, 80.0, 0.0, 0.0, -1, NULL);
      
      check(mgr->num_pending == 3);
      order_manager_cancel_pending(mgr, 5);  /* cancels asset 5 */
      check(mgr->num_pending == 1);          /* asset 7 still there */
    }

    it("handles OCO limits correctly") {
      order_manager_begin_bar(mgr, 1000.0);
      uint64_t id1 = 0, id2 = 0;
      order_manager_limit(mgr, 2, +1, 0.1, 95.0, 0.0, 0.0, -1, &id1);
      order_manager_limit(mgr, 2, -1, 0.1, 105.0, 0.0, 0.0, -1, &id2);
      
      order_manager_link_oco(mgr, id1, id2);
      check(mgr->num_pending == 2);

      order_manager_begin_bar(mgr, 1001.0);
      /* triggers the buy limit at 95.0 */
      order_manager_check_triggers(mgr, 2, 94.0, 100.0, 93.0, 96.0, 99.0);
      check(order_manager_position(mgr, 2) > 0.0);
      
      /* The OCO link should have cancelled the sell order */
      check(mgr->num_pending == 0);
      order_manager_cancel_pending(mgr, 2);
    }
    
    it("trailing stop adjusts the regular stop automatically") {
      /* open long position */
      order_manager_begin_bar(mgr, 1000.0);
      order_manager_market(mgr, 3, +1, 0.1, 100.0, 99.0, 95.0, 0.0);
      check(order_manager_position(mgr, 3) > 0.0);
      
      /* Apply trailing stop of $2 */
      order_manager_set_trailing_stop(mgr, 3, 2.0, false);
      
      /* Price goes up to 105. Stop should trail 2 below high -> 103 */
      order_manager_begin_bar(mgr, 1001.0);
      order_manager_check_triggers(mgr, 3, 102.0, 105.0, 103.5, 104.0, 100.0);
      
      /* Check internal STOP LOSS property on position */
      // Find asset 3 in array
      double actual_sl = 0;
      for (size_t i = 0; i < mgr->capacity; i++) {
        if (mgr->positions[i].asset_id == 3) {
            actual_sl = mgr->positions[i].stop_loss;
            break;
        }
      }
      check(actual_sl >= 102.9); /* allow float diff */
      /* verify position is still alive */
      check(order_manager_position(mgr, 3) > 0.0);
      
      /* Price drops below 103 -> trailing stop hit -> position closed */
      order_manager_begin_bar(mgr, 1002.0);
      order_manager_check_triggers(mgr, 3, 104.0, 104.0, 100.0, 101.0, 104.0);
      check(order_manager_position(mgr, 3) == 0.0);
    }

    order_manager_free(mgr);
    turbo_arena_free(&arena);
  }
}

/* =========================================================================
 * exprtk_mod_strategy tests
 * ========================================================================= */

spec("strategy_module") {

  describe("Signal Setters — buy/sell/flat") {
    exprtk_env_t env;
    t_make_env(&env);

    it("buy() sets signal=+1, size, stop_loss, take_profit") {
      t_eval("buy(0.05, 95.0, 110.0)", &env);
      check(fabs(t_read_num(&env, "signal")      - 1.0)   < 1e-9);
      check(fabs(t_read_num(&env, "size")        - 0.05)  < 1e-9);
      check(fabs(t_read_num(&env, "stop_loss")   - 95.0)  < 1e-9);
      check(fabs(t_read_num(&env, "take_profit") - 110.0) < 1e-9);
    }

    it("sell() sets signal=-1") {
      t_eval("sell(1.0, 0, 0)", &env);
      check(fabs(t_read_num(&env, "signal") - (-1.0)) < 1e-9);
    }

    it("flat() sets signal=0") {
      t_bind_num(&env, "signal", 1.0);
      t_eval("flat()", &env);
      check(fabs(t_read_num(&env, "signal")) < 1e-9);
    }

    it("set_sl() updates stop_loss without touching signal") {
      t_bind_num(&env, "signal", 1.0);
      t_eval("set_sl(88.0)", &env);
      check(fabs(t_read_num(&env, "stop_loss") - 88.0) < 1e-9);
      check(fabs(t_read_num(&env, "signal")    - 1.0)  < 1e-9);  /* unchanged */
    }

    it("set_tp() updates take_profit without touching signal") {
      t_bind_num(&env, "signal", -1.0);
      t_eval("set_tp(75.0)", &env);
      check(fabs(t_read_num(&env, "take_profit") - 75.0) < 1e-9);
      check(fabs(t_read_num(&env, "signal")      - (-1.0)) < 1e-9);
    }

    exprtk_env_free(&env);
  }

  describe("Position Query Helpers") {
    exprtk_env_t env;
    t_make_env(&env);

    it("is_long() = 1 when position > 0") {
      t_bind_num(&env, "position", 100.0);
      exprtk_value_t v = t_eval("is_long()", &env);
      check(fabs(v.data.number - 1.0) < 1e-9);
    }

    it("is_long() = 0 when position = 0") {
      t_bind_num(&env, "position", 0.0);
      exprtk_value_t v = t_eval("is_long()", &env);
      check(v.data.number < 1e-9);
    }

    it("is_short() = 1 when position < 0") {
      t_bind_num(&env, "position", -50.0);
      exprtk_value_t v = t_eval("is_short()", &env);
      check(fabs(v.data.number - 1.0) < 1e-9);
    }

    it("is_flat() = 1 when position == 0") {
      t_bind_num(&env, "position", 0.0);
      exprtk_value_t v = t_eval("is_flat()", &env);
      check(fabs(v.data.number - 1.0) < 1e-9);
    }

    it("is_flat() = 0 when position != 0") {
      t_bind_num(&env, "position", 5.0);
      exprtk_value_t v = t_eval("is_flat()", &env);
      check(v.data.number < 1e-9);
    }

    it("pos() mirrors position env var") {
      t_bind_num(&env, "position", 42.0);
      exprtk_value_t v = t_eval("pos()", &env);
      check(fabs(v.data.number - 42.0) < 1e-9);
    }

    it("entry_px() mirrors entry_price env var") {
      t_bind_num(&env, "entry_price", 123.45);
      exprtk_value_t v = t_eval("entry_px()", &env);
      check(fabs(v.data.number - 123.45) < 1e-9);
    }

    exprtk_env_free(&env);
  }

  describe("Vec Cross-Section Helpers") {
    exprtk_env_t env;
    t_make_env(&env);

    /* scores: [10, 40, 0, 25]  (index 2 = 0 → inactive) */
    static double scores[4] = { 10.0, 40.0, 0.0, 25.0 };
    t_bind_vec(&env, "scores", scores, 4);

    it("vec_rank: inactive element stays 0, highest → ~1") {
      exprtk_node_t *r = exprtk_parse("vec_rank(scores)", 0);
      exprtk_value_t v = exprtk_eval(r, &env);
      check(v.type == exprtk_VAL_VECTOR && v.data.vector.size == 4);
      check(v.data.vector.data[2] < 1e-9);    /* inactive */
      check(v.data.vector.data[1] > 0.99);    /* max score */
      check(v.data.vector.data[0] < v.data.vector.data[3]); /* 10 < 25 */
      exprtk_free(r);
    }

    it("vec_top(scores, 1): only the highest element is selected") {
      exprtk_node_t *r = exprtk_parse("vec_top(scores, 1)", 0);
      exprtk_value_t v = exprtk_eval(r, &env);
      check(v.type == exprtk_VAL_VECTOR);
      check(v.data.vector.data[1] > 0.99);  /* index 1 score=40 wins */
      check(v.data.vector.data[0] < 1e-9);  /* not selected */
      check(v.data.vector.data[2] < 1e-9);  /* inactive, not selected */
      check(v.data.vector.data[3] < 1e-9);  /* not selected */
      exprtk_free(r);
    }

    it("vec_top(scores, 2): top two elements selected") {
      exprtk_node_t *r = exprtk_parse("vec_top(scores, 2)", 0);
      exprtk_value_t v = exprtk_eval(r, &env);
      check(v.type == exprtk_VAL_VECTOR);
      /* scores: 10, 40, 0, 25 → top 2 active: indices 1 and 3 */
      check(v.data.vector.data[1] > 0.99);
      check(v.data.vector.data[3] > 0.99);
      check(v.data.vector.data[0] < 1e-9);
      exprtk_free(r);
    }

    it("vec_filter_gt(scores, 20): mask for active elements > 20") {
      exprtk_node_t *r = exprtk_parse("vec_filter_gt(scores, 20)", 0);
      exprtk_value_t v = exprtk_eval(r, &env);
      check(v.type == exprtk_VAL_VECTOR);
      check(v.data.vector.data[0] < 1e-9);   /* 10 ≤ 20 */
      check(v.data.vector.data[1] > 0.99);   /* 40 > 20 */
      check(v.data.vector.data[2] < 1e-9);   /* 0 inactive */
      check(v.data.vector.data[3] > 0.99);   /* 25 > 20 */
      exprtk_free(r);
    }

    it("vec_filter_lt(scores, 20): mask for active elements < 20") {
      exprtk_node_t *r = exprtk_parse("vec_filter_lt(scores, 20)", 0);
      exprtk_value_t v = exprtk_eval(r, &env);
      check(v.type == exprtk_VAL_VECTOR);
      check(v.data.vector.data[0] > 0.99);   /* 10 < 20 */
      check(v.data.vector.data[1] < 1e-9);   /* 40 not < 20 */
      check(v.data.vector.data[2] < 1e-9);   /* 0 inactive */
      check(v.data.vector.data[3] < 1e-9);   /* 25 not < 20 */
      exprtk_free(r);
    }

    it("vec_where converts mask to indices correctly") {
      /* mask for scores > 20: [0, 1, 0, 1] */
      exprtk_value_t v = t_eval("vec_where(vec_filter_gt(scores, 20))", &env);
      check(v.type == exprtk_VAL_VECTOR);
      check(v.data.vector.size == 2);
      check(fabs(v.data.vector.data[0] - 1.0) < 1e-9); /* index 1 */
      check(fabs(v.data.vector.data[1] - 3.0) < 1e-9); /* index 3 */
    }

    it("vec_sort_idx returns descending indices correctly") {
      /* scores: [10, 40, 0, 25] -> sorted: 40(1), 25(3), 10(0), 0(2) */
      exprtk_value_t v = t_eval("vec_sort_idx(scores)", &env);
      check(v.type == exprtk_VAL_VECTOR);
      check(v.data.vector.size == 4);
      check(fabs(v.data.vector.data[0] - 1.0) < 1e-9);
      check(fabs(v.data.vector.data[1] - 3.0) < 1e-9);
      check(fabs(v.data.vector.data[2] - 0.0) < 1e-9);
      check(fabs(v.data.vector.data[3] - 2.0) < 1e-9);
    }

    it("vec_shift(scores, 1) shifts right correctly") {
      /* scores: [10, 40, 0, 25] -> [0, 10, 40, 0] */
      exprtk_value_t v = t_eval("vec_shift(scores, 1)", &env);
      check(v.type == exprtk_VAL_VECTOR);
      check(v.data.vector.data[0] < 1e-9);
      check(fabs(v.data.vector.data[1] - 10.0) < 1e-9);
      check(fabs(v.data.vector.data[2] - 40.0) < 1e-9);
      check(v.data.vector.data[3] < 1e-9);
    }

    it("vec_shift(scores, -1) shifts left correctly") {
      /* scores: [10, 40, 0, 25] -> [40, 0, 25, 0] */
      exprtk_value_t v = t_eval("vec_shift(scores, -1)", &env);
      check(v.type == exprtk_VAL_VECTOR);
      check(fabs(v.data.vector.data[0] - 40.0) < 1e-9);
      check(v.data.vector.data[1] < 1e-9);
      check(fabs(v.data.vector.data[2] - 25.0) < 1e-9);
      check(v.data.vector.data[3] < 1e-9);
    }

    it("vec_corr computes perfect positive correlation") {
      exprtk_value_t v = t_eval("v1 = [1, 2, 3, 4, 5]; v2 = [2, 4, 6, 8, 10]; vec_corr(v1, v2)", &env);
      check(v.type == exprtk_VAL_NUMBER);
      check(fabs(v.data.number - 1.0) < 1e-7);
    }

    it("vec_corr computes perfect negative correlation") {
      exprtk_value_t v = t_eval("v1 = [1, 2, 3]; v2 = [3, 2, 1]; vec_corr(v1, v2)", &env);
      check(v.type == exprtk_VAL_NUMBER);
      check(fabs(v.data.number + 1.0) < 1e-7);
    }

    it("vec_roll_max(scores, 2) computes rolling maximums correctly") {
      /* scores: [10, 40, 0, 25] -> roll_max(2) -> [10, 40, 40, 25] */
      exprtk_value_t v = t_eval("vec_roll_max(scores, 2)", &env);
      check(v.type == exprtk_VAL_VECTOR);
      check(fabs(v.data.vector.data[0] - 10.0) < 1e-9);
      check(fabs(v.data.vector.data[1] - 40.0) < 1e-9);
      check(fabs(v.data.vector.data[2] - 40.0) < 1e-9);
      check(fabs(v.data.vector.data[3] - 25.0) < 1e-9);
    }

    it("vec_roll_min(scores, 2) computes rolling minimums correctly") {
      /* scores: [10, 40, 0, 25] -> roll_min(2) -> [10, 10, 0, 0] */
      exprtk_value_t v = t_eval("vec_roll_min(scores, 2)", &env);
      check(v.type == exprtk_VAL_VECTOR);
      check(fabs(v.data.vector.data[0] - 10.0) < 1e-9);
      check(fabs(v.data.vector.data[1] - 10.0) < 1e-9);
      check(fabs(v.data.vector.data[2] - 0.0) < 1e-9);
      check(fabs(v.data.vector.data[3] - 0.0) < 1e-9);
    }

    it("vec_vmax(v1, v2) computes element-wise maximum correctly") {
      exprtk_value_t v = t_eval("v1 = [1, 5, 2]; v2 = [3, 2, 4]; vec_vmax(v1, v2)", &env);
      check(v.type == exprtk_VAL_VECTOR);
      check(fabs(v.data.vector.data[0] - 3.0) < 1e-9);
      check(fabs(v.data.vector.data[1] - 5.0) < 1e-9);
      check(fabs(v.data.vector.data[2] - 4.0) < 1e-9);
    }

    it("vec_vmax(v, scalar) compares vector against scalar") {
      exprtk_value_t v = t_eval("v1 = [1, 5, 2]; vec_vmax(v1, 3.0)", &env);
      check(v.type == exprtk_VAL_VECTOR);
      check(fabs(v.data.vector.data[0] - 3.0) < 1e-9);
      check(fabs(v.data.vector.data[1] - 5.0) < 1e-9);
      check(fabs(v.data.vector.data[2] - 3.0) < 1e-9);
    }

    it("vec_fill(3, 7.0) creates vector filled with 7") {
      exprtk_value_t v = t_eval("vec_fill(3, 7.0)", &env);
      check(v.type == exprtk_VAL_VECTOR);
      check(v.data.vector.size == 3);
      check(fabs(v.data.vector.data[0] - 7.0) < 1e-9);
      check(fabs(v.data.vector.data[1] - 7.0) < 1e-9);
      check(fabs(v.data.vector.data[2] - 7.0) < 1e-9);
    }

    it("vec_at(scores, 1) returns 40.0") {
      exprtk_value_t v = t_eval("vec_at(scores, 1)", &env);
      check(v.type == exprtk_VAL_NUMBER);
      check(fabs(v.data.number - 40.0) < 1e-9);
    }

    it("vec_any/all verify vector boolean logic") {
      check(t_eval("vec_any([0, 0, 1, 0])", &env).data.number > 0.5);
      check(t_eval("vec_any([0, 0, 0, 0])", &env).data.number < 0.5);
      check(t_eval("vec_all([1, 1, 1, 1])", &env).data.number > 0.5);
      check(t_eval("vec_all([1, 0, 1, 1])", &env).data.number < 0.5);
    }

    it("vec_cross detects golden and dead crosses") {
      /* v1: [1, 2, 4, 3, 1]
       * v2: [2, 2, 2, 2, 2]
       * cross: [0, 0, 1, 0, -1] -> 
       *   at idx 2: 2<=2 and 4>2 (1.0)
       *   at idx 4: 3>=2 and 1<2 (-1.0)
       */
      const char *script = 
        "v1 = [1, 2, 4, 3, 1]; v2 = [2, 2, 2, 2, 2]; vec_cross(v1, v2)";
      exprtk_value_t v = t_eval(script, &env);
      check(v.type == exprtk_VAL_VECTOR);
      check(v.data.vector.size == 5);
      check(v.data.vector.data[0] < 1e-9);
      check(v.data.vector.data[1] < 1e-9);
      check(fabs(v.data.vector.data[2] - 1.0) < 1e-9);
      check(v.data.vector.data[3] < 1e-9);
      check(fabs(v.data.vector.data[4] + 1.0) < 1e-9);
    }

    it("should execute Dual Thrust logic") {
      /* 
       * Dual Thrust Core Logic:
       * range = max(HH-LC, HC-LL)
       * buy > Open + K * range
       */
      const char *script = 
        "H_d = [105, 110, 108]; " // Daily Highs
        "L_d = [98, 102, 104]; "   // Daily Lows
        "C_d = [102, 107, 106]; " // Daily Closes
        "K = 0.5; "
        "HH = vec_max(H_d); "
        "LC = vec_min(C_d); "
        "HC = vec_max(C_d); "
        "LL = vec_min(L_d); "
        "range = max(HH - LC, HC - LL); "
        "open = 106; "
        "trigger = open + K * range; "
        "trigger"; 

      exprtk_value_t res = t_eval(script, &env);
      check_float_eq(res.data.number, 110.5, 1e-4);
    }

    exprtk_env_free(&env);
  }

  describe("Risk Sizing Helpers") {
    exprtk_env_t env;
    t_make_env(&env);

    it("kelly_size(0.6, 0.02, 0.01) = 0.6 - 0.4/2 = 0.4") {
      exprtk_value_t v = t_eval("kelly_size(0.6, 0.02, 0.01)", &env);
      check(fabs(v.data.number - 0.4) < 1e-6);
    }

    it("kelly_size clamps to 0 for poor win rate") {
      /* w=0.2, ratio=0.5 → Kelly = 0.2 - 0.8/0.5 = -1.4 → clamped to 0 */
      exprtk_value_t v = t_eval("kelly_size(0.2, 0.01, 0.02)", &env);
      check(v.data.number <= 0.0);
    }

    it("risk_size(0.01, 100, 95) = 1% risk / 5% stop = 0.20") {
      /* risk_pct=0.01, entry=100, sl=95 → dist/entry=0.05 → size=0.20 */
      exprtk_value_t v = t_eval("risk_size(0.01, 100, 95)", &env);
      check(fabs(v.data.number - 0.20) < 1e-6);
    }

    it("risk_size clamps to 1.0 when stop is very tight") {
      /* Very tiny distance → would produce ratio > 1, clamped */
      exprtk_value_t v = t_eval("risk_size(0.5, 100, 99.9)", &env);
      check(v.data.number <= 1.0);
    }

    it("atr_sl(2.5, 2.0) = 5.0") {
      exprtk_value_t v = t_eval("atr_sl(2.5, 2.0)", &env);
      check(fabs(v.data.number - 5.0) < 1e-9);
    }

    it("atr_sl(3.0) uses default 2x multiplier → 6.0") {
      exprtk_value_t v = t_eval("atr_sl(3.0)", &env);
      check(fabs(v.data.number - 6.0) < 1e-9);
    }

    exprtk_env_free(&env);
  }

  describe("Bar Vector Helpers") {
    exprtk_env_t env;
    t_make_env(&env);

    static double closes[5] = { 10.0, 12.0, 11.0, 14.0, 13.0 };
    t_bind_vec(&env, "C", closes, 5);

    it("last(C) returns newest close (index 4 = 13.0)") {
      exprtk_value_t v = t_eval("last(C)", &env);
      check(fabs(v.data.number - 13.0) < 1e-9);
    }

    it("prev(C, 1) returns one-bar-ago (index 3 = 14.0)") {
      exprtk_value_t v = t_eval("prev(C, 1)", &env);
      check(fabs(v.data.number - 14.0) < 1e-9);
    }

    it("prev(C, 4) returns oldest bar (index 0 = 10.0)") {
      exprtk_value_t v = t_eval("prev(C, 4)", &env);
      check(fabs(v.data.number - 10.0) < 1e-9);
    }

    it("vec_sum(C) = 60.0") {
      exprtk_value_t v = t_eval("vec_sum(C)", &env);
      check(fabs(v.data.number - 60.0) < 1e-9);
    }

    it("vec_mean(C) = 12.0") {
      exprtk_value_t v = t_eval("vec_mean(C)", &env);
      check(fabs(v.data.number - 12.0) < 1e-9);
    }

    it("vec_max(C) = 14.0") {
      exprtk_value_t v = t_eval("vec_max(C)", &env);
      check(fabs(v.data.number - 14.0) < 1e-9);
    }

    it("vec_min(C) = 10.0") {
      exprtk_value_t v = t_eval("vec_min(C)", &env);
      check(fabs(v.data.number - 10.0) < 1e-9);
    }

    it("vec_std(C) returns a positive number") {
      exprtk_value_t v = t_eval("vec_std(C)", &env);
      check(v.data.number > 0.0);
    }

    it("vec_resample(C, period, mode) downsamples using various modes") {
      /* closes = { 10.0, 12.0, 11.0, 14.0, 13.0 } */
      
      /* Mode 0: Last. period=2. Length 5/2 = 2.
         out[1] = last of [14.0, 13.0] = 13.0
         out[0] = last of [12.0, 11.0] = 11.0 */
      exprtk_value_t v0 = t_eval("vec_resample(C, 2, 0)", &env);
      check(v0.type == exprtk_VAL_VECTOR);
      check(v0.data.vector.size == 2);
      check(fabs(v0.data.vector.data[0] - 11.0) < 1e-9);
      check(fabs(v0.data.vector.data[1] - 13.0) < 1e-9);

      /* Mode 1: First.
         out[1] = first of [14.0, 13.0] = 14.0
         out[0] = first of [12.0, 11.0] = 12.0 */
      exprtk_value_t v1 = t_eval("vec_resample(C, 2, 1)", &env);
      check(fabs(v1.data.vector.data[0] - 12.0) < 1e-9);
      check(fabs(v1.data.vector.data[1] - 14.0) < 1e-9);
      
      /* Mode 2: Max.
         out[1] = max of [14.0, 13.0] = 14.0
         out[0] = max of [12.0, 11.0] = 12.0 */
      exprtk_value_t v2 = t_eval("vec_resample(C, 2, 2)", &env);
      check(fabs(v2.data.vector.data[0] - 12.0) < 1e-9);
      check(fabs(v2.data.vector.data[1] - 14.0) < 1e-9);

      /* Mode 4: Sum.
         out[1] = sum of [14.0, 13.0] = 27.0
         out[0] = sum of [12.0, 11.0] = 23.0 */
      exprtk_value_t v4 = t_eval("vec_resample(C, 2, 4)", &env);
      check(fabs(v4.data.vector.data[0] - 23.0) < 1e-9);
      check(fabs(v4.data.vector.data[1] - 27.0) < 1e-9);

      /* Mode 5: Mean.
         out[1] = mean of [14.0, 13.0] = 13.5
         out[0] = mean of [12.0, 11.0] = 11.5 */
      exprtk_value_t v5 = t_eval("vec_resample(C, 2, 5)", &env);
      check(fabs(v5.data.vector.data[0] - 11.5) < 1e-9);
      check(fabs(v5.data.vector.data[1] - 13.5) < 1e-9);
    }

    exprtk_env_free(&env);
  }

  describe("Trade Log Queries") {
    exprtk_env_t env;
    t_make_env(&env);

    it("num_trades() reads num_trades from env") {
      t_bind_num(&env, "num_trades", 7.0);
      exprtk_value_t v = t_eval("num_trades()", &env);
      check(fabs(v.data.number - 7.0) < 1e-9);
    }

    it("win_rate() = num_wins / num_trades") {
      t_bind_num(&env, "num_trades", 10.0);
      t_bind_num(&env, "num_wins",    6.0);
      exprtk_value_t v = t_eval("win_rate()", &env);
      check(fabs(v.data.number - 0.6) < 1e-9);
    }

    it("win_rate() = 0 when no trades yet") {
      t_bind_num(&env, "num_trades", 0.0);
      t_bind_num(&env, "num_wins",   0.0);
      exprtk_value_t v = t_eval("win_rate()", &env);
      check(v.data.number < 1e-9);
    }

    it("cum_pnl() reads cum_pnl from env") {
      t_bind_num(&env, "cum_pnl", 1234.56);
      exprtk_value_t v = t_eval("cum_pnl()", &env);
      check(fabs(v.data.number - 1234.56) < 1e-6);
    }

    exprtk_env_free(&env);
  }

  describe("rank_pct scalar helper") {
    exprtk_env_t env;
    t_make_env(&env);

    it("rank_pct(25, 10, 40) = (25-10)/(40-10) = 0.5") {
      exprtk_value_t v = t_eval("rank_pct(25, 10, 40)", &env);
      check(fabs(v.data.number - 0.5) < 1e-9);
    }

    it("rank_pct(10, 10, 40) = 0.0") {
      exprtk_value_t v = t_eval("rank_pct(10, 10, 40)", &env);
      check(fabs(v.data.number) < 1e-9);
    }

    it("rank_pct(40, 10, 40) = 1.0") {
      exprtk_value_t v = t_eval("rank_pct(40, 10, 40)", &env);
      check(fabs(v.data.number - 1.0) < 1e-9);
    }

    exprtk_env_free(&env);
  }

  describe("Strategy module mounts without conflicts") {
    exprtk_env_t env;
    exprtk_env_init(&env);
    exprtk_env_add_module(&env, exprtk_module_strategy());
    exprtk_env_add_module(&env, exprtk_module_ta());
    exprtk_env_add_module(&env, exprtk_module_finance());

    it("can mount strategy + ta + finance together (3 modules)") {
      check((int)env.module_count == 3);
    }

    exprtk_env_free(&env);
  }

  describe("Live Order Management") {
    static turbo_arena_t l_arena;
    static order_manager_t *l_mgr;
    static order_executor_t l_mock;

    before_each() {
      g_submits = 0;
      turbo_arena_init(&l_arena, 1024 * 64);
      memset(&l_mock, 0, sizeof(l_mock));
      l_mgr = order_manager_create(&MARKET_US_EQUITY, 100000.0, 10, &l_arena);
    }

    after_each (){
      order_manager_free(l_mgr);
      turbo_arena_free(&l_arena);
    }

    it("should delegate orders to live executor if present") {
      l_mock.name = "Mock";
      l_mock.submit = mock_submit;
      order_manager_set_executor(l_mgr, &l_mock);

      /* Test market submission */
      order_manager_market(l_mgr, 123, 1, 0.1, 100.0, 100.0, 0, 0);
      check(g_submits == 1);
      check(order_manager_position(l_mgr, 123) == 0.0);

      /* Test limit submission */
      uint64_t oid = 0;
      order_manager_limit(l_mgr, 123, 1, 0.1, 95.0, 0, 0, 1, &oid);
      check(g_submits == 2);
      check(oid > 0);
    }

    it("should call sync on live executor") {
      l_mock.sync = mock_sync;
      order_manager_set_executor(l_mgr, &l_mock);

      /* Initial state */
      l_mgr->cash = 1000.0;
      l_mgr->equity = 1000.0;

      order_manager_sync(l_mgr);
      check(fabs(l_mgr->cash - 1234.5) < 1e-6);
      check(fabs(l_mgr->equity - 5678.9) < 1e-6);
    }

    it("should process reported fills and update position") {
      /* Report a fill for a market order (OID=123) */
      order_manager_report_fill(l_mgr, 123, 10, +1, 100.0, 50.0, 45.0, 60.0);

      check(order_manager_position(l_mgr, 10) == 100.0);
      check(fabs(order_manager_entry_price(l_mgr, 10) - 50.0) < 1e-6);
      /* 100 * 50 = 5000 + commission 0.05% = 2.50. Cash = 100000 - 5002.50 = 94997.50 */
      check(l_mgr->cash < 95000.0);
    }

    it("should load an executor from a DLL (mock_executor)") {
      /* Determine path to the DLL. In test_strategy's dir. */
#ifdef _WIN32
      const char *dll_path = "./mock_executor.dll";
#else
      const char *dll_path = "./libmock_executor.so";
#endif
      order_executor_loader_t *loader = order_executor_load(dll_path, "{\"key\": \"val\"}");
      check(loader != NULL);
      if (loader) {
        check(loader->exec != NULL);
        check(strcmp(loader->exec->name, "MockDLL") == 0);
        
        /* Set it and try a sync */
        order_manager_set_executor(l_mgr, loader->exec);
        order_manager_sync(l_mgr); /* Should print to console in reality, but here we just check it doesn't crash */
        
        order_executor_unload(loader);
      }
    }
  }
}
