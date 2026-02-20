/**
 * @file exprtk.h
 * @brief exprtk-like Parser Public API
 */

#ifndef exprtk_H
#define exprtk_H

#include "exprtk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Parse the input string into an AST
exprtk_node_t *exprtk_parse(const char *input, size_t length);

// Clean up (frees the arena)
void exprtk_free(exprtk_node_t *node);

// Evaluate AST
exprtk_value_t exprtk_eval(const exprtk_node_t *node, exprtk_env_t *env);

// Complexity checks
size_t exprtk_node_count(const exprtk_node_t *node);
size_t exprtk_node_depth(const exprtk_node_t *node);

// Node allocation (internal/parser use)
exprtk_node_t *exprtk_node_create(turbo_arena_t *arena, exprtk_node_type_t type);

// Environment management
void exprtk_env_init(exprtk_env_t *env);
void exprtk_env_free(exprtk_env_t *env);
void exprtk_env_set(exprtk_env_t *env, const char *name, exprtk_value_t value);
exprtk_value_t exprtk_env_get(exprtk_env_t *env, const char *name);
void exprtk_env_register_func(exprtk_env_t *env, const char *name, exprtk_native_fn fn, void *user_data);
void exprtk_env_set_constant(exprtk_env_t *env, const char *name, exprtk_value_t value);

// Math Functions
double exprtk_median(const double *data, size_t n, turbo_arena_t *arena);
double exprtk_percentile(const double *data, size_t n, double p, turbo_arena_t *arena);
double exprtk_geometric_mean(const double *data, size_t n);
double exprtk_harmonic_mean(const double *data, size_t n);
double exprtk_skewness(const double *data, size_t n);
double exprtk_kurtosis(const double *data, size_t n);

double exprtk_fibonacci(int n);
long long exprtk_gcd(long long u, long long v);
double exprtk_normal_rand(double mu, double sigma);

double exprtk_det2(const double *A);
double exprtk_det3(const double *A);
int    exprtk_inv2(const double *A, double *out);
int    exprtk_inv3(const double *A, double *out);
void   exprtk_matmul(const double *A, const double *B, size_t m, size_t k, size_t n, double *out);
void   exprtk_transpose(const double *A, size_t rows, size_t cols, double *out);
int    exprtk_eig2(const double *A, double *ev);
int    exprtk_eig3(const double *A, double *ev);
double exprtk_trace2(const double *A);

// Backtest
size_t exprtk_bt_backtest(const double *open, const double *close, const double *signal, size_t n, double cash0, double commission, double *equity, double *trades);
size_t exprtk_bt_stats(const double *equity, const double *trades, size_t n, size_t num_trades, double annual, double *out);
size_t exprtk_bt_backtest_ex(const double *open, const double *high, const double *low, const double *close, const double *volume, const double *signal, size_t n, double cash0, double commission, const double *config, size_t config_n, double *equity, double *trades, turbo_arena_t *arena);
size_t exprtk_bt_portfolio(const double *prices, const double *signals, size_t na, size_t nb, double cash0, double commission, double *equity, double *weights, turbo_arena_t *arena);

// Portfolio
void   exprtk_pf_cov_matrix(const double *returns, size_t na, size_t np, double *out, turbo_arena_t *arena);
double exprtk_pf_min_variance(const double *cov, size_t n, double *weights, turbo_arena_t *arena);
double exprtk_pf_max_sharpe(const double *mu, const double *cov, size_t n, double rf, double *weights, turbo_arena_t *arena);
double exprtk_pf_markowitz(const double *mu, const double *cov, size_t n, double target, double *weights, turbo_arena_t *arena);
double exprtk_pf_risk_parity(const double *cov, size_t n, double *weights, turbo_arena_t *arena);

// Risk
int    exprtk_var_hist(const double *returns, size_t n, double confidence, double *out, turbo_arena_t *arena);
int    exprtk_var_param(const double *returns, size_t n, double confidence, double *out);
int    exprtk_cvar(const double *returns, size_t n, double confidence, double *out, turbo_arena_t *arena);
double exprtk_kelly(double win_rate, double avg_win, double avg_loss);
double exprtk_fixed_frac(double equity, double risk_pct, double stop_dist);
double exprtk_optimal_f(const double *trades, size_t n, double *out);
double exprtk_drawdown(const double *equity, size_t n, double *out);
int    exprtk_drawdown_stats(const double *equity, size_t n, double *out);

// Signal
size_t exprtk_crossover(const double *fast, const double *slow, size_t n, double *out);
size_t exprtk_crossunder(const double *fast, const double *slow, size_t n, double *out);
size_t exprtk_signal_combine(const double *signals, const double *weights, size_t n_bars, size_t n_signals, double *out);
size_t exprtk_candle_doji(const double *O, const double *H, const double *L, const double *C, size_t n, double threshold, double *out);
size_t exprtk_candle_hammer(const double *O, const double *H, const double *L, const double *C, size_t n, double *out);
size_t exprtk_candle_engulfing(const double *O, const double *H, const double *L, const double *C, size_t n, double *out);
size_t exprtk_candle_morningstar(const double *O, const double *H, const double *L, const double *C, size_t n, double *out);

// Timeseries
size_t exprtk_ts_diff(const double *data, size_t n, size_t order, double *out, turbo_arena_t *arena);
size_t exprtk_ts_autocorr(const double *data, size_t n, size_t max_lag, double *out);
size_t exprtk_ts_pacf(const double *data, size_t n, size_t max_lag, double *out, turbo_arena_t *arena);
int    exprtk_ts_adf(const double *data, size_t n, size_t p, double *out, turbo_arena_t *arena);
size_t exprtk_ts_garch(const double *returns, size_t n, double alpha, double beta, double *out);
double exprtk_ts_hurst(const double *data, size_t n, double *out, turbo_arena_t *arena);

// TA-Lib Overlap
size_t exprtk_ta_sma(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_ema(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_wma(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_dema(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_tema(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_kama(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_t3(const double *in, size_t n, size_t period, double vfactor, double *out, turbo_arena_t *arena);
size_t exprtk_ta_trima(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_bbands(const double *in, size_t n, size_t period, double mult, double *upper, double *middle, double *lower);
size_t exprtk_ta_midpoint(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_midprice(const double *hi, const double *lo, size_t n, size_t period, double *out);
size_t exprtk_ta_sar(const double *hi, const double *lo, size_t n, double accel_init, double accel_max, double *out);
size_t exprtk_ta_savgol(const double *in, size_t n, size_t window, double *out);

// TA-Lib Momentum
size_t exprtk_ta_rsi(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_macd(const double *in, size_t n, size_t fast_p, size_t slow_p, size_t sig_p, double *line, double *sig, double *hist, turbo_arena_t *arena);
size_t exprtk_ta_stoch(const double *hi, const double *lo, const double *cl, size_t n, size_t k_p, size_t d_p, double *out_k, double *out_d, turbo_arena_t *arena);
size_t exprtk_ta_stochrsi(const double *in, size_t n, size_t rsi_p, size_t k_p, size_t d_p, double *out_k, double *out_d, turbo_arena_t *arena);
size_t exprtk_ta_willr(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out);
size_t exprtk_ta_cci(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_mom(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_roc(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_apo(const double *in, size_t n, size_t fast_p, size_t slow_p, double *out, turbo_arena_t *arena);
size_t exprtk_ta_ppo(const double *in, size_t n, size_t fast_p, size_t slow_p, double *out, turbo_arena_t *arena);
size_t exprtk_ta_trix(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_ultosc(const double *hi, const double *lo, const double *cl, size_t n, size_t p1, size_t p2, size_t p3, double *out, turbo_arena_t *arena);
size_t exprtk_ta_aroon(const double *hi, const double *lo, size_t n, size_t period, double *up, double *dn);
size_t exprtk_ta_aroonosc(const double *hi, const double *lo, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_cmo(const double *in, size_t n, size_t period, double *out);

// TA-Lib Volatility
size_t exprtk_ta_trange(const double *hi, const double *lo, const double *cl, size_t n, double *out);
size_t exprtk_ta_atr(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_natr(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena);

// TA-Lib Volume
size_t exprtk_ta_obv(const double *cl, const double *vol, size_t n, double *out);
size_t exprtk_ta_ad(const double *hi, const double *lo, const double *cl, const double *vol, size_t n, double *out);
size_t exprtk_ta_adosc(const double *hi, const double *lo, const double *cl, const double *vol, size_t n, size_t fast_p, size_t slow_p, double *out, turbo_arena_t *arena);
size_t exprtk_ta_mfi(const double *hi, const double *lo, const double *cl, const double *vol, size_t n, size_t period, double *out, turbo_arena_t *arena);

// TA-Lib Trend
size_t exprtk_ta_plus_di(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_minus_di(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_dx(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_adx(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_adxr(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena);

// TA-Lib Statistics
size_t exprtk_ta_stddev(const double *in, size_t n, size_t period, double mult, double *out, turbo_arena_t *arena);
size_t exprtk_ta_var(const double *in, size_t n, size_t period, double mult, double *out, turbo_arena_t *arena);
size_t exprtk_ta_linearreg(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_linearreg_slope(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_linearreg_intercept(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_linearreg_angle(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_tsf(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_beta(const double *in0, const double *in1, size_t n, size_t period, double *out);
size_t exprtk_ta_correl(const double *in0, const double *in1, size_t n, size_t period, double *out);

// TA-Lib Price
size_t exprtk_ta_avgprice(const double *o, const double *hi, const double *lo, const double *cl, size_t n, double *out);
size_t exprtk_ta_medprice(const double *hi, const double *lo, size_t n, double *out);
size_t exprtk_ta_typprice(const double *hi, const double *lo, const double *cl, size_t n, double *out);
size_t exprtk_ta_wclprice(const double *hi, const double *lo, const double *cl, size_t n, double *out);

// TA-Lib Options
size_t exprtk_ta_bsm_call(const double *S, const double *K, const double *T, const double *r, const double *sigma, size_t n, double *out);
size_t exprtk_ta_bsm_put(const double *S, const double *K, const double *T, const double *r, const double *sigma, size_t n, double *out);
size_t exprtk_ta_bsm_delta_call(const double *S, const double *K, const double *T, const double *r, const double *sigma, size_t n, double *out);
size_t exprtk_ta_bsm_delta_put(const double *S, const double *K, const double *T, const double *r, const double *sigma, size_t n, double *out);
size_t exprtk_ta_bsm_gamma(const double *S, const double *K, const double *T, const double *r, const double *sigma, size_t n, double *out);
size_t exprtk_ta_bsm_theta_call(const double *S, const double *K, const double *T, const double *r, const double *sigma, size_t n, double *out);
size_t exprtk_ta_bsm_theta_put(const double *S, const double *K, const double *T, const double *r, const double *sigma, size_t n, double *out);
size_t exprtk_ta_bsm_vega(const double *S, const double *K, const double *T, const double *r, const double *sigma, size_t n, double *out);
size_t exprtk_ta_bsm_rho_call(const double *S, const double *K, const double *T, const double *r, const double *sigma, size_t n, double *out);
size_t exprtk_ta_bsm_rho_put(const double *S, const double *K, const double *T, const double *r, const double *sigma, size_t n, double *out);
size_t exprtk_ta_bsm_iv_call(const double *price, const double *S, const double *K, const double *T, const double *r, size_t n, double *out);
size_t exprtk_ta_bsm_iv_put(const double *price, const double *S, const double *K, const double *T, const double *r, size_t n, double *out);
size_t exprtk_ta_opt_binomial(double S, double K, double T, double r, double sigma, size_t steps, int is_call, double *out, turbo_arena_t *arena);

#ifdef __cplusplus
}
#endif

#endif // exprtk_H

