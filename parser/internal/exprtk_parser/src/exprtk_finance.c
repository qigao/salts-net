/**
 * @file exprtk_finance.c
 * @brief Finance, backtest, portfolio, risk, signal, and time-series functions
 *
 * Extracted from the monolithic exprtk.c during refactoring.
 * Contains: backtesting engine, portfolio optimization, risk metrics,
 * signal processing, candlestick patterns, and time-series analysis.
 */

#include "exprtk_internal.h"

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
    for (size_t i = 1; i < n; ++i) {
        if (equity[i] > 0 && equity[i - 1] > 0) {
            double r = (equity[i] - equity[i - 1]) / equity[i - 1];
            sum_ret += r; sum_ret2 += r * r; if (r < 0) sum_neg2 += r * r;
            ret_count++;
        }
    }
    if (ret_count > 0 && annual > 0) {
        double years = (double)ret_count / annual;
        if (years > 0 && eqN / eq0 > 0) out[1] = (pow(eqN / eq0, 1.0 / years) - 1.0) * 100.0;
    }
    double peak = equity[0], max_dd = 0;
    for (size_t i = 1; i < n; ++i) {
        if (equity[i] > peak) peak = equity[i];
        double dd = (peak - equity[i]) / peak;
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
        for (size_t i = 0; i < num_trades; ++i) {
            double p = trades[i]; sum_p += p;
            if (p > 0) { sum_w += p; wins++; } else sum_l += p;
            if (p > best) best = p; if (p < worst) worst = p;
        }
        out[6] = (double)wins / (double)num_trades * 100.0;
        out[7] = (fabs(sum_l) > 0) ? sum_w / fabs(sum_l) : 0;
        out[9] = out[12] = sum_p / (double)num_trades;
        out[10] = best; out[11] = worst;
        if (num_trades > 1) {
            double var = 0, mean = sum_p / (double)num_trades;
            for (size_t i = 0; i < num_trades; ++i) { double d = trades[i] - mean; var += d * d; }
            double std = sqrt(var / (double)(num_trades - 1));
            out[13] = (std > 0) ? sqrt((double)num_trades) * mean / std : 0;
        }
    }
    return 14;
}

size_t exprtk_bt_backtest_ex(const double *open, const double *high, const double *low, const double *close, const double *volume, const double *signal, size_t n, double cash0, double commission, const double *config, size_t config_n, double *equity, double *trades, turbo_arena_t *arena) {
    if (n == 0 || cash0 <= 0 || config_n < 3) return 0;
    double sl = config[0], tp = config[1], slip_pct = config[2];
    double h_spread = (config_n > 3) ? config[3] : 0, v_impact = (config_n > 4) ? config[4] : 0, v_scale = (config_n > 5) ? config[5] : 0;
    (void)v_impact;
    double *atr = TEMP_ALLOC(arena, n);
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
    double *pos = TEMP_ALLOC(arena, na);
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
    double *mean = TEMP_ALLOC(arena, na);
    for (size_t t = 0; t < np; ++t) for (size_t i = 0; i < na; ++i) mean[i] += returns[t * na + i];
    for (size_t i = 0; i < na; ++i) mean[i] /= (double)np;
    for (size_t i = 0; i < na; ++i) {
        for (size_t j = i; j < na; ++j) {
            double c = 0; for (size_t t = 0; t < np; ++t) c += (returns[t * na + i] - mean[i]) * (returns[t * na + j] - mean[j]);
            c /= (double)(np - 1); out[i * na + j] = out[j * na + i] = c;
        }
    }
    TEMP_FREE(arena, mean);
}

double exprtk_pf_min_variance(const double *cov, size_t n, double *weights, turbo_arena_t *arena) {
    if (n == 0) return 0;
    double *inv = TEMP_ALLOC(arena, n * n);
    memcpy(inv, cov, n * n * sizeof(double));
    if (!gauss_jordan_invert(inv, n, arena)) { TEMP_FREE(arena, inv); return 0; }
    double *w_un = TEMP_ALLOC(arena, n), sum_w = 0;
    for (size_t i = 0; i < n; ++i) { for (size_t j = 0; j < n; ++j) w_un[i] += inv[i * n + j]; sum_w += w_un[i]; }
    if (fabs(sum_w) < 1e-15) { TEMP_FREE(arena, inv); TEMP_FREE(arena, w_un); return 0; }
    double var = 0;
    for (size_t i = 0; i < n; ++i) { weights[i] = w_un[i] / sum_w; for (size_t j = 0; j < n; ++j) var += weights[i] * cov[i * n + j] * (w_un[j] / sum_w); }
    TEMP_FREE(arena, inv); TEMP_FREE(arena, w_un); return var;
}

double exprtk_pf_max_sharpe(const double *mu, const double *cov, size_t n, double rf, double *weights, turbo_arena_t *arena) {
    if (n == 0) return 0;
    double *inv = TEMP_ALLOC(arena, n * n);
    memcpy(inv, cov, n * n * sizeof(double));
    if (!gauss_jordan_invert(inv, n, arena)) { TEMP_FREE(arena, inv); return 0; }
    double *w_un = TEMP_ALLOC(arena, n), sum_w = 0;
    for (size_t i = 0; i < n; ++i) { for (size_t j = 0; j < n; ++j) w_un[i] += inv[i * n + j] * (mu[j] - rf); sum_w += w_un[i]; }
    if (fabs(sum_w) < 1e-15) { TEMP_FREE(arena, inv); TEMP_FREE(arena, w_un); return 0; }
    for (size_t i = 0; i < n; ++i) weights[i] = w_un[i] / sum_w;
    double p_ret = 0, p_var = 0;
    for (size_t i = 0; i < n; ++i) { p_ret += weights[i] * mu[i]; for (size_t j = 0; j < n; ++j) p_var += weights[i] * cov[i * n + j] * weights[j]; }
    TEMP_FREE(arena, inv); TEMP_FREE(arena, w_un); return (p_var > 0) ? (p_ret - rf) / sqrt(p_var) : 0;
}

double exprtk_pf_markowitz(const double *mu, const double *cov, size_t n, double target, double *weights, turbo_arena_t *arena) {
    if (n == 0) return 0;
    double *inv = TEMP_ALLOC(arena, n * n);
    memcpy(inv, cov, n * n * sizeof(double));
    if (!gauss_jordan_invert(inv, n, arena)) { TEMP_FREE(arena, inv); return 0; }
    double A = 0, B = 0, C = 0;
    double *inv1 = TEMP_ALLOC(arena, n), *inv_mu = TEMP_ALLOC(arena, n);
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
    double *w = TEMP_ALLOC(arena, n);
    for (size_t i = 0; i < n; ++i) w[i] = 1.0 / (double)n;
    for (int iter = 0; iter < 500; ++iter) {
        double *mrc = TEMP_ALLOC(arena, n), p_var = 0;
        for (size_t i = 0; i < n; ++i) { for (size_t j = 0; j < n; ++j) mrc[i] += cov[i * n + j] * w[j]; p_var += w[i] * mrc[i]; }
        if (p_var < 1e-15) { TEMP_FREE(arena, mrc); break; }
        double *w_new = TEMP_ALLOC(arena, n), sum_w = 0;
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
    double *sorted = TEMP_ALLOC(arena, n);
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
    double *sorted = TEMP_ALLOC(arena, n);
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
    for (size_t t = 0; t < n_bars; ++t) {
        double res = 0, w_sum = 0;
        for (size_t j = 0; j < n_signals; ++j) { res += signals[t * n_signals + j] * weights[j]; w_sum += fabs(weights[j]); }
        out[t] = (w_sum > 0) ? res / w_sum : 0;
        if (fabs(out[t]) > 1e-9) count++;
    }
    return count;
}

size_t exprtk_candle_doji(const double *O, const double *H, const double *L, const double *C, size_t n, double threshold, double *out) {
    size_t count = 0;
    for (size_t i = 0; i < n; ++i) {
        double body = fabs(C[i] - O[i]), range = H[i] - L[i];
        if (range > 0 && body / range < threshold) { out[i] = 1.0; count++; }
        else out[i] = 0;
    }
    return count;
}

size_t exprtk_candle_hammer(const double *O, const double *H, const double *L, const double *C, size_t n, double *out) {
    size_t count = 0;
    for (size_t i = 0; i < n; ++i) {
        double body = fabs(C[i] - O[i]), upper = H[i] - fmax(O[i], C[i]), lower = fmin(O[i], C[i]) - L[i];
        if (lower > 2.0 * body && upper < 0.1 * lower) { out[i] = 1.0; count++; }
        else out[i] = 0;
    }
    return count;
}

size_t exprtk_candle_engulfing(const double *O, const double *H, const double *L, const double *C, size_t n, double *out) {
    size_t count = 0; if (n < 2) return 0; out[0] = 0;
    for (size_t i = 1; i < n; ++i) {
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
    double *tmp = TEMP_ALLOC(arena, n); memcpy(tmp, data, n * sizeof(double));
    for (size_t o = 0; o < order; ++o) {
        for (size_t i = n - 1; i > o; --i) tmp[i] = tmp[i] - tmp[i - 1];
    }
    for (size_t i = 0; i < n; ++i) out[i] = (i < order) ? 0 : tmp[i];
    TEMP_FREE(arena, tmp); return n - order;
}

size_t exprtk_ts_autocorr(const double *data, size_t n, size_t max_lag, double *out) {
    if (n < 2 || max_lag >= n) return 0;
    double sum = 0; for (size_t i = 0; i < n; ++i) sum += data[i];
    double mean = sum / (double)n, var = 0;
    for (size_t i = 0; i < n; ++i) var += (data[i] - mean) * (data[i] - mean);
    if (var < 1e-15) { for (size_t l = 0; l <= max_lag; ++l) out[l] = 1.0; return max_lag + 1; }
    for (size_t l = 0; l <= max_lag; ++l) {
        double cv = 0; for (size_t i = l; i < n; ++i) cv += (data[i] - mean) * (data[i - l] - mean);
        out[l] = cv / var;
    }
    return max_lag + 1;
}

size_t exprtk_ts_pacf(const double *data, size_t n, size_t max_lag, double *out, turbo_arena_t *arena) {
    if (n < 2 || max_lag >= n) return 0;
    double *rho = (double*)malloc((max_lag + 1) * sizeof(double));
    exprtk_ts_autocorr(data, n, max_lag, rho);
    out[0] = 1.0; if (max_lag == 0) { TEMP_FREE(arena, rho); return 1; }
    out[1] = rho[1];
    double *phi = (double*)calloc((max_lag + 1) * (max_lag + 1), sizeof(double));
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
    double *dy = TEMP_ALLOC(arena, m), *y_lag = TEMP_ALLOC(arena, m), *X = (double*)malloc(m * (p + 1) * sizeof(double));
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
    double *x = TEMP_ALLOC(arena, m), *y = TEMP_ALLOC(arena, m);
    size_t idx = 0;
    for (size_t k = 4; k <= n / 2; k *= 2) {
        size_t num_blocks = n / k; double sum_rs = 0;
        for (size_t b = 0; b < num_blocks; ++b) {
            double b_sum = 0; for (size_t i = 0; i < k; ++i) b_sum += data[b * k + i];
            double b_mean = b_sum / (double)k, b_var = 0;
            double *cum = TEMP_ALLOC(arena, k), c_sum = 0;
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
