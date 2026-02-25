/**
 * @file fin.h
 * @brief Finance, TA, and Timeseries module public API.
 *
 * Provides per-env module descriptors (ta, finance, timeseries) and
 * the underlying C-level indicator / backtest / risk functions.
 */
#ifndef FIN_H
#define FIN_H

#include "exprtk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Static module accessors — mount via exprtk_env_add_module() */
const exprtk_module_t *exprtk_module_ta(void);
const exprtk_module_t *exprtk_module_finance(void);
const exprtk_module_t *exprtk_module_timeseries(void);

/* ========================================================================= */
/* TA-Lib Overlap                                                            */
/* ========================================================================= */
size_t exprtk_ta_sma(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_ema(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_wma(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_dema(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_tema(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_kama(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_t3(const double *in, size_t n, size_t period, double vfactor, double *out, turbo_arena_t *arena);
size_t exprtk_ta_trima(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_bbands(const double *in, size_t n, size_t period, double mult, double *upper, double *middle, double *lower);
size_t exprtk_ta_midpoint(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_midprice(const double *hi, const double *lo, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_sar(const double *hi, const double *lo, size_t n, double accel_init, double accel_max, double *out);
size_t exprtk_ta_savgol(const double *in, size_t n, size_t window, double *out);
size_t exprtk_ta_bbi(const double *in, size_t n, double *out, turbo_arena_t *arena);
size_t exprtk_ta_hma(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_supertrend(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double mult, double *trend, double *upper, double *lower, turbo_arena_t *arena);

size_t exprtk_ta_vwap(const double *hi, const double *lo, const double *cl, const double *vol, size_t n, double *out);
size_t exprtk_ta_donchian(const double *hi, const double *lo, size_t n, size_t period, double *upper, double *lower, double *middle, turbo_arena_t *arena);
size_t exprtk_ta_keltner(const double *hi, const double *lo, const double *cl, size_t n, size_t ema_p, size_t atr_p, double mult, double *upper, double *middle, double *lower, turbo_arena_t *arena);
size_t exprtk_ta_ichimoku(const double *hi, const double *lo, const double *cl, size_t n, size_t tenkan_p, size_t kijun_p, size_t senkou_p, double *tenkan, double *kijun, double *senkou_a, double *senkou_b, double *chikou, turbo_arena_t *arena);

size_t exprtk_ta_pivot_high(const double *hi, size_t n, size_t left, size_t right, double *out);
size_t exprtk_ta_pivot_low (const double *lo, size_t n, size_t left, size_t right, double *out);
size_t exprtk_ta_rma(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_zlema(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_alma(const double *in, size_t n, size_t period, double offset, double sigma, double *out, turbo_arena_t *arena);
size_t exprtk_ta_vidya(const double *in, size_t n, size_t cmo_p, size_t ema_p, double *out, turbo_arena_t *arena);
size_t exprtk_ta_rvi(const double *in, size_t n, size_t std_p, size_t ema_p, double *out, turbo_arena_t *arena);
size_t exprtk_ta_vhf(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_volatility_ratio(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_er(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_bias(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_psy(const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_pressure(const double *hi, const double *lo, const double *cl, const double *vol, size_t n, double *buy_pres, double *sell_pres);
size_t exprtk_ta_kvo(const double *hi, const double *lo, const double *cl, const double *vol, size_t n, size_t fast_p, size_t slow_p, size_t sig_p, double *kvo, double *sig, turbo_arena_t *arena);
size_t exprtk_ta_arbr(const double *hi, const double *lo, const double *op, const double *cl, size_t n, size_t period, double *ar, double *br);
size_t exprtk_candle_body_size  (const double *O, const double *C, const double *H, const double *L, size_t n, double *out);
size_t exprtk_candle_wick_upper (const double *O, const double *C, const double *H, size_t n, double *out);
size_t exprtk_candle_wick_lower (const double *O, const double *C, const double *L, size_t n, double *out);
size_t exprtk_candle_direction  (const double *O, const double *C, size_t n, double *out);
size_t exprtk_candle_fuzzy_bull(const double *O, const double *H, const double *L, const double *C, size_t n, double *out);
size_t exprtk_candle_fuzzy_bear(const double *O, const double *H, const double *L, const double *C, size_t n, double *out);








/* TA-Lib Momentum */
size_t exprtk_ta_rsi(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_macd(const double *in, size_t n, size_t fast_p, size_t slow_p, size_t sig_p, double *line, double *sig, double *hist, turbo_arena_t *arena);
size_t exprtk_ta_stoch(const double *hi, const double *lo, const double *cl, size_t n, size_t k_p, size_t d_p, double *out_k, double *out_d, turbo_arena_t *arena);
size_t exprtk_ta_stochrsi(const double *in, size_t n, size_t rsi_p, size_t k_p, size_t d_p, double *out_k, double *out_d, turbo_arena_t *arena);
size_t exprtk_ta_willr(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_cci(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_mom(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_roc(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_apo(const double *in, size_t n, size_t fast_p, size_t slow_p, double *out, turbo_arena_t *arena);
size_t exprtk_ta_ppo(const double *in, size_t n, size_t fast_p, size_t slow_p, double *out, turbo_arena_t *arena);
size_t exprtk_ta_trix(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_ultosc(const double *hi, const double *lo, const double *cl, size_t n, size_t p1, size_t p2, size_t p3, double *out, turbo_arena_t *arena);
size_t exprtk_ta_aroon(const double *hi, const double *lo, size_t n, size_t period, double *up, double *dn, turbo_arena_t *arena);

size_t exprtk_ta_aroonosc(const double *hi, const double *lo, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_cmo(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena);

/* TA-Lib Volatility */
size_t exprtk_ta_trange(const double *hi, const double *lo, const double *cl, size_t n, double *out);
size_t exprtk_ta_atr(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_rsrs(const double *hi, const double *lo, size_t n, size_t n_reg, size_t m_z, double *slope, double *zscore, turbo_arena_t *arena);
size_t exprtk_ta_smart_money(const double *p, const double *v, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_qrs(const double *slope, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_vmacd_mtm(const double *v, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_noise_area(const double *op, const double *cl, size_t n, size_t period, double *upper, double *lower, turbo_arena_t *arena);
size_t exprtk_ta_w_factor(const double *ret, const double *amt, const double *cnt, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_cpv(const double *ret, const double *vol, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_smma(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_alligator(const double *in, size_t n, double *jaw, double *teeth, double *lips, turbo_arena_t *arena);
size_t exprtk_ta_natr(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena);

/* TA-Lib Volume */
size_t exprtk_ta_obv(const double *cl, const double *vol, size_t n, double *out);
size_t exprtk_ta_ad(const double *hi, const double *lo, const double *cl, const double *vol, size_t n, double *out);
size_t exprtk_ta_shadow(const double *op, const double *hi, const double *lo, const double *cl, size_t n, double *upper, double *lower, turbo_arena_t *arena);
size_t exprtk_ta_adosc(const double *hi, const double *lo, const double *cl, const double *vol, size_t n, size_t fast_p, size_t slow_p, double *out, turbo_arena_t *arena);
size_t exprtk_ta_mfi(const double *hi, const double *lo, const double *cl, const double *vol, size_t n, size_t period, double *out, turbo_arena_t *arena);

/* TA-Lib Trend */
size_t exprtk_ta_plus_dm(const double *hi, const double *lo, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_minus_dm(const double *hi, const double *lo, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_plus_di(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_minus_di(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_dx(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_adx(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_adxr(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena);

/* TA-Lib Statistics */
size_t exprtk_ta_stddev(const double *in, size_t n, size_t period, double mult, double *out, turbo_arena_t *arena);
size_t exprtk_ta_var(const double *in, size_t n, size_t period, double mult, double *out, turbo_arena_t *arena);
size_t exprtk_ta_linearreg(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_linearreg_slope(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_linearreg_intercept(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_linearreg_angle(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_tsf(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_beta(const double *in0, const double *in1, size_t n, size_t period, double *out);
size_t exprtk_ta_correl(const double *in0, const double *in1, size_t n, size_t period, double *out);

/* TA-Lib Price */
size_t exprtk_ta_avgprice(const double *o, const double *hi, const double *lo, const double *cl, size_t n, double *out);
size_t exprtk_ta_medprice(const double *hi, const double *lo, size_t n, double *out);
size_t exprtk_ta_typprice(const double *hi, const double *lo, const double *cl, size_t n, double *out);
size_t exprtk_ta_wclprice(const double *hi, const double *lo, const double *cl, size_t n, double *out);

/* TA-Lib Options */
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

/* Signal */
size_t exprtk_crossover(const double *fast, const double *slow, size_t n, double *out);
size_t exprtk_crossunder(const double *fast, const double *slow, size_t n, double *out);
size_t exprtk_signal_combine(const double *signals, const double *weights, size_t n_bars, size_t n_signals, double *out);
size_t exprtk_candle_doji(const double *O, const double *H, const double *L, const double *C, size_t n, double threshold, double *out);
size_t exprtk_candle_hammer(const double *O, const double *H, const double *L, const double *C, size_t n, double *out);
size_t exprtk_candle_engulfing(const double *O, const double *H, const double *L, const double *C, size_t n, double *out);
size_t exprtk_candle_morningstar(const double *O, const double *H, const double *L, const double *C, size_t n, double *out);

/* Timeseries */
size_t exprtk_ts_diff(const double *data, size_t n, size_t order, double *out, turbo_arena_t *arena);
size_t exprtk_ts_autocorr(const double *data, size_t n, size_t max_lag, double *out);
size_t exprtk_ts_pacf(const double *data, size_t n, size_t max_lag, double *out, turbo_arena_t *arena);
int    exprtk_ts_adf(const double *data, size_t n, size_t p, double *out, turbo_arena_t *arena);
size_t exprtk_ts_garch(const double *returns, size_t n, double alpha, double beta, double *out);
size_t exprtk_ts_match(const double *data, const double *pattern, size_t n, size_t m, double *out);
size_t exprtk_ts_match_cosine(const double *data, const double *pattern, size_t n, size_t m, double *out);
size_t exprtk_ts_match_normalized(const double *data, const double *pattern, size_t n, size_t m, double *out, turbo_arena_t *arena);
size_t exprtk_ts_match_correl(const double *data, const double *pattern, size_t n, size_t m, double *out, turbo_arena_t *arena);
size_t exprtk_ts_match_returns(const double *data, const double *pattern, size_t n, size_t m, double *out, turbo_arena_t *arena);
size_t exprtk_ts_match_candle(const double *O, const double *H, const double *L, const double *C,
                            const double *pO, const double *pH, const double *pL, const double *pC,
                            size_t n, size_t m, double *out, turbo_arena_t *arena);
size_t exprtk_ts_match_dtw(const double *data, const double *pattern, size_t n, size_t m, double *out, turbo_arena_t *arena);
size_t exprtk_ts_dwt(const double *data, size_t n, size_t levels, double *approx, double *detail, turbo_arena_t *arena);
size_t exprtk_ts_emd(const double *data, size_t n, size_t max_imfs, double *imfs, turbo_arena_t *arena);
double exprtk_ts_hurst(const double *data, size_t n, double *out, turbo_arena_t *arena);

/* ── Factor Processing ── */
size_t exprtk_vec_rank(const double *in, size_t n, double *out, turbo_arena_t *arena);
size_t exprtk_vec_zscore(const double *in, size_t n, double *out, turbo_arena_t *arena);
size_t exprtk_vec_winsorize(const double *in, size_t n, double limit_pct, double *out, turbo_arena_t *arena);
double exprtk_vec_standardize(const double *in, size_t n, double *out, turbo_arena_t *arena);
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
