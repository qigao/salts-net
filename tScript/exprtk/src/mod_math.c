/**
 * @file math.c
 * @brief Core Math implementation and TurboScript Math Module.
 * Consolidates pure math functions and ExprTk module registration.
 */

#include "exprtk_module.h"
#include "exprtk_internal.h"
#include "arena_buffer.h"
#include <simde/x86/avx2.h>
#include <simde/x86/fma.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ALLOC_DBL(arena, n) TURBO_ARENA_ALLOC_ARRAY(arena, double, n)

/* ========================================================================= */
/* 1. SIMD Helper Functions (Good Taste: No Special Cases)                 */
/* ========================================================================= */

// Threshold: SIMD overhead only worth it for n >= 8
#define SIMD_THRESHOLD 8

// Vector sum reduction: sum(arr[0..n-1])
static inline double simd_sum(const double *arr, size_t n) {
    if (n < SIMD_THRESHOLD) {
        double sum = 0;
        for (size_t i = 0; i < n; i++) sum += arr[i];
        return sum;
    }

    simde__m256d acc = simde_mm256_setzero_pd();
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        acc = simde_mm256_add_pd(acc, simde_mm256_loadu_pd(&arr[i]));
    }

    double tmp[4];
    simde_mm256_storeu_pd(tmp, acc);
    double sum = tmp[0] + tmp[1] + tmp[2] + tmp[3];

    for (; i < n; i++) sum += arr[i];
    return sum;
}

// Dot product: sum(a[i] * b[i])
static inline double simd_dot(const double *a, const double *b, size_t n) {
    if (n < SIMD_THRESHOLD) {
        double sum = 0;
        for (size_t i = 0; i < n; i++) sum += a[i] * b[i];
        return sum;
    }

    simde__m256d acc = simde_mm256_setzero_pd();
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        simde__m256d va = simde_mm256_loadu_pd(&a[i]);
        simde__m256d vb = simde_mm256_loadu_pd(&b[i]);
        acc = simde_mm256_fmadd_pd(va, vb, acc);
    }

    double tmp[4];
    simde_mm256_storeu_pd(tmp, acc);
    double sum = tmp[0] + tmp[1] + tmp[2] + tmp[3];

    for (; i < n; i++) sum += a[i] * b[i];
    return sum;
}

// Squared norm: sum(a[i]^2)
static inline double simd_norm_sq(const double *a, size_t n) {
    if (n < SIMD_THRESHOLD) {
        double sum = 0;
        for (size_t i = 0; i < n; i++) sum += a[i] * a[i];
        return sum;
    }

    simde__m256d acc = simde_mm256_setzero_pd();
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        simde__m256d va = simde_mm256_loadu_pd(&a[i]);
        acc = simde_mm256_fmadd_pd(va, va, acc);
    }

    double tmp[4];
    simde_mm256_storeu_pd(tmp, acc);
    double sum = tmp[0] + tmp[1] + tmp[2] + tmp[3];

    for (; i < n; i++) sum += a[i] * a[i];
    return sum;
}

// Vector addition: dst[i] = a[i] + b[i]
static inline void simd_add(const double *a, const double *b, double *dst, size_t n) {
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; i++) dst[i] = a[i] + b[i];
        return;
    }

    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        simde__m256d va = simde_mm256_loadu_pd(&a[i]);
        simde__m256d vb = simde_mm256_loadu_pd(&b[i]);
        simde_mm256_storeu_pd(&dst[i], simde_mm256_add_pd(va, vb));
    }
    for (; i < n; i++) dst[i] = a[i] + b[i];
}

// Vector subtraction: dst[i] = a[i] - b[i]
static inline void simd_sub(const double *a, const double *b, double *dst, size_t n) {
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; i++) dst[i] = a[i] - b[i];
        return;
    }

    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        simde__m256d va = simde_mm256_loadu_pd(&a[i]);
        simde__m256d vb = simde_mm256_loadu_pd(&b[i]);
        simde_mm256_storeu_pd(&dst[i], simde_mm256_sub_pd(va, vb));
    }
    for (; i < n; i++) dst[i] = a[i] - b[i];
}

// Scalar multiplication: dst[i] = a[i] * scale
static inline void simd_scale(const double *a, double *dst, double scale, size_t n) {
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; i++) dst[i] = a[i] * scale;
        return;
    }

    simde__m256d vs = simde_mm256_set1_pd(scale);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        simde__m256d va = simde_mm256_loadu_pd(&a[i]);
        simde_mm256_storeu_pd(&dst[i], simde_mm256_mul_pd(va, vs));
    }
    for (; i < n; i++) dst[i] = a[i] * scale;
}

/* ========================================================================= */
/* 2. Core Mathematical Implementations                                     */
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

double inv_normal_cdf(double p) {
    if (p <= 0 || p >= 1) return 0;
    const double pp = (p < 0.5) ? p : (1.0 - p);
    const double t = sqrt(-2.0 * log(pp));
    const double c0 = 2.515517;
    const double c1 = 0.802853;
    const double c2 = 0.010328;
    const double d1 = 1.432788;
    const double d2 = 1.89269;
    const double d3 = 0.001308;
    double x = t - (c0 + c1 * t + c2 * t * t) / (1.0 + d1 * t + d2 * t * t + d3 * t * t * t);
    return (p < 0.5) ? -x : x;
}

ols_result_t ols_fit(const double *y, const double *x, size_t n) {
    ols_result_t r = {0};
    if (n < 3) return r;

    // First pass: compute sums using SIMD
    double sx = simd_sum(x, n);
    double sy = simd_sum(y, n);
    double sxx = simd_dot(x, x, n);
    double sxy = simd_dot(x, y, n);

    double denom = (double)n * sxx - sx * sx;
    if (fabs(denom) < 1e-15) return r;
    r.slope = ((double)n * sxy - sx * sy) / denom;
    r.intercept = (sy - r.slope * sx) / (double)n;

    // Second pass: compute residual sum of squares
    double sse = 0;
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; ++i) {
            double e = y[i] - (r.intercept + r.slope * x[i]);
            sse += e * e;
        }
    } else {
        simde__m256d v_intercept = simde_mm256_set1_pd(r.intercept);
        simde__m256d v_slope = simde_mm256_set1_pd(r.slope);
        simde__m256d acc = simde_mm256_setzero_pd();
        size_t i = 0;
        for (; i + 4 <= n; i += 4) {
            simde__m256d vy = simde_mm256_loadu_pd(&y[i]);
            simde__m256d vx = simde_mm256_loadu_pd(&x[i]);
            simde__m256d pred = simde_mm256_fmadd_pd(v_slope, vx, v_intercept);
            simde__m256d e = simde_mm256_sub_pd(vy, pred);
            acc = simde_mm256_fmadd_pd(e, e, acc);
        }
        double tmp[4];
        simde_mm256_storeu_pd(tmp, acc);
        sse = tmp[0] + tmp[1] + tmp[2] + tmp[3];
        for (; i < n; i++) {
            double e = y[i] - (r.intercept + r.slope * x[i]);
            sse += e * e;
        }
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

        size_t j;
        if (pivot != col) {
            // Row swap with SIMD
            j = 0;
            for (; j + 4 <= cols; j += 4) {
                simde__m256d v_col = simde_mm256_loadu_pd(&aug[col * cols + j]);
                simde__m256d v_pivot = simde_mm256_loadu_pd(&aug[pivot * cols + j]);
                simde_mm256_storeu_pd(&aug[col * cols + j], v_pivot);
                simde_mm256_storeu_pd(&aug[pivot * cols + j], v_col);
            }
            for (; j < cols; ++j) {
                double tmp = aug[col * cols + j];
                aug[col * cols + j] = aug[pivot * cols + j];
                aug[pivot * cols + j] = tmp;
            }
        }

        double diag = aug[col * cols + col];
        // Scale row with SIMD
        j = 0;
        simde__m256d v_inv_diag = simde_mm256_set1_pd(1.0 / diag);
        for (; j + 4 <= cols; j += 4) {
            simde__m256d v = simde_mm256_loadu_pd(&aug[col * cols + j]);
            simde_mm256_storeu_pd(&aug[col * cols + j], simde_mm256_mul_pd(v, v_inv_diag));
        }
        for (; j < cols; ++j) aug[col * cols + j] /= diag;

        // Eliminate column with SIMD
        for (size_t row = 0; row < n; ++row) {
            if (row == col) continue;
            double factor = aug[row * cols + col];
            simde__m256d v_factor = simde_mm256_set1_pd(factor);
            j = 0;
            for (; j + 4 <= cols; j += 4) {
                simde__m256d v_row = simde_mm256_loadu_pd(&aug[row * cols + j]);
                simde__m256d v_col = simde_mm256_loadu_pd(&aug[col * cols + j]);
                simde_mm256_storeu_pd(&aug[row * cols + j],
                    simde_mm256_fnmadd_pd(v_factor, v_col, v_row));
            }
            for (; j < cols; ++j) aug[row * cols + j] -= factor * aug[col * cols + j];
        }
    }
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n; ++j) mat[i * n + j] = aug[i * cols + n + j];
    TEMP_FREE(arena, aug);
    return 1;
}

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

    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; ++i) {
            if (data[i] <= 0) return 0;
            sum_log += log(data[i]);
        }
    } else {
        // Check for non-positive values first (can't vectorize log of negative)
        for (size_t i = 0; i < n; ++i) {
            if (data[i] <= 0) return 0;
        }
        // Now compute log sum with SIMD (using scalar log, but vectorized accumulation)
        simde__m256d acc = simde_mm256_setzero_pd();
        size_t i = 0;
        for (; i + 4 <= n; i += 4) {
            // Note: SIMDe doesn't have vectorized log, so we do scalar logs
            double logs[4];
            logs[0] = log(data[i]);
            logs[1] = log(data[i+1]);
            logs[2] = log(data[i+2]);
            logs[3] = log(data[i+3]);
            acc = simde_mm256_add_pd(acc, simde_mm256_loadu_pd(logs));
        }
        double tmp[4];
        simde_mm256_storeu_pd(tmp, acc);
        sum_log = tmp[0] + tmp[1] + tmp[2] + tmp[3];
        for (; i < n; ++i) sum_log += log(data[i]);
    }

    return exp(sum_log / (double)n);
}

double exprtk_harmonic_mean(const double *data, size_t n) {
    if (n == 0) return 0;
    double sum_inv = 0;

    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; ++i) {
            if (fabs(data[i]) < 1e-15) return 0;
            sum_inv += 1.0 / data[i];
        }
    } else {
        // Check for zeros first
        for (size_t i = 0; i < n; ++i) {
            if (fabs(data[i]) < 1e-15) return 0;
        }
        // Compute reciprocal sum with SIMD
        simde__m256d acc = simde_mm256_setzero_pd();
        simde__m256d one = simde_mm256_set1_pd(1.0);
        size_t i = 0;
        for (; i + 4 <= n; i += 4) {
            simde__m256d vd = simde_mm256_loadu_pd(&data[i]);
            acc = simde_mm256_add_pd(acc, simde_mm256_div_pd(one, vd));
        }
        double tmp[4];
        simde_mm256_storeu_pd(tmp, acc);
        sum_inv = tmp[0] + tmp[1] + tmp[2] + tmp[3];
        for (; i < n; ++i) sum_inv += 1.0 / data[i];
    }

    return (double)n / sum_inv;
}

double exprtk_skewness(const double *data, size_t n) {
    if (n < 3) return 0;
    double mean = simd_sum(data, n) / (double)n;

    // Single pass: compute m2 and m3 with SIMD
    double m2 = 0, m3 = 0;
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; ++i) {
            double d = data[i] - mean;
            m2 += d * d;
            m3 += d * d * d;
        }
    } else {
        simde__m256d v_mean = simde_mm256_set1_pd(mean);
        simde__m256d acc2 = simde_mm256_setzero_pd();
        simde__m256d acc3 = simde_mm256_setzero_pd();
        size_t i = 0;
        for (; i + 4 <= n; i += 4) {
            simde__m256d vd = simde_mm256_sub_pd(simde_mm256_loadu_pd(&data[i]), v_mean);
            simde__m256d vd2 = simde_mm256_mul_pd(vd, vd);
            acc2 = simde_mm256_add_pd(acc2, vd2);
            acc3 = simde_mm256_fmadd_pd(vd2, vd, acc3);
        }
        double tmp2[4], tmp3[4];
        simde_mm256_storeu_pd(tmp2, acc2);
        simde_mm256_storeu_pd(tmp3, acc3);
        m2 = tmp2[0] + tmp2[1] + tmp2[2] + tmp2[3];
        m3 = tmp3[0] + tmp3[1] + tmp3[2] + tmp3[3];
        for (; i < n; ++i) {
            double d = data[i] - mean;
            m2 += d * d;
            m3 += d * d * d;
        }
    }

    double var = m2 / (double)(n - 1);
    double std_dev = sqrt(var);
    if (std_dev < 1e-15) return 0;
    return ((double)n / ((double)(n - 1) * (double)(n - 2))) * (m3 / (std_dev * std_dev * std_dev));
}

double exprtk_kurtosis(const double *data, size_t n) {
    if (n < 4) return 0;
    double mean = simd_sum(data, n) / (double)n;

    // Single pass: compute m2 and m4 with SIMD
    double m2 = 0, m4 = 0;
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; ++i) {
            double d = data[i] - mean;
            double d2 = d * d;
            m2 += d2;
            m4 += d2 * d2;
        }
    } else {
        simde__m256d v_mean = simde_mm256_set1_pd(mean);
        simde__m256d acc2 = simde_mm256_setzero_pd();
        simde__m256d acc4 = simde_mm256_setzero_pd();
        size_t i = 0;
        for (; i + 4 <= n; i += 4) {
            simde__m256d vd = simde_mm256_sub_pd(simde_mm256_loadu_pd(&data[i]), v_mean);
            simde__m256d vd2 = simde_mm256_mul_pd(vd, vd);
            acc2 = simde_mm256_add_pd(acc2, vd2);
            acc4 = simde_mm256_fmadd_pd(vd2, vd2, acc4);
        }
        double tmp2[4], tmp4[4];
        simde_mm256_storeu_pd(tmp2, acc2);
        simde_mm256_storeu_pd(tmp4, acc4);
        m2 = tmp2[0] + tmp2[1] + tmp2[2] + tmp2[3];
        m4 = tmp4[0] + tmp4[1] + tmp4[2] + tmp4[3];
        for (; i < n; ++i) {
            double d = data[i] - mean;
            double d2 = d * d;
            m2 += d2;
            m4 += d2 * d2;
        }
    }

    double var = m2 / (double)(n - 1);
    if (var < 1e-15) return 0;
    double nn = (double)n;
    return (nn * (nn + 1.0) / ((nn - 1.0) * (nn - 2.0) * (nn - 3.0))) * (m4 / (var * var))
            - 3.0 * (nn - 1.0) * (nn - 1.0) / ((nn - 2.0) * (nn - 3.0));
}

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

double exprtk_det2(const double *A) {
    return A[0]*A[3] - A[1]*A[2];
}

double exprtk_det3(const double *A) {
    return A[0] * (A[4]*A[8] - A[5]*A[7]) - 
           A[1] * (A[3]*A[8] - A[5]*A[6]) + 
           A[2] * (A[3]*A[7] - A[4]*A[6]);
}

int exprtk_inv2(const double *A, double *out) {
    double det = exprtk_det2(A);
    if (fabs(det) < 1e-15) return 0;
    double idet = 1.0 / det;
    out[0] = A[3] * idet; out[1] = -A[1] * idet;
    out[2] = -A[2] * idet; out[3] = A[0] * idet;
    return 1;
}

int exprtk_inv3(const double *A, double *out) {
    double det = exprtk_det3(A);
    if (fabs(det) < 1e-15) return 0;
    double idet = 1.0 / det;
    out[0] = (A[4]*A[8] - A[5]*A[7]) * idet;
    out[1] = (A[2]*A[7] - A[1]*A[8]) * idet;
    out[2] = (A[1]*A[5] - A[2]*A[4]) * idet;
    out[3] = (A[5]*A[6] - A[3]*A[8]) * idet;
    out[4] = (A[0]*A[8] - A[2]*A[6]) * idet;
    out[5] = (A[2]*A[3] - A[0]*A[5]) * idet;
    out[6] = (A[3]*A[7] - A[4]*A[6]) * idet;
    out[7] = (A[1]*A[6] - A[0]*A[7]) * idet;
    out[8] = (A[0]*A[4] - A[1]*A[3]) * idet;
    return 1;
}

void exprtk_matmul(const double *A, const double *B, size_t m, size_t k, size_t n, double *out) {
    // Matrix multiplication: C[m x n] = A[m x k] * B[k x n]
    // Optimized with SIMD: process 4 columns of B at a time

    for (size_t i = 0; i < m; ++i) {
        size_t j = 0;

        // Process 4 columns at a time with AVX2
        for (; j + 4 <= n; j += 4) {
            simde__m256d sum = simde_mm256_setzero_pd();
            for (size_t p = 0; p < k; ++p) {
                simde__m256d a_val = simde_mm256_set1_pd(A[i * k + p]);
                simde__m256d b_val = simde_mm256_loadu_pd(&B[p * n + j]);
                sum = simde_mm256_fmadd_pd(a_val, b_val, sum);
            }
            simde_mm256_storeu_pd(&out[i * n + j], sum);
        }

        // Handle remaining columns (0-3 columns)
        for (; j < n; ++j) {
            double sum = 0;
            for (size_t p = 0; p < k; ++p) {
                sum += A[i * k + p] * B[p * n + j];
            }
            out[i * n + j] = sum;
        }
    }
}

void exprtk_transpose(const double *A, size_t rows, size_t cols, double *out) {
    // Cache-friendly blocked transpose for better performance on large matrices
    const size_t BLOCK = 8;

    if (rows < BLOCK && cols < BLOCK) {
        // Small matrix: direct transpose
        for (size_t i = 0; i < rows; ++i)
            for (size_t j = 0; j < cols; ++j)
                out[j * rows + i] = A[i * cols + j];
    } else {
        // Large matrix: blocked transpose to improve cache locality
        for (size_t i = 0; i < rows; i += BLOCK) {
            for (size_t j = 0; j < cols; j += BLOCK) {
                size_t i_max = (i + BLOCK < rows) ? i + BLOCK : rows;
                size_t j_max = (j + BLOCK < cols) ? j + BLOCK : cols;
                for (size_t ii = i; ii < i_max; ++ii)
                    for (size_t jj = j; jj < j_max; ++jj)
                        out[jj * rows + ii] = A[ii * cols + jj];
            }
        }
    }
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

/* ========================================================================= */
/* 2. TurboScript Module Wrappers                                           */
/* ========================================================================= */

static exprtk_value_t fn_integrate(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    if ((argc == 3 || argc == 4) && args[0].type == exprtk_VAL_STRING) {
        char f_name[256];
        size_t len = args[0].data.string.len;
        if (len > 255) len = 255;
        memcpy(f_name, args[0].data.string.data, len);
        f_name[len] = '\0';

        double a = args[1].data.number;
        double b = args[2].data.number;
        int n = (argc == 4) ? (int)args[3].data.number : 1000;
        if (n <= 0) n = 1000;
        if (n % 2 != 0) n++;

        double h = (b - a) / n;
        double sum = 0;

        exprtk_value_t vx_a = exprtk_val_num(a);
        exprtk_value_t vx_b = exprtk_val_num(b);
        exprtk_value_t v_a = exprtk_call_internal(f_name, 1, &vx_a, env, arena);
        exprtk_value_t v_b = exprtk_call_internal(f_name, 1, &vx_b, env, arena);
        sum = v_a.data.number + v_b.data.number;

        for (int i = 1; i < n; ++i) {
            if (env && (env->aborted || env->flow != exprtk_FLOW_NORMAL)) break;
            double x = a + i * h;
            exprtk_value_t vx_val = exprtk_val_num(x);
            exprtk_value_t v_x = exprtk_call_internal(f_name, 1, &vx_val, env, arena);
            sum += (i % 2 == 0 ? 2.0 : 4.0) * v_x.data.number;
        }
        return exprtk_val_num((h / 3.0) * sum);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_derivative(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    if ((argc == 2 || argc == 3) && args[0].type == exprtk_VAL_STRING) {
        char f_name[256];
        size_t len = args[0].data.string.len;
        if (len > 255) len = 255;
        memcpy(f_name, args[0].data.string.data, len);
        f_name[len] = '\0';

        double x = args[1].data.number;
        double h = (argc == 3) ? args[2].data.number : 1e-6;

        exprtk_value_t v1_arg = exprtk_val_num(x + h);
        exprtk_value_t v2_arg = exprtk_val_num(x - h);
        exprtk_value_t v1 = exprtk_call_internal(f_name, 1, &v1_arg, env, arena);
        exprtk_value_t v2 = exprtk_call_internal(f_name, 1, &v2_arg, env, arena);

        return exprtk_val_num((v1.data.number - v2.data.number) / (2.0 * h));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_det2(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR && args[0].data.vector.size >= 4)
        return exprtk_val_num(exprtk_det2(args[0].data.vector.data));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_det3(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR && args[0].data.vector.size >= 9)
        return exprtk_val_num(exprtk_det3(args[0].data.vector.data));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_inv2(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR && args[0].data.vector.size >= 4) {
        double *res = ALLOC_DBL(arena, 4);
        if (res && exprtk_inv2(args[0].data.vector.data, res)) return exprtk_val_vec(res, 4);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_inv3(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR && args[0].data.vector.size >= 9) {
        double *res = ALLOC_DBL(arena, 9);
        if (res && exprtk_inv3(args[0].data.vector.data, res)) return exprtk_val_vec(res, 9);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_matmul(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 5 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t m = (size_t)args[2].data.number, k = (size_t)args[3].data.number, n = (size_t)args[4].data.number;
        if (args[0].data.vector.size >= m * k && args[1].data.vector.size >= k * n) {
            double *res = ALLOC_DBL(arena, m * n);
            if (res) { exprtk_matmul(args[0].data.vector.data, args[1].data.vector.data, m, k, n, res); return exprtk_val_vec(res, m * n); }
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_transpose(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 3 && args[0].type == exprtk_VAL_VECTOR) {
        size_t rows = (size_t)args[1].data.number, cols = (size_t)args[2].data.number;
        if (args[0].data.vector.size >= rows * cols) {
            double *res = ALLOC_DBL(arena, rows * cols);
            if (res) { exprtk_transpose(args[0].data.vector.data, rows, cols, res); return exprtk_val_vec(res, rows * cols); }
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_eig2(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR && args[0].data.vector.size >= 4) {
        double *ev = ALLOC_DBL(arena, 2);
        if (ev) { exprtk_eig2(args[0].data.vector.data, ev); return exprtk_val_vec(ev, 2); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_eig3(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 2 && args[1].type == exprtk_VAL_VECTOR && args[0].data.vector.size >= 9 && args[1].data.vector.size >= 3)
        return exprtk_val_num((double)exprtk_eig3(args[0].data.vector.data, args[1].data.vector.data));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_trace2(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR && args[0].data.vector.size >= 4)
        return exprtk_val_num(exprtk_trace2(args[0].data.vector.data));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_sin(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(sin(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_cos(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(cos(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_tan(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(tan(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_sqrt(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(sqrt(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_abs(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(fabs(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_exp(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(exp(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_log(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(log(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ceil(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(ceil(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_floor(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(floor(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_round(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(round(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_len(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) {
        if (args[0].type == exprtk_VAL_STRING) return exprtk_val_num((double)args[0].data.string.len);
        if (args[0].type == exprtk_VAL_VECTOR) return exprtk_val_num((double)args[0].data.vector.size);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_min(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        if (args[0].data.vector.size == 0) return exprtk_val_num(0);
        double res = args[0].data.vector.data[0];
        for (size_t i = 1; i < args[0].data.vector.size; ++i)
            if (args[0].data.vector.data[i] < res) res = args[0].data.vector.data[i];
        return exprtk_val_num(res);
    }
    if (argc > 0) {
        double res = args[0].data.number;
        for (size_t i = 1; i < argc; ++i)
            if (args[i].data.number < res) res = args[i].data.number;
        return exprtk_val_num(res);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_max(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        if (args[0].data.vector.size == 0) return exprtk_val_num(0);
        double res = args[0].data.vector.data[0];
        for (size_t i = 1; i < args[0].data.vector.size; ++i)
            if (args[0].data.vector.data[i] > res) res = args[0].data.vector.data[i];
        return exprtk_val_num(res);
    }
    if (argc > 0) {
        double res = args[0].data.number;
        for (size_t i = 1; i < argc; ++i)
            if (args[i].data.number > res) res = args[i].data.number;
        return exprtk_val_num(res);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_avg(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)arena;
    if (argc != 1 || args[0].type != exprtk_VAL_VECTOR) {
        if (env) env->aborted = 1;
        return exprtk_val_num(0);
    }
    if (args[0].data.vector.size == 0) return exprtk_val_num(0);
    double sum = 0;
    for (size_t i = 0; i < args[0].data.vector.size; ++i) sum += args[0].data.vector.data[i];
    return exprtk_val_num(sum / (double)args[0].data.vector.size);
}

static exprtk_value_t fn_sum(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        double sum = 0;
        for (size_t i = 0; i < args[0].data.vector.size; ++i) sum += args[0].data.vector.data[i];
        return exprtk_val_num(sum);
    }
    if (argc > 0) {
        double sum = 0;
        for (size_t i = 0; i < argc; ++i) sum += args[i].data.number;
        return exprtk_val_num(sum);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_fibonacci(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(exprtk_fibonacci((int)args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_gcd(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 2) return exprtk_val_num((double)exprtk_gcd((long long)args[0].data.number, (long long)args[1].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_normal_rand(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 0 || argc == 2) {
        double mu = (argc == 2) ? args[0].data.number : 0.0;
        double sigma = (argc == 2) ? args[1].data.number : 1.0;
        return exprtk_val_num(exprtk_normal_rand(mu, sigma));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vector_find_value(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        double target = args[1].data.number;
        for (size_t i = 0; i < args[0].data.vector.size; ++i) {
            if (fabs(args[0].data.vector.data[i] - target) < 1e-9) return exprtk_val_num((double)i);
        }
        return exprtk_val_num(-1.0);
    }
    return exprtk_val_num(-1.0);
}

static exprtk_value_t fn_vector_find_all(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        double target = args[1].data.number;
        size_t n = args[0].data.vector.size;
        size_t count = 0;
        for (size_t i = 0; i < n; ++i) {
            if (fabs(args[0].data.vector.data[i] - target) < 1e-9) count++;
        }
        double *res = (double *)turbo_arena_alloc(arena, count * sizeof(double));
        if (res) {
            size_t k = 0;
            for (size_t i = 0; i < n; ++i) {
                if (fabs(args[0].data.vector.data[i] - target) < 1e-9) res[k++] = (double)i;
            }
            return exprtk_val_vec(res, count);
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_median(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        if (args[0].data.vector.size == 0) return exprtk_val_num(0);
        return exprtk_val_num(exprtk_median(args[0].data.vector.data, args[0].data.vector.size, arena));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_percentile(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        if (args[0].data.vector.size == 0) return exprtk_val_num(0);
        return exprtk_val_num(exprtk_percentile(args[0].data.vector.data, args[0].data.vector.size, args[1].data.number, arena));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_geometric_mean(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        if (args[0].data.vector.size == 0) return exprtk_val_num(0);
        return exprtk_val_num(exprtk_geometric_mean(args[0].data.vector.data, args[0].data.vector.size));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_harmonic_mean(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        if (args[0].data.vector.size == 0) return exprtk_val_num(0);
        return exprtk_val_num(exprtk_harmonic_mean(args[0].data.vector.data, args[0].data.vector.size));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_skewness(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        if (args[0].data.vector.size == 0) return exprtk_val_num(0);
        return exprtk_val_num(exprtk_skewness(args[0].data.vector.data, args[0].data.vector.size));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_kurtosis(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        if (args[0].data.vector.size == 0) return exprtk_val_num(0);
        return exprtk_val_num(exprtk_kurtosis(args[0].data.vector.data, args[0].data.vector.size));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_inv_normal_cdf(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(inv_normal_cdf(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ols_fit(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        if (args[0].data.vector.size == 0 || args[0].data.vector.size != args[1].data.vector.size) return exprtk_val_num(0);
        ols_result_t r = ols_fit(args[0].data.vector.data, args[1].data.vector.data, args[0].data.vector.size);
        double *res = ALLOC_DBL(arena, 4);
        if (res) {
            res[0] = r.slope;
            res[1] = r.intercept;
            res[2] = r.res_var;
            res[3] = r.t_slope;
            return exprtk_val_vec(res, 4);
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_inv(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = (size_t)args[1].data.number;
        if (args[0].data.vector.size >= n * n) {
            double *res = ALLOC_DBL(arena, n * n);
            if (res) {
                memcpy(res, args[0].data.vector.data, n * n * sizeof(double));
                if (gauss_jordan_invert(res, n, arena)) return exprtk_val_vec(res, n * n);
            }
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_dot(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        if (args[0].data.vector.size == args[1].data.vector.size) {
            size_t n = args[0].data.vector.size;
            double *a = args[0].data.vector.data;
            double *b = args[1].data.vector.data;
            return exprtk_val_num(simd_dot(a, b, n));
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_cross(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        if (args[0].data.vector.size == 3 && args[1].data.vector.size == 3) {
            double *res = ALLOC_DBL(arena, 3);
            if (res) {
                double *a = args[0].data.vector.data;
                double *b = args[1].data.vector.data;
                res[0] = a[1]*b[2] - a[2]*b[1];
                res[1] = a[2]*b[0] - a[0]*b[2];
                res[2] = a[0]*b[1] - a[1]*b[0];
                return exprtk_val_vec(res, 3);
            }
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_norm(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *a = args[0].data.vector.data;
        return exprtk_val_num(sqrt(simd_norm_sq(a, n)));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_asin(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(asin(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_angle(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        if (args[0].data.vector.size == 3 && args[1].data.vector.size == 3) {
            double *a = args[0].data.vector.data;
            double *b = args[1].data.vector.data;
            double dot = simd_dot(a, b, 3);
            double mag_a = sqrt(simd_norm_sq(a, 3));
            double mag_b = sqrt(simd_norm_sq(b, 3));
            if (mag_a * mag_b > 0) return exprtk_val_num(acos(dot / (mag_a * mag_b)));
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_proj(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        if (args[0].data.vector.size == 3 && args[1].data.vector.size == 3) {
            double *res = ALLOC_DBL(arena, 3);
            if (res) {
                 double *a = args[0].data.vector.data;
                 double *b = args[1].data.vector.data;
                 double dot = simd_dot(a, b, 3);
                 double mag_b_sq = simd_norm_sq(b, 3);
                 if (mag_b_sq > 0) {
                     double scale = dot / mag_b_sq;
                     simd_scale(b, res, scale, 3);
                 } else {
                     res[0] = res[1] = res[2] = 0;
                 }
                 return exprtk_val_vec(res, 3);
            }
        }
    }
    return exprtk_val_num(0);
}

/* ========================================================================= */
/* Flex Engine SIMD Types & Operations                                       */
/* ========================================================================= */
static exprtk_value_t fn_vec_add(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        if (n == args[1].data.vector.size) {
            double *res = ALLOC_DBL(arena, n);
            double *a = args[0].data.vector.data, *b = args[1].data.vector.data;
            simd_add(a, b, res, n);
            return exprtk_val_vec(res, n);
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_sub(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        if (n == args[1].data.vector.size) {
            double *res = ALLOC_DBL(arena, n);
            double *a = args[0].data.vector.data, *b = args[1].data.vector.data;
            simd_sub(a, b, res, n);
            return exprtk_val_vec(res, n);
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_scale(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_NUMBER) {
        size_t n = args[0].data.vector.size;
        double *res = ALLOC_DBL(arena, n);
        double *a = args[0].data.vector.data;
        double s = args[1].data.number;
        simd_scale(a, res, s, n);
        return exprtk_val_vec(res, n);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vec2_perp(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR && args[0].data.vector.size >= 2) {
        double *res = ALLOC_DBL(arena, 2);
        res[0] = -args[0].data.vector.data[1];
        res[1] = args[0].data.vector.data[0];
        return exprtk_val_vec(res, 2);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_normalize(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *a = args[0].data.vector.data;
        double len = sqrt(simd_norm_sq(a, n));
        if (len > 1e-15) {
            double *res = ALLOC_DBL(arena, n);
            simd_scale(a, res, 1.0 / len, n);
            return exprtk_val_vec(res, n);
        }
        return exprtk_val_vec(a, n);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_transform_create(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc >= 5) {
        double x = args[0].data.number, y = args[1].data.number;
        double rot_deg = args[2].data.number;
        double sx = args[3].data.number, sy = args[4].data.number;
        double *res = ALLOC_DBL(arena, 6);
        if (rot_deg == 0) {
            res[0] = sx; res[1] = 0;  res[2] = x;
            res[3] = 0;  res[4] = sy; res[5] = y;
        } else {
            double rad = rot_deg * (3.14159265358979323846 / 180.0);
            double c = cos(rad), s = sin(rad);
            res[0] = sx * c; res[1] = -sy * s; res[2] = x;
            res[3] = sx * s; res[4] = sy * c;  res[5] = y;
        }
        return exprtk_val_vec(res, 6);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_transform_pt(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[0].data.vector.size >= 6 && args[1].data.vector.size >= 2) {
        double *m = args[0].data.vector.data;
        double *p = args[1].data.vector.data;
        double *res = ALLOC_DBL(arena, 2);
        double px = p[0], py = p[1];
        res[0] = m[0]*px + m[1]*py + m[2];
        res[1] = m[3]*px + m[4]*py + m[5];
        return exprtk_val_vec(res, 2);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_transform_mul(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[0].data.vector.size >= 6 && args[1].data.vector.size >= 6) {
        double *res = ALLOC_DBL(arena, 6);
        double *m = args[0].data.vector.data, *o = args[1].data.vector.data;
        res[0] = m[0] * o[0] + m[1] * o[3];
        res[1] = m[0] * o[1] + m[1] * o[4];
        res[2] = m[0] * o[2] + m[1] * o[5] + m[2];
        res[3] = m[3] * o[0] + m[4] * o[3];
        res[4] = m[3] * o[1] + m[4] * o[4];
        res[5] = m[3] * o[2] + m[4] * o[5] + m[5];
        return exprtk_val_vec(res, 6);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_transform_inv(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR && args[0].data.vector.size >= 6) {
        double *m = args[0].data.vector.data;
        double det = m[0]*m[4] - m[1]*m[3];
        if (fabs(det) < 1e-15) return exprtk_val_num(0);
        double *res = ALLOC_DBL(arena, 6);
        double idet = 1.0 / det;
        res[0] = m[4]*idet;
        res[1] = -m[1]*idet;
        res[2] = (m[1]*m[5] - m[4]*m[2])*idet;
        res[3] = -m[3]*idet;
        res[4] = m[0]*idet;
        res[5] = (m[3]*m[2] - m[0]*m[5])*idet;
        return exprtk_val_vec(res, 6);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_rotate_vec(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 3 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_NUMBER && args[2].type == exprtk_VAL_VECTOR) {
        if (args[0].data.vector.size == 3 && args[2].data.vector.size == 3) {
             double *res = ALLOC_DBL(arena, 3);
             if (res) {
                 double *v = args[0].data.vector.data;
                 double *axis = args[2].data.vector.data;
                 double angle = args[1].data.number;
                 double c = cos(angle), s = sin(angle);
                 double dot = v[0]*axis[0] + v[1]*axis[1] + v[2]*axis[2];
                 for (int i=0; i<3; i++) {
                     res[i] = v[i] * c + (axis[(i+1)%3]*v[(i+2)%3] - axis[(i+2)%3]*v[(i+1)%3]) * s + axis[i] * dot * (1.0 - c);
                 }
                 return exprtk_val_vec(res, 3);
             }
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_lookat(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 3 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR) {
        if(args[0].data.vector.size == 3 && args[1].data.vector.size == 3 && args[2].data.vector.size == 3) {
            double *res = ALLOC_DBL(arena, 16);
            if (res) {
                 double *eye = args[0].data.vector.data;
                 double *center = args[1].data.vector.data;
                 double *up = args[2].data.vector.data;
                 double f[3] = {center[0]-eye[0], center[1]-eye[1], center[2]-eye[2]};
                 double flen = sqrt(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
                 if (flen > 0) { f[0]/=flen; f[1]/=flen; f[2]/=flen; }
                 double s[3] = {f[1]*up[2] - f[2]*up[1], f[2]*up[0] - f[0]*up[2], f[0]*up[1] - f[1]*up[0]};
                 double slen = sqrt(s[0]*s[0] + s[1]*s[1] + s[2]*s[2]);
                 if (slen > 0) { s[0]/=slen; s[1]/=slen; s[2]/=slen; }
                 double u[3] = {s[1]*f[2] - s[2]*f[1], s[2]*f[0] - s[0]*f[2], s[0]*f[1] - s[1]*f[0]};
                 res[0]=s[0]; res[1]=s[1]; res[2]=s[2]; res[3]=-(s[0]*eye[0]+s[1]*eye[1]+s[2]*eye[2]);
                 res[4]=u[0]; res[5]=u[1]; res[6]=u[2]; res[7]=-(u[0]*eye[0]+u[1]*eye[1]+u[2]*eye[2]);
                 res[8]=-f[0]; res[9]=-f[1]; res[10]=-f[2]; res[11]=(f[0]*eye[0]+f[1]*eye[1]+f[2]*eye[2]);
                 res[12]=0; res[13]=0; res[14]=0; res[15]=1;
                 return exprtk_val_vec(res, 16);
            }
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_perspective(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 4) {
         double *res = ALLOC_DBL(arena, 16);
         if (res) {
              double fovy = args[0].data.number;
              double aspect = args[1].data.number;
              double zNear = args[2].data.number;
              double zFar = args[3].data.number;
              double f = 1.0 / tan(fovy / 2.0);
              memset(res, 0, 16 * sizeof(double));
              res[0] = f / aspect;
              res[5] = f;
              res[10] = (zFar + zNear) / (zNear - zFar);
              res[11] = (2.0 * zFar * zNear) / (zNear - zFar);
              res[14] = -1.0;
              return exprtk_val_vec(res, 16);
         }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ortho(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 6) {
         double *res = ALLOC_DBL(arena, 16);
         if (res) {
              double l = args[0].data.number, r = args[1].data.number;
              double b = args[2].data.number, t = args[3].data.number;
              double n = args[4].data.number, f = args[5].data.number;
              memset(res, 0, 16 * sizeof(double));
              res[0] = 2.0 / (r - l);
              res[5] = 2.0 / (t - b);
              res[10] = -2.0 / (f - n);
              res[3] = -(r + l) / (r - l);
              res[7] = -(t + b) / (t - b);
              res[11] = -(f + n) / (f - n);
              res[15] = 1.0;
              return exprtk_val_vec(res, 16);
         }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_mat4_mul(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        if(args[0].data.vector.size == 16 && args[1].data.vector.size == 16) {
            double *res = ALLOC_DBL(arena, 16);
            if (res) {
                 exprtk_matmul(args[0].data.vector.data, args[1].data.vector.data, 4, 4, 4, res);
                 return exprtk_val_vec(res, 16);
            }
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_mat3_mul(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        if(args[0].data.vector.size == 9 && args[1].data.vector.size == 9) {
            double *res = ALLOC_DBL(arena, 9);
            if (res) {
                 exprtk_matmul(args[0].data.vector.data, args[1].data.vector.data, 3, 3, 3, res);
                 return exprtk_val_vec(res, 9);
            }
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_acos(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(acos(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_atan(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(atan(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_atan2(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 2) return exprtk_val_num(atan2(args[0].data.number, args[1].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_sinh(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(sinh(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_cosh(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(cosh(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_tanh(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(tanh(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_pow(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 2) return exprtk_val_num(pow(args[0].data.number, args[1].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_clamp(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 3) {
        double x = args[0].data.number;
        double lo = args[1].data.number;
        double hi = args[2].data.number;
        if (lo > hi) {
            double tmp = lo;
            lo = hi;
            hi = tmp;
        }
        if (x < lo) x = lo;
        if (x > hi) x = hi;
        return exprtk_val_num(x);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_lerp(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 3) {
        double a = args[0].data.number;
        double b = args[1].data.number;
        double t = args[2].data.number;
        return exprtk_val_num(a + (b - a) * t);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_smoothstep(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 3) {
        double edge0 = args[0].data.number;
        double edge1 = args[1].data.number;
        double x = args[2].data.number;
        if (fabs(edge1 - edge0) < 1e-15) {
            return exprtk_val_num(x < edge0 ? 0.0 : 1.0);
        }
        double t = (x - edge0) / (edge1 - edge0);
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        return exprtk_val_num(t * t * (3.0 - 2.0 * t));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_radians(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) {
        const double pi = 3.14159265358979323846;
        return exprtk_val_num(args[0].data.number * (pi / 180.0));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_degrees(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) {
        const double pi = 3.14159265358979323846;
        return exprtk_val_num(args[0].data.number * (180.0 / pi));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_is_nan(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) {
        double x = args[0].data.number;
        return exprtk_val_num((x != x) ? 1.0 : 0.0);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_is_inf(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) {
        double x = args[0].data.number;
        return exprtk_val_num((x == INFINITY || x == -INFINITY) ? 1.0 : 0.0);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_hypot(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 2) return exprtk_val_num(hypot(args[0].data.number, args[1].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_log1p(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(log1p(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_expm1(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(expm1(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_step(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 2) return exprtk_val_num(args[1].data.number < args[0].data.number ? 0.0 : 1.0);
    return exprtk_val_num(0);
}

static exprtk_value_t fn_fract(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) {
        double x = args[0].data.number;
        return exprtk_val_num(x - floor(x));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_saturate(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) {
        double x = args[0].data.number;
        if (x < 0.0) x = 0.0;
        if (x > 1.0) x = 1.0;
        return exprtk_val_num(x);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_inverse_lerp(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 3) {
        double a = args[0].data.number;
        double b = args[1].data.number;
        double x = args[2].data.number;
        double d = b - a;
        if (fabs(d) < 1e-15) return exprtk_val_num(0);
        return exprtk_val_num((x - a) / d);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_remap(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 5) {
        double in_min = args[1].data.number;
        double in_max = args[2].data.number;
        double out_min = args[3].data.number;
        double out_max = args[4].data.number;
        double d = in_max - in_min;
        if (fabs(d) < 1e-15) return exprtk_val_num(out_min);
        double t = (args[0].data.number - in_min) / d;
        return exprtk_val_num(out_min + (out_max - out_min) * t);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_relu(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) {
        double x = args[0].data.number;
        return exprtk_val_num(x > 0.0 ? x : 0.0);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_sigmoid(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) {
        double x = args[0].data.number;
        if (x >= 0.0) {
            double z = exp(-x);
            return exprtk_val_num(1.0 / (1.0 + z));
        }
        double z = exp(x);
        return exprtk_val_num(z / (1.0 + z));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_softplus(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) {
        double x = args[0].data.number;
        if (x > 40.0) return exprtk_val_num(x);
        if (x < -40.0) return exprtk_val_num(exp(x));
        return exprtk_val_num(log1p(exp(x)));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_copysign(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 2) return exprtk_val_num(copysign(args[0].data.number, args[1].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_cbrt(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) {
        double x = args[0].data.number;
        if (x == 0.0) return exprtk_val_num(0.0);
        return exprtk_val_num(copysign(pow(fabs(x), 1.0 / 3.0), x));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_exp2(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(pow(2.0, args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_logn(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 2) {
        double x = args[0].data.number;
        double base = args[1].data.number;
        if (x <= 0.0 || base <= 0.0 || fabs(base - 1.0) < 1e-15) return exprtk_val_num(0);
        return exprtk_val_num(log(x) / log(base));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_log10(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(log10(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_log2(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(log2(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_trunc(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(trunc(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_sgn(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) {
        double val = args[0].data.number;
        return exprtk_val_num((val > 0) - (val < 0));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_mod(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 2) return exprtk_val_num(fmod(args[0].data.number, args[1].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_rand(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    (void)argc; (void)args;
    return exprtk_val_num((double)rand() / RAND_MAX);
}

/* ========================================================================= */
/* 3. Module Definition                                                     */
/* ========================================================================= */

static const exprtk_func_entry_t math_entries[] = {
    { "abs",               fn_abs },
    { "acos",              fn_acos },
    { "angle",             fn_angle },
    { "asin",              fn_asin },
    { "atan",              fn_atan },
    { "atan2",             fn_atan2 },
    { "avg",               fn_avg },
    { "cbrt",              fn_cbrt },
    { "ceil",              fn_ceil },
    { "clamp",             fn_clamp },
    { "copysign",          fn_copysign },
    { "cos",               fn_cos },
    { "cosh",              fn_cosh },
    { "cross",             fn_cross },
    { "degrees",           fn_degrees },
    { "derivative",        fn_derivative },
    { "det2",              fn_det2 },
    { "det3",              fn_det3 },
    { "dot",               fn_dot },
    { "eig2",              fn_eig2 },
    { "eig3",              fn_eig3 },
    { "exp",               fn_exp },
    { "exp2",              fn_exp2 },
    { "expm1",             fn_expm1 },
    { "fibonacci",         fn_fibonacci },
    { "fract",             fn_fract },
    { "floor",             fn_floor },
    { "gcd",               fn_gcd },
    { "geometric_mean",    fn_geometric_mean },
    { "harmonic_mean",     fn_harmonic_mean },
    { "hypot",             fn_hypot },
    { "integrate",         fn_integrate },
    { "is_inf",            fn_is_inf },
    { "is_nan",            fn_is_nan },
    { "inv",               fn_inv },
    { "inv_normal_cdf",    fn_inv_normal_cdf },
    { "inv2",              fn_inv2 },
    { "inv3",              fn_inv3 },
    { "inverse_lerp",      fn_inverse_lerp },
    { "kurtosis",          fn_kurtosis },
    { "lerp",              fn_lerp },
    { "len",               fn_len },
    { "log",               fn_log },
    { "log10",             fn_log10 },
    { "log1p",             fn_log1p },
    { "log2",              fn_log2 },
    { "logn",              fn_logn },
    { "lookat",            fn_lookat },
    { "mat3_mul",          fn_mat3_mul },
    { "mat4_mul",          fn_mat4_mul },
    { "matmul",            fn_matmul },
    { "max",               fn_max },
    { "median",            fn_median },
    { "min",               fn_min },
    { "mod",               fn_mod },
    { "norm",              fn_norm },
    { "normal_rand",       fn_normal_rand },
    { "ols_fit",           fn_ols_fit },
    { "ortho",             fn_ortho },
    { "percentile",        fn_percentile },
    { "perspective",       fn_perspective },
    { "pow",               fn_pow },
    { "proj",              fn_proj },
    { "rand",              fn_rand },
    { "relu",              fn_relu },
    { "remap",             fn_remap },
    { "rotate",            fn_rotate_vec },
    { "radians",           fn_radians },
    { "round",             fn_round },
    { "sgn",               fn_sgn },
    { "sin",               fn_sin },
    { "sigmoid",           fn_sigmoid },
    { "sinh",              fn_sinh },
    { "size",              fn_len },
    { "saturate",          fn_saturate },
    { "skewness",          fn_skewness },
    { "softplus",          fn_softplus },
    { "smoothstep",        fn_smoothstep },
    { "sqrt",              fn_sqrt },
    { "step",              fn_step },
    { "sum",               fn_sum },
    { "tan",               fn_tan },
    { "tanh",              fn_tanh },
    { "trace2",            fn_trace2 },
    { "transform_create",  fn_transform_create },
    { "transform_inv",     fn_transform_inv },
    { "transform_mul",     fn_transform_mul },
    { "transform_pt",      fn_transform_pt },
    { "transpose",         fn_transpose },
    { "trunc",             fn_trunc },
    { "vec2_perp",         fn_vec2_perp },
    { "vec_add",           fn_vec_add },
    { "vec_normalize",     fn_vec_normalize },
    { "vec_scale",         fn_vec_scale },
    { "vec_sub",           fn_vec_sub },
    { "vector_find_all",   fn_vector_find_all },
    { "vector_find_value", fn_vector_find_value },
};

static const exprtk_module_t math_module = {
    "math", math_entries, sizeof(math_entries) / sizeof(math_entries[0])
};

const exprtk_module_t *exprtk_module_math(void) { return &math_module; }
