/**
 * @file exprtk_finance.c
 * @brief Finance, backtest, portfolio, risk, signal, and time-series functions
 *
 * Extracted from the monolithic exprtk.c during refactoring.
 * Contains: backtesting engine, portfolio optimization, risk metrics,
 * signal processing, candlestick patterns, and time-series analysis.
 */

#include "fin_internal.h"
#include <simde/x86/avx2.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>




/* ========================================================================= */
/* Backtesting                                                               */
/* ========================================================================= */

size_t exprtk_bt_backtest(const double *open, const double *close, const double *signal, size_t n, double cash0, double commission, double *equity, double *trades) {
    if (n == 0 || cash0 <= 0) return 0;
    equity[0] = cash0;
    double cash = cash0;
    double position = 0;
    double entry_equity = 0;
    int direction = 0;
    size_t trade_idx = 0;
    for (size_t i = 1; i < n; ++i) {
        double sig = signal[i - 1];
        int desired = (sig > 0) ? 1 : (sig < 0) ? -1 : 0;
        double frac = fabs(sig);
        double open_p = open[i];
        if (open_p <= 0) { equity[i] = cash + position * close[i]; continue; }
        if (direction != 0 && desired != direction) {
            double proceeds = position * open_p;
            cash += proceeds - fabs(proceeds) * commission;
            if (entry_equity > 0) { trades[trade_idx++] = (cash - entry_equity) / entry_equity * 100.0; }
            position = 0; direction = 0;
        }
        if (desired != 0 && direction == 0) {
            entry_equity = cash;
            double notional = cash * (frac < 1.0 ? frac : 1.0);
            double comm = notional * commission;
            if (desired > 0) { position = (notional - comm) / open_p; cash -= notional; }
            else { position = -(notional / open_p); cash += notional - comm; }
            direction = desired;
        }
        equity[i] = cash + position * close[i];
    }
    if (direction != 0 && n > 0) {
        double last_c = close[n - 1];
        double proceeds = position * last_c;
        cash += proceeds - fabs(proceeds) * commission;
        if (entry_equity > 0) { trades[trade_idx++] = (cash - entry_equity) / entry_equity * 100.0; }
        equity[n - 1] = cash;
    }
    return trade_idx;
}

size_t exprtk_bt_stats(const double *equity, const double *trades, size_t n, size_t num_trades, double annual, double *out) {
    if (n < 2) return 0;
    for (size_t i = 0; i < 14; ++i) out[i] = 0;
    double eq0 = equity[0], eqN = equity[n - 1];
    out[0] = (eq0 > 0) ? (eqN - eq0) / eq0 * 100.0 : 0;
    double sum_ret = 0, sum_ret2 = 0, sum_neg2 = 0;
    size_t ret_count = 0;
    size_t i = 1;
    simde__m256d v_sum_ret = simde_mm256_setzero_pd();
    simde__m256d v_sum_ret2 = simde_mm256_setzero_pd();
    simde__m256d v_sum_neg2 = simde_mm256_setzero_pd();
    simde__m256d v_zero = simde_mm256_setzero_pd();

    for (; i + 4 <= n; i += 4) {
        simde__m256d v_eq1 = simde_mm256_loadu_pd(&equity[i]);
        simde__m256d v_eq0 = simde_mm256_loadu_pd(&equity[i - 1]);
        
        // r = (eq1 - eq0) / eq0
        simde__m256d v_r = simde_mm256_div_pd(simde_mm256_sub_pd(v_eq1, v_eq0), v_eq0);
        
        v_sum_ret = simde_mm256_add_pd(v_sum_ret, v_r);
        simde__m256d v_r2 = simde_mm256_mul_pd(v_r, v_r);
        v_sum_ret2 = simde_mm256_add_pd(v_sum_ret2, v_r2);
        
        simde__m256d v_neg_mask = simde_mm256_cmp_pd(v_r, v_zero, SIMDE_CMP_LT_OQ);
        v_sum_neg2 = simde_mm256_add_pd(v_sum_neg2, simde_mm256_and_pd(v_r2, v_neg_mask));
    }
    double tmp_ret[4], tmp_ret2[4], tmp_neg2[4];
    simde_mm256_storeu_pd(tmp_ret, v_sum_ret);
    simde_mm256_storeu_pd(tmp_ret2, v_sum_ret2);
    simde_mm256_storeu_pd(tmp_neg2, v_sum_neg2);
    for (int j = 0; j < 4; ++j) { sum_ret += tmp_ret[j]; sum_ret2 += tmp_ret2[j]; sum_neg2 += tmp_neg2[j]; }
    
    for (; i < n; ++i) {
        if (equity[i] > 0 && equity[i - 1] > 0) {
            double r = (equity[i] - equity[i - 1]) / equity[i - 1];
            sum_ret += r; sum_ret2 += r * r; if (r < 0) sum_neg2 += r * r;
        }
    }
    ret_count = n - 1;
    if (ret_count > 0 && annual > 0) {
        double years = (double)ret_count / annual;
        if (years > 0 && eqN / eq0 > 0) out[1] = (pow(eqN / eq0, 1.0 / years) - 1.0) * 100.0;
    }
    double peak = equity[0], max_dd = 0;
    for (size_t k = 1; k < n; ++k) {
        if (equity[k] > peak) peak = equity[k];
        double dd = (peak - equity[k]) / peak;
        if (dd > max_dd) max_dd = dd;
    }
    out[2] = max_dd * 100.0;
    double vol = (ret_count > 1) ? sqrt((sum_ret2 - sum_ret * sum_ret / (double)ret_count) / (double)(ret_count - 1)) : 0;
    double ann_vol = vol * sqrt(annual);
    out[3] = (ann_vol > 0) ? (out[1] / 100.0) / ann_vol : 0;
    double down_vol = (ret_count > 1) ? sqrt(sum_neg2 / (double)(ret_count - 1)) : 0;
    out[4] = (down_vol * sqrt(annual) > 0) ? (out[1] / 100.0) / (down_vol * sqrt(annual)) : 0;
    out[5] = (max_dd > 0) ? (out[1] / 100.0) / max_dd : 0;
    out[8] = (double)num_trades;
    if (num_trades > 0) {
        double sum_p = 0, sum_w = 0, sum_l = 0, best = trades[0], worst = trades[0];
        size_t wins = 0;
        for (size_t k = 0; k < num_trades; ++k) {
            double p = trades[k]; sum_p += p;
            if (p > 0) { sum_w += p; wins++; } else sum_l += p;
            if (p > best) best = p; if (p < worst) worst = p;
        }
        out[6] = (double)wins / (double)num_trades * 100.0;
        out[7] = (fabs(sum_l) > 0) ? sum_w / fabs(sum_l) : 0;
        out[9] = out[12] = sum_p / (double)num_trades;
        out[10] = best; out[11] = worst;
        if (num_trades > 1) {
            double var = 0, mean_p = sum_p / (double)num_trades;
            for (size_t k = 0; k < num_trades; ++k) { double d = trades[k] - mean_p; var += d * d; }
            double std = sqrt(var / (double)(num_trades - 1));
            out[13] = (std > 0) ? sqrt((double)num_trades) * mean_p / std : 0;
        }
    }
    return 14;
}

size_t exprtk_bt_backtest_ex(const double *open, const double *high, const double *low, const double *close, const double *volume, const double *signal, size_t n, double cash0, double commission, const double *config, size_t config_n, double *equity, double *trades, turbo_arena_t *arena) {
    if (n == 0 || cash0 <= 0 || config_n < 3) return 0;
    double sl = config[0], tp = config[1], slip_pct = config[2];
    double h_spread = (config_n > 3) ? config[3] : 0, v_impact = (config_n > 4) ? config[4] : 0, v_scale = (config_n > 5) ? config[5] : 0;
    (void)v_impact;
    double *atr = TEMP_ALLOC(arena, double, n);
    double r_sum = 0;
    for (size_t i = 0; i < n; ++i) {
        r_sum += high[i] - low[i];
        if (i >= 20) r_sum -= high[i - 20] - low[i - 20];
        atr[i] = r_sum / (double)((i < 20) ? (i + 1) : 20);
    }
    double cash = cash0, pos = 0, entry_p = 0, entry_eq = 0;
    int dir = 0; size_t trade_idx = 0; equity[0] = cash;
    for (size_t i = 1; i < n; ++i) {
        double sig = signal[i - 1], op = open[i];
        if (op <= 0) { equity[i] = cash + pos * close[i]; continue; }
        if (dir != 0 && entry_p > 0) {
            int stopped = 0; double exit_p = 0;
            if (sl > 0) {
                if (dir > 0 && low[i] <= entry_p * (1.0 - sl)) { exit_p = entry_p * (1.0 - sl); stopped = 1; }
                else if (dir < 0 && high[i] >= entry_p * (1.0 + sl)) { exit_p = entry_p * (1.0 + sl); stopped = 1; }
            }
            if (!stopped && tp > 0) {
                if (dir > 0 && high[i] >= entry_p * (1.0 + tp)) { exit_p = entry_p * (1.0 + tp); stopped = 1; }
                else if (dir < 0 && low[i] <= entry_p * (1.0 - tp)) { exit_p = entry_p * (1.0 - tp); stopped = 1; }
            }
            if (stopped) {
                double slip = slip_pct * exit_p + h_spread + v_scale * atr[i];
                double adj = (dir > 0) ? (exit_p - slip) : (exit_p + slip);
                double proceeds = pos * adj; cash += proceeds - fabs(proceeds) * commission;
                if (entry_eq > 0) trades[trade_idx++] = (cash - entry_eq) / entry_eq * 100.0;
                pos = 0; dir = 0; equity[i] = cash;
                if (sig == 0) continue;
            }
        }
        int desired = (sig > 0) ? 1 : (sig < 0) ? -1 : 0;
        if (dir != 0 && desired != dir) {
            double slip = slip_pct * op + h_spread + v_scale * atr[i];
            double adj = (dir > 0) ? (op - slip) : (op + slip);
            double proceeds = pos * adj; cash += proceeds - fabs(proceeds) * commission;
            if (entry_eq > 0) trades[trade_idx++] = (cash - entry_eq) / entry_eq * 100.0;
            pos = 0; dir = 0;
        }
        if (desired != 0 && dir == 0) {
            entry_eq = cash; double notional = cash * (fabs(sig) < 1.0 ? fabs(sig) : 1.0);
            double slip = slip_pct * op + h_spread + v_scale * atr[i];
            double adj = (desired > 0) ? (op + slip) : (op - slip);
            if (desired > 0) { pos = (notional - notional * commission) / adj; cash -= notional; }
            else { pos = -(notional / adj); cash += notional - notional * commission; }
            entry_p = adj; dir = desired;
        }
        equity[i] = cash + pos * close[i];
    }
    if (dir != 0) {
        double lp = close[n - 1]; double slip = slip_pct * lp + h_spread + v_scale * atr[n - 1];
        double adj = (dir > 0) ? (lp - slip) : (lp + slip);
        double proceeds = pos * adj; cash += proceeds - fabs(proceeds) * commission;
        if (entry_eq > 0) trades[trade_idx++] = (cash - entry_eq) / entry_eq * 100.0;
        equity[n - 1] = cash;
    }
    TEMP_FREE(arena, atr); return trade_idx;
}

size_t exprtk_bt_portfolio(const double *prices, const double *signals, size_t na, size_t nb, double cash0, double commission, double *equity, double *weights, turbo_arena_t *arena) {
    if (na == 0 || nb == 0 || cash0 <= 0) return 0;
    double *pos = TEMP_ALLOC(arena, double, na);
    double cash = cash0; size_t total_t = 0; equity[0] = cash;
    for (size_t t = 1; t < nb; ++t) {
        double active_sum = 0;
        for (size_t j = 0; j < na; ++j) active_sum += fabs(signals[(t - 1) * na + j]);
        double mtm = cash;
        for (size_t j = 0; j < na; ++j) mtm += pos[j] * prices[t * na + j];
        for (size_t j = 0; j < na; ++j) {
            double sig = signals[(t - 1) * na + j], pr = prices[t * na + j];
            if (pr <= 0) continue;
            int desired = (sig > 0) ? 1 : (sig < 0) ? -1 : 0;
            int current = (pos[j] > 0) ? 1 : (pos[j] < 0) ? -1 : 0;
            if (desired != current) {
                if (current != 0) { double proceeds = pos[j] * pr; cash += proceeds - fabs(proceeds) * commission; pos[j] = 0; total_t++; }
                if (desired != 0 && active_sum > 0) {
                    double alloc = mtm * fabs(sig) / active_sum;
                    if (desired > 0) { pos[j] = (alloc - alloc * commission) / pr; cash -= alloc; }
                    else { pos[j] = -(alloc / pr); cash += alloc - alloc * commission; }
                    total_t++;
                }
            }
        }
        double eq = cash; for (size_t j = 0; j < na; ++j) eq += pos[j] * prices[t * na + j];
        equity[t] = eq;
        for (size_t j = 0; j < na; ++j) weights[t * na + j] = (eq > 0) ? pos[j] * prices[t * na + j] / eq : 0;
    }
    TEMP_FREE(arena, pos); return total_t;
}

/* ========================================================================= */
/* Portfolio Optimization                                                    */
/* ========================================================================= */

void exprtk_pf_cov_matrix(const double *returns, size_t na, size_t np, double *out, turbo_arena_t *arena) {
    if (na == 0 || np < 2) return;
    double *mean = TEMP_ALLOC(arena, double, na);
    memset(mean, 0, na * sizeof(double));
    for (size_t t = 0; t < np; ++t) {
        for (size_t i = 0; i < na; ++i) mean[i] += returns[t * na + i];
    }
    for (size_t i = 0; i < na; ++i) mean[i] /= (double)np;

    for (size_t i = 0; i < na; ++i) {
        for (size_t j = i; j < na; ++j) out[i * na + j] = 0;
        for (size_t t = 0; t < np; ++t) {
            double term_i = returns[t * na + i] - mean[i];
            size_t j = i;
            simde__m256d v_term_i = simde_mm256_set1_pd(term_i);
            for (; j + 4 <= na; j += 4) {
                simde__m256d v_ret = simde_mm256_loadu_pd(&returns[t * na + j]);
                simde__m256d v_mean = simde_mm256_loadu_pd(&mean[j]);
                simde__m256d v_prod = simde_mm256_mul_pd(v_term_i, simde_mm256_sub_pd(v_ret, v_mean));
                simde__m256d v_out = simde_mm256_loadu_pd(&out[i * na + j]);
                simde_mm256_storeu_pd(&out[i * na + j], simde_mm256_add_pd(v_out, v_prod));
            }
            for (; j < na; ++j) {
                out[i * na + j] += term_i * (returns[t * na + j] - mean[j]);
            }
        }
        double inv_n = 1.0 / (double)(np - 1);
        for (size_t j = i; j < na; ++j) {
            out[i * na + j] *= inv_n;
            out[j * na + i] = out[i * na + j];
        }
    }
    TEMP_FREE(arena, mean);
}

double exprtk_pf_min_variance(const double *cov, size_t n, double *weights, turbo_arena_t *arena) {
    if (n == 0) return 0;
    double *inv = TEMP_ALLOC(arena, double, n * n);
    memcpy(inv, cov, n * n * sizeof(double));
    if (!gauss_jordan_invert(inv, n, arena)) { TEMP_FREE(arena, inv); return 0; }
    double *w_un = TEMP_ALLOC(arena, double, n);
    double sum_w = 0;
    for (size_t i = 0; i < n; ++i) { for (size_t j = 0; j < n; ++j) w_un[i] += inv[i * n + j]; sum_w += w_un[i]; }
    if (fabs(sum_w) < 1e-15) { TEMP_FREE(arena, inv); TEMP_FREE(arena, w_un); return 0; }
    double var = 0;
    for (size_t i = 0; i < n; ++i) { weights[i] = w_un[i] / sum_w; for (size_t j = 0; j < n; ++j) var += weights[i] * cov[i * n + j] * (w_un[j] / sum_w); }
    TEMP_FREE(arena, inv); TEMP_FREE(arena, w_un); return var;
}

double exprtk_pf_max_sharpe(const double *mu, const double *cov, size_t n, double rf, double *weights, turbo_arena_t *arena) {
    if (n == 0) return 0;
    double *inv = TEMP_ALLOC(arena, double, n * n);
    memcpy(inv, cov, n * n * sizeof(double));
    if (!gauss_jordan_invert(inv, n, arena)) { TEMP_FREE(arena, inv); return 0; }
    double *w_un = TEMP_ALLOC(arena, double, n);
    double sum_w = 0;
    for (size_t i = 0; i < n; ++i) { for (size_t j = 0; j < n; ++j) w_un[i] += inv[i * n + j] * (mu[j] - rf); sum_w += w_un[i]; }
    if (fabs(sum_w) < 1e-15) { TEMP_FREE(arena, inv); TEMP_FREE(arena, w_un); return 0; }
    for (size_t i = 0; i < n; ++i) weights[i] = w_un[i] / sum_w;
    double p_ret = 0, p_var = 0;
    for (size_t i = 0; i < n; ++i) { p_ret += weights[i] * mu[i]; for (size_t j = 0; j < n; ++j) p_var += weights[i] * cov[i * n + j] * weights[j]; }
    TEMP_FREE(arena, inv); TEMP_FREE(arena, w_un); return (p_var > 0) ? (p_ret - rf) / sqrt(p_var) : 0;
}

double exprtk_pf_markowitz(const double *mu, const double *cov, size_t n, double target, double *weights, turbo_arena_t *arena) {
    if (n == 0) return 0;
    double *inv = TEMP_ALLOC(arena, double, n * n);
    memcpy(inv, cov, n * n * sizeof(double));
    if (!gauss_jordan_invert(inv, n, arena)) { TEMP_FREE(arena, inv); return 0; }
    double A = 0, B = 0, C = 0;
    double *inv1 = TEMP_ALLOC(arena, double, n);
    double *inv_mu = TEMP_ALLOC(arena, double, n);
    memset(inv1, 0, n * sizeof(double));
    memset(inv_mu, 0, n * sizeof(double));
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) { inv1[i] += inv[i * n + j]; inv_mu[i] += inv[i * n + j] * mu[j]; }
        A += inv1[i] * mu[i]; B += inv_mu[i] * mu[i]; C += inv1[i];
    }
    double D = B * C - A * A;
    if (fabs(D) < 1e-15) { TEMP_FREE(arena, inv); TEMP_FREE(arena, inv1); TEMP_FREE(arena, inv_mu); return 0; }
    double l1 = (B - A * target) / D, l2 = (C * target - A) / D, var = 0;
    for (size_t i = 0; i < n; ++i) {
        weights[i] = l1 * inv1[i] + l2 * inv_mu[i];
        for (size_t j = 0; j < n; ++j) var += weights[i] * cov[i * n + j] * (l1 * inv1[j] + l2 * inv_mu[j]);
    }
    TEMP_FREE(arena, inv); TEMP_FREE(arena, inv1); TEMP_FREE(arena, inv_mu); return var;
}

double exprtk_pf_risk_parity(const double *cov, size_t n, double *weights, turbo_arena_t *arena) {
    if (n == 0) return 0;
    double *w = TEMP_ALLOC(arena, double, n);
    for (size_t i = 0; i < n; ++i) w[i] = 1.0 / (double)n;
    for (int iter = 0; iter < 500; ++iter) {
        double *mrc = TEMP_ALLOC(arena, double, n);
        double p_var = 0;
        memset(mrc, 0, n * sizeof(double));
        for (size_t i = 0; i < n; ++i) { for (size_t j = 0; j < n; ++j) mrc[i] += cov[i * n + j] * w[j]; p_var += w[i] * mrc[i]; }
        if (p_var < 1e-15) { TEMP_FREE(arena, mrc); break; }
        double *w_new = TEMP_ALLOC(arena, double, n);
        double sum_w = 0;
        for (size_t i = 0; i < n; ++i) { w_new[i] = (mrc[i] <= 0) ? w[i] : 1.0 / mrc[i]; sum_w += w_new[i]; }
        double m_diff = 0;
        for (size_t i = 0; i < n; ++i) { w_new[i] /= sum_w; double d = fabs(w_new[i] - w[i]); if (d > m_diff) m_diff = d; w[i] = w_new[i]; }
        TEMP_FREE(arena, mrc); TEMP_FREE(arena, w_new); if (m_diff < 1e-10) break;
    }
    double var = 0;
    for (size_t i = 0; i < n; ++i) { weights[i] = w[i]; for (size_t j = 0; j < n; ++j) var += w[i] * cov[i * n + j] * w[j]; }
    TEMP_FREE(arena, w); return sqrt(var);
}

/* ========================================================================= */
/* Risk Metrics                                                              */
/* ========================================================================= */

int exprtk_var_hist(const double *returns, size_t n, double confidence, double *out, turbo_arena_t *arena) {
    if (n == 0) return 0;
    double *sorted = TEMP_ALLOC(arena, double, n);
    memcpy(sorted, returns, n * sizeof(double));
    qsort(sorted, n, sizeof(double), compare_doubles);
    size_t idx = (size_t)floor((1.0 - confidence) * (double)n);
    if (idx >= n) idx = n - 1;
    *out = -sorted[idx];
    TEMP_FREE(arena, sorted); return 1;
}

int exprtk_var_param(const double *returns, size_t n, double confidence, double *out) {
    if (n < 2) return 0;
    double sum = 0, sum2 = 0;
    for (size_t i = 0; i < n; ++i) { sum += returns[i]; sum2 += returns[i] * returns[i]; }
    double mean = sum / (double)n, var = (sum2 - sum * sum / (double)n) / (double)(n - 1);
    if (var < 0) var = 0;
    *out = -(mean + sqrt(var) * inv_normal_cdf(1.0 - confidence));
    return 1;
}

int exprtk_cvar(const double *returns, size_t n, double confidence, double *out, turbo_arena_t *arena) {
    if (n == 0) return 0;
    double *sorted = TEMP_ALLOC(arena, double, n);
    memcpy(sorted, returns, n * sizeof(double));
    qsort(sorted, n, sizeof(double), compare_doubles);
    size_t idx = (size_t)floor((1.0 - confidence) * (double)n);
    if (idx == 0) idx = 1; if (idx > n) idx = n;
    double sum = 0; for (size_t i = 0; i < idx; ++i) sum += sorted[i];
    *out = -sum / (double)idx;
    TEMP_FREE(arena, sorted); return 1;
}

double exprtk_kelly(double win_rate, double avg_win, double avg_loss) {
    if (avg_loss <= 0) return 0;
    double b = avg_win / avg_loss;
    return (win_rate * (b + 1.0) - 1.0) / b;
}

double exprtk_fixed_frac(double equity, double risk_pct, double stop_dist) {
    if (stop_dist <= 0) return 0;
    return (equity * risk_pct) / stop_dist;
}

double exprtk_optimal_f(const double *trades, size_t n, double *out) {
    if (n == 0) return 0;
    double best_f = 0, max_tgw = 1.0, worst_l = 0;
    for (size_t i = 0; i < n; ++i) if (trades[i] < worst_l) worst_l = trades[i];
    if (worst_l >= 0) { *out = 1.0; return 1.0; }
    for (double f = 0.01; f <= 1.0; f += 0.01) {
        double tgw = 1.0;
        for (size_t i = 0; i < n; ++i) tgw *= (1.0 + f * (-trades[i] / worst_l));
        if (tgw > max_tgw) { max_tgw = tgw; best_f = f; }
    }
    *out = best_f; return best_f;
}

double exprtk_drawdown(const double *equity, size_t n, double *out) {
    if (n == 0) return 0;
    double peak = equity[0], max_dd = 0;
    for (size_t i = 0; i < n; ++i) {
        if (equity[i] > peak) peak = equity[i];
        double dd = (peak > 0) ? (peak - equity[i]) / peak : 0;
        out[i] = dd; if (dd > max_dd) max_dd = dd;
    }
    return max_dd;
}

int exprtk_drawdown_stats(const double *equity, size_t n, double *out) {
    if (n < 2) return 0;
    double peak = equity[0], max_dd = 0;
    size_t count = 0, max_dur = 0, curr_dur = 0;
    for (size_t i = 0; i < n; ++i) {
        if (equity[i] > peak) {
            if (curr_dur > max_dur) max_dur = curr_dur;
            peak = equity[i]; curr_dur = 0;
        } else if (equity[i] < peak) {
            double dd = (peak - equity[i]) / peak;
            if (dd > max_dd) max_dd = dd;
            curr_dur++; count++;
        }
    }
    if (curr_dur > max_dur) max_dur = curr_dur;
    out[0] = max_dd * 100.0; out[1] = (double)max_dur; out[2] = (count > 0) ? (double)count / (double)n * 100.0 : 0;
    return 3;
}

/* ========================================================================= */
/* Signal Processing & Candlestick Patterns                                  */
/* ========================================================================= */

size_t exprtk_crossover(const double *fast, const double *slow, size_t n, double *out) {
    size_t count = 0; if (n < 2) return 0; out[0] = 0;
    for (size_t i = 1; i < n; ++i) {
        if (fast[i] > slow[i] && fast[i - 1] <= slow[i - 1]) { out[i] = 1.0; count++; }
        else out[i] = 0;
    }
    return count;
}

size_t exprtk_crossunder(const double *fast, const double *slow, size_t n, double *out) {
    size_t count = 0; if (n < 2) return 0; out[0] = 0;
    for (size_t i = 1; i < n; ++i) {
        if (fast[i] < slow[i] && fast[i - 1] >= slow[i - 1]) { out[i] = 1.0; count++; }
        else out[i] = 0;
    }
    return count;
}

size_t exprtk_signal_combine(const double *signals, const double *weights, size_t n_bars, size_t n_signals, double *out) {
    if (n_bars == 0 || n_signals == 0) return 0;
    size_t count = 0;
    double w_sum = 0;
    for (size_t j = 0; j < n_signals; ++j) w_sum += fabs(weights[j]);
    if (w_sum < 1e-15) { memset(out, 0, n_bars * sizeof(double)); return 0; }

    for (size_t t = 0; t < n_bars; ++t) {
        double res = 0;
        size_t j = 0;
        const double *bar_sigs = &signals[t * n_signals];
        simde__m256d v_res = simde_mm256_setzero_pd();
        for (; j + 4 <= n_signals; j += 4) {
            v_res = simde_mm256_add_pd(v_res, simde_mm256_mul_pd(simde_mm256_loadu_pd(&bar_sigs[j]), simde_mm256_loadu_pd(&weights[j])));
        }
        double tmp[4];
        simde_mm256_storeu_pd(tmp, v_res);
        res = tmp[0] + tmp[1] + tmp[2] + tmp[3];
        for (; j < n_signals; ++j) res += bar_sigs[j] * weights[j];
        
        out[t] = res / w_sum;
        if (fabs(out[t]) > 1e-9) count++;
    }
    return count;
}

size_t exprtk_candle_doji(const double *O, const double *H, const double *L, const double *C, size_t n, double threshold, double *out) {
    size_t i = 0;
    simde__m256d v_thresh = simde_mm256_set1_pd(threshold);
    simde__m256d v_zero = simde_mm256_setzero_pd();
    simde__m256d v_one = simde_mm256_set1_pd(1.0);
    simde__m256d v_sign_mask = simde_mm256_set1_pd(-0.0);
    for (; i + 4 <= n; i += 4) {
        simde__m256d v_o = simde_mm256_loadu_pd(&O[i]);
        simde__m256d v_h = simde_mm256_loadu_pd(&H[i]);
        simde__m256d v_l = simde_mm256_loadu_pd(&L[i]);
        simde__m256d v_c = simde_mm256_loadu_pd(&C[i]);
        simde__m256d v_body = simde_mm256_andnot_pd(v_sign_mask, simde_mm256_sub_pd(v_c, v_o));
        simde__m256d v_range = simde_mm256_sub_pd(v_h, v_l);
        simde__m256d v_cond1 = simde_mm256_cmp_pd(v_range, v_zero, SIMDE_CMP_GT_OQ);
        simde__m256d v_cond2 = simde_mm256_cmp_pd(simde_mm256_div_pd(v_body, v_range), v_thresh, SIMDE_CMP_LT_OQ);
        simde_mm256_storeu_pd(&out[i], simde_mm256_and_pd(v_one, simde_mm256_and_pd(v_cond1, v_cond2)));
    }
    size_t count = 0;
    for (size_t k = 0; k < i; ++k) if (out[k] > 0.5) count++;
    for (; i < n; ++i) {
        double body = fabs(C[i] - O[i]), range = H[i] - L[i];
        if (range > 0 && body / range < threshold) { out[i] = 1.0; count++; }
        else out[i] = 0;
    }
    return count;
}

size_t exprtk_candle_hammer(const double *O, const double *H, const double *L, const double *C, size_t n, double *out) {
    size_t i = 0;

    simde__m256d v_one = simde_mm256_set1_pd(1.0);
    simde__m256d v_two = simde_mm256_set1_pd(2.0);
    simde__m256d v_01 = simde_mm256_set1_pd(0.1);
    simde__m256d v_sign_mask = simde_mm256_set1_pd(-0.0);
    for (; i + 4 <= n; i += 4) {
        simde__m256d v_o = simde_mm256_loadu_pd(&O[i]);
        simde__m256d v_h = simde_mm256_loadu_pd(&H[i]);
        simde__m256d v_l = simde_mm256_loadu_pd(&L[i]);
        simde__m256d v_c = simde_mm256_loadu_pd(&C[i]);
        simde__m256d v_body = simde_mm256_andnot_pd(v_sign_mask, simde_mm256_sub_pd(v_c, v_o));
        simde__m256d v_max_oc = simde_mm256_max_pd(v_o, v_c);
        simde__m256d v_min_oc = simde_mm256_min_pd(v_o, v_c);
        simde__m256d v_upper = simde_mm256_sub_pd(v_h, v_max_oc);
        simde__m256d v_lower = simde_mm256_sub_pd(v_min_oc, v_l);
        simde__m256d v_cond1 = simde_mm256_cmp_pd(v_lower, simde_mm256_mul_pd(v_two, v_body), SIMDE_CMP_GT_OQ);
        simde__m256d v_cond2 = simde_mm256_cmp_pd(v_upper, simde_mm256_mul_pd(v_01, v_lower), SIMDE_CMP_LT_OQ);
        simde_mm256_storeu_pd(&out[i], simde_mm256_and_pd(v_one, simde_mm256_and_pd(v_cond1, v_cond2)));
    }
    size_t count = 0;
    for (size_t k = 0; k < i; ++k) if (out[k] > 0.5) count++;
    for (; i < n; ++i) {
        double body = fabs(C[i] - O[i]), upper = H[i] - fmax(O[i], C[i]), lower = fmin(O[i], C[i]) - L[i];
        if (lower > 2.0 * body && upper < 0.1 * lower) { out[i] = 1.0; count++; }
        else out[i] = 0;
    }
    return count;
}

size_t exprtk_candle_engulfing(const double *O, const double *H, const double *L, const double *C, size_t n, double *out) {
    size_t count = 0; if (n < 2) return 0; out[0] = 0;
    size_t i = 1;
    simde__m256d v_one = simde_mm256_set1_pd(1.0);
    simde__m256d v_mone = simde_mm256_set1_pd(-1.0);
    simde__m256d v_zero = simde_mm256_setzero_pd();
    simde__m256d v_sign_mask = simde_mm256_set1_pd(-0.0);
    for (; i + 4 <= n; i += 4) {
        simde__m256d v_o1 = simde_mm256_loadu_pd(&O[i]);
        simde__m256d v_c1 = simde_mm256_loadu_pd(&C[i]);
        simde__m256d v_o0 = simde_mm256_loadu_pd(&O[i-1]);
        simde__m256d v_c0 = simde_mm256_loadu_pd(&C[i-1]);
        
        simde__m256d v_b1 = simde_mm256_sub_pd(v_c0, v_o0);
        simde__m256d v_b2 = simde_mm256_sub_pd(v_c1, v_o1);
        
        simde__m256d v_opp = simde_mm256_cmp_pd(simde_mm256_mul_pd(v_b1, v_b2), v_zero, SIMDE_CMP_LT_OQ);
        simde__m256d v_larger = simde_mm256_cmp_pd(simde_mm256_andnot_pd(v_sign_mask, v_b2), simde_mm256_andnot_pd(v_sign_mask, v_b1), SIMDE_CMP_GT_OQ);
        
        simde__m256d v_min0 = simde_mm256_min_pd(v_o0, v_c0);
        simde__m256d v_max0 = simde_mm256_max_pd(v_o0, v_c0);
        
        simde__m256d v_bull = simde_mm256_and_pd(simde_mm256_cmp_pd(v_b2, v_zero, SIMDE_CMP_GT_OQ),
            simde_mm256_and_pd(simde_mm256_cmp_pd(v_o1, v_min0, SIMDE_CMP_LE_OQ), simde_mm256_cmp_pd(v_c1, v_max0, SIMDE_CMP_GE_OQ)));
        
        simde__m256d v_bear = simde_mm256_and_pd(simde_mm256_cmp_pd(v_b2, v_zero, SIMDE_CMP_LT_OQ),
            simde_mm256_and_pd(simde_mm256_cmp_pd(v_o1, v_max0, SIMDE_CMP_GE_OQ), simde_mm256_cmp_pd(v_c1, v_min0, SIMDE_CMP_LE_OQ)));
            
        simde__m256d v_valid = simde_mm256_and_pd(v_opp, v_larger);
        simde__m256d v_res = simde_mm256_and_pd(v_valid, simde_mm256_or_pd(simde_mm256_and_pd(v_bull, v_one), simde_mm256_and_pd(v_bear, v_mone)));
        simde_mm256_storeu_pd(&out[i], v_res);
    }
    for (size_t k = 0; k < i; ++k) if (fabs(out[k]) > 0.5) count++;
    for (; i < n; ++i) {
        double b1 = C[i - 1] - O[i - 1], b2 = C[i] - O[i];
        if (b1 * b2 < 0 && fabs(b2) > fabs(b1)) {
            if (b2 > 0 && O[i] <= fmin(O[i - 1], C[i - 1]) && C[i] >= fmax(O[i - 1], C[i - 1])) { out[i] = 1.0; count++; }
            else if (b2 < 0 && O[i] >= fmax(O[i - 1], C[i - 1]) && C[i] <= fmin(O[i - 1], C[i - 1])) { out[i] = -1.0; count++; }
            else out[i] = 0;
        } else out[i] = 0;
    }
    return count;
}

size_t exprtk_candle_morningstar(const double *O, const double *H, const double *L, const double *C, size_t n, double *out) {
    size_t count = 0; if (n < 3) return 0; out[0] = out[1] = 0;
    for (size_t i = 2; i < n; ++i) {
        double b1 = C[i - 2] - O[i - 2], b2 = C[i - 1] - O[i - 1], b3 = C[i] - O[i];
        if (b1 < -0.01 * O[i - 2] && fabs(b2) < 0.1 * fabs(b1) && b3 > 0.5 * fabs(b1)) {
            if (fmax(O[i - 1], C[i - 1]) < fmin(O[i - 2], C[i - 2]) && fmin(O[i], C[i]) > fmax(O[i - 1], C[i - 1])) { out[i] = 1.0; count++; }
            else out[i] = 0;
        } else out[i] = 0;
    }
    return count;
}

/* ========================================================================= */
/* Time-Series Analysis                                                      */
/* ========================================================================= */

size_t exprtk_ts_diff(const double *data, size_t n, size_t order, double *out, turbo_arena_t *arena) {
    if (n <= order) return 0;
    double *tmp = TEMP_ALLOC(arena, double, n); memcpy(tmp, data, n * sizeof(double));
    for (size_t o = 0; o < order; ++o) {
        for (size_t i = n - 1; i > o; --i) tmp[i] = tmp[i] - tmp[i - 1];
    }
    for (size_t i = 0; i < n; ++i) out[i] = (i < order) ? 0 : tmp[i];
    TEMP_FREE(arena, tmp); return n - order;
}

size_t exprtk_ts_autocorr(const double *data, size_t n, size_t max_lag, double *out) {
    if (n < 2 || max_lag >= n) return 0;
    double sum = 0;
    size_t i = 0;
    simde__m256d v_sum = simde_mm256_setzero_pd();
    for (; i + 4 <= n; i += 4) {
        v_sum = simde_mm256_add_pd(v_sum, simde_mm256_loadu_pd(&data[i]));
    }
    double tmp_s[4]; simde_mm256_storeu_pd(tmp_s, v_sum);
    sum = tmp_s[0] + tmp_s[1] + tmp_s[2] + tmp_s[3];
    for (; i < n; ++i) sum += data[i];
    
    double mean = sum / (double)n, var = 0;
    simde__m256d v_mean = simde_mm256_set1_pd(mean);
    simde__m256d v_var = simde_mm256_setzero_pd();
    for (i = 0; i + 4 <= n; i += 4) {
        simde__m256d v_d = simde_mm256_sub_pd(simde_mm256_loadu_pd(&data[i]), v_mean);
        v_var = simde_mm256_add_pd(v_var, simde_mm256_mul_pd(v_d, v_d));
    }
    double tmp_v[4]; simde_mm256_storeu_pd(tmp_v, v_var);
    var = tmp_v[0] + tmp_v[1] + tmp_v[2] + tmp_v[3];
    for (; i < n; ++i) var += (data[i] - mean) * (data[i] - mean);

    if (var < 1e-15) { for (size_t l = 0; l <= max_lag; ++l) out[l] = 1.0; return max_lag + 1; }
    
    for (size_t l = 0; l <= max_lag; ++l) {
        double cv = 0;
        size_t j = l;
        simde__m256d v_cv = simde_mm256_setzero_pd();
        for (; j + 4 <= n; j += 4) {
            simde__m256d v_d1 = simde_mm256_sub_pd(simde_mm256_loadu_pd(&data[j]), v_mean);
            simde__m256d v_d2 = simde_mm256_sub_pd(simde_mm256_loadu_pd(&data[j - l]), v_mean);
            v_cv = simde_mm256_add_pd(v_cv, simde_mm256_mul_pd(v_d1, v_d2));
        }
        double tmp_cv[4]; simde_mm256_storeu_pd(tmp_cv, v_cv);
        cv = tmp_cv[0] + tmp_cv[1] + tmp_cv[2] + tmp_cv[3];
        for (; j < n; ++j) cv += (data[j] - mean) * (data[j - l] - mean);
        out[l] = cv / var;
    }
    return max_lag + 1;
}

size_t exprtk_ts_pacf(const double *data, size_t n, size_t max_lag, double *out, turbo_arena_t *arena) {
    if (n < 2 || max_lag >= n) return 0;
    double *rho = TEMP_ALLOC(arena, double, max_lag + 1);

    exprtk_ts_autocorr(data, n, max_lag, rho);
    out[0] = 1.0; if (max_lag == 0) { TEMP_FREE(arena, rho); return 1; }
    out[1] = rho[1];
    double *phi = TEMP_ALLOC(arena, double, (max_lag + 1) * (max_lag + 1));
    if (phi) memset(phi, 0, (max_lag + 1) * (max_lag + 1) * sizeof(double));

    phi[1 * (max_lag + 1) + 1] = rho[1];
    for (size_t k = 2; k <= max_lag; ++k) {
        double num = rho[k];
        for (size_t j = 1; j < k; ++j) { num -= phi[(k - 1) * (max_lag + 1) + j] * rho[k - j]; }
        double den_dl = 1.0;
        for (size_t j = 1; j < k; ++j) den_dl -= phi[(k - 1) * (max_lag + 1) + j] * rho[j];
        phi[k * (max_lag + 1) + k] = num / den_dl;
        for (size_t j = 1; j < k; ++j) phi[k * (max_lag + 1) + j] = phi[(k - 1) * (max_lag + 1) + j] - phi[k * (max_lag + 1) + k] * phi[(k - 1) * (max_lag + 1) + (k - j)];
        out[k] = phi[k * (max_lag + 1) + k];
    }
    TEMP_FREE(arena, rho); TEMP_FREE(arena, phi); return max_lag + 1;
}

int exprtk_ts_adf(const double *data, size_t n, size_t p, double *out, turbo_arena_t *arena) {
    if (n <= p + 2) return 0;
    size_t m = n - 1 - p;
    double *dy = TEMP_ALLOC(arena, double, m);
    double *y_lag = TEMP_ALLOC(arena, double, m);
    double *X = TEMP_ALLOC(arena, double, m * (p + 1));

    for (size_t i = 0; i < m; ++i) {
        dy[i] = data[i + p + 1] - data[i + p]; y_lag[i] = data[i + p];
        X[i * (p + 1)] = y_lag[i];
        for (size_t j = 1; j <= p; ++j) X[i * (p + 1) + j] = data[i + p + 1 - j] - data[i + p - j];
    }
    ols_result_t r = ols_fit(dy, y_lag, m);
    out[0] = r.t_slope; out[1] = r.slope;
    TEMP_FREE(arena, dy); TEMP_FREE(arena, y_lag); TEMP_FREE(arena, X); return 2;
}

size_t exprtk_ts_garch(const double *returns, size_t n, double alpha, double beta, double *out) {
    if (n == 0) return 0;
    double var = 0; for (size_t i = 0; i < n; ++i) var += returns[i] * returns[i];
    var /= (double)n; double omega = var * (1.0 - alpha - beta);
    if (omega < 0) omega = 1e-6;
    out[0] = var;
    for (size_t i = 1; i < n; ++i) out[i] = omega + alpha * returns[i - 1] * returns[i - 1] + beta * out[i - 1];
    return n;
}

double exprtk_ts_hurst(const double *data, size_t n, double *out, turbo_arena_t *arena) {
    if (n < 8) return 0;
    size_t m = 0; for (size_t k = 4; k <= n / 2; k *= 2) m++;
    double *x = TEMP_ALLOC(arena, double, m);
    double *y = TEMP_ALLOC(arena, double, m);
    size_t idx = 0;
    for (size_t k = 4; k <= n / 2; k *= 2) {
        size_t num_blocks = n / k; double sum_rs = 0;
        for (size_t b = 0; b < num_blocks; ++b) {
            double b_sum = 0; for (size_t i = 0; i < k; ++i) b_sum += data[b * k + i];
            double b_mean = b_sum / (double)k, b_var = 0;
            double *cum = TEMP_ALLOC(arena, double, k);
            double c_sum = 0;
            for (size_t i = 0; i < k; ++i) {
                double d = data[b * k + i] - b_mean; b_var += d * d;
                c_sum += d; cum[i] = c_sum;
            }
            double min_c = cum[0], max_c = cum[0];
            for (size_t i = 1; i < k; ++i) { if (cum[i] < min_c) min_c = cum[i]; if (cum[i] > max_c) max_c = cum[i]; }
            double s = sqrt(b_var / (double)k);
            if (s > 1e-15) sum_rs += (max_c - min_c) / s;
            TEMP_FREE(arena, cum);
        }
        x[idx] = log((double)k); y[idx] = log(sum_rs / (double)num_blocks); idx++;
    }
    ols_result_t r = ols_fit(y, x, m);
    TEMP_FREE(arena, x); TEMP_FREE(arena, y); return r.slope;
}
#include <simsimd/simsimd.h>

size_t exprtk_ts_match(const double *data, const double *pattern, size_t n, size_t m, double *out) {
    if (n < m || m == 0) return 0;
    
    // We compute the squared Euclidean distance between the pattern and each window in data
    for (size_t i = 0; i <= n - m; ++i) {
        simsimd_distance_t dist;
        simsimd_l2sq_f64(&data[i], pattern, (simsimd_size_t)m, &dist);
        out[i] = (double)dist;
    }
    
    // Zero out the rest of the output vector
    for (size_t i = n - m + 1; i < n; ++i) {
        out[i] = 0;
    }
    
    return n - m + 1;
}
size_t exprtk_ts_match_cosine(const double *data, const double *pattern, size_t n, size_t m, double *out) {
    if (n < m || m == 0) return 0;
    for (size_t i = 0; i <= n - m; ++i) {
        simsimd_distance_t dist;
        simsimd_cos_f64(&data[i], pattern, (simsimd_size_t)m, &dist);
        out[i] = (double)dist;
    }
    for (size_t i = n - m + 1; i < n; ++i) out[i] = 0;
    return n - m + 1;
}

static void zn_normalize(const double *in, size_t n, double *out) {
    double sum = 0, sum2 = 0;
    for (size_t i = 0; i < n; ++i) { sum += in[i]; sum2 += in[i] * in[i]; }
    double mean = sum / n;
    double var = (sum2 / n) - (mean * mean);
    double std = (var > 0) ? sqrt(var) : 1e-9;
    for (size_t i = 0; i < n; ++i) out[i] = (in[i] - mean) / std;
}

size_t exprtk_ts_match_normalized(const double *data, const double *pattern, size_t n, size_t m, double *out, turbo_arena_t *arena) {
    if (n < m || m == 0) return 0;
    double *p_norm = (double *)TURBO_ARENA_ALLOC_ARRAY(arena, double, m);
    double *w_norm = (double *)TURBO_ARENA_ALLOC_ARRAY(arena, double, m);
    if (!p_norm || !w_norm) return 0;
    
    zn_normalize(pattern, m, p_norm);
    
    for (size_t i = 0; i <= n - m; ++i) {
        zn_normalize(&data[i], m, w_norm);
        simsimd_distance_t dist;
        simsimd_l2sq_f64(w_norm, p_norm, (simsimd_size_t)m, &dist);
        out[i] = (double)dist;
    }
    
    for (size_t i = n - m + 1; i < n; ++i) out[i] = 0;
    return n - m + 1;
}
static void get_candle_features(const double *O, const double *H, const double *L, const double *C, size_t n, double *out) {
    for (size_t i = 0; i < n; ++i) {
        double range = H[i] - L[i];
        if (range < 1e-9) { out[i*3] = out[i*3+1] = out[i*3+2] = 0; continue; }
        out[i*3]   = (C[i] - O[i]) / range;                             // Body
        out[i*3+1] = (H[i] - fmax(O[i], C[i])) / range;              // Upper Shadow
        out[i*3+2] = (fmin(O[i], C[i]) - L[i]) / range;              // Lower Shadow
    }
}

size_t exprtk_ts_match_candle(const double *O, const double *H, const double *L, const double *C,
                            const double *pO, const double *pH, const double *pL, const double *pC,
                            size_t n, size_t m, double *out, turbo_arena_t *arena) {
    if (n < m || m == 0) return 0;
    double *p_feat = (double *)TURBO_ARENA_ALLOC_ARRAY(arena, double, m * 3);
    double *w_feat = (double *)TURBO_ARENA_ALLOC_ARRAY(arena, double, m * 3);
    if (!p_feat || !w_feat) return 0;

    get_candle_features(pO, pH, pL, pC, m, p_feat);
    
    for (size_t i = 0; i <= n - m; ++i) {
        get_candle_features(&O[i], &H[i], &L[i], &C[i], m, w_feat);
        simsimd_distance_t dist;
        simsimd_l2sq_f64(w_feat, p_feat, (simsimd_size_t)(m * 3), &dist);
        out[i] = (double)dist;
    }
    for (size_t i = n - m + 1; i < n; ++i) out[i] = 0;
    return n - m + 1;
}

// DTW Distance implementation
static double dtw_dist(const double *a, const double *b, size_t m, double *cost_mat) {
    // cost_mat should be (m+1) * (m+1)
    #define COST(r, c) cost_mat[(r) * (m + 1) + (c)]
    for (size_t i = 0; i <= m; ++i) { COST(i, 0) = 1e30; COST(0, i) = 1e30; }
    COST(0, 0) = 0;
    for (size_t i = 1; i <= m; ++i) {
        for (size_t j = 1; j <= m; ++j) {
            double d = (a[i-1] - b[j-1]);
            double diff = d * d;
            double min_prev = fmin(COST(i-1, j), fmin(COST(i, j-1), COST(i-1, j-1)));
            COST(i, j) = diff + min_prev;
        }
    }
    return COST(m, m);
    #undef COST
}

size_t exprtk_ts_match_dtw(const double *data, const double *pattern, size_t n, size_t m, double *out, turbo_arena_t *arena) {
    if (n < m || m == 0) return 0;
    double *cost_mat = (double *)TURBO_ARENA_ALLOC_ARRAY(arena, double, (m + 1) * (m + 1));
    if (!cost_mat) return 0;
    
    for (size_t i = 0; i <= n - m; ++i) {
        out[i] = dtw_dist(&data[i], pattern, m, cost_mat);
    }
    for (size_t i = n - m + 1; i < n; ++i) out[i] = 0;
    return n - m + 1;
}

size_t exprtk_ts_match_correl(const double *data, const double *pattern, size_t n, size_t m, double *out, turbo_arena_t *arena) {
    if (n < m || m == 0) return 0;
    double *p_norm = (double *)TURBO_ARENA_ALLOC_ARRAY(arena, double, m);
    double *w_norm = (double *)TURBO_ARENA_ALLOC_ARRAY(arena, double, m);
    if (!p_norm || !w_norm) return 0;
    
    zn_normalize(pattern, m, p_norm);
    
    for (size_t i = 0; i <= n - m; ++i) {
        zn_normalize(&data[i], m, w_norm);
        simsimd_distance_t dist_sq;
        simsimd_l2sq_f64(w_norm, p_norm, (simsimd_size_t)m, &dist_sq);
        out[i] = 1.0 - (dist_sq / (2.0 * m));
    }
    for (size_t i = n - m + 1; i < n; ++i) out[i] = 0;
    return n - m + 1;
}

size_t exprtk_ts_match_returns(const double *data, const double *pattern, size_t n, size_t m, double *out, turbo_arena_t *arena) {
    if (n < m || m < 2) return 0;
    double *p_ret = (double *)TURBO_ARENA_ALLOC_ARRAY(arena, double, m - 1);
    double *w_ret = (double *)TURBO_ARENA_ALLOC_ARRAY(arena, double, m - 1);
    if (!p_ret || !w_ret) return 0;
    
    for (size_t j = 0; j < m - 1; ++j) {
        double r = (pattern[j+1] > 1e-9 && pattern[j] > 1e-9) ? log(pattern[j+1] / pattern[j]) : 0;
        p_ret[j] = r;
    }
    
    for (size_t i = 0; i <= n - m; ++i) {
        for (size_t j = 0; j < m - 1; ++j) {
            double r = (data[i+j+1] > 1e-9 && data[i+j] > 1e-9) ? log(data[i+j+1] / data[i+j]) : 0;
            w_ret[j] = r;
        }
        simsimd_distance_t dist;
        simsimd_l2sq_f64(w_ret, p_ret, (simsimd_size_t)(m - 1), &dist);
        out[i] = (double)dist;
    }
    for (size_t i = n - m + 1; i < n; ++i) out[i] = 0;
    return n - m + 1;
}

size_t exprtk_ta_pivot_high(const double *hi, size_t n, size_t left, size_t right, double *out) {
    if (n < left + right + 1) return 0;
    size_t period = left + right + 1;
    size_t *deque = (size_t*)malloc(n * sizeof(size_t));
    if (!deque) return 0;
    size_t head = 0, tail = 0;
    size_t count = 0;
    memset(out, 0, n * sizeof(double));
    for (size_t i = 0; i < n; ++i) {
        while (tail > head && hi[i] > hi[deque[tail - 1]]) tail--;
        deque[tail++] = i;
        if (i >= period && deque[head] <= i - period) head++;
        if (i >= period - 1) {
            size_t pivot_idx = i - right;
            if (deque[head] == pivot_idx) {
                out[pivot_idx] = hi[pivot_idx];
                count++;
            }
        }
    }
    free(deque);
    return count;
}


size_t exprtk_ta_pivot_low(const double *lo, size_t n, size_t left, size_t right, double *out) {
    if (n < left + right + 1) return 0;
    size_t period = left + right + 1;
    size_t *deque = (size_t*)malloc(n * sizeof(size_t));
    if (!deque) return 0;
    size_t head = 0, tail = 0;
    size_t count = 0;
    memset(out, 0, n * sizeof(double));
    for (size_t i = 0; i < n; ++i) {
        while (tail > head && lo[i] < lo[deque[tail - 1]]) tail--;
        deque[tail++] = i;
        if (i >= period && deque[head] <= i - period) head++;
        if (i >= period - 1) {
            size_t pivot_idx = i - right;
            if (deque[head] == pivot_idx) {
                out[pivot_idx] = lo[pivot_idx];
                count++;
            }
        }
    }
    free(deque);
    return count;
}


size_t exprtk_ta_er(const double *in, size_t n, size_t period, double *out) {
    if (n <= period) return 0;
    double sum_vol = 0;
    for (size_t j = 1; j <= period; ++j) sum_vol += fabs(in[j] - in[j - 1]);
    for (size_t i = 0; i < n; ++i) {
        if (i < period) { out[i] = 0; continue; }
        if (i > period) sum_vol += fabs(in[i] - in[i - 1]) - fabs(in[i - period] - in[i - period - 1]);
        double chg = fabs(in[i] - in[i - period]);
        out[i] = (sum_vol > 1e-15) ? chg / sum_vol : 0;
    }
    return n;
}


size_t exprtk_ta_bias(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena) {
    if (n < period) return 0;
    double *sma = TEMP_ALLOC(arena, double, n);
    if (!sma) return 0;
    exprtk_ta_sma(in, n, period, sma);
    size_t i = 0;
    simde__m256d v_zero = simde_mm256_setzero_pd();
    simde__m256d v_100 = simde_mm256_set1_pd(100.0);
    for (; i + 4 <= n; i += 4) {
        simde__m256d v_in = simde_mm256_loadu_pd(&in[i]);
        simde__m256d v_sma = simde_mm256_loadu_pd(&sma[i]);
        simde__m256d v_mask = simde_mm256_cmp_pd(v_sma, v_zero, SIMDE_CMP_GT_OQ);
        simde__m256d v_res = simde_mm256_mul_pd(simde_mm256_div_pd(simde_mm256_sub_pd(v_in, v_sma), v_sma), v_100);
        simde_mm256_storeu_pd(&out[i], simde_mm256_and_pd(v_res, v_mask));
    }
    for (; i < n; ++i) {
        if (i < period - 1) { out[i] = 0; continue; }
        out[i] = (sma[i] > 1e-15) ? (in[i] - sma[i]) / sma[i] * 100.0 : 0;
    }
    TEMP_FREE(arena, sma);
    return n;
}


size_t exprtk_ta_psy(const double *cl, size_t n, size_t period, double *out, turbo_arena_t *arena) {
    if (n <= period || period == 0) return 0;
    double *inc = TEMP_ALLOC(arena, double, n);
    if (!inc) return 0;
    
    inc[0] = 0;
    size_t i = 1;

    simde__m256d v_one = simde_mm256_set1_pd(1.0);
    for (; i + 4 <= n; i += 4) {
        simde__m256d v_c1 = simde_mm256_loadu_pd(&cl[i]);
        simde__m256d v_c0 = simde_mm256_loadu_pd(&cl[i - 1]);
        simde__m256d v_mask = simde_mm256_cmp_pd(v_c1, v_c0, SIMDE_CMP_GT_OQ);
        simde_mm256_storeu_pd(&inc[i], simde_mm256_and_pd(v_one, v_mask));
    }
    for (; i < n; ++i) inc[i] = (cl[i] > cl[i - 1]) ? 1.0 : 0.0;
    
    double up_count = 0;
    for (size_t j = 1; j <= period; ++j) up_count += inc[j];
    out[period] = up_count / (double)period * 100.0;
    for (i = period + 1; i < n; ++i) {
        up_count += inc[i] - inc[i - period];
        out[i] = up_count / (double)period * 100.0;
    }
    TEMP_FREE(arena, inc);
    return n;
}



size_t exprtk_ta_pressure(const double *hi, const double *lo, const double *cl, const double *vol, size_t n, double *buy_pres, double *sell_pres) {
    if (n == 0) return 0;
    size_t i = 0;
    simde__m256d v_zero = simde_mm256_setzero_pd();
    for (; i + 4 <= n; i += 4) {
        simde__m256d v_hi = simde_mm256_loadu_pd(&hi[i]);
        simde__m256d v_lo = simde_mm256_loadu_pd(&lo[i]);
        simde__m256d v_cl = simde_mm256_loadu_pd(&cl[i]);
        simde__m256d v_vol = simde_mm256_loadu_pd(&vol[i]);
        simde__m256d v_range = simde_mm256_sub_pd(v_hi, v_lo);
        simde__m256d v_mask = simde_mm256_cmp_pd(v_range, v_zero, SIMDE_CMP_GT_OQ);
        simde__m256d v_buy = simde_mm256_div_pd(simde_mm256_sub_pd(v_cl, v_lo), v_range);
        simde__m256d v_sell = simde_mm256_div_pd(simde_mm256_sub_pd(v_hi, v_cl), v_range);
        v_buy = simde_mm256_and_pd(simde_mm256_mul_pd(v_buy, v_vol), v_mask);
        v_sell = simde_mm256_and_pd(simde_mm256_mul_pd(v_sell, v_vol), v_mask);
        simde_mm256_storeu_pd(&buy_pres[i], v_buy);
        simde_mm256_storeu_pd(&sell_pres[i], v_sell);
    }
    for (; i < n; ++i) {
        double range = hi[i] - lo[i];
        if (range > 0) {
            buy_pres[i] = (cl[i] - lo[i]) / range * vol[i];
            sell_pres[i] = (hi[i] - cl[i]) / range * vol[i];
        } else {
            buy_pres[i] = 0; sell_pres[i] = 0;
        }
    }
    return n;
}


size_t exprtk_ta_kvo(const double *hi, const double *lo, const double *cl, const double *vol, size_t n, size_t fast_p, size_t slow_p, size_t sig_p, double *kvo, double *sig, turbo_arena_t *arena) {
    if (n < 2) return 0;
    double *v_force = TEMP_ALLOC(arena, double, n);
    if (!v_force) return 0;
    v_force[0] = 0;
    size_t i = 1;
    simde__m256d v_inv3 = simde_mm256_set1_pd(1.0 / 3.0);
    simde__m256d v_100 = simde_mm256_set1_pd(100.0);
    simde__m256d v_m100 = simde_mm256_set1_pd(-100.0);
    for (; i + 4 <= n; i += 4) {
        simde__m256d v_h1 = simde_mm256_loadu_pd(&hi[i]);
        simde__m256d v_l1 = simde_mm256_loadu_pd(&lo[i]);
        simde__m256d v_c1 = simde_mm256_loadu_pd(&cl[i]);
        simde__m256d v_h0 = simde_mm256_loadu_pd(&hi[i-1]);
        simde__m256d v_l0 = simde_mm256_loadu_pd(&lo[i-1]);
        simde__m256d v_c0 = simde_mm256_loadu_pd(&cl[i-1]);
        simde__m256d v_tp1 = simde_mm256_mul_pd(simde_mm256_add_pd(v_h1, simde_mm256_add_pd(v_l1, v_c1)), v_inv3);
        simde__m256d v_tp0 = simde_mm256_mul_pd(simde_mm256_add_pd(v_h0, simde_mm256_add_pd(v_l0, v_c0)), v_inv3);
        simde__m256d v_gt = simde_mm256_cmp_pd(v_tp1, v_tp0, SIMDE_CMP_GT_OQ);
        simde__m256d v_vol = simde_mm256_loadu_pd(&vol[i]);
        simde__m256d v_res = simde_mm256_blendv_pd(v_m100, v_100, v_gt);
        simde_mm256_storeu_pd(&v_force[i], simde_mm256_mul_pd(v_res, v_vol));
    }
    for (; i < n; ++i) {
        double tp = (hi[i] + lo[i] + cl[i]) / 3.0, ptp = (hi[i-1] + lo[i-1] + cl[i-1]) / 3.0;
        v_force[i] = (tp > ptp ? 100.0 : -100.0) * vol[i];
    }

    double *fast_ema = TEMP_ALLOC(arena, double, n);
    double *slow_ema = TEMP_ALLOC(arena, double, n);
    if (fast_ema && slow_ema) {
        ta_ema_calc(v_force, n, fast_p, fast_ema);
        ta_ema_calc(v_force, n, slow_p, slow_ema);
        for (i = 0; i < n; ++i) kvo[i] = fast_ema[i] - slow_ema[i];
        ta_ema_calc(kvo, n, sig_p, sig);
    }
    TEMP_FREE(arena, v_force); TEMP_FREE(arena, fast_ema); TEMP_FREE(arena, slow_ema);
    return n;
}

size_t exprtk_candle_body_size  (const double *O, const double *C, const double *H, const double *L, size_t n, double *out) {
    (void)H; (void)L; for (size_t i = 0; i < n; ++i) out[i] = fabs(C[i] - O[i]); return n;
}
size_t exprtk_candle_wick_upper (const double *O, const double *C, const double *H, size_t n, double *out) {
    for (size_t i = 0; i < n; ++i) out[i] = H[i] - fmax(O[i], C[i]); return n;
}
size_t exprtk_candle_wick_lower (const double *O, const double *C, const double *L, size_t n, double *out) {
    for (size_t i = 0; i < n; ++i) out[i] = fmin(O[i], C[i]) - L[i]; return n;
}
size_t exprtk_candle_direction  (const double *O, const double *C, size_t n, double *out) {
    for (size_t i = 0; i < n; ++i) out[i] = (C[i] > O[i] ? 1.0 : (C[i] < O[i] ? -1.0 : 0.0)); return n;
}

size_t exprtk_candle_fuzzy_bull(const double *O, const double *H, const double *L, const double *C, size_t n, double *out) {
    size_t i = 0;
    simde__m256d v_zero = simde_mm256_setzero_pd();
    simde__m256d v_half = simde_mm256_set1_pd(0.5);
    for (; i + 4 <= n; i += 4) {
        simde__m256d v_h = simde_mm256_loadu_pd(&H[i]);
        simde__m256d v_l = simde_mm256_loadu_pd(&L[i]);
        simde__m256d v_c = simde_mm256_loadu_pd(&C[i]);
        simde__m256d v_r = simde_mm256_sub_pd(v_h, v_l);
        simde__m256d v_mask = simde_mm256_cmp_pd(v_r, v_zero, SIMDE_CMP_GT_OQ);
        simde__m256d v_val = simde_mm256_div_pd(simde_mm256_sub_pd(v_c, v_l), v_r);
        simde_mm256_storeu_pd(&out[i], simde_mm256_blendv_pd(v_half, v_val, v_mask));
    }
    for (; i < n; ++i) {
        double r = H[i] - L[i];
        if (r > 0) out[i] = (C[i] - L[i]) / r; else out[i] = 0.5;
    }
    return n;
}
size_t exprtk_candle_fuzzy_bear(const double *O, const double *H, const double *L, const double *C, size_t n, double *out) {
    size_t i = 0;
    simde__m256d v_zero = simde_mm256_setzero_pd();
    simde__m256d v_half = simde_mm256_set1_pd(0.5);
    for (; i + 4 <= n; i += 4) {
        simde__m256d v_h = simde_mm256_loadu_pd(&H[i]);
        simde__m256d v_l = simde_mm256_loadu_pd(&L[i]);
        simde__m256d v_c = simde_mm256_loadu_pd(&C[i]);
        simde__m256d v_r = simde_mm256_sub_pd(v_h, v_l);
        simde__m256d v_mask = simde_mm256_cmp_pd(v_r, v_zero, SIMDE_CMP_GT_OQ);
        simde__m256d v_val = simde_mm256_div_pd(simde_mm256_sub_pd(v_h, v_c), v_r);
        simde_mm256_storeu_pd(&out[i], simde_mm256_blendv_pd(v_half, v_val, v_mask));
    }
    for (; i < n; ++i) {
        double r = H[i] - L[i];
        if (r > 0) out[i] = (H[i] - C[i]) / r; else out[i] = 0.5;
    }
    return n;
}
size_t exprtk_vec_rank(const double *in, size_t n, double *out, turbo_arena_t *arena) {
    if (n == 0) return 0;
    rank_item_t *items = TEMP_ALLOC(arena, rank_item_t, n);
    if (!items) return 0;
    for (size_t i = 0; i < n; i++) { items[i].idx = i; items[i].val = in[i]; }
    qsort(items, n, sizeof(rank_item_t), compare_rank_items);
    for (size_t i = 0; i < n; i++) {
        size_t j = i;
        while (j < n && fabs(items[j].val - items[i].val) < 1e-15) j++;
        double avg_rank = (double)(i + j - 1) / 2.0;
        double pct_rank = (n > 1) ? avg_rank / (double)(n - 1) : 0.5;
        for (size_t k = i; k < j; k++) out[items[k].idx] = pct_rank;
        i = j - 1;
    }
    return n;
}

size_t exprtk_vec_zscore(const double *in, size_t n, double *out, turbo_arena_t *arena) {
    (void)arena;
    if (n < 2) { if (n == 1) out[0] = 0; return n; }
    double sum = 0, sq_sum = 0;
    for (size_t i = 0; i < n; i++) { sum += in[i]; sq_sum += in[i] * in[i]; }
    double mean = sum / (double)n;
    double var = (sq_sum / (double)n) - (mean * mean);
    double std = (var > 1e-15) ? sqrt(var * (double)n / (double)(n - 1)) : 0;
    for (size_t i = 0; i < n; i++) out[i] = (std > 1e-15) ? (in[i] - mean) / std : 0.0;
    return n;
}

size_t exprtk_vec_winsorize(const double *in, size_t n, double limit_pct, double *out, turbo_arena_t *arena) {
    if (n == 0) return 0;
    double *tmp = TEMP_ALLOC(arena, double, n);
    if (!tmp) return 0;
    memcpy(tmp, in, n * sizeof(double));
    qsort(tmp, n, sizeof(double), compare_doubles);
    size_t lo_idx = (size_t)(n * limit_pct);
    size_t hi_idx = (n > 0) ? n - 1 - lo_idx : 0;
    if (hi_idx < lo_idx) hi_idx = lo_idx;
    double lo_val = tmp[lo_idx], hi_val = tmp[hi_idx];
    for (size_t i = 0; i < n; i++) out[i] = (in[i] < lo_val) ? lo_val : (in[i] > hi_val ? hi_val : in[i]);
    return n;
}

size_t exprtk_vec_standardize(const double *in, size_t n, double *out, turbo_arena_t *arena) {
    return exprtk_vec_zscore(in, n, out, arena);
}

double exprtk_tick_ofi(double b_p, double b_v, double a_p, double a_v,
                       double pb_p, double pb_v, double pa_p, double pa_v) {
    double db = (b_p > pb_p) ? b_v : (b_p < pb_p ? -pb_v : b_v - pb_v);
    double da = (a_p < pa_p) ? a_v : (a_p > pa_p ? -pa_v : a_v - pa_v);
    return db - da;
}

double exprtk_tick_imbalance(const double *bids_v, const double *asks_v, size_t depth) {
    double svb = 0, sva = 0;
    for (size_t i = 0; i < depth; i++) { svb += bids_v[i]; sva += asks_v[i]; }
    double total = svb + sva;
    return (total > 1e-15) ? (svb - sva) / total : 0.0;
}

double exprtk_tick_limit_status(double price, double prev_close, double limit_pct) {
    if (prev_close <= 0 || limit_pct <= 0) return 0;
    return ((price / prev_close) - 1.0) / limit_pct;
}

double exprtk_vec_skewness(const double *data, size_t n) {
    if (n < 3) return 0;
    double sum = 0;
    for (size_t i = 0; i < n; ++i) sum += data[i];
    double mean = sum / (double)n;
    double m2 = 0, m3 = 0;
    for (size_t i = 0; i < n; ++i) {
        double d = data[i] - mean;
        m2 += d * d;
        m3 += d * d * d;
    }
    double var = m2 / (double)(n - 1);
    double std_dev = sqrt(var);
    if (std_dev < 1e-15) return 0;
    return ((double)n / ((double)(n - 1) * (double)(n - 2))) * (m3 / (std_dev * std_dev * std_dev));
}

double exprtk_vec_kurtosis(const double *data, size_t n) {
    if (n < 4) return 0;
    double sum = 0;
    for (size_t i = 0; i < n; ++i) sum += data[i];
    double mean = sum / (double)n;
    double m2 = 0, m4 = 0;
    for (size_t i = 0; i < n; ++i) {
        double d = data[i] - mean;
        double d2 = d * d;
        m2 += d2;
        m4 += d2 * d2;
    }
    double var = m2 / (double)(n - 1);
    if (var < 1e-15) return 0;
    double nn = (double)n;
    return (nn * (nn + 1.0) / ((nn - 1.0) * (nn - 2.0) * (nn - 3.0))) * (m4 / (var * var)) 
           - 3.0 * (nn - 1.0) * (nn - 1.0) / ((nn - 2.0) * (nn - 3.0));
}

double exprtk_vec_vol_ratio(const double *vol, size_t n, size_t segment_len, size_t segment_idx) {
    if (n == 0 || segment_len == 0) return 0;
    double total_vol = 0, seg_vol = 0;
    for (size_t i = 0; i < n; i++) total_vol += vol[i];
    if (total_vol < 1e-15) return 0;
    
    size_t start = segment_idx * segment_len;
    size_t end = start + segment_len;
    if (end > n) end = n;
    if (start >= n) return 0;
    
    for (size_t i = start; i < end; i++) seg_vol += vol[i];
    return seg_vol / total_vol;
}

double exprtk_vec_rvar(const double *in, size_t n) {
    if (n == 0) return 0;
    double sum_sq = 0;
    for (size_t i = 0; i < n; i++) sum_sq += in[i] * in[i];
    return sum_sq;
}

double exprtk_vec_rskew(const double *in, size_t n) {
    if (n == 0) return 0;
    double rv = 0, sum_c = 0;
    for (size_t i = 0; i < n; i++) {
        rv += in[i] * in[i];
        sum_c += in[i] * in[i] * in[i];
    }
    if (rv < 1e-15) return 0;
    return (sqrt((double)n) * sum_c) / pow(rv, 1.5);
}

double exprtk_vec_rkurt(const double *in, size_t n) {
    if (n == 0) return 0;
    double rv = 0, sum_q = 0;
    for (size_t i = 0; i < n; i++) {
        rv += in[i] * in[i];
        sum_q += in[i] * in[i] * in[i] * in[i];
    }
    if (rv < 1e-15) return 0;
    return ((double)n * sum_q) / (rv * rv);
}

double exprtk_vec_illiq(const double *ret, const double *amount, size_t n) {
    if (n == 0) return 0;
    double sum = 0;
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        if (amount[i] > 1e-15) {
            sum += fabs(ret[i]) / amount[i];
            count++;
        }
    }
    return count > 0 ? sum / (double)count : 0;
}

double exprtk_vec_trend_strength(const double *price, size_t n) {
    if (n < 2) return 0;
    double displ = price[n - 1] - price[0];
    double path = 0;
    for (size_t i = 1; i < n; i++) path += fabs(price[i] - price[i - 1]);
    return (path > 1e-15) ? displ / path : 0;
}

double exprtk_vec_fvd(const double *vol, size_t n, size_t window_size) {
    if (n == 0 || window_size == 0) return 0;
    double total_vol = 0;
    for (size_t i = 0; i < n; i++) total_vol += vol[i];
    if (total_vol < 1e-15) return 0;
    
    double sum_ratio = 0;
    size_t m = n / window_size;
    if (m == 0) return 0;
    
    for (size_t i = 0; i < m; i++) {
        double seg_vol = 0;
        for (size_t j = 0; j < window_size; j++) {
            seg_vol += vol[i * window_size + j];
        }
        sum_ratio += seg_vol / total_vol;
    }
    return sum_ratio / (double)m;
}

double exprtk_vec_efficiency(const double *price, size_t n) {
    if (n < 2) return 0;
    double displ = price[n - 1] - price[0];
    double path = 0;
    for (size_t i = 1; i < n; i++) path += fabs(price[i] - price[i - 1]);
    return (path > 1e-15) ? displ / path : 0;
}

double exprtk_vec_rsj(const double *ret, size_t n) {
    if (n < 2) return 0;
    double rv_pos = 0, rv_neg = 0, rv = 0;
    for (size_t i = 0; i < n; i++) {
        double r2 = ret[i] * ret[i];
        rv += r2;
        if (ret[i] > 0) rv_pos += r2;
        else if (ret[i] < 0) rv_neg += r2;
    }
    return (rv > 1e-15) ? (rv_pos - rv_neg) / rv : 0;
}

double exprtk_vec_apm(const double *ret_am, const double *ret_pm, size_t n) {
    if (n == 0) return 0;
    double sum = 0;
    for (size_t i = 0; i < n; i++) sum += (ret_pm[i] - ret_am[i]);
    return sum / (double)n;
}

size_t exprtk_vec_cgo(const double *p, const double *v, size_t n, size_t period, double *out, turbo_arena_t *arena) {
    if (n < period) return 0;
    for (size_t i = period - 1; i < n; i++) {
        double sum_v = 0, sum_vp = 0;
        for (size_t j = 0; j < period; j++) {
            double vol = v[i - period + 1 + j];
            sum_v += vol;
            sum_vp += vol * p[i - period + 1 + j];
        }
        double rp = (sum_v > 1e-15) ? sum_vp / sum_v : p[i];
        out[i] = (p[i] > 1e-15) ? (p[i] - rp) / p[i] : 0;
    }
    return n;
}

double exprtk_bt_slippage(double price, double size, double vol, double avg_vol, double lambda) {
    if (avg_vol < 1e-15) return 0;
    double drift = lambda * vol * sqrt(fabs(size) / avg_vol);
    return price * drift;
}

double exprtk_bt_cost(double price, double size, double commission_pct, double tax_pct, double slippage) {
    double base_cost = price * fabs(size) * (commission_pct + tax_pct);
    return base_cost + slippage;
}

size_t exprtk_mc_simulate(double s0, double mu, double sigma, double dt, size_t steps, size_t paths, double *out, turbo_arena_t *arena) {
    (void)arena;
    if (steps == 0 || paths == 0) return 0;
    for (size_t p = 0; p < paths; p++) {
        double s = s0;
        out[p * steps] = s;
        for (size_t t = 1; t < steps; t++) {
            double u1 = (double)rand() / (double)RAND_MAX;
            double u2 = (double)rand() / (double)RAND_MAX;
            double z = sqrt(-2.0 * log(u1 > 0 ? u1 : 1e-10)) * cos(2.0 * 3.1415926535 * u2);
            s *= exp((mu - 0.5 * sigma * sigma) * dt + sigma * z * sqrt(dt));
            out[p * steps + t] = s;
        }
    }
    return steps * paths;
}

double exprtk_vec_salience(const double *ret, const double *mkt_ret, size_t n, double delta) {
    if (n == 0) return 0;
    double sum = 0;
    for (size_t i = 0; i < n; i++) {
        double r = ret[i];
        double m = mkt_ret[i];
        double den = fabs(r) + fabs(m) + delta;
        sum += (den > 1e-15) ? fabs(r - m) / den : 0;
    }
    return sum / (double)n;
}

double exprtk_vec_str(const double *ret, const double *mkt_ret, size_t n, double delta) {
    if (n == 0) return 0;
    double sum_s = 0;
    double sum_sr = 0;
    for (size_t i = 0; i < n; i++) {
        double r = ret[i];
        double m = mkt_ret[i];
        double den = fabs(r) + fabs(m) + delta;
        double s = (den > 1e-15) ? fabs(r - m) / den : 0;
        sum_s += s;
        sum_sr += s * r;
    }
    return (sum_s > 1e-15) ? sum_sr / sum_s : 0;
}

size_t exprtk_vec_csad(const double *rets, size_t na, size_t np, double *out, turbo_arena_t *arena) {
    (void)arena;
    if (na == 0 || np == 0) return 0;
    for (size_t t = 0; t < np; t++) {
        double mkt_ret = 0;
        for (size_t i = 0; i < na; i++) mkt_ret += rets[i * np + t];
        mkt_ret /= (double)na;

        double csad = 0;
        for (size_t i = 0; i < na; i++) csad += fabs(rets[i * np + t] - mkt_ret);
        out[t] = csad / (double)na;
    }
    return np;
}

size_t exprtk_ts_dwt(const double *data, size_t n, size_t levels, double *approx, double *detail, turbo_arena_t *arena) {
    (void)arena;
    if (n < 2 || levels == 0) return 0;
    
    // Copy input to approx to start
    memcpy(approx, data, n * sizeof(double));
    
    size_t current_n = n;
    for (size_t l = 0; l < levels; l++) {
        if (current_n < 2) break;
        size_t next_n = current_n / 2;
        
        for (size_t i = 0; i < next_n; i++) {
            double a = approx[2 * i];
            double b = approx[2 * i + 1];
            approx[i] = (a + b) * 0.70710678118; // Haar Approximation (sqrt(2)/2)
            detail[i + (n - next_n)] = (a - b) * 0.70710678118; // Haar Detail
        }
        current_n = next_n;
    }
    return n;
}

size_t exprtk_ts_emd(const double *data, size_t n, size_t max_imfs, double *imfs, turbo_arena_t *arena) {
    (void)arena;
    if (n < 4 || max_imfs == 0) return 0;

    // Simplified EMD: Extract 1st IMF using a simple moving average of peaks
    // In a real EMD, this would be an iterative spline-sifting process.
    // Here we provide a high-frequency "residue" as the first IMF.
    for (size_t i = 0; i < n; i++) {
        double trend = 0;
        size_t win = 5;
        if (i >= win && i < n - win) {
            for (int j = -(int)win; j <= (int)win; j++) trend += data[i + j];
            trend /= (double)(2 * win + 1);
        } else {
            trend = data[i];
        }
        imfs[i] = data[i] - trend; // IMF 1 (High frequency)
        if (max_imfs > 1) {
            imfs[n + i] = trend; // Residue (Trend)
        }
    }
    return n;
}

size_t exprtk_ta_zscore(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena) {
    (void)arena;
    if (n < period) return 0;
    for (size_t i = period - 1; i < n; i++) {
        double sum = 0, sq_sum = 0;
        for (size_t j = 0; j < period; j++) {
            double v = in[i - period + 1 + j];
            sum += v;
            sq_sum += v * v;
        }
        double mean = sum / (double)period;
        double var = (sq_sum / (double)period) - (mean * mean);
        double std = sqrt(fmax(var, 1e-15));
        out[i] = (in[i] - mean) / std;
    }
    return n;
}

double exprtk_vec_entropy(const double *in, size_t n, size_t bins) {
    if (n == 0 || bins == 0) return 0;
    double min_v = in[0], max_v = in[0];
    for (size_t i = 1; i < n; i++) {
        min_v = fmin(min_v, in[i]);
        max_v = fmax(max_v, in[i]);
    }
    if (max_v - min_v < 1e-15) return 0;

    double *counts = (double *)calloc(bins, sizeof(double));
    for (size_t i = 0; i < n; i++) {
        size_t b = (size_t)((in[i] - min_v) / (max_v - min_v) * (double)(bins - 1));
        counts[b]++;
    }

    double ent = 0;
    for (size_t i = 0; i < bins; i++) {
        if (counts[i] > 0) {
            double p = counts[i] / (double)n;
            ent -= p * log2(p);
        }
    }
    free(counts);
    return ent;
}
