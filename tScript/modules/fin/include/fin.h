/**
 * @file fin.h
 * @brief Finance module public API.
 *
 * Declares Finance-domain C APIs used by the fin module
 * (backtest, risk, portfolio, signals, factors, market microstructure).
 */
#ifndef FIN_H
#define FIN_H

#include "exprtk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* Backtest                                                                  */
/* ========================================================================= */
size_t exprtk_bt_backtest(const double *open, const double *close, const double *signal, size_t n, double cash0, double commission, double *equity, double *trades);
size_t exprtk_bt_stats(const double *equity, const double *trades, size_t n, size_t num_trades, double annual, double *out);
size_t exprtk_bt_backtest_ex(const double *open, const double *high, const double *low, const double *close, const double *volume, const double *signal, size_t n, double cash0, double commission, const double *config, size_t config_n, double *equity, double *trades, turbo_arena_t *arena);
size_t exprtk_bt_portfolio(const double *prices, const double *signals, size_t na, size_t nb, double cash0, double commission, double *equity, double *weights, turbo_arena_t *arena);
double exprtk_bt_slippage(double price, double size, double vol, double avg_vol, double lambda);
double exprtk_bt_cost(double price, double size, double commission_pct, double tax_pct, double slippage);

/* Portfolio */
void   exprtk_pf_cov_matrix(const double *returns, size_t na, size_t np, double *out, turbo_arena_t *arena);
double exprtk_pf_min_variance(const double *cov, size_t n, double *weights, turbo_arena_t *arena);
double exprtk_pf_max_sharpe(const double *mu, const double *cov, size_t n, double rf, double *weights, turbo_arena_t *arena);
double exprtk_pf_markowitz(const double *mu, const double *cov, size_t n, double target, double *weights, turbo_arena_t *arena);
double exprtk_pf_risk_parity(const double *cov, size_t n, double *weights, turbo_arena_t *arena);

/* Risk */
int    exprtk_var_hist(const double *returns, size_t n, double confidence, double *out, turbo_arena_t *arena);
int    exprtk_var_param(const double *returns, size_t n, double confidence, double *out);
int    exprtk_cvar(const double *returns, size_t n, double confidence, double *out, turbo_arena_t *arena);
double exprtk_kelly(double win_rate, double avg_win, double avg_loss);
double exprtk_fixed_frac(double equity, double risk_pct, double stop_dist);
double exprtk_optimal_f(const double *trades, size_t n, double *out);
double exprtk_drawdown(const double *equity, size_t n, double *out);
int    exprtk_drawdown_stats(const double *equity, size_t n, double *out);
size_t exprtk_mc_simulate(double s0, double mu, double sigma, double dt, size_t steps, size_t paths, double *out, turbo_arena_t *arena);

/* Performance Metrics */
double exprtk_sharpe(const double *returns, size_t n, double rf, double annual_factor);
double exprtk_sortino(const double *returns, size_t n, double rf, double annual_factor);
double exprtk_calmar(const double *equity, size_t n, double annual_factor);
double exprtk_profit_factor(const double *trades, size_t n);
double exprtk_expectancy(const double *trades, size_t n);
double exprtk_payoff_ratio(const double *trades, size_t n);
size_t exprtk_max_dd_duration(const double *equity, size_t n);
double exprtk_ulcer_index(const double *equity, size_t n);
double exprtk_information_ratio(const double *ret, const double *bench, size_t n);
double exprtk_treynor(const double *ret, const double *bench, size_t n, double rf);

/* Signal */
size_t exprtk_crossover(const double *fast, const double *slow, size_t n, double *out);
size_t exprtk_crossunder(const double *fast, const double *slow, size_t n, double *out);
size_t exprtk_signal_combine(const double *signals, const double *weights, size_t n_bars, size_t n_signals, double *out);
size_t exprtk_candle_doji(const double *O, const double *H, const double *L, const double *C, size_t n, double threshold, double *out);
size_t exprtk_candle_hammer(const double *O, const double *H, const double *L, const double *C, size_t n, double *out);
size_t exprtk_candle_engulfing(const double *O, const double *H, const double *L, const double *C, size_t n, double *out);
size_t exprtk_candle_morningstar(const double *O, const double *H, const double *L, const double *C, size_t n, double *out);

 
/* ── Factor Processing ── */
size_t exprtk_vec_rank(const double *in, size_t n, double *out, turbo_arena_t *arena);
size_t exprtk_vec_zscore(const double *in, size_t n, double *out, turbo_arena_t *arena);
size_t exprtk_vec_winsorize(const double *in, size_t n, double limit_pct, double *out, turbo_arena_t *arena);
size_t exprtk_vec_standardize(const double *in, size_t n, double *out, turbo_arena_t *arena);
size_t exprtk_ta_zscore(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena);
double exprtk_vec_entropy(const double *in, size_t n, size_t bins);
double exprtk_vec_skewness(const double *in, size_t n);
double exprtk_vec_kurtosis(const double *in, size_t n);
double exprtk_vec_vol_ratio(const double *vol, size_t n, size_t segment_len, size_t segment_idx);
double exprtk_vec_rvar(const double *in, size_t n);
double exprtk_vec_rskew(const double *in, size_t n);
double exprtk_vec_rkurt(const double *in, size_t n);
double exprtk_vec_illiq(const double *ret, const double *amount, size_t n);
double exprtk_vec_trend_strength(const double *price, size_t n);
size_t exprtk_vec_csad(const double *rets, size_t na, size_t np, double *out, turbo_arena_t *arena);
double exprtk_vec_fvd(const double *vol, size_t n, size_t window_size);
double exprtk_vec_efficiency(const double *price, size_t n);
double exprtk_vec_rsj(const double *ret, size_t n);
double exprtk_vec_apm(const double *ret_am, const double *ret_pm, size_t n);
size_t exprtk_vec_cgo(const double *p, const double *v, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_vec_quantile(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena);
double exprtk_vec_salience(const double *ret, const double *mkt_ret, size_t n, double delta);
double exprtk_vec_str(const double *ret, const double *mkt_ret, size_t n, double delta);

/* ── Tick / Order Book Analysis ── */
double exprtk_tick_ofi(double b_p, double b_v, double a_p, double a_v,
                       double pb_p, double pb_v, double pa_p, double pa_v);
double exprtk_tick_imbalance(const double *bids_v, const double *asks_v, size_t depth);
double exprtk_tick_limit_status(double price, double prev_close, double limit_pct);

/* ── Event-driven strategy module (buy/sell/flat, rank, risk sizing, ...) ── */
const exprtk_module_t *exprtk_module_strategy(void);

#ifdef __cplusplus
}
#endif

#endif /* FIN_H */
