/**
 * @file strategy.h
 * @brief Event-driven strategy runner — the on_bar coroutine engine.
 *
 * strategy_ctx_t wires together:
 *   - universe_t         (is this asset tradable? what's the adj factor?)
 *   - provider_t         (where does bar data come from?)
 *   - bar_window_t[]     (rolling OHLCV for each asset)
 *   - order_manager_t    (positions, fills, SL/TP, equity curve)
 *   - turbo_script_ctx_t (compiled strategy expression, re-executed each bar)
 *
 * The compiled TurboScript expression IS the on_bar handler.  On each bar,
 * the engine:
 *   1. Adjusts and pushes OHLCV into the per-asset bar_window.
 *   2. Linearizes the window and binds O[], H[], L[], C[], V[] vectors.
 *   3. Binds scalar state variables (position, cash, equity, bar_index, asset_id).
 *   4. Re-evaluates the compiled expression.
 *   5. Reads the strategy's output variables (signal, size, stop_loss, take_profit).
 *   6. Passes the decision through order_manager.
 *   7. Records equity.
 *
 * For live trading, replace the backtest iterator with a coroutine-driven
 * WebSocket/TCP feed — the on_bar logic is identical.
 */
#ifndef STRATEGY_H
#define STRATEGY_H

#include <stddef.h>
#include <stdbool.h>
#include "universe.h"
#include "market_rules.h"
#include "provider.h"
#include "bar_window.h"
#include "order_manager.h"
#include "exprtk_types.h"   /* turbo_arena_t, exprtk_env_t */

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Strategy Configuration
 * ========================================================================= */

/** Warm-up: minimum number of bars required before the strategy fires.
 *  During warm-up, indicators are updated but no orders are submitted. */
#define STRATEGY_DEFAULT_WARMUP  50

typedef struct {
    size_t warmup_bars;     /**< Bars to skip before allowing orders         */
    size_t window_capacity; /**< Rolling window size per asset               */
    double initial_cash;    /**< Starting capital                            */
    double slippage_pct;    /**< Slippage fraction per trade                 */
    double spread;          /**< Fixed half-spread (absolute)                */
} strategy_config_t;

/* Default config */
static inline strategy_config_t strategy_config_default(void) {
    strategy_config_t c = {0};
    c.warmup_bars     = STRATEGY_DEFAULT_WARMUP;
    c.window_capacity = 500;
    c.initial_cash    = 1000000.0;
    c.slippage_pct    = 0.0005;
    c.spread          = 0.0;
    return c;
}

/* =========================================================================
 * Strategy Context
 * ========================================================================= */

typedef struct {
    /* ── External dependencies (not owned) ─────────────────────────────── */
    universe_t          *universe;
    provider_t          *provider;

    /* ── Script engine ──────────────────────────────────────────────────── */
    exprtk_env_t        *env;       /**< TurboScript environment             */
    struct exprtk_parse_ctx_s *parsed; /**< Compiled expression tree         */

    /* ── Per-asset state ────────────────────────────────────────────────── */
    bar_window_t       **windows;   /**< [num_assets] rolling OHLCV buffers  */
    size_t               num_assets;

    /* ── Order management ───────────────────────────────────────────────── */
    order_manager_t     *order_mgr;

    /* ── Bound script variables (output from strategy script) ───────────── */
    double               var_signal;      /**< +1 buy, -1 sell, 0 flat       */
    double               var_size;        /**< Fraction of equity to trade    */
    double               var_stop_loss;
    double               var_take_profit;

    /* ── Simulation stats ───────────────────────────────────────────────── */
    size_t               total_bars;
    size_t               warmup_bars;
    double               current_date;

    strategy_config_t    config;
    turbo_arena_t       *arena;
} strategy_ctx_t;

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

/**
 * @brief Create a strategy context.
 *
 * @param universe  Pre-loaded, finalized universe.
 * @param provider  Data provider for bar streaming.
 * @param cfg       Configuration (use strategy_config_default() for defaults).
 * @param arena     Lifetime arena.
 * @return Context pointer, or NULL on OOM.
 */
strategy_ctx_t *strategy_create(universe_t *universe,
                                  provider_t *provider,
                                  const strategy_config_t *cfg,
                                  turbo_arena_t *arena);

/**
 * @brief Free a strategy context (releases internally owned heap memory).
 */
void strategy_free(strategy_ctx_t *ctx);

/* =========================================================================
 * Script Compilation
 * ========================================================================= */

/**
 * @brief Compile a TurboScript strategy string.
 *
 * The script may reference the following bound variables:
 *
 *   Read-only (updated each bar):
 *     O[]  H[]  L[]  C[]  V[]   — rolling window vectors (adjusted)
 *     n                          — number of valid bars in window
 *     position                   — current signed position in lots
 *     cash                       — available cash
 *     equity                     — total account equity
 *     asset_id                   — numeric ID of current asset
 *     bar_index                  — monotonic bar counter
 *     prev_close                 — previous bar's close (for price limits)
 *
 *   Read-write (written by script → read by engine):
 *     signal                     — +1 buy, -1 sell, 0 no action
 *     size                       — fraction of equity (0..1]
 *     stop_loss                  — SL price level (0 = use prev SL)
 *     take_profit                — TP price level (0 = use prev TP)
 *
 * @param ctx   Strategy context.
 * @param code  TurboScript source code string.
 * @return 0 on success, -1 on parse error.
 */
int strategy_compile(strategy_ctx_t *ctx, const char *code);

/* =========================================================================
 * Backtest Execution (single-asset)
 * ========================================================================= */

/**
 * @brief Run a backtest for a single asset over its full history.
 *
 * @param ctx        Strategy context (must have compiled script).
 * @param asset_id   Asset to backtest.
 * @param start_date Start date (epoch days, 0 = use universe start_date).
 * @param end_date   End date (epoch days, 0 = use universe end_date).
 * @return Number of bars processed, or <0 on error.
 */
int strategy_run_single(strategy_ctx_t *ctx,
                          uint32_t asset_id,
                          double start_date,
                          double end_date);

/* =========================================================================
 * Backtest Execution (multi-asset, full universe)
 * ========================================================================= */

/**
 * @brief Run a backtest across all active assets in the universe.
 *
 * On each trading date, the engine:
 *   1. Calls universe_advance() to update the active mask.
 *   2. Force-closes any delisted positions.
 *   3. For each active asset, calls the on_bar handler.
 *   4. Marks to market and records the equity curve.
 *
 * Requires the provider to support multi-asset streaming
 * (provider->open_multi_stream != NULL), or will fall back to
 * sequentially streaming individual assets for each date.
 *
 * @return Number of date-bars processed, or <0 on error.
 */
int strategy_run_universe(strategy_ctx_t *ctx,
                            double start_date,
                            double end_date);

/* =========================================================================
 * Per-Bar Hook (called internally, exposed for testing)
 * ========================================================================= */

/**
 * @brief Execute the strategy script for one asset on one bar.
 *
 * Updates the bar window, binds all variables, executes the compiled
 * script, and processes the resulting signal through the order manager.
 *
 * @param ctx        Strategy context.
 * @param asset_id   Current asset.
 * @param bar        Bar data (raw, pre-adjustment).
 * @param prev_close Previous bar's close (for price limit validation).
 * @param is_warmup  If true, indicators update but no orders are submitted.
 */
void strategy_on_bar(strategy_ctx_t *ctx,
                      uint32_t asset_id,
                      const provider_bar_t *bar,
                      double prev_close,
                      bool is_warmup);

/* =========================================================================
 * Results Access
 * ========================================================================= */

/**
 * @brief Get the equity curve (one entry per bar recorded).
 * @param out_len  Receives the number of equity points.
 * @return Pointer to the equity curve array (owned by order_mgr).
 */
const double *strategy_equity_curve(const strategy_ctx_t *ctx, size_t *out_len);

/**
 * @brief Get trade-level returns array (pnl_pct per closed trade).
 * @return Number of trades written into out.
 */
size_t strategy_trade_returns(const strategy_ctx_t *ctx, double *out, size_t max);

/**
 * @brief Final equity value.
 */
double strategy_final_equity(const strategy_ctx_t *ctx);

/**
 * @brief Total return as a fraction of initial capital.
 */
double strategy_total_return(const strategy_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* STRATEGY_H */
