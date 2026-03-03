/**
 * @file exprtk_finance.c
 * @brief Performance metrics: Sharpe, Sortino, Calmar, profit factor, etc.
 */
#include "fin.h"
#include <math.h>
#include <simde/x86/avx2.h>
#include <simde/x86/fma.h>

#define SIMD_THRESHOLD 8

static inline double simd_sum(const double *arr, size_t n) {
    if (n < SIMD_THRESHOLD) {
        double s = 0;
        for (size_t i = 0; i < n; i++) s += arr[i];
        return s;
    }
    simde__m256d acc = simde_mm256_setzero_pd();
    size_t i = 0;
    for (; i + 4 <= n; i += 4)
        acc = simde_mm256_add_pd(acc, simde_mm256_loadu_pd(&arr[i]));
    double tmp[4];
    simde_mm256_storeu_pd(tmp, acc);
    double s = tmp[0] + tmp[1] + tmp[2] + tmp[3];
    for (; i < n; i++) s += arr[i];
    return s;
}

/* Sharpe = (mean(r) - rf) / std(r) * sqrt(annual_factor) */
double exprtk_sharpe(const double *returns, size_t n, double rf, double annual_factor) {
    if (n < 2) return 0.0;
    double mean = simd_sum(returns, n) / (double)n;

    simde__m256d v_mean = simde_mm256_set1_pd(mean);
    simde__m256d acc = simde_mm256_setzero_pd();
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        simde__m256d d = simde_mm256_sub_pd(simde_mm256_loadu_pd(&returns[i]), v_mean);
        acc = simde_mm256_fmadd_pd(d, d, acc);
    }
    double tmp[4];
    simde_mm256_storeu_pd(tmp, acc);
    double var = tmp[0] + tmp[1] + tmp[2] + tmp[3];
    for (; i < n; i++) { double d = returns[i] - mean; var += d * d; }

    double std = sqrt(var / (double)(n - 1));
    if (std < 1e-15) return 0.0;
    return (mean - rf) / std * sqrt(annual_factor);
}

/* Sortino = (mean(r) - rf) / downside_std(r) * sqrt(annual_factor) */
double exprtk_sortino(const double *returns, size_t n, double rf, double annual_factor) {
    if (n < 2) return 0.0;
    double mean = simd_sum(returns, n) / (double)n;

    double down_sum = 0;
    size_t down_count = 0;
    for (size_t i = 0; i < n; i++) {
        double d = returns[i] - rf;
        if (d < 0) { down_sum += d * d; down_count++; }
    }
    if (down_count == 0) return 0.0;
    double down_std = sqrt(down_sum / (double)down_count);
    if (down_std < 1e-15) return 0.0;
    return (mean - rf) / down_std * sqrt(annual_factor);
}

/* Calmar = annual_return / max_drawdown */
double exprtk_calmar(const double *equity, size_t n, double annual_factor) {
    if (n < 2 || equity[0] < 1e-15) return 0.0;
    double total_return = equity[n - 1] / equity[0] - 1.0;
    double annual_return = total_return * annual_factor / (double)n;

    double peak = equity[0], max_dd = 0;
    for (size_t i = 1; i < n; i++) {
        if (equity[i] > peak) peak = equity[i];
        double dd = (peak - equity[i]) / peak;
        if (dd > max_dd) max_dd = dd;
    }
    if (max_dd < 1e-15) return 0.0;
    return annual_return / max_dd;
}

/* profit_factor = sum(winning) / abs(sum(losing)) */
double exprtk_profit_factor(const double *trades, size_t n) {
    double gross_profit = 0, gross_loss = 0;
    for (size_t i = 0; i < n; i++) {
        if (trades[i] > 0) gross_profit += trades[i];
        else gross_loss -= trades[i];
    }
    if (gross_loss < 1e-15) return (gross_profit > 0) ? 1e15 : 0.0;
    return gross_profit / gross_loss;
}

/* expectancy = win_rate * avg_win - loss_rate * avg_loss */
double exprtk_expectancy(const double *trades, size_t n) {
    if (n == 0) return 0.0;
    double sum_win = 0, sum_loss = 0;
    size_t n_win = 0, n_loss = 0;
    for (size_t i = 0; i < n; i++) {
        if (trades[i] > 0) { sum_win += trades[i]; n_win++; }
        else if (trades[i] < 0) { sum_loss -= trades[i]; n_loss++; }
    }
    double win_rate = (double)n_win / (double)n;
    double loss_rate = (double)n_loss / (double)n;
    double avg_win = (n_win > 0) ? sum_win / (double)n_win : 0;
    double avg_loss = (n_loss > 0) ? sum_loss / (double)n_loss : 0;
    return win_rate * avg_win - loss_rate * avg_loss;
}

/* payoff_ratio = avg_win / avg_loss */
double exprtk_payoff_ratio(const double *trades, size_t n) {
    double sum_win = 0, sum_loss = 0;
    size_t n_win = 0, n_loss = 0;
    for (size_t i = 0; i < n; i++) {
        if (trades[i] > 0) { sum_win += trades[i]; n_win++; }
        else if (trades[i] < 0) { sum_loss -= trades[i]; n_loss++; }
    }
    if (n_win == 0 || n_loss == 0) return 0.0;
    return (sum_win / (double)n_win) / (sum_loss / (double)n_loss);
}

/* max_dd_duration: longest drawdown in bars */
size_t exprtk_max_dd_duration(const double *equity, size_t n) {
    if (n < 2) return 0;
    double peak = equity[0];
    size_t max_dur = 0, cur_dur = 0;
    for (size_t i = 1; i < n; i++) {
        if (equity[i] >= peak) {
            peak = equity[i];
            if (cur_dur > max_dur) max_dur = cur_dur;
            cur_dur = 0;
        } else {
            cur_dur++;
        }
    }
    if (cur_dur > max_dur) max_dur = cur_dur;
    return max_dur;
}

/* ulcer_index = sqrt(mean(dd^2)) */
double exprtk_ulcer_index(const double *equity, size_t n) {
    if (n < 2) return 0.0;
    double peak = equity[0], sum_dd2 = 0;
    for (size_t i = 1; i < n; i++) {
        if (equity[i] > peak) peak = equity[i];
        double dd_pct = (peak - equity[i]) / peak * 100.0;
        sum_dd2 += dd_pct * dd_pct;
    }
    return sqrt(sum_dd2 / (double)(n - 1));
}

/* information_ratio = mean(excess) / std(excess) */
double exprtk_information_ratio(const double *ret, const double *bench, size_t n) {
    if (n < 2) return 0.0;
    double sum = 0, sum2 = 0;
    for (size_t i = 0; i < n; i++) {
        double e = ret[i] - bench[i];
        sum += e;
        sum2 += e * e;
    }
    double mean = sum / (double)n;
    double var = sum2 / (double)(n - 1) - (sum * sum) / ((double)n * (double)(n - 1));
    double te = sqrt(fabs(var));
    if (te < 1e-15) return 0.0;
    return mean / te;
}

/* treynor = (mean(ret) - rf) / beta, where beta = cov(ret,bench)/var(bench) */
double exprtk_treynor(const double *ret, const double *bench, size_t n, double rf) {
    if (n < 2) return 0.0;
    double sum_r = 0, sum_b = 0;
    for (size_t i = 0; i < n; i++) { sum_r += ret[i]; sum_b += bench[i]; }
    double mean_r = sum_r / (double)n;
    double mean_b = sum_b / (double)n;

    double cov = 0, var_b = 0;
    for (size_t i = 0; i < n; i++) {
        double dr = ret[i] - mean_r;
        double db = bench[i] - mean_b;
        cov += dr * db;
        var_b += db * db;
    }
    if (var_b < 1e-15) return 0.0;
    double beta = cov / var_b;
    if (fabs(beta) < 1e-15) return 0.0;
    return (mean_r - rf) / beta;
}
