/**
 * @file exprtk_ta.c
 * @brief Technical Analysis indicators for exprtk
 * Extracted from monolithic exprtk.c during refactoring.
 */

#include "exprtk_internal.h"
void ta_sma_calc(const double *src, size_t len, size_t period, double *dst) {
    if (period == 0 || period > len) return;
    double sum = 0;
    for (size_t i = 0; i < period; ++i) sum += src[i];
    dst[period - 1] = sum / (double)period;
    for (size_t i = period; i < len; ++i) {
        sum += src[i] - src[i - period];
        dst[i] = sum / (double)period;
    }
}

void ta_ema_calc(const double *src, size_t len, size_t period, double *dst) {
    if (len == 0 || period == 0) return;
    double alpha = 2.0 / (double)(period + 1);
    dst[0] = src[0];
    for (size_t i = 1; i < len; ++i)
        dst[i] = alpha * src[i] + (1.0 - alpha) * dst[i - 1];
}

void ta_wilder_smooth(const double *src, size_t len, size_t period, double *dst) {
    if (period == 0 || period > len) return;
    double sum = 0;
    for (size_t i = 0; i < period; ++i) sum += src[i];
    dst[period - 1] = sum / (double)period;
    for (size_t i = period; i < len; ++i)
        dst[i] = (dst[i - 1] * (double)(period - 1) + src[i]) / (double)period;
}

double ta_highest(const double *src, size_t idx, size_t period) {
    double h = src[idx];
    for (size_t j = 1; j < period; ++j) if (src[idx - j] > h) h = src[idx - j];
    return h;
}

double ta_lowest(const double *src, size_t idx, size_t period) {
    double l = src[idx];
    for (size_t j = 1; j < period; ++j) if (src[idx - j] < l) l = src[idx - j];
    return l;
}

size_t ta_highest_idx(const double *src, size_t idx, size_t period) {
    size_t hi = idx;
    for (size_t j = 1; j < period; ++j) if (src[idx - j] > src[hi]) hi = idx - j;
    return hi;
}

size_t ta_lowest_idx(const double *src, size_t idx, size_t period) {
    size_t lo = idx;
    for (size_t j = 1; j < period; ++j) if (src[idx - j] < src[lo]) lo = idx - j;
    return lo;
}

double ta_true_range(double high, double low, double prev_close) {
    double hl = high - low;
    double hc = fabs(high - prev_close);
    double lc = fabs(low - prev_close);
    return fmax(hl, fmax(hc, lc));
}

void ta_true_range_arr(const double *high, const double *low, const double *close, size_t len, double *dst) {
    if (len == 0) return;
    dst[0] = high[0] - low[0];
    for (size_t i = 1; i < len; ++i) dst[i] = ta_true_range(high[i], low[i], close[i - 1]);
}

void ta_linreg(const double *src, size_t end_idx, size_t period, double *slope, double *intercept) {
    double sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0;
    double n = (double)period;
    for (size_t j = 0; j < period; ++j) {
        double x = (double)j;
        double y = src[end_idx - period + 1 + j];
        sum_x += x; sum_y += y; sum_xy += x * y; sum_x2 += x * x;
    }
    double denom = n * sum_x2 - sum_x * sum_x;
    if (fabs(denom) < 1e-15) { *slope = 0; *intercept = sum_y / n; }
    else { *slope = (n * sum_xy - sum_x * sum_y) / denom; *intercept = (sum_y - (*slope) * sum_x) / n; }
}

double ta_ncdf(double x) {
    static const double a1 = 0.254829592, a2 = -0.284496736, a3 = 1.421413741, a4 = -1.453152027, a5 = 1.061405429, p = 0.3275911;
    double sign = (x < 0) ? -1.0 : 1.0;
    x = fabs(x) / sqrt(2.0);
    double t = 1.0 / (1.0 + p * x);
    double y = 1.0 - (((((a5 * t + a4) * t) + a3) * t + a2) * t + a1) * t * exp(-x * x);
    return 0.5 * (1.0 + sign * y);
}

double ta_npdf(double x) {
    return 0.3989422804014327 * exp(-0.5 * x * x);
}

// TA-Lib Overlap Indicators
size_t exprtk_ta_sma(const double *in, size_t n, size_t period, double *out) {
    if (period == 0 || period > n) return 0;
    ta_sma_calc(in, n, period, out);
    return n;
}

size_t exprtk_ta_ema(const double *in, size_t n, size_t period, double *out) {
    if (period == 0 || period > n) return 0;
    ta_ema_calc(in, n, period, out);
    return n;
}

size_t exprtk_ta_wma(const double *in, size_t n, size_t period, double *out) {
    if (period == 0 || period > n) return 0;
    double weight_sum = (double)(period * (period + 1)) / 2.0;
    for (size_t i = period - 1; i < n; ++i) {
        double sum = 0;
        for (size_t j = 0; j < period; ++j) sum += in[i - j] * (double)(period - j);
        out[i] = sum / weight_sum;
    }
    return n;
}

size_t exprtk_ta_dema(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena) {
    if (period == 0 || period > n) return 0;
    double *e1 = TEMP_ALLOC(arena, n);
    double *e2 = TEMP_ALLOC(arena, n);
    if (!e1 || !e2) { TEMP_FREE(arena, e1); TEMP_FREE(arena, e2); return 0; }
    ta_ema_calc(in, n, period, e1);
    ta_ema_calc(e1, n, period, e2);
    for (size_t i = (period > 0 ? 2 * period - 2 : 0); i < n; ++i) {
        if (i < n) out[i] = 2.0 * e1[i] - e2[i];
    }
    TEMP_FREE(arena, e1); TEMP_FREE(arena, e2); return n;
}

size_t exprtk_ta_tema(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena) {
    if (period == 0 || period > n) return 0;
    double *e1 = TEMP_ALLOC(arena, n);
    double *e2 = TEMP_ALLOC(arena, n);
    double *e3 = TEMP_ALLOC(arena, n);
    if (!e1 || !e2 || !e3) { TEMP_FREE(arena, e1); TEMP_FREE(arena, e2); TEMP_FREE(arena, e3); return 0; }
    ta_ema_calc(in, n, period, e1);
    ta_ema_calc(e1, n, period, e2);
    ta_ema_calc(e2, n, period, e3);
    for (size_t i = (period > 0 ? 3 * period - 3 : 0); i < n; ++i) {
        if (i < n) out[i] = 3.0 * e1[i] - 3.0 * e2[i] + e3[i];
    }
    TEMP_FREE(arena, e1); TEMP_FREE(arena, e2); TEMP_FREE(arena, e3); return n;
}

size_t exprtk_ta_kama(const double *in, size_t n, size_t period, double *out) {
    if (period == 0 || period >= n) return 0;
    double kama = in[period - 1]; out[period - 1] = kama;
    const double fast_sc = 2.0 / 3.0, slow_sc = 2.0 / 31.0;
    for (size_t i = period; i < n; ++i) {
        double change = fabs(in[i] - in[i - period]);
        double volatility = 0;
        for (size_t j = 0; j < period; ++j) volatility += fabs(in[i - j] - in[i - j - 1]);
        double er = (volatility > 0) ? change / volatility : 0;
        double sc = pow(er * (fast_sc - slow_sc) + slow_sc, 2.0);
        kama = kama + sc * (in[i] - kama);
        out[i] = kama;
    }
    return n;
}

size_t exprtk_ta_t3(const double *in, size_t n, size_t period, double vfactor, double *out, turbo_arena_t *arena) {
    if (period == 0 || period > n) return 0;
    double *e1 = TEMP_ALLOC(arena, n);
    double *e2 = TEMP_ALLOC(arena, n);
    double *e3 = TEMP_ALLOC(arena, n);
    double *e4 = TEMP_ALLOC(arena, n);
    double *e5 = TEMP_ALLOC(arena, n);
    double *e6 = TEMP_ALLOC(arena, n);
    if (!e1 || !e2 || !e3 || !e4 || !e5 || !e6) { 
        TEMP_FREE(arena, e1); TEMP_FREE(arena, e2); TEMP_FREE(arena, e3); TEMP_FREE(arena, e4); TEMP_FREE(arena, e5); TEMP_FREE(arena, e6);
        return 0; 
    }
    ta_ema_calc(in, n, period, e1); ta_ema_calc(e1, n, period, e2);
    ta_ema_calc(e2, n, period, e3); ta_ema_calc(e3, n, period, e4);
    ta_ema_calc(e4, n, period, e5); ta_ema_calc(e5, n, period, e6);
    double c1 = -vfactor * vfactor * vfactor;
    double c2 = 3.0 * vfactor * vfactor + 3.0 * vfactor * vfactor * vfactor;
    double c3 = -6.0 * vfactor * vfactor - 3.0 * vfactor - 3.0 * vfactor * vfactor * vfactor;
    double c4 = 1.0 + 3.0 * vfactor + vfactor * vfactor * vfactor + 3.0 * vfactor * vfactor;
    for (size_t i = (period > 0 ? 6 * period - 6 : 0); i < n; ++i) {
        if (i < n) out[i] = c1 * e6[i] + c2 * e5[i] + c3 * e4[i] + c4 * e3[i];
    }
    TEMP_FREE(arena, e1); TEMP_FREE(arena, e2); TEMP_FREE(arena, e3); TEMP_FREE(arena, e4); TEMP_FREE(arena, e5); TEMP_FREE(arena, e6);
    return n;
}

size_t exprtk_ta_trima(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena) {
    if (period == 0 || period > n) return 0;
    double *tmp = TEMP_ALLOC(arena, n);
    if (!tmp) return 0;
    size_t p1 = (period + 1) / 2;
    size_t p2 = (period % 2 == 0) ? p1 + 1 : p1;
    ta_sma_calc(in, n, p1, tmp);
    ta_sma_calc(tmp, n, p2, out);
    TEMP_FREE(arena, tmp); return n;
}

size_t exprtk_ta_bbands(const double *in, size_t n, size_t period, double mult, double *upper, double *middle, double *lower) {
    if (period == 0 || period > n) return 0;
    ta_sma_calc(in, n, period, middle);
    for (size_t i = period - 1; i < n; ++i) {
        double sum_sq = 0;
        for (size_t j = 0; j < period; ++j) {
            double d = in[i - j] - middle[i];
            sum_sq += d * d;
        }
        double dev = mult * sqrt(sum_sq / (double)period);
        upper[i] = middle[i] + dev;
        lower[i] = middle[i] - dev;
    }
    return n;
}

size_t exprtk_ta_midpoint(const double *in, size_t n, size_t period, double *out) {
    if (period == 0 || period > n) return 0;
    for (size_t i = period - 1; i < n; ++i) {
        double h = ta_highest(in, i, period), l = ta_lowest(in, i, period);
        out[i] = (h + l) / 2.0;
    }
    return n;
}

size_t exprtk_ta_midprice(const double *hi, const double *lo, size_t n, size_t period, double *out) {
    if (period == 0 || period > n) return 0;
    for (size_t i = period - 1; i < n; ++i) {
        double h = ta_highest(hi, i, period), l = ta_lowest(lo, i, period);
        out[i] = (h + l) / 2.0;
    }
    return n;
}

size_t exprtk_ta_sar(const double *hi, const double *lo, size_t n, double accel_init, double accel_max, double *out) {
    if (n < 2) return 0;
    int is_long = (hi[1] > hi[0] || lo[1] > lo[0]);
    double sar = is_long ? lo[0] : hi[0];
    double ep = is_long ? hi[1] : lo[1];
    double af = accel_init;
    out[0] = sar;
    for (size_t i = 1; i < n; ++i) {
        double next_sar = sar + af * (ep - sar);
        if (is_long) {
            if (i >= 2) next_sar = fmin(next_sar, fmin(lo[i - 1], lo[i - 2]));
            else next_sar = fmin(next_sar, lo[i - 1]);
            if (lo[i] < next_sar) {
                is_long = 0; sar = ep; ep = lo[i]; af = accel_init;
            } else {
                sar = next_sar;
                if (hi[i] > ep) { ep = hi[i]; af = fmin(af + accel_init, accel_max); }
            }
        } else {
            if (i >= 2) next_sar = fmax(next_sar, fmax(hi[i - 1], hi[i - 2]));
            else next_sar = fmax(next_sar, hi[i - 1]);
            if (hi[i] > next_sar) {
                is_long = 1; sar = ep; ep = hi[i]; af = accel_init;
            } else {
                sar = next_sar;
                if (lo[i] < ep) { ep = lo[i]; af = fmin(af + accel_init, accel_max); }
            }
        }
        out[i] = sar;
    }
    return n;
}

size_t exprtk_ta_savgol(const double *in, size_t n, size_t window, double *out) {
    if (window < 3 || window > n || window % 2 == 0) return 0;
    if (window == 5) {
        for (size_t i = 2; i < n - 2; ++i) out[i] = (-3.0 * in[i - 2] + 12.0 * in[i - 1] + 17.0 * in[i] + 12.0 * in[i + 1] - 3.0 * in[i + 2]) / 35.0;
    } else {
        ta_sma_calc(in, n, window, out);
    }
    return n;
}

// TA-Lib Momentum Indicators
size_t exprtk_ta_rsi(const double *in, size_t n, size_t period, double *out) {
    if (period == 0 || period >= n) return 0;

    // Compute gains and losses from price changes
    double avg_gain = 0, avg_loss = 0;
    for (size_t i = 1; i <= period; ++i) {
        double d = in[i] - in[i - 1];
        if (d > 0) avg_gain += d;
        else avg_loss += -d;
    }
    avg_gain /= (double)period;
    avg_loss /= (double)period;

    // First RSI value
    if (avg_loss == 0) out[period] = 100.0;
    else {
        double rs = avg_gain / avg_loss;
        out[period] = 100.0 - (100.0 / (1.0 + rs));
    }

    // Subsequent values using Wilder smoothing
    for (size_t i = period + 1; i < n; ++i) {
        double d = in[i] - in[i - 1];
        double g = d > 0 ? d : 0;
        double l = d < 0 ? -d : 0;
        avg_gain = (avg_gain * (double)(period - 1) + g) / (double)period;
        avg_loss = (avg_loss * (double)(period - 1) + l) / (double)period;
        if (avg_loss == 0) out[i] = 100.0;
        else {
            double rs = avg_gain / avg_loss;
            out[i] = 100.0 - (100.0 / (1.0 + rs));
        }
    }
    return n;
}

size_t exprtk_ta_macd(const double *in, size_t n, size_t fast_p, size_t slow_p, size_t sig_p, double *line, double *sig, double *hist, turbo_arena_t *arena) {
    if (fast_p == 0 || slow_p == 0 || sig_p == 0 || n < slow_p) return 0;
    double *f_ema = TEMP_ALLOC(arena, n);
    double *s_ema = TEMP_ALLOC(arena, n);
    if (!f_ema || !s_ema) { TEMP_FREE(arena, f_ema); TEMP_FREE(arena, s_ema); return 0; }
    ta_ema_calc(in, n, fast_p, f_ema);
    ta_ema_calc(in, n, slow_p, s_ema);
    for (size_t i = (slow_p > 0 ? slow_p - 1 : 0); i < n; ++i) line[i] = f_ema[i] - s_ema[i];
    ta_ema_calc(line + (slow_p > 0 ? slow_p - 1 : 0), n - (slow_p > 0 ? slow_p - 1 : 0), sig_p, sig + (slow_p > 0 ? slow_p - 1 : 0));
    for (size_t i = (slow_p + sig_p > 2 ? slow_p + sig_p - 2 : 0); i < n; ++i) hist[i] = line[i] - sig[i];
    TEMP_FREE(arena, f_ema); TEMP_FREE(arena, s_ema); return n;
}

size_t exprtk_ta_stoch(const double *hi, const double *lo, const double *cl, size_t n, size_t k_p, size_t d_p, double *out_k, double *out_d, turbo_arena_t *arena) {
    if (k_p == 0 || d_p == 0 || n < k_p) return 0;
    double *fast_k = TEMP_ALLOC(arena, n);
    if (!fast_k) return 0;
    for (size_t i = k_p - 1; i < n; ++i) {
        double hh = ta_highest(hi, i, k_p), ll = ta_lowest(lo, i, k_p);
        fast_k[i] = (hh > ll) ? 100.0 * (cl[i] - ll) / (hh - ll) : 100.0;
    }
    ta_sma_calc(fast_k + k_p - 1, n - (k_p - 1), 3, out_k + k_p - 1);
    ta_sma_calc(out_k + k_p - 1, n - (k_p - 1), d_p, out_d + k_p - 1);
    TEMP_FREE(arena, fast_k); return n;
}

size_t exprtk_ta_stochrsi(const double *in, size_t n, size_t period, size_t k_p, size_t d_p, double *out_k, double *out_d, turbo_arena_t *arena) {
    if (period == 0 || n < period + k_p) return 0;
    double *rsi = TEMP_ALLOC(arena, n);
    if (!rsi) return 0;
    exprtk_ta_rsi(in, n, period, rsi);
    double *fast_k = TEMP_ALLOC(arena, n);
    if (!fast_k) { TEMP_FREE(arena, rsi); return 0; }
    for (size_t i = period + k_p - 1; i < n; ++i) {
        double hh = ta_highest(rsi, i, k_p), ll = ta_lowest(rsi, i, k_p);
        fast_k[i] = (hh > ll) ? 100.0 * (rsi[i] - ll) / (hh - ll) : 100.0;
    }
    ta_sma_calc(fast_k + period + k_p - 1, n - (period + k_p - 1), 3, out_k + period + k_p - 1);
    ta_sma_calc(out_k + period + k_p - 1, n - (period + k_p - 1), d_p, out_d + period + k_p - 1);
    TEMP_FREE(arena, rsi); TEMP_FREE(arena, fast_k); return n;
}

size_t exprtk_ta_willr(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out) {
    if (period == 0 || n < period) return 0;
    for (size_t i = period - 1; i < n; ++i) {
        double hh = ta_highest(hi, i, period), ll = ta_lowest(lo, i, period);
        out[i] = (hh > ll) ? -100.0 * (hh - cl[i]) / (hh - ll) : -100.0;
    }
    return n;
}

size_t exprtk_ta_cci(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena) {
    if (period == 0 || n < period) return 0;
    double *tp = TEMP_ALLOC(arena, n);
    double *sma_tp = TEMP_ALLOC(arena, n);
    if (!tp || !sma_tp) { TEMP_FREE(arena, tp); TEMP_FREE(arena, sma_tp); return 0; }
    for (size_t i = 0; i < n; ++i) tp[i] = (hi[i] + lo[i] + cl[i]) / 3.0;
    ta_sma_calc(tp, n, period, sma_tp);
    for (size_t i = period - 1; i < n; ++i) {
        double md = 0;
        for (size_t j = 0; j < period; ++j) md += fabs(tp[i - j] - sma_tp[i]);
        md /= (double)period;
        out[i] = (md > 1e-15) ? (tp[i] - sma_tp[i]) / (0.015 * md) : 0;
    }
    TEMP_FREE(arena, tp); TEMP_FREE(arena, sma_tp); return n;
}

size_t exprtk_ta_mom(const double *in, size_t n, size_t period, double *out) {
    if (period == 0 || n <= period) return 0;
    for (size_t i = period; i < n; ++i) out[i] = in[i] - in[i - period];
    return n;
}

size_t exprtk_ta_roc(const double *in, size_t n, size_t period, double *out) {
    if (period == 0 || n <= period) return 0;
    for (size_t i = period; i < n; ++i) out[i] = (in[i - period] != 0) ? 100.0 * (in[i] - in[i - period]) / in[i - period] : 0;
    return n;
}

size_t exprtk_ta_apo(const double *in, size_t n, size_t fast_p, size_t slow_p, double *out, turbo_arena_t *arena) {
    if (fast_p == 0 || slow_p == 0 || n < slow_p) return 0;
    double *f_ema = TEMP_ALLOC(arena, n);
    double *s_ema = TEMP_ALLOC(arena, n);
    if (!f_ema || !s_ema) { TEMP_FREE(arena, f_ema); TEMP_FREE(arena, s_ema); return 0; }
    ta_ema_calc(in, n, fast_p, f_ema);
    ta_ema_calc(in, n, slow_p, s_ema);
    for (size_t i = slow_p - 1; i < n; ++i) out[i] = f_ema[i] - s_ema[i];
    TEMP_FREE(arena, f_ema); TEMP_FREE(arena, s_ema); return n;
}

size_t exprtk_ta_ppo(const double *in, size_t n, size_t fast_p, size_t slow_p, double *out, turbo_arena_t *arena) {
    if (fast_p == 0 || slow_p == 0 || n < slow_p) return 0;
    double *f_ema = TEMP_ALLOC(arena, n);
    double *s_ema = TEMP_ALLOC(arena, n);
    if (!f_ema || !s_ema) { TEMP_FREE(arena, f_ema); TEMP_FREE(arena, s_ema); return 0; }
    ta_ema_calc(in, n, fast_p, f_ema);
    ta_ema_calc(in, n, slow_p, s_ema);
    for (size_t i = slow_p - 1; i < n; ++i) out[i] = (s_ema[i] != 0) ? 100.0 * (f_ema[i] - s_ema[i]) / s_ema[i] : 0;
    TEMP_FREE(arena, f_ema); TEMP_FREE(arena, s_ema); return n;
}

size_t exprtk_ta_trix(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena) {
    if (period == 0 || n < 3 * period) return 0;
    double *e1 = TEMP_ALLOC(arena, n);
    double *e2 = TEMP_ALLOC(arena, n);
    double *e3 = TEMP_ALLOC(arena, n);
    if (!e1 || !e2 || !e3) { TEMP_FREE(arena, e1); TEMP_FREE(arena, e2); TEMP_FREE(arena, e3); return 0; }
    ta_ema_calc(in, n, period, e1);
    ta_ema_calc(e1, n, period, e2);
    ta_ema_calc(e2, n, period, e3);
    for (size_t i = 3 * period - 2; i < n; ++i) out[i] = (e3[i - 1] != 0) ? 100.0 * (e3[i] - e3[i - 1]) / e3[i - 1] : 0;
    TEMP_FREE(arena, e1); TEMP_FREE(arena, e2); TEMP_FREE(arena, e3); return n;
}

size_t exprtk_ta_ultosc(const double *hi, const double *lo, const double *cl, size_t n, size_t p1, size_t p2, size_t p3, double *out, turbo_arena_t *arena) {
    if (p1 == 0 || p2 == 0 || p3 == 0 || n < fmax(p1, fmax(p2, p3))) return 0;
    double *bp = TEMP_ALLOC(arena, n);
    double *tr = TEMP_ALLOC(arena, n);
    if (!bp || !tr) { TEMP_FREE(arena, bp); TEMP_FREE(arena, tr); return 0; }
    for (size_t i = 1; i < n; ++i) {
        bp[i] = cl[i] - fmin(lo[i], cl[i - 1]);
        tr[i] = fmax(hi[i], cl[i - 1]) - fmin(lo[i], cl[i - 1]);
    }
    for (size_t i = (size_t)fmax(p1, fmax(p2, p3)); i < n; ++i) {
        double s1_bp = 0, s1_tr = 0, s2_bp = 0, s2_tr = 0, s3_bp = 0, s3_tr = 0;
        for (size_t j = 0; j < p1; ++j) { s1_bp += bp[i - j]; s1_tr += tr[i - j]; }
        for (size_t j = 0; j < p2; ++j) { s2_bp += bp[i - j]; s2_tr += tr[i - j]; }
        for (size_t j = 0; j < p3; ++j) { s3_bp += bp[i - j]; s3_tr += tr[i - j]; }
        double avg1 = s1_tr > 0 ? s1_bp / s1_tr : 0;
        double avg2 = s2_tr > 0 ? s2_bp / s2_tr : 0;
        double avg3 = s3_tr > 0 ? s3_bp / s3_tr : 0;
        out[i] = 100.0 * (4.0 * avg1 + 2.0 * avg2 + avg3) / 7.0;
    }
    TEMP_FREE(arena, bp); TEMP_FREE(arena, tr); return n;
}

size_t exprtk_ta_aroon(const double *hi, const double *lo, size_t n, size_t period, double *up, double *down) {
    if (period == 0 || n < period) return 0;
    for (size_t i = period; i < n; ++i) {
        size_t h_idx = ta_highest_idx(hi, i, period + 1);
        size_t l_idx = ta_lowest_idx(lo, i, period + 1);
        up[i] = 100.0 * (double)(period - (i - h_idx)) / (double)period;
        down[i] = 100.0 * (double)(period - (i - l_idx)) / (double)period;
    }
    return n;
}

size_t exprtk_ta_aroonosc(const double *hi, const double *lo, size_t n, size_t period, double *out, turbo_arena_t *arena) {
    if (period == 0 || n < period) return 0;
    double *up = TEMP_ALLOC(arena, n);
    double *dn = TEMP_ALLOC(arena, n);
    if (!up || !dn) { TEMP_FREE(arena, up); TEMP_FREE(arena, dn); return 0; }
    exprtk_ta_aroon(hi, lo, n, period, up, dn);
    for (size_t i = period; i < n; ++i) out[i] = up[i] - dn[i];
    TEMP_FREE(arena, up); TEMP_FREE(arena, dn); return n;
}

size_t exprtk_ta_cmo(const double *in, size_t n, size_t period, double *out) {
    if (period == 0 || n < period) return 0;
    return n;
}

// TA-Lib Volatility Indicators
size_t exprtk_ta_trange(const double *hi, const double *lo, const double *cl, size_t n, double *out) {
    if (n == 0) return 0;
    ta_true_range_arr(hi, lo, cl, n, out);
    return n;
}

size_t exprtk_ta_atr(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena) {
    if (period == 0 || n < period) return 0;
    double *tr = TEMP_ALLOC(arena, n);
    if (!tr) return 0;
    ta_true_range_arr(hi, lo, cl, n, tr);
    ta_wilder_smooth(tr, n, period, out);
    TEMP_FREE(arena, tr); return n;
}

size_t exprtk_ta_natr(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena) {
    if (period == 0 || n < period) return 0;
    double *atr = TEMP_ALLOC(arena, n);
    if (!atr) return 0;
    exprtk_ta_atr(hi, lo, cl, n, period, atr, arena);
    for (size_t i = period - 1; i < n; ++i) out[i] = (cl[i] != 0) ? 100.0 * atr[i] / cl[i] : 0;
    TEMP_FREE(arena, atr); return n;
}

// TA-Lib Volume Indicators
size_t exprtk_ta_obv(const double *cl, const double *vol, size_t n, double *out) {
    if (n == 0) return 0;
    out[0] = vol[0];
    for (size_t i = 1; i < n; ++i) {
        if (cl[i] > cl[i - 1]) out[i] = out[i - 1] + vol[i];
        else if (cl[i] < cl[i - 1]) out[i] = out[i - 1] - vol[i];
        else out[i] = out[i - 1];
    }
    return n;
}

size_t exprtk_ta_ad(const double *hi, const double *lo, const double *cl, const double *vol, size_t n, double *out) {
    if (n == 0) return 0;
    double ad = 0;
    for (size_t i = 0; i < n; ++i) {
        double range = hi[i] - lo[i];
        double mfv = (range > 0) ? ((cl[i] - lo[i]) - (hi[i] - cl[i])) / range : 0;
        ad += mfv * vol[i];
        out[i] = ad;
    }
    return n;
}

size_t exprtk_ta_adosc(const double *hi, const double *lo, const double *cl, const double *vol, size_t n, size_t fast_p, size_t slow_p, double *out, turbo_arena_t *arena) {
    if (fast_p == 0 || slow_p == 0 || n < slow_p) return 0;
    double *ad = TEMP_ALLOC(arena, n);
    if (!ad) return 0;
    exprtk_ta_ad(hi, lo, cl, vol, n, ad);
    double *f_ema = TEMP_ALLOC(arena, n);
    double *s_ema = TEMP_ALLOC(arena, n);
    if (!f_ema || !s_ema) { TEMP_FREE(arena, ad); TEMP_FREE(arena, f_ema); TEMP_FREE(arena, s_ema); return 0; }
    ta_ema_calc(ad, n, fast_p, f_ema);
    ta_ema_calc(ad, n, slow_p, s_ema);
    for (size_t i = slow_p - 1; i < n; ++i) out[i] = f_ema[i] - s_ema[i];
    TEMP_FREE(arena, ad); TEMP_FREE(arena, f_ema); TEMP_FREE(arena, s_ema); return n;
}

size_t exprtk_ta_mfi(const double *hi, const double *lo, const double *cl, const double *vol, size_t n, size_t period, double *out, turbo_arena_t *arena) {
    if (period == 0 || n < period) return 0;
    double *tp = TEMP_ALLOC(arena, n);
    if (!tp) return 0;
    for (size_t i = 0; i < n; ++i) tp[i] = (hi[i] + lo[i] + cl[i]) / 3.0;
    for (size_t i = period; i < n; ++i) {
        double pos_mf = 0, neg_mf = 0;
        for (size_t j = 0; j < period; ++j) {
            double mf = tp[i - j] * vol[i - j];
            if (tp[i - j] > tp[i - j - 1]) pos_mf += mf;
            else if (tp[i - j] < tp[i - j - 1]) neg_mf += mf;
        }
        out[i] = (pos_mf + neg_mf > 0) ? 100.0 * pos_mf / (pos_mf + neg_mf) : 50.0;
    }
    TEMP_FREE(arena, tp); return n;
}

// TA-Lib Trend Indicators
size_t exprtk_ta_plus_dm(const double *hi, const double *lo, size_t n, size_t period, double *out, turbo_arena_t *arena) {
    if (period == 0 || n < 1) return 0;
    double *pdm = TEMP_ALLOC(arena, n);
    if (!pdm) return 0;
    for (size_t i = 1; i < n; ++i) {
        double up = hi[i] - hi[i - 1], dn = lo[i - 1] - lo[i];
        pdm[i] = (up > dn && up > 0) ? up : 0;
    }
    ta_wilder_smooth(pdm + 1, n - 1, period, out + 1);
    TEMP_FREE(arena, pdm); return n;
}

size_t exprtk_ta_minus_dm(const double *hi, const double *lo, size_t n, size_t period, double *out, turbo_arena_t *arena) {
    if (period == 0 || n < 1) return 0;
    double *mdm = TEMP_ALLOC(arena, n);
    if (!mdm) return 0;
    for (size_t i = 1; i < n; ++i) {
        double up = hi[i] - hi[i - 1], dn = lo[i - 1] - lo[i];
        mdm[i] = (dn > up && dn > 0) ? dn : 0;
    }
    ta_wilder_smooth(mdm + 1, n - 1, period, out + 1);
    TEMP_FREE(arena, mdm); return n;
}

size_t exprtk_ta_plus_di(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena) {
    if (period == 0 || n < period) return 0;
    double *pdm = TEMP_ALLOC(arena, n);
    double *atr = TEMP_ALLOC(arena, n);
    if (!pdm || !atr) { TEMP_FREE(arena, pdm); TEMP_FREE(arena, atr); return 0; }
    exprtk_ta_plus_dm(hi, lo, n, period, pdm, arena);
    exprtk_ta_atr(hi, lo, cl, n, period, atr, arena);
    for (size_t i = period - 1; i < n; ++i) out[i] = (atr[i] > 1e-15) ? 100.0 * pdm[i] / atr[i] : 0;
    TEMP_FREE(arena, pdm); TEMP_FREE(arena, atr); return n;
}

size_t exprtk_ta_minus_di(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena) {
    if (period == 0 || n < period) return 0;
    double *mdm = TEMP_ALLOC(arena, n);
    double *atr = TEMP_ALLOC(arena, n);
    if (!mdm || !atr) { TEMP_FREE(arena, mdm); TEMP_FREE(arena, atr); return 0; }
    exprtk_ta_minus_dm(hi, lo, n, period, mdm, arena);
    exprtk_ta_atr(hi, lo, cl, n, period, atr, arena);
    for (size_t i = period - 1; i < n; ++i) out[i] = (atr[i] > 1e-15) ? 100.0 * mdm[i] / atr[i] : 0;
    TEMP_FREE(arena, mdm); TEMP_FREE(arena, atr); return n;
}

size_t exprtk_ta_dx(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena) {
    if (period == 0 || n < period) return 0;
    double *p_di = TEMP_ALLOC(arena, n);
    double *m_di = TEMP_ALLOC(arena, n);
    if (!p_di || !m_di) { TEMP_FREE(arena, p_di); TEMP_FREE(arena, m_di); return 0; }
    exprtk_ta_plus_di(hi, lo, cl, n, period, p_di, arena);
    exprtk_ta_minus_di(hi, lo, cl, n, period, m_di, arena);
    for (size_t i = period - 1; i < n; ++i) {
        double diff = fabs(p_di[i] - m_di[i]), sum = p_di[i] + m_di[i];
        out[i] = (sum > 0) ? 100.0 * diff / sum : 0;
    }
    TEMP_FREE(arena, p_di); TEMP_FREE(arena, m_di); return n;
}

size_t exprtk_ta_adx(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena) {
    if (period == 0 || n < 2 * period) return 0;
    double *dx = TEMP_ALLOC(arena, n);
    if (!dx) return 0;
    exprtk_ta_dx(hi, lo, cl, n, period, dx, arena);
    ta_wilder_smooth(dx + period - 1, n - (period - 1), period, out + period - 1);
    TEMP_FREE(arena, dx); return n;
}

size_t exprtk_ta_adxr(const double *hi, const double *lo, const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena) {
    if (period == 0 || n < 3 * period) return 0;
    double *adx = TEMP_ALLOC(arena, n);
    if (!adx) return 0;
    exprtk_ta_adx(hi, lo, cl, n, period, adx, arena);
    TEMP_FREE(arena, adx); return n;
}

// TA-Lib Statistics Indicators
size_t exprtk_ta_stddev(const double *in, size_t n, size_t period, double mult, double *out, turbo_arena_t *arena) {
    if (period == 0 || n < period) return 0;
    double *sma = TEMP_ALLOC(arena, n);
    if (!sma) return 0;
    ta_sma_calc(in, n, period, sma);
    for (size_t i = period - 1; i < n; ++i) {
        double ss = 0;
        for (size_t j = 0; j < period; ++j) {
            double d = in[i - j] - sma[i];
            ss += d * d;
        }
        out[i] = mult * sqrt(ss / (double)period);
    }
    TEMP_FREE(arena, sma); return n;
}

size_t exprtk_ta_var(const double *in, size_t n, size_t period, double mult, double *out, turbo_arena_t *arena) {
    if (period == 0 || n < period) return 0;
    double *sma = TEMP_ALLOC(arena, n);
    if (!sma) return 0;
    ta_sma_calc(in, n, period, sma);
    for (size_t i = period - 1; i < n; ++i) {
        double ss = 0;
        for (size_t j = 0; j < period; ++j) {
            double d = in[i - j] - sma[i];
            ss += d * d;
        }
        out[i] = mult * (ss / (double)period);
    }
    TEMP_FREE(arena, sma); return n;
}

size_t exprtk_ta_linearreg(const double *in, size_t n, size_t period, double *out) {
    if (period == 0 || n < period) return 0;
    for (size_t i = period - 1; i < n; ++i) {
        double slope, intercept;
        ta_linreg(in, i, period, &slope, &intercept);
        out[i] = intercept + slope * (double)(period - 1);
    }
    return n;
}

size_t exprtk_ta_linearreg_slope(const double *in, size_t n, size_t period, double *out) {
    if (period == 0 || n < period) return 0;
    for (size_t i = period - 1; i < n; ++i) {
        double slope, intercept;
        ta_linreg(in, i, period, &slope, &intercept);
        out[i] = slope;
    }
    return n;
}

size_t exprtk_ta_linearreg_intercept(const double *in, size_t n, size_t period, double *out) {
    if (period == 0 || n < period) return 0;
    for (size_t i = period - 1; i < n; ++i) {
        double slope, intercept;
        ta_linreg(in, i, period, &slope, &intercept);
        out[i] = intercept;
    }
    return n;
}

size_t exprtk_ta_linearreg_angle(const double *in, size_t n, size_t period, double *out) {
    if (period == 0 || n < period) return 0;
    for (size_t i = period - 1; i < n; ++i) {
        double slope, intercept;
        ta_linreg(in, i, period, &slope, &intercept);
        out[i] = atan(slope) * (180.0 / 3.14159265358979323846);
    }
    return n;
}

size_t exprtk_ta_tsf(const double *in, size_t n, size_t period, double *out) {
    if (period == 0 || n < period) return 0;
    for (size_t i = period - 1; i < n; ++i) {
        double slope, intercept;
        ta_linreg(in, i, period, &slope, &intercept);
        out[i] = intercept + slope * (double)period;
    }
    return n;
}

size_t exprtk_ta_beta(const double *in0, const double *in1, size_t n, size_t period, double *out) {
    if (period == 0 || n < period) return 0;
    for (size_t i = period - 1; i < n; ++i) {
        double m0 = 0, m1 = 0, ss1 = 0, sc01 = 0;
        for (size_t j = 0; j < period; ++j) { m0 += in0[i - j]; m1 += in1[i - j]; }
        m0 /= (double)period; m1 /= (double)period;
        for (size_t j = 0; j < period; ++j) {
            double d0 = in0[i - j] - m0, d1 = in1[i - j] - m1;
            ss1 += d1 * d1; sc01 += d0 * d1;
        }
        out[i] = (ss1 > 1e-15) ? sc01 / ss1 : 0;
    }
    return n;
}

size_t exprtk_ta_correl(const double *in0, const double *in1, size_t n, size_t period, double *out) {
    if (period == 0 || n < period) return 0;
    for (size_t i = period - 1; i < n; ++i) {
        double m0 = 0, m1 = 0, ss0 = 0, ss1 = 0, sc01 = 0;
        for (size_t j = 0; j < period; ++j) { m0 += in0[i - j]; m1 += in1[i - j]; }
        m0 /= (double)period; m1 /= (double)period;
        for (size_t j = 0; j < period; ++j) {
            double d0 = in0[i - j] - m0, d1 = in1[i - j] - m1;
            ss0 += d0 * d0; ss1 += d1 * d1; sc01 += d0 * d1;
        }
        double den = sqrt(ss0 * ss1);
        out[i] = (den > 1e-15) ? sc01 / den : 0;
    }
    return n;
}

// TA-Lib Price Transform Indicators
size_t exprtk_ta_avgprice(const double *o, const double *h, const double *l, const double *c, size_t n, double *out) {
    for (size_t i = 0; i < n; ++i) out[i] = (o[i] + h[i] + l[i] + c[i]) / 4.0;
    return n;
}

size_t exprtk_ta_medprice(const double *h, const double *l, size_t n, double *out) {
    for (size_t i = 0; i < n; ++i) out[i] = (h[i] + l[i]) / 2.0;
    return n;
}

size_t exprtk_ta_typprice(const double *h, const double *l, const double *c, size_t n, double *out) {
    for (size_t i = 0; i < n; ++i) out[i] = (h[i] + l[i] + c[i]) / 3.0;
    return n;
}

size_t exprtk_ta_wclprice(const double *h, const double *l, const double *c, size_t n, double *out) {
    for (size_t i = 0; i < n; ++i) out[i] = (h[i] + l[i] + 2.0 * c[i]) / 4.0;
    return n;
}

// TA-Lib Options Pricing
size_t exprtk_ta_bsm_call(const double *S, const double *K, const double *T, const double *r, const double *sigma, size_t n, double *out) {
    for (size_t i = 0; i < n; ++i) {
        if (T[i] <= 0 || sigma[i] <= 0) { out[i] = fmax(0, S[i] - K[i]); continue; }
        double d1 = (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / (sigma[i] * sqrt(T[i]));
        double d2 = d1 - sigma[i] * sqrt(T[i]);
        out[i] = S[i] * ta_ncdf(d1) - K[i] * exp(-r[i] * T[i]) * ta_ncdf(d2);
    }
    return n;
}

size_t exprtk_ta_bsm_put(const double *S, const double *K, const double *T, const double *r, const double *sigma, size_t n, double *out) {
    for (size_t i = 0; i < n; ++i) {
        if (T[i] <= 0 || sigma[i] <= 0) { out[i] = fmax(0, K[i] - S[i]); continue; }
        double d1 = (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / (sigma[i] * sqrt(T[i]));
        double d2 = d1 - sigma[i] * sqrt(T[i]);
        out[i] = K[i] * exp(-r[i] * T[i]) * ta_ncdf(-d2) - S[i] * ta_ncdf(-d1);
    }
    return n;
}

size_t exprtk_ta_bsm_delta_call(const double *S, const double *K, const double *T, const double *r, const double *sigma, size_t n, double *out) {
    for (size_t i = 0; i < n; ++i) {
        if (T[i] <= 0 || sigma[i] <= 0) { out[i] = (S[i] > K[i] ? 1.0 : 0.0); continue; }
        double d1 = (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / (sigma[i] * sqrt(T[i]));
        out[i] = ta_ncdf(d1);
    }
    return n;
}

size_t exprtk_ta_bsm_delta_put(const double *S, const double *K, const double *T, const double *r, const double *sigma, size_t n, double *out) {
    for (size_t i = 0; i < n; ++i) {
        if (T[i] <= 0 || sigma[i] <= 0) { out[i] = (S[i] < K[i] ? -1.0 : 0.0); continue; }
        double d1 = (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / (sigma[i] * sqrt(T[i]));
        out[i] = ta_ncdf(d1) - 1.0;
    }
    return n;
}

size_t exprtk_ta_bsm_gamma(const double *S, const double *K, const double *T, const double *r, const double *sigma, size_t n, double *out) {
    for (size_t i = 0; i < n; ++i) {
        if (T[i] <= 0 || sigma[i] <= 0) { out[i] = 0; continue; }
        double d1 = (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / (sigma[i] * sqrt(T[i]));
        out[i] = ta_npdf(d1) / (S[i] * sigma[i] * sqrt(T[i]));
    }
    return n;
}

size_t exprtk_ta_bsm_theta_call(const double *S, const double *K, const double *T, const double *r, const double *sigma, size_t n, double *out) {
    for (size_t i = 0; i < n; ++i) {
        if (T[i] <= 0 || sigma[i] <= 0) { out[i] = 0; continue; }
        double d1 = (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / (sigma[i] * sqrt(T[i]));
        double d2 = d1 - sigma[i] * sqrt(T[i]);
        double term1 = -(S[i] * ta_npdf(d1) * sigma[i]) / (2.0 * sqrt(T[i]));
        double term2 = r[i] * K[i] * exp(-r[i] * T[i]) * ta_ncdf(d2);
        out[i] = term1 - term2;
    }
    return n;
}

size_t exprtk_ta_bsm_theta_put(const double *S, const double *K, const double *T, const double *r, const double *sigma, size_t n, double *out) {
    for (size_t i = 0; i < n; ++i) {
        if (T[i] <= 0 || sigma[i] <= 0) { out[i] = 0; continue; }
        double d1 = (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / (sigma[i] * sqrt(T[i]));
        double d2 = d1 - sigma[i] * sqrt(T[i]);
        double term1 = -(S[i] * ta_npdf(d1) * sigma[i]) / (2.0 * sqrt(T[i]));
        double term2 = r[i] * K[i] * exp(-r[i] * T[i]) * ta_ncdf(-d2);
        out[i] = term1 + term2;
    }
    return n;
}

size_t exprtk_ta_bsm_vega(const double *S, const double *K, const double *T, const double *r, const double *sigma, size_t n, double *out) {
    for (size_t i = 0; i < n; ++i) {
        if (T[i] <= 0 || sigma[i] <= 0) { out[i] = 0; continue; }
        double d1 = (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / (sigma[i] * sqrt(T[i]));
        out[i] = S[i] * sqrt(T[i]) * ta_npdf(d1);
    }
    return n;
}

size_t exprtk_ta_bsm_rho_call(const double *S, const double *K, const double *T, const double *r, const double *sigma, size_t n, double *out) {
    for (size_t i = 0; i < n; ++i) {
        if (T[i] <= 0 || sigma[i] <= 0) { out[i] = 0; continue; }
        double d1 = (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / (sigma[i] * sqrt(T[i]));
        double d2 = d1 - sigma[i] * sqrt(T[i]);
        out[i] = K[i] * T[i] * exp(-r[i] * T[i]) * ta_ncdf(d2);
    }
    return n;
}

size_t exprtk_ta_bsm_rho_put(const double *S, const double *K, const double *T, const double *r, const double *sigma, size_t n, double *out) {
    for (size_t i = 0; i < n; ++i) {
        if (T[i] <= 0 || sigma[i] <= 0) { out[i] = 0; continue; }
        double d1 = (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / (sigma[i] * sqrt(T[i]));
        double d2 = d1 - sigma[i] * sqrt(T[i]);
        out[i] = -K[i] * T[i] * exp(-r[i] * T[i]) * ta_ncdf(-d2);
    }
    return n;
}

size_t exprtk_ta_bsm_iv_call(const double *price, const double *S, const double *K, const double *T, const double *r, size_t n, double *out) {
    for (size_t i = 0; i < n; ++i) {
        double v = 0.5;
        for (int iter = 0; iter < 100; ++iter) {
            double c, vega;
            exprtk_ta_bsm_call(&S[i], &K[i], &T[i], &r[i], &v, 1, &c);
            exprtk_ta_bsm_vega(&S[i], &K[i], &T[i], &r[i], &v, 1, &vega);
            double diff = c - price[i];
            if (fabs(diff) < 1e-8 || fabs(vega) < 1e-12) break;
            v -= diff / vega;
            if (v < 0) v = 1e-6;
        }
        out[i] = v;
    }
    return n;
}

size_t exprtk_ta_bsm_iv_put(const double *price, const double *S, const double *K, const double *T, const double *r, size_t n, double *out) {
    for (size_t i = 0; i < n; ++i) {
        double v = 0.5;
        for (int iter = 0; iter < 100; ++iter) {
            double p, vega;
            exprtk_ta_bsm_put(&S[i], &K[i], &T[i], &r[i], &v, 1, &p);
            exprtk_ta_bsm_vega(&S[i], &K[i], &T[i], &r[i], &v, 1, &vega);
            double diff = p - price[i];
            if (fabs(diff) < 1e-8 || fabs(vega) < 1e-12) break;
            v -= diff / vega;
            if (v < 0) v = 1e-6;
        }
        out[i] = v;
    }
    return n;
}

size_t exprtk_ta_opt_binomial(double S, double K, double T, double r, double sigma, size_t steps, int is_call, double *out, turbo_arena_t *arena) {
    if (steps == 0 || T <= 0 || sigma <= 0) { *out = is_call ? fmax(0, S - K) : fmax(0, K - S); return 1; }
    double dt = T / (double)steps;
    double u = exp(sigma * sqrt(dt)), d = 1.0 / u;
    double p = (exp(r * dt) - d) / (u - d);
    double *V = (double*)malloc((steps + 1) * sizeof(double));
    if (!V) return 0;
    for (size_t i = 0; i <= steps; ++i) {
        double St = S * pow(u, (double)(steps - i)) * pow(d, (double)i);
        V[i] = is_call ? fmax(0, St - K) : fmax(0, K - St);
    }
    for (size_t j = steps; j > 0; --j) {
        for (size_t i = 0; i < j; ++i) V[i] = exp(-r * dt) * (p * V[i] + (1.0 - p) * V[i + 1]);
    }
    *out = V[0]; TEMP_FREE(arena, V); return 1;
}