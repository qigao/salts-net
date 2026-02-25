/**
 * @file exprtk_math.c
 * @brief Math, statistics, and linear algebra functions for exprtk
 *
 * Extracted from the monolithic exprtk.c during refactoring.
 * Contains: comparison helpers, distribution functions, statistics,
 * fibonacci, GCD, random, determinants, matrix operations, eigenvalues.
 */

#include "exprtk_internal.h"

/* ========================================================================= */
/* Comparison helpers                                                        */
/* ========================================================================= */

int compare_doubles(const void *a, const void *b) {
    double arg1 = *(const double*)a;
    double arg2 = *(const double*)b;
    if (arg1 < arg2) return -1;
    if (arg1 > arg2) return 1;
    return 0;
}

int compare_rank_items(const void *a, const void *b) {
    double v1 = ((const rank_item_t*)a)->val;
    double v2 = ((const rank_item_t*)b)->val;
    if (v1 < v2) return -1;
    if (v1 > v2) return 1;
    return 0;
}

/* ========================================================================= */
/* Distribution & Regression helpers                                         */
/* ========================================================================= */

double inv_normal_cdf(double p) {
    if (p <= 0 || p >= 1) return 0;
    const double pp = (p < 0.5) ? p : (1.0 - p);
    const double t = sqrt(-2.0 * log(pp));
    const double c0 = 2.515517;
    const double c1 = 0.802853;
    const double c2 = 0.010328;
    const double d1 = 1.432788;
    const double d2 = 0.189269;
    const double d3 = 0.001308;
    double x = t - (c0 + c1 * t + c2 * t * t) / (1.0 + d1 * t + d2 * t * t + d3 * t * t * t);
    return (p < 0.5) ? -x : x;
}

ols_result_t ols_fit(const double *y, const double *x, size_t n) {
    ols_result_t r = {0};
    if (n < 3) return r;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < n; ++i) {
        sx += x[i]; sy += y[i]; sxx += x[i] * x[i]; sxy += x[i] * y[i];
    }
    double denom = (double)n * sxx - sx * sx;
    if (fabs(denom) < 1e-15) return r;
    r.slope = ((double)n * sxy - sx * sy) / denom;
    r.intercept = (sy - r.slope * sx) / (double)n;
    double sse = 0;
    for (size_t i = 0; i < n; ++i) {
        double e = y[i] - (r.intercept + r.slope * x[i]);
        sse += e * e;
    }
    r.res_var = sse / (double)(n - 2);
    double se_slope_sq = r.res_var * (double)n / denom;
    r.t_slope = (se_slope_sq > 0) ? r.slope / sqrt(se_slope_sq) : 0;
    return r;
}

int gauss_jordan_invert(double *mat, size_t n, turbo_arena_t *arena) {
    if (n == 0) return 0;
    double *aug = TEMP_ALLOC(arena, double, n * 2 * n);
    if (!aug) return 0;
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) aug[i * 2 * n + j] = mat[i * n + j];
        aug[i * 2 * n + n + i] = 1.0;
    }
    size_t cols = 2 * n;
    for (size_t col = 0; col < n; ++col) {
        size_t pivot = col;
        double max_val = fabs(aug[col * cols + col]);
        for (size_t row = col + 1; row < n; ++row) {
            double v = fabs(aug[row * cols + col]);
            if (v > max_val) { max_val = v; pivot = row; }
        }
        if (max_val < 1e-15) { TEMP_FREE(arena, aug); return 0; }
        if (pivot != col) {
            for (size_t j = 0; j < cols; ++j) {
                double tmp = aug[col * cols + j];
                aug[col * cols + j] = aug[pivot * cols + j];
                aug[pivot * cols + j] = tmp;
            }
        }
        double diag = aug[col * cols + col];
        for (size_t j = 0; j < cols; ++j) aug[col * cols + j] /= diag;
        for (size_t row = 0; row < n; ++row) {
            if (row == col) continue;
            double factor = aug[row * cols + col];
            for (size_t j = 0; j < cols; ++j) aug[row * cols + j] -= factor * aug[col * cols + j];
        }
    }
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n; ++j) mat[i * n + j] = aug[i * cols + n + j];
    TEMP_FREE(arena, aug);
    return 1;
}

/* ========================================================================= */
/* Statistics                                                                */
/* ========================================================================= */

double exprtk_median(const double *data, size_t n, turbo_arena_t *arena) {
    if (n == 0) return 0;
    double *sorted = TEMP_ALLOC(arena, double, n);
    memcpy(sorted, data, n * sizeof(double));
    qsort(sorted, n, sizeof(double), compare_doubles);
    double res;
    if (n % 2 == 1) res = sorted[n / 2];
    else res = (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
    TEMP_FREE(arena, sorted);
    return res;
}

double exprtk_percentile(const double *data, size_t n, double p, turbo_arena_t *arena) {
    if (n == 0) return 0;
    p /= 100.0;
    double *sorted = TEMP_ALLOC(arena, double, n);
    memcpy(sorted, data, n * sizeof(double));
    qsort(sorted, n, sizeof(double), compare_doubles);
    double res;
    if (n == 1) res = sorted[0];
    else {
        double idx = p * (double)(n - 1);
        size_t lo = (size_t)idx;
        size_t hi = (lo + 1 < n) ? lo + 1 : lo;
        double frac = idx - (double)lo;
        res = sorted[lo] + frac * (sorted[hi] - sorted[lo]);
    }
    TEMP_FREE(arena, sorted);
    return res;
}

double exprtk_geometric_mean(const double *data, size_t n) {
    if (n == 0) return 0;
    double sum_log = 0;
    for (size_t i = 0; i < n; ++i) {
        if (data[i] <= 0) return 0;
        sum_log += log(data[i]);
    }
    return exp(sum_log / (double)n);
}

double exprtk_harmonic_mean(const double *data, size_t n) {
    if (n == 0) return 0;
    double sum_inv = 0;
    for (size_t i = 0; i < n; ++i) {
        if (fabs(data[i]) < 1e-15) return 0;
        sum_inv += 1.0 / data[i];
    }
    return (double)n / sum_inv;
}

double exprtk_skewness(const double *data, size_t n) {
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

double exprtk_kurtosis(const double *data, size_t n) {
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

/* ========================================================================= */
/* Number Theory & Random                                                    */
/* ========================================================================= */

double exprtk_fibonacci(int n) {
    if (n <= 0) return 0;
    if (n == 1) return 1;
    double a = 0, b = 1;
    for (int i = 2; i <= n; ++i) { double c = a + b; a = b; b = c; }
    return b;
}

long long exprtk_gcd(long long u, long long v) {
    u = llabs(u); v = llabs(v);
    while (v != 0) { long long r = u % v; u = v; v = r; }
    return u;
}

double exprtk_normal_rand(double mu, double sigma) {
    static int have_next = 0;
    static double next_val = 0;
    if (have_next) {
        have_next = 0;
        return mu + sigma * next_val;
    } else {
        double u1 = (double)rand() / RAND_MAX;
        double u2 = (double)rand() / RAND_MAX;
        if (u1 < 1e-30) u1 = 1e-30;
        double r = sqrt(-2.0 * log(u1));
        double theta = 2.0 * 3.14159265358979323846 * u2;
        next_val = r * sin(theta);
        have_next = 1;
        return mu + sigma * r * cos(theta);
    }
}

/* ========================================================================= */
/* Linear Algebra                                                            */
/* ========================================================================= */

double exprtk_det2(const double *A) {
    return A[0] * A[3] - A[1] * A[2];
}

double exprtk_det3(const double *A) {
    return A[0] * (A[4] * A[8] - A[5] * A[7])
         - A[1] * (A[3] * A[8] - A[5] * A[6])
         + A[2] * (A[3] * A[7] - A[4] * A[6]);
}

int exprtk_inv2(const double *A, double *out) {
    double d = A[0] * A[3] - A[1] * A[2];
    if (fabs(d) < 1e-15) return 0;
    double inv_d = 1.0 / d;
    out[0] = A[3] * inv_d;  out[1] = -A[1] * inv_d;
    out[2] = -A[2] * inv_d; out[3] = A[0] * inv_d;
    return 1;
}

int exprtk_inv3(const double *A, double *out) {
    double c00 = A[4] * A[8] - A[5] * A[7];
    double c01 = A[5] * A[6] - A[3] * A[8];
    double c02 = A[3] * A[7] - A[4] * A[6];
    double d = A[0] * c00 + A[1] * c01 + A[2] * c02;
    if (fabs(d) < 1e-15) return 0;
    double inv_d = 1.0 / d;
    out[0] = c00 * inv_d;
    out[1] = (A[2] * A[7] - A[1] * A[8]) * inv_d;
    out[2] = (A[1] * A[5] - A[2] * A[4]) * inv_d;
    out[3] = c01 * inv_d;
    out[4] = (A[0] * A[8] - A[2] * A[6]) * inv_d;
    out[5] = (A[2] * A[3] - A[0] * A[5]) * inv_d;
    out[6] = c02 * inv_d;
    out[7] = (A[1] * A[6] - A[0] * A[7]) * inv_d;
    out[8] = (A[0] * A[4] - A[1] * A[3]) * inv_d;
    return 1;
}

void exprtk_matmul(const double *A, const double *B, size_t m, size_t k, size_t n, double *out) {
    for (size_t i = 0; i < m; ++i) {
        for (size_t j = 0; j < n; ++j) {
            double sum = 0;
            for (size_t p = 0; p < k; ++p) sum += A[i * k + p] * B[p * n + j];
            out[i * n + j] = sum;
        }
    }
}

void exprtk_transpose(const double *A, size_t rows, size_t cols, double *out) {
    for (size_t i = 0; i < rows; ++i)
        for (size_t j = 0; j < cols; ++j)
            out[j * rows + i] = A[i * cols + j];
}

int exprtk_eig2(const double *A, double *ev) {
    double tr = A[0] + A[3];
    double det = A[0] * A[3] - A[1] * A[2];
    double disc = tr * tr - 4.0 * det;
    if (disc < 0) { ev[0] = tr / 2.0; ev[1] = tr / 2.0; return 0; }
    double sq = sqrt(disc);
    ev[0] = (tr + sq) / 2.0;
    ev[1] = (tr - sq) / 2.0;
    return 1;
}

int exprtk_eig3(const double *A, double *ev) {
    double c2 = A[0] + A[4] + A[8];
    double c1 = A[0] * A[4] - A[1] * A[3] + A[0] * A[8] - A[2] * A[6] + A[4] * A[8] - A[5] * A[7];
    double c0 = A[0] * (A[4] * A[8] - A[5] * A[7]) - A[1] * (A[3] * A[8] - A[5] * A[6]) + A[2] * (A[3] * A[7] - A[4] * A[6]);
    double c2_3 = c2 / 3.0;
    double p = (3.0 * c1 - c2 * c2) / 3.0;
    double qq = (9.0 * c2 * c1 - 2.0 * c2 * c2 * c2 - 27.0 * c0) / 27.0;
    double disc = qq * qq / 4.0 + p * p * p / 27.0;
    if (disc <= 0) {
        double m = 2.0 * sqrt(-p / 3.0);
        double theta = acos(fmin(1.0, fmax(-1.0, 3.0 * qq / (p * m)))) / 3.0;
        ev[0] = c2_3 + m * cos(theta);
        ev[1] = c2_3 + m * cos(theta - 2.0 * 3.14159265358979323846 / 3.0);
        ev[2] = c2_3 + m * cos(theta - 4.0 * 3.14159265358979323846 / 3.0);
    } else {
        double sq = sqrt(disc);
        double u = pow(-qq / 2.0 + sq, 1.0 / 3.0);
        double v = pow(-qq / 2.0 - sq, 1.0 / 3.0);
        ev[0] = c2_3 + u + v;
        ev[1] = c2_3 - (u + v) / 2.0;
        ev[2] = ev[1];
    }
    if (ev[0] < ev[1]) { double t = ev[0]; ev[0] = ev[1]; ev[1] = t; }
    if (ev[1] < ev[2]) { double t = ev[1]; ev[1] = ev[2]; ev[2] = t; }
    if (ev[0] < ev[1]) { double t = ev[0]; ev[0] = ev[1]; ev[1] = t; }
    return (disc <= 0) ? 3 : 1;
}

double exprtk_trace2(const double *A) {
    return A[0] + A[3];
}
