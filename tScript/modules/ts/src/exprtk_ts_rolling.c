/**
 * @file exprtk_ts_rolling.c
 * @brief Rolling, expanding, EWM window functions and return calculations.
 */
#include "ts.h"
#include "ts_internal.h"
#include <math.h>
#include <string.h>

/* ========================================================================= */
/* Rolling Window Functions                                                  */
/* ========================================================================= */

size_t exprtk_ts_rolling_mean(const double *in, size_t n, size_t period, double *out) {
    if (period == 0 || n < period) return 0;
    double sum = 0;
    for (size_t i = 0; i < period; i++) sum += in[i];
    for (size_t i = 0; i < period - 1; i++) out[i] = 0;
    out[period - 1] = sum / (double)period;
    for (size_t i = period; i < n; i++) {
        sum += in[i] - in[i - period];
        out[i] = sum / (double)period;
    }
    return n;
}

size_t exprtk_ts_rolling_std(const double *in, size_t n, size_t period, double *out) {
    if (period < 2 || n < period) return 0;
    double sum = 0, sum2 = 0;
    for (size_t i = 0; i < period; i++) { sum += in[i]; sum2 += in[i] * in[i]; }
    for (size_t i = 0; i < period - 1; i++) out[i] = 0;
    double mean = sum / (double)period;
    out[period - 1] = sqrt((sum2 / (double)period - mean * mean) * (double)period / (double)(period - 1));
    for (size_t i = period; i < n; i++) {
        sum += in[i] - in[i - period];
        sum2 += in[i] * in[i] - in[i - period] * in[i - period];
        mean = sum / (double)period;
        double var = sum2 / (double)period - mean * mean;
        out[i] = sqrt(fabs(var) * (double)period / (double)(period - 1));
    }
    return n;
}

size_t exprtk_ts_rolling_skew(const double *in, size_t n, size_t period, double *out) {
    if (period < 3 || n < period) return 0;
    for (size_t i = 0; i < period - 1; i++) out[i] = 0;
    for (size_t i = period - 1; i < n; i++) {
        double sum = 0, sum2 = 0, sum3 = 0;
        for (size_t j = 0; j < period; j++) {
            double v = in[i - period + 1 + j];
            sum += v; sum2 += v * v; sum3 += v * v * v;
        }
        double mean = sum / (double)period;
        double var = sum2 / (double)period - mean * mean;
        double std = sqrt(fabs(var));
        if (std < 1e-15) { out[i] = 0; continue; }
        double m3 = sum3 / (double)period - 3.0 * mean * sum2 / (double)period + 2.0 * mean * mean * mean;
        out[i] = m3 / (std * std * std) * (double)period * (double)period / ((double)(period - 1) * (double)(period - 2));
    }
    return n;
}

size_t exprtk_ts_rolling_kurt(const double *in, size_t n, size_t period, double *out) {
    if (period < 4 || n < period) return 0;
    for (size_t i = 0; i < period - 1; i++) out[i] = 0;
    for (size_t i = period - 1; i < n; i++) {
        double sum = 0, sum2 = 0, sum4 = 0;
        for (size_t j = 0; j < period; j++) {
            double v = in[i - period + 1 + j];
            double v2 = v * v;
            sum += v; sum2 += v2; sum4 += v2 * v2;
        }
        double mean = sum / (double)period;
        double var = sum2 / (double)period - mean * mean;
        if (fabs(var) < 1e-15) { out[i] = 0; continue; }
        /* Compute centered 4th moment */
        double m4 = 0;
        for (size_t j = 0; j < period; j++) {
            double d = in[i - period + 1 + j] - mean;
            double d2 = d * d;
            m4 += d2 * d2;
        }
        m4 /= (double)period;
        out[i] = m4 / (var * var) - 3.0;
    }
    return n;
}

size_t exprtk_ts_rolling_corr(const double *a, const double *b, size_t n, size_t period, double *out) {
    if (period < 2 || n < period) return 0;
    for (size_t i = 0; i < period - 1; i++) out[i] = 0;
    for (size_t i = period - 1; i < n; i++) {
        double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
        for (size_t j = 0; j < period; j++) {
            size_t k = i - period + 1 + j;
            sa += a[k]; sb += b[k];
            saa += a[k] * a[k]; sbb += b[k] * b[k];
            sab += a[k] * b[k];
        }
        double p = (double)period;
        double denom = sqrt((p * saa - sa * sa) * (p * sbb - sb * sb));
        out[i] = (fabs(denom) > 1e-15) ? (p * sab - sa * sb) / denom : 0;
    }
    return n;
}

size_t exprtk_ts_rolling_beta(const double *y, const double *x, size_t n, size_t period, double *out) {
    if (period < 2 || n < period) return 0;
    for (size_t i = 0; i < period - 1; i++) out[i] = 0;
    for (size_t i = period - 1; i < n; i++) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (size_t j = 0; j < period; j++) {
            size_t k = i - period + 1 + j;
            sx += x[k]; sy += y[k];
            sxx += x[k] * x[k]; sxy += x[k] * y[k];
        }
        double p = (double)period;
        double denom = p * sxx - sx * sx;
        out[i] = (fabs(denom) > 1e-15) ? (p * sxy - sx * sy) / denom : 0;
    }
    return n;
}

/* ========================================================================= */
/* Expanding Window Functions                                                */
/* ========================================================================= */

size_t exprtk_ts_expanding_mean(const double *in, size_t n, double *out) {
    if (n == 0) return 0;
    double sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += in[i];
        out[i] = sum / (double)(i + 1);
    }
    return n;
}

size_t exprtk_ts_expanding_std(const double *in, size_t n, double *out) {
    if (n < 2) { if (n == 1) out[0] = 0; return n; }
    double sum = 0, sum2 = 0;
    out[0] = 0;
    for (size_t i = 0; i < n; i++) {
        sum += in[i];
        sum2 += in[i] * in[i];
        if (i == 0) { out[i] = 0; continue; }
        double mean = sum / (double)(i + 1);
        double var = sum2 / (double)(i + 1) - mean * mean;
        out[i] = sqrt(fabs(var) * (double)(i + 1) / (double)i);
    }
    return n;
}

/* ========================================================================= */
/* Exponentially Weighted Moving Functions                                   */
/* ========================================================================= */

size_t exprtk_ts_ewm_mean(const double *in, size_t n, size_t span, double *out) {
    if (n == 0 || span == 0) return 0;
    double alpha = 2.0 / ((double)span + 1.0);
    out[0] = in[0];
    for (size_t i = 1; i < n; i++)
        out[i] = alpha * in[i] + (1.0 - alpha) * out[i - 1];
    return n;
}

size_t exprtk_ts_ewm_std(const double *in, size_t n, size_t span, double *out) {
    if (n < 2 || span == 0) { if (n >= 1) out[0] = 0; return n; }
    double alpha = 2.0 / ((double)span + 1.0);
    double ewm = in[0], ewm2 = in[0] * in[0];
    out[0] = 0;
    for (size_t i = 1; i < n; i++) {
        ewm = alpha * in[i] + (1.0 - alpha) * ewm;
        ewm2 = alpha * in[i] * in[i] + (1.0 - alpha) * ewm2;
        double var = ewm2 - ewm * ewm;
        out[i] = sqrt(fabs(var));
    }
    return n;
}

/* ========================================================================= */
/* Return Calculations                                                       */
/* ========================================================================= */

size_t exprtk_ts_pct_change(const double *in, size_t n, size_t period, double *out) {
    if (n == 0 || period == 0) return 0;
    for (size_t i = 0; i < period && i < n; i++) out[i] = 0;
    for (size_t i = period; i < n; i++)
        out[i] = (fabs(in[i - period]) > 1e-15) ? in[i] / in[i - period] - 1.0 : 0;
    return n;
}

size_t exprtk_ts_log_return(const double *in, size_t n, double *out) {
    if (n < 2) { if (n == 1) out[0] = 0; return n; }
    out[0] = 0;
    for (size_t i = 1; i < n; i++)
        out[i] = (in[i - 1] > 1e-15 && in[i] > 1e-15) ? log(in[i] / in[i - 1]) : 0;
    return n;
}

size_t exprtk_ts_cum_return(const double *in, size_t n, double *out) {
    if (n == 0) return 0;
    out[0] = 1.0 + in[0];
    for (size_t i = 1; i < n; i++)
        out[i] = out[i - 1] * (1.0 + in[i]);
    return n;
}
