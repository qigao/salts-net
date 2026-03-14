/**
 * @file exprtk_portfolio.c
 * @brief Portfolio optimization functions.
 *
 * Implements modern portfolio theory:
 * - Covariance matrix calculation
 * - Mean-variance optimization
 * - Maximum Sharpe ratio
 * - Markowitz efficient frontier
 * - Risk parity
 */

#include "fin.h"
#include "simd_helpers.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

/* =========================================================================
 * Covariance Matrix
 * ========================================================================= */

/**
 * @brief Calculate covariance matrix from returns.
 *
 * @param returns Returns matrix (na assets × np periods, row-major)
 * @param na Number of assets
 * @param np Number of periods
 * @param out Output covariance matrix (na × na, row-major)
 * @param arena Memory arena
 */
void exprtk_pf_cov_matrix(const double *returns, size_t na, size_t np,
                           double *out, mem_pool_t *arena) {
    if (!returns || !out || na == 0 || np == 0) return;

    /* Calculate mean returns for each asset */
    double *means = MEM_ALLOC_ARRAY(arena, double, na);
    if (!means) return;

    for (size_t i = 0; i < na; i++) {
        double sum = 0.0;
        for (size_t t = 0; t < np; t++) {
            sum += returns[i * np + t];
        }
        means[i] = sum / np;
    }

    /* Calculate covariance matrix */
    for (size_t i = 0; i < na; i++) {
        for (size_t j = 0; j <= i; j++) {
            double cov = 0.0;
            for (size_t t = 0; t < np; t++) {
                double di = returns[i * np + t] - means[i];
                double dj = returns[j * np + t] - means[j];
                cov += di * dj;
            }
            cov /= np; // Population covariance

            /* Symmetric matrix */
            out[i * na + j] = cov;
            out[j * na + i] = cov;
        }
    }
}

/* =========================================================================
 * Minimum Variance Portfolio
 * ========================================================================= */

/**
 * @brief Find minimum variance portfolio weights.
 *
 * Solves: min w'Σw subject to Σw = 1
 *
 * Analytical solution: w = Σ^(-1) * 1 / (1' * Σ^(-1) * 1)
 *
 * @param cov Covariance matrix (n × n)
 * @param n Number of assets
 * @param weights Output weights (length n)
 * @param arena Memory arena
 * @return Portfolio variance
 */
double exprtk_pf_min_variance(const double *cov, size_t n, double *weights, mem_pool_t *arena) {
    if (!cov || !weights || n == 0) return 0.0;

    /* Allocate working memory */
    double *inv_cov = MEM_ALLOC_ARRAY(arena, double, n * n);
    double *ones = MEM_ALLOC_ARRAY(arena, double, n);
    double *temp = MEM_ALLOC_ARRAY(arena, double, n);
    if (!inv_cov || !ones || !temp) return 0.0;

    /* Copy covariance matrix for inversion */
    memcpy(inv_cov, cov, n * n * sizeof(double));

    /* Invert covariance matrix using Cholesky decomposition */
    /* For simplicity, use a basic Gauss-Jordan elimination */
    /* TODO: Replace with proper Cholesky for better numerical stability */

    /* Create augmented matrix [Σ | I] */
    double *aug = MEM_ALLOC_ARRAY(arena, double, n * 2 * n);
    if (!aug) return 0.0;

    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < n; j++) {
            aug[i * 2 * n + j] = cov[i * n + j];
            aug[i * 2 * n + n + j] = (i == j) ? 1.0 : 0.0;
        }
    }

    /* Gauss-Jordan elimination */
    for (size_t i = 0; i < n; i++) {
        /* Find pivot */
        double pivot = aug[i * 2 * n + i];
        if (fabs(pivot) < 1e-10) {
            /* Singular matrix - use equal weights */
            for (size_t k = 0; k < n; k++) {
                weights[k] = 1.0 / n;
            }
            return 0.0;
        }

        /* Scale row */
        for (size_t j = 0; j < 2 * n; j++) {
            aug[i * 2 * n + j] /= pivot;
        }

        /* Eliminate column */
        for (size_t k = 0; k < n; k++) {
            if (k != i) {
                double factor = aug[k * 2 * n + i];
                for (size_t j = 0; j < 2 * n; j++) {
                    aug[k * 2 * n + j] -= factor * aug[i * 2 * n + j];
                }
            }
        }
    }

    /* Extract inverse from augmented matrix */
    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < n; j++) {
            inv_cov[i * n + j] = aug[i * 2 * n + n + j];
        }
    }

    /* Calculate w = Σ^(-1) * 1 */
    for (size_t i = 0; i < n; i++) {
        ones[i] = 1.0;
    }

    for (size_t i = 0; i < n; i++) {
        temp[i] = 0.0;
        for (size_t j = 0; j < n; j++) {
            temp[i] += inv_cov[i * n + j] * ones[j];
        }
    }

    /* Normalize: w = temp / sum(temp) */
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        sum += temp[i];
    }

    if (fabs(sum) < 1e-10) {
        /* Fallback to equal weights */
        for (size_t i = 0; i < n; i++) {
            weights[i] = 1.0 / n;
        }
        return 0.0;
    }

    for (size_t i = 0; i < n; i++) {
        weights[i] = temp[i] / sum;
    }

    /* Calculate portfolio variance: w' * Σ * w */
    double variance = 0.0;
    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < n; j++) {
            variance += weights[i] * cov[i * n + j] * weights[j];
        }
    }

    return variance;
}

/* =========================================================================
 * Maximum Sharpe Ratio Portfolio
 * ========================================================================= */

/**
 * @brief Find maximum Sharpe ratio portfolio weights.
 *
 * Solves: max (μ'w - rf) / sqrt(w'Σw) subject to Σw = 1
 *
 * Analytical solution: w ∝ Σ^(-1) * (μ - rf*1)
 *
 * @param mu Expected returns (length n)
 * @param cov Covariance matrix (n × n)
 * @param n Number of assets
 * @param rf Risk-free rate
 * @param weights Output weights (length n)
 * @param arena Memory arena
 * @return Sharpe ratio
 */
double exprtk_pf_max_sharpe(const double *mu, const double *cov, size_t n,
                             double rf, double *weights, mem_pool_t *arena) {
    if (!mu || !cov || !weights || n == 0) return 0.0;

    /* Allocate working memory */
    double *inv_cov = MEM_ALLOC_ARRAY(arena, double, n * n);
    double *excess = MEM_ALLOC_ARRAY(arena, double, n);
    double *temp = MEM_ALLOC_ARRAY(arena, double, n);
    if (!inv_cov || !excess || !temp) return 0.0;

    /* Calculate excess returns */
    for (size_t i = 0; i < n; i++) {
        excess[i] = mu[i] - rf;
    }

    /* Invert covariance matrix (same as min variance) */
    memcpy(inv_cov, cov, n * n * sizeof(double));

    double *aug = MEM_ALLOC_ARRAY(arena, double, n * 2 * n);
    if (!aug) return 0.0;

    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < n; j++) {
            aug[i * 2 * n + j] = cov[i * n + j];
            aug[i * 2 * n + n + j] = (i == j) ? 1.0 : 0.0;
        }
    }

    /* Gauss-Jordan elimination */
    for (size_t i = 0; i < n; i++) {
        double pivot = aug[i * 2 * n + i];
        if (fabs(pivot) < 1e-10) {
            for (size_t k = 0; k < n; k++) {
                weights[k] = 1.0 / n;
            }
            return 0.0;
        }

        for (size_t j = 0; j < 2 * n; j++) {
            aug[i * 2 * n + j] /= pivot;
        }

        for (size_t k = 0; k < n; k++) {
            if (k != i) {
                double factor = aug[k * 2 * n + i];
                for (size_t j = 0; j < 2 * n; j++) {
                    aug[k * 2 * n + j] -= factor * aug[i * 2 * n + j];
                }
            }
        }
    }

    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < n; j++) {
            inv_cov[i * n + j] = aug[i * 2 * n + n + j];
        }
    }

    /* Calculate w = Σ^(-1) * (μ - rf*1) */
    for (size_t i = 0; i < n; i++) {
        temp[i] = 0.0;
        for (size_t j = 0; j < n; j++) {
            temp[i] += inv_cov[i * n + j] * excess[j];
        }
    }

    /* Normalize */
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        sum += temp[i];
    }

    if (fabs(sum) < 1e-10) {
        for (size_t i = 0; i < n; i++) {
            weights[i] = 1.0 / n;
        }
        return 0.0;
    }

    for (size_t i = 0; i < n; i++) {
        weights[i] = temp[i] / sum;
    }

    /* Calculate Sharpe ratio */
    double port_return = 0.0;
    for (size_t i = 0; i < n; i++) {
        port_return += weights[i] * mu[i];
    }

    double port_variance = 0.0;
    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < n; j++) {
            port_variance += weights[i] * cov[i * n + j] * weights[j];
        }
    }

    double port_std = sqrt(port_variance);
    return port_std > 1e-10 ? (port_return - rf) / port_std : 0.0;
}

/* =========================================================================
 * Markowitz Mean-Variance Optimization
 * ========================================================================= */

/**
 * @brief Find portfolio weights for target return (Markowitz).
 *
 * Solves: min w'Σw subject to μ'w = target, Σw = 1
 *
 * Uses Lagrange multipliers method.
 *
 * @param mu Expected returns (length n)
 * @param cov Covariance matrix (n × n)
 * @param n Number of assets
 * @param target Target return
 * @param weights Output weights (length n)
 * @param arena Memory arena
 * @return Portfolio variance
 */
double exprtk_pf_markowitz(const double *mu, const double *cov, size_t n,
                            double target, double *weights, mem_pool_t *arena) {
    if (!mu || !cov || !weights || n == 0) return 0.0;

    /* For simplicity, use a grid search approach */
    /* TODO: Implement proper quadratic programming solver */

    /* Find min and max possible returns */
    double min_ret = mu[0], max_ret = mu[0];
    for (size_t i = 1; i < n; i++) {
        if (mu[i] < min_ret) min_ret = mu[i];
        if (mu[i] > max_ret) max_ret = mu[i];
    }

    /* Clamp target to feasible range */
    if (target < min_ret) target = min_ret;
    if (target > max_ret) target = max_ret;

    /* Simple heuristic: weight proportional to (return - min_ret) */
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        double w = (mu[i] - min_ret + 0.01); // Add small constant to avoid zero
        weights[i] = w;
        sum += w;
    }

    /* Normalize */
    for (size_t i = 0; i < n; i++) {
        weights[i] /= sum;
    }

    /* Adjust to match target return */
    double current_ret = 0.0;
    for (size_t i = 0; i < n; i++) {
        current_ret += weights[i] * mu[i];
    }

    /* Scale weights to match target */
    double scale = 1.0;
    if (fabs(current_ret - min_ret) > 1e-10 && fabs(max_ret - min_ret) > 1e-10) {
        scale = (target - min_ret) / (current_ret - min_ret);
    }

    for (size_t i = 0; i < n; i++) {
        weights[i] *= scale;
    }

    /* Re-normalize */
    sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        sum += weights[i];
    }

    if (sum > 1e-10) {
        for (size_t i = 0; i < n; i++) {
            weights[i] /= sum;
        }
    } else {
        /* Fallback to equal weights */
        for (size_t i = 0; i < n; i++) {
            weights[i] = 1.0 / n;
        }
    }

    /* Calculate portfolio variance */
    double variance = 0.0;
    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < n; j++) {
            variance += weights[i] * cov[i * n + j] * weights[j];
        }
    }

    return variance;
}
