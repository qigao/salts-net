/**
 * @file strategy.c
 * @brief Event-driven on_bar strategy runner implementation.
 */

#include "strategy.h"
#include "fin.h"               /* exprtk_module_strategy, exprtk_module_ta, etc. */
#include "exprtk_types.h"
#include "exprtk.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* =========================================================================
 * Internal bind helpers
 * ========================================================================= */

/** Bind a named scalar variable into the TurboScript environment. */
static void bind_num(exprtk_env_t *env, const char *name, double val) {
    exprtk_value_t v = { .type = exprtk_VAL_NUMBER, .data = { .number = val } };
    exprtk_env_set(env, name, v);
}

/** Bind a named vector (double array) into the TurboScript environment. */
static void bind_vec(exprtk_env_t *env, const char *name,
                     const double *data, size_t len) {
    exprtk_value_t v;
    v.type = exprtk_VAL_VECTOR;
    v.data.vector.data = (double *)data;   /* cast away const — env doesn't own it */
    v.data.vector.size = len;
    exprtk_env_set(env, name, v);
}

/** Read a scalar output variable written by the strategy script. */
static double read_num(exprtk_env_t *env, const char *name, double def) {
    exprtk_value_t v = exprtk_env_get(env, name);
    if (v.type == exprtk_VAL_NUMBER) return v.data.number;
    return def;
}

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

strategy_ctx_t *strategy_create(universe_t *universe,
                                  provider_t *provider,
                                  const strategy_config_t *cfg,
                                  turbo_arena_t *arena) {
    strategy_ctx_t *ctx = (strategy_ctx_t *)turbo_arena_alloc(arena, sizeof(strategy_ctx_t));
    if (!ctx) return NULL;
    memset(ctx, 0, sizeof(*ctx));

    ctx->universe    = universe;
    ctx->provider    = provider;
    ctx->arena       = arena;
    ctx->config      = cfg ? *cfg : strategy_config_default();
    ctx->warmup_bars = ctx->config.warmup_bars;
    ctx->num_assets  = universe ? universe->num_assets : 0;

    /* Allocate per-asset bar windows */
    if (ctx->num_assets > 0) {
        ctx->windows = (bar_window_t **)turbo_arena_alloc(
            arena, ctx->num_assets * sizeof(bar_window_t *));
        if (!ctx->windows) return NULL;
        for (size_t i = 0; i < ctx->num_assets; i++) {
            ctx->windows[i] = bar_window_create(ctx->config.window_capacity, arena);
            if (!ctx->windows[i]) return NULL;
        }
    }

    /* Create order manager */
    ctx->order_mgr = order_manager_create(
        provider ? provider->market : NULL,
        ctx->config.initial_cash,
        ctx->num_assets > 0 ? ctx->num_assets : 256,
        arena);
    if (!ctx->order_mgr) return NULL;

    ctx->order_mgr->slippage_pct = ctx->config.slippage_pct;
    ctx->order_mgr->spread       = ctx->config.spread;

    /* Create TurboScript environment */
    ctx->env = (exprtk_env_t *)turbo_arena_alloc(arena, sizeof(exprtk_env_t));
    if (!ctx->env) return NULL;
    exprtk_env_init(ctx->env);

    /* Mount strategy scripting module (buy/sell/flat/rank/...) */
    exprtk_env_add_module(ctx->env, exprtk_module_strategy());

    /* Pre-declare output variables so the script can assign them */
    bind_num(ctx->env, "signal",      0.0);
    bind_num(ctx->env, "size",        0.1);
    bind_num(ctx->env, "stop_loss",   0.0);
    bind_num(ctx->env, "take_profit", 0.0);

    /* Trade-log state (updated per-bar for adaptive strategies) */
    bind_num(ctx->env, "num_trades", 0.0);
    bind_num(ctx->env, "num_wins",   0.0);
    bind_num(ctx->env, "cum_pnl",    0.0);
    bind_num(ctx->env, "entry_price", 0.0);

    return ctx;
}

void strategy_free(strategy_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->env) exprtk_env_free(ctx->env);
    if (ctx->order_mgr) order_manager_free(ctx->order_mgr);
    /* windows and ctx itself are arena-owned */
}

/* =========================================================================
 * Hook Detection & Invocation
 * ========================================================================= */

void strategy_init_hooks(strategy_ctx_t *ctx) {
    if (!ctx || !ctx->env) return;
    ctx->has_hooks    = false;
    ctx->hook_on_init = NULL;
    ctx->hook_on_bar  = NULL;
    ctx->hook_on_fill = NULL;
    ctx->hook_on_stop = NULL;

    for (exprtk_func_t *f = ctx->env->funcs; f; f = f->next) {
        if (!f->name || !f->is_script) continue;
        if      (strcmp(f->name, "on_init") == 0) ctx->hook_on_init = f;
        else if (strcmp(f->name, "on_bar")  == 0) ctx->hook_on_bar  = f;
        else if (strcmp(f->name, "on_fill") == 0) ctx->hook_on_fill = f;
        else if (strcmp(f->name, "on_stop") == 0) ctx->hook_on_stop = f;
    }
    ctx->has_hooks = (ctx->hook_on_init || ctx->hook_on_bar ||
                      ctx->hook_on_fill || ctx->hook_on_stop);
}

/** Invoke a script-defined hook function (0 args). */
static void call_hook(strategy_ctx_t *ctx, exprtk_func_t *hook) {
    if (!hook || !hook->data.script.body) return;
    exprtk_env_t *env = ctx->env;
    env->flow = exprtk_FLOW_NORMAL;
    exprtk_eval(hook->data.script.body, env);
    env->flow = exprtk_FLOW_NORMAL;
}

/** Invoke on_fill hook with a fill map argument. */
static void call_hook_on_fill(strategy_ctx_t *ctx, const trade_record_t *tr) {
    if (!ctx->hook_on_fill || !tr) return;
    exprtk_env_t *env = ctx->env;

    /* Build fill map and bind as "fill" variable */
    exprtk_value_t m = exprtk_val_map();
    exprtk_map_set(&m, "asset_id",  exprtk_val_num((double)tr->asset_id));
    exprtk_map_set(&m, "direction", exprtk_val_num((double)tr->direction));
    exprtk_map_set(&m, "price",     exprtk_val_num(tr->exit_price));
    exprtk_map_set(&m, "quantity",  exprtk_val_num(tr->quantity));
    exprtk_map_set(&m, "pnl",       exprtk_val_num(tr->pnl));
    exprtk_env_set(env, "fill", m);

    env->flow = exprtk_FLOW_NORMAL;
    exprtk_eval(ctx->hook_on_fill->data.script.body, env);
    env->flow = exprtk_FLOW_NORMAL;

    /* Free the map we allocated */
    exprtk_map_free(&m);
}

/* =========================================================================
 * Strategy Reset (for walk-forward re-runs)
 * ========================================================================= */

void strategy_reset(strategy_ctx_t *ctx) {
    if (!ctx) return;

    /* Reset order manager to initial state */
    if (ctx->order_mgr) {
        ctx->order_mgr->cash         = ctx->order_mgr->initial_cash;
        ctx->order_mgr->equity       = ctx->order_mgr->initial_cash;
        ctx->order_mgr->num_pending  = 0;
        ctx->order_mgr->num_trades   = 0;
        ctx->order_mgr->equity_len   = 0;
        ctx->order_mgr->next_order_id = 1;
        /* Zero out all positions */
        if (ctx->order_mgr->positions)
            memset(ctx->order_mgr->positions, 0,
                   ctx->order_mgr->capacity * sizeof(position_t));
    }

    /* Clear all bar windows */
    for (size_t i = 0; i < ctx->num_assets; i++) {
        if (ctx->windows && ctx->windows[i])
            bar_window_clear(ctx->windows[i]);
    }

    ctx->total_bars   = 0;
    ctx->current_date = 0.0;
}

/* =========================================================================
 * Script Compilation
 * ========================================================================= */

int strategy_compile(strategy_ctx_t *ctx, const char *code) {
    if (!ctx || !ctx->env || !code) return -1;
    exprtk_parse_ctx_t parse = {0};
    turbo_arena_t scratch;
    turbo_arena_init(&scratch, 1024 * 256);
    parse.arena = &scratch;

    ctx->parsed = (struct exprtk_parse_ctx_s *)turbo_arena_alloc(
        ctx->arena, sizeof(exprtk_parse_ctx_t));
    if (!ctx->parsed) { turbo_arena_free(&scratch); return -1; }

    /* Parse the script */
    exprtk_parse_ctx_t *pc = (exprtk_parse_ctx_t *)ctx->parsed;
    *pc = parse;

    int err = 0;
    pc->root = exprtk_parse_ext(code, strlen(code), ctx->arena, &err,
                                 pc->error_msg, sizeof(pc->error_msg));
    pc->error = err;

    /* Execute top-level code to register script-defined functions */
    if (!err && pc->root) {
        exprtk_eval(pc->root, ctx->env);
        ctx->env->flow = exprtk_FLOW_NORMAL;
    }

    /* Detect on_init/on_bar/on_fill/on_stop hooks */
    if (!err) strategy_init_hooks(ctx);

    turbo_arena_free(&scratch);
    return err ? -1 : 0;
}

/* =========================================================================
 * Per-Bar Execution
 * ========================================================================= */

void strategy_on_bar(strategy_ctx_t *ctx,
                      uint32_t asset_id,
                      const provider_bar_t *bar,
                      double prev_close,
                      bool is_warmup) {
    /* Find array index for this asset */
    size_t idx = universe_find_asset(ctx->universe, asset_id);
    if (idx == SIZE_MAX) return;

    bar_window_t *w = ctx->windows[idx];

    /* 1. Push adjusted bar into window */
    double adj = universe_adj_factor(ctx->universe, asset_id);
    bar_window_push_adjusted(w, bar->date,
                              bar->open, bar->high, bar->low, bar->close, bar->volume,
                              adj);

    /* During warm-up: update indicators but don't trade */
    if (is_warmup || !bar_window_ready(w, ctx->warmup_bars)) return;

    /* 2. Linearize ring buffer → chronological arrays */
    bar_window_linearize(w);

    /* 3. Check SL/TP and pending orders against this bar's H/L */
    order_manager_check_triggers(ctx->order_mgr, asset_id,
                                  bar->open, bar->high, bar->low, bar->close,
                                  prev_close);

    if (!ctx->parsed || !ctx->env) return;

    /* 4. Bind read-only state */
    bind_vec(ctx->env, "O", w->open,   w->count);
    bind_vec(ctx->env, "H", w->high,   w->count);
    bind_vec(ctx->env, "L", w->low,    w->count);
    bind_vec(ctx->env, "C", w->close,  w->count);
    bind_vec(ctx->env, "V", w->volume, w->count);
    bind_num(ctx->env, "n",          (double)w->count);
    bind_num(ctx->env, "asset_id",   (double)asset_id);
    bind_num(ctx->env, "bar_index",  (double)ctx->total_bars);
    bind_num(ctx->env, "position",   order_manager_position(ctx->order_mgr, asset_id));
    bind_num(ctx->env, "entry_price",order_manager_entry_price(ctx->order_mgr, asset_id));
    bind_num(ctx->env, "cash",       ctx->order_mgr->cash);
    bind_num(ctx->env, "equity",     ctx->order_mgr->equity);
    bind_num(ctx->env, "prev_close", prev_close);

    /* Update trade-log state for adaptive logic */
    {
        size_t nt = ctx->order_mgr->num_trades;
        double nw = 0.0, cpnl = 0.0;
        for (size_t ti = 0; ti < nt; ti++) {
            cpnl += ctx->order_mgr->trades[ti].pnl;
            if (ctx->order_mgr->trades[ti].pnl > 0.0) nw += 1.0;
        }
        bind_num(ctx->env, "num_trades", (double)nt);
        bind_num(ctx->env, "num_wins",   nw);
        bind_num(ctx->env, "cum_pnl",    cpnl);
    }

    /* 5. Reset output variables */
    bind_num(ctx->env, "signal",      0.0);
    bind_num(ctx->env, "size",        0.1);
    bind_num(ctx->env, "signal_type", 0.0); /* 0: MKT, 1: LIMIT, 2: STOP */
    bind_num(ctx->env, "signal_price",0.0);
    bind_num(ctx->env, "bars_valid",  1.0); /* 1: DAY, -1: GTC */
    bind_num(ctx->env, "stop_loss",   0.0);
    bind_num(ctx->env, "take_profit", 0.0);
    bind_num(ctx->env, "trailing_dist", 0.0);
    bind_num(ctx->env, "trailing_pct",  0.0);

    /* 6. Execute the compiled strategy */
    exprtk_parse_ctx_t *pc = (exprtk_parse_ctx_t *)ctx->parsed;
    if (ctx->has_hooks && ctx->hook_on_bar) {
        call_hook(ctx, ctx->hook_on_bar);
    } else if (pc->root) {
        exprtk_eval(pc->root, ctx->env);
    }

    /* 7. Read strategy decisions */
    double sig  = read_num(ctx->env, "signal",      0.0);
    double sz   = read_num(ctx->env, "size",        0.1);
    double type = read_num(ctx->env, "signal_type", 0.0);
    double prc  = read_num(ctx->env, "signal_price",0.0);
    int valid   = (int)read_num(ctx->env, "bars_valid",  1.0);
    double sl   = read_num(ctx->env, "stop_loss",   0.0);
    double tp   = read_num(ctx->env, "take_profit", 0.0);
    double tdst = read_num(ctx->env, "trailing_dist", 0.0);
    double tpct = read_num(ctx->env, "trailing_pct",  0.0);

    /* 8. Translate signal to order */
    double pos = order_manager_position(ctx->order_mgr, asset_id);
    
    if (tdst > 0.0 && pos != 0.0) {
        order_manager_set_trailing_stop(ctx->order_mgr, asset_id, tdst, tpct > 0.0);
    }

    if (sig > 0.5 && pos <= 0.0) {
        /* Buy signal — not already long */
        if (pos < 0.0)
            order_manager_close(ctx->order_mgr, asset_id, bar->open, bar->date);
            
        if (type == 1.0) {
            order_manager_limit(ctx->order_mgr, asset_id, +1, sz, prc, sl, tp, valid, NULL);
        } else if (type == 2.0) {
            order_manager_stop(ctx->order_mgr, asset_id, +1, sz, prc, sl, tp, valid, NULL);
        } else {
            order_manager_market(ctx->order_mgr, asset_id, +1, sz,
                                  bar->open, prev_close, sl, tp);
        }
    } else if (sig < -0.5 && pos >= 0.0) {
        /* Sell/short signal */
        if (pos > 0.0)
            order_manager_close(ctx->order_mgr, asset_id, bar->open, bar->date);
        
        if (ctx->order_mgr->rules && ctx->order_mgr->rules->short_allowed) {
            if (type == 1.0) {
                order_manager_limit(ctx->order_mgr, asset_id, -1, sz, prc, sl, tp, valid, NULL);
            } else if (type == 2.0) {
                order_manager_stop(ctx->order_mgr, asset_id, -1, sz, prc, sl, tp, valid, NULL);
            } else {
                order_manager_market(ctx->order_mgr, asset_id, -1, sz,
                                      bar->open, prev_close, sl, tp);
            }
        }
    } else if (fabs(sig) < 0.01 && pos != 0.0) {
        /* Flat signal — close existing position */
        order_manager_cancel_pending(ctx->order_mgr, asset_id);
        order_manager_close(ctx->order_mgr, asset_id, bar->open, bar->date);
    }
}

/* =========================================================================
 * Single-Asset Backtest
 * ========================================================================= */

int strategy_run_single(strategy_ctx_t *ctx,
                          uint32_t asset_id,
                          double start_date,
                          double end_date) {
    if (!ctx || !ctx->provider || !ctx->provider->open_stream) return -1;

    provider_t *p = ctx->provider;

    /* Find asset in universe */
    size_t idx = universe_find_asset(ctx->universe, asset_id);
    if (idx == SIZE_MAX) return -1;
    const universe_asset_t *a = &ctx->universe->assets[idx];

    if (start_date <= 0.0) start_date = a->start_date;
    if (end_date   <= 0.0) end_date   = (a->end_date > 0.0) ? a->end_date : 1e12;

    /* Open bar stream */
    void *stream;
    if (a->ticker[0]) {
        /* Use ticker-based CSV open if available */
        typedef void *(*open_by_ticker_fn)(provider_t *, const char *,
                                           uint32_t, double, double, turbo_arena_t *);
        /* We call the internal helper by function pointer convention;
         * for now fall back to generic open_stream. */
        stream = p->open_stream(p, asset_id, start_date, end_date, ctx->arena);
    } else {
        stream = p->open_stream(p, asset_id, start_date, end_date, ctx->arena);
    }
    if (!stream) return -1;

    provider_bar_t bar;
    provider_bar_t prev_bar = {0};
    int processed = 0;

    order_manager_begin_bar(ctx->order_mgr, start_date);

    /* Call on_init() hook before the bar loop */
    if (ctx->has_hooks && ctx->hook_on_init)
        call_hook(ctx, ctx->hook_on_init);

    /* Track trade count to detect new fills */
    size_t prev_num_trades = ctx->order_mgr->num_trades;

    while (p->next_bar(p, stream, &bar) == 1) {
        universe_advance(ctx->universe, bar.date);
        order_manager_begin_bar(ctx->order_mgr, bar.date);
        ctx->current_date = bar.date;

        bool warmup = (processed < (int)ctx->warmup_bars);
        strategy_on_bar(ctx, asset_id, &bar, prev_bar.close, warmup);

        /* Fire on_fill() for any new trades generated this bar */
        if (ctx->has_hooks && ctx->hook_on_fill) {
            size_t cur_trades = ctx->order_mgr->num_trades;
            for (size_t ti = prev_num_trades; ti < cur_trades; ti++)
                call_hook_on_fill(ctx, &ctx->order_mgr->trades[ti]);
            prev_num_trades = cur_trades;
        }

        /* Mark-to-market using this bar's close */
        double closes[1] = { bar.close * universe_adj_factor(ctx->universe, asset_id) };
        order_manager_mark_to_market(ctx->order_mgr, closes, 1);
        order_manager_record_equity(ctx->order_mgr);

        prev_bar = bar;
        processed++;
        ctx->total_bars++;
    }

    p->close_stream(p, stream);

    /* Close any open position at the last price */
    if (prev_bar.close > 0.0) {
        const trade_record_t *final_tr =
            order_manager_close(ctx->order_mgr, asset_id, prev_bar.close, end_date);
        if (ctx->has_hooks && ctx->hook_on_fill && final_tr)
            call_hook_on_fill(ctx, final_tr);
    }

    /* Call on_stop() hook after the bar loop */
    if (ctx->has_hooks && ctx->hook_on_stop)
        call_hook(ctx, ctx->hook_on_stop);

    return processed;
}

/* =========================================================================
 * Multi-Asset Universe Backtest
 * ========================================================================= */

int strategy_run_universe(strategy_ctx_t *ctx,
                            double start_date,
                            double end_date) {
    /* Multi-asset streaming requires the provider to support it.
     * Fallback: iterate over each active asset sequentially for all dates.
     * A proper aligned multi-stream is left for Phase 3 provider extension. */

    if (!ctx || !ctx->universe || !ctx->provider) return -1;

    int total = 0;
    size_t na = ctx->universe->num_assets;

    for (size_t i = 0; i < na; i++) {
        const universe_asset_t *a = &ctx->universe->assets[i];
        double s = (start_date > 0) ? start_date : a->start_date;
        double e = (end_date   > 0) ? end_date   : (a->end_date > 0 ? a->end_date : 1e12);

        /* Reset per-asset window */
        if (ctx->windows[i]) bar_window_clear(ctx->windows[i]);

        int rc = strategy_run_single(ctx, a->id, s, e);
        if (rc > 0) total += rc;
    }
    return total;
}

/* =========================================================================
 * Results Access
 * ========================================================================= */

const double *strategy_equity_curve(const strategy_ctx_t *ctx, size_t *out_len) {
    if (!ctx || !ctx->order_mgr) { if (out_len) *out_len = 0; return NULL; }
    if (out_len) *out_len = ctx->order_mgr->equity_len;
    return ctx->order_mgr->equity_curve;
}

size_t strategy_trade_returns(const strategy_ctx_t *ctx, double *out, size_t max) {
    if (!ctx || !ctx->order_mgr) return 0;
    return order_manager_trade_returns(ctx->order_mgr, out, max);
}

double strategy_final_equity(const strategy_ctx_t *ctx) {
    if (!ctx || !ctx->order_mgr) return 0.0;
    return ctx->order_mgr->equity;
}

double strategy_total_return(const strategy_ctx_t *ctx) {
    if (!ctx || !ctx->order_mgr) return 0.0;
    double init = ctx->order_mgr->initial_cash;
    return (init > 1e-9) ? (ctx->order_mgr->equity - init) / init : 0.0;
}
