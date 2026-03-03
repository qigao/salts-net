/**
 * @file simd_helpers.c
 * @brief SIMD-optimized vector operations
 *
 * Uses SIMDe for portable SIMD generation (transforms to Neon/SSE/WASM automatically),
 * preventing fallback to standard C loops on non-AVX architectures.
 */

#include "simd_helpers.h"
#include <string.h>
#include <math.h>

#define SIMDE_ENABLE_NATIVE_ALIASES
#include <simde/x86/avx2.h>

#if defined(_MSC_VER)
    #include <intrin.h>
#endif

/* ========================================================================= */
/* CPU Feature Detection                                                     */
/* ========================================================================= */

int simd_has_avx(void) {
#if defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_IX86))
    int cpuInfo[4];
    __cpuid(cpuInfo, 1);
    return (cpuInfo[2] & (1 << 28)) != 0;
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
    return __builtin_cpu_supports("avx");
#else
    return 0;
#endif
}

int simd_has_avx2(void) {
#if defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_IX86))
    int cpuInfo[4];
    __cpuid(cpuInfo, 0);
    if (cpuInfo[0] >= 7) {
        __cpuidex(cpuInfo, 7, 0);
        return (cpuInfo[1] & (1 << 5)) != 0;
    }
    return 0;
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
    return __builtin_cpu_supports("avx2");
#else
    return 0;
#endif
}

/* ========================================================================= */
/* SIMD Vector Operations                                                    */
/* ========================================================================= */

double simd_sum_avx(const double *data, size_t n) {
    __m256d sum_vec = _mm256_setzero_pd();
    size_t i = 0;

    // Process 4 doubles at a time
    for (; i + 4 <= n; i += 4) {
        __m256d v = _mm256_loadu_pd(&data[i]);
        sum_vec = _mm256_add_pd(sum_vec, v);
    }

    // Horizontal sum
    double sum_arr[4];
    _mm256_storeu_pd(sum_arr, sum_vec);
    double sum = sum_arr[0] + sum_arr[1] + sum_arr[2] + sum_arr[3];

    // Handle remaining elements
    for (; i < n; i++) {
        sum += data[i];
    }

    return sum;
}

double simd_dot_avx(const double *a, const double *b, size_t n) {
    __m256d sum_vec = _mm256_setzero_pd();
    size_t i = 0;

    for (; i + 4 <= n; i += 4) {
        __m256d va = _mm256_loadu_pd(&a[i]);
        __m256d vb = _mm256_loadu_pd(&b[i]);
        sum_vec = _mm256_add_pd(sum_vec, _mm256_mul_pd(va, vb));
    }

    double sum_arr[4];
    _mm256_storeu_pd(sum_arr, sum_vec);
    double sum = sum_arr[0] + sum_arr[1] + sum_arr[2] + sum_arr[3];

    for (; i < n; i++) {
        sum += a[i] * b[i];
    }

    return sum;
}

void simd_scale_avx(const double *in, double *out, double scalar, size_t n) {
    __m256d scalar_vec = _mm256_set1_pd(scalar);
    size_t i = 0;

    for (; i + 4 <= n; i += 4) {
        __m256d v = _mm256_loadu_pd(&in[i]);
        __m256d result = _mm256_mul_pd(v, scalar_vec);
        _mm256_storeu_pd(&out[i], result);
    }

    for (; i < n; i++) {
        out[i] = in[i] * scalar;
    }
}

void simd_add_avx(const double *a, const double *b, double *out, size_t n) {
    size_t i = 0;

    for (; i + 4 <= n; i += 4) {
        __m256d va = _mm256_loadu_pd(&a[i]);
        __m256d vb = _mm256_loadu_pd(&b[i]);
        __m256d result = _mm256_add_pd(va, vb);
        _mm256_storeu_pd(&out[i], result);
    }

    for (; i < n; i++) {
        out[i] = a[i] + b[i];
    }
}

void simd_sub_avx(const double *a, const double *b, double *out, size_t n) {
    size_t i = 0;

    for (; i + 4 <= n; i += 4) {
        __m256d va = _mm256_loadu_pd(&a[i]);
        __m256d vb = _mm256_loadu_pd(&b[i]);
        __m256d result = _mm256_sub_pd(va, vb);
        _mm256_storeu_pd(&out[i], result);
    }

    for (; i < n; i++) {
        out[i] = a[i] - b[i];
    }
}

void simd_mul_avx(const double *a, const double *b, double *out, size_t n) {
    size_t i = 0;

    for (; i + 4 <= n; i += 4) {
        __m256d va = _mm256_loadu_pd(&a[i]);
        __m256d vb = _mm256_loadu_pd(&b[i]);
        __m256d result = _mm256_mul_pd(va, vb);
        _mm256_storeu_pd(&out[i], result);
    }

    for (; i < n; i++) {
        out[i] = a[i] * b[i];
    }
}

void simd_zscore_avx(const double *data, double *out, size_t n, double mean, double sd) {
    if (sd < 1e-15) {
        memset(out, 0, n * sizeof(double));
        return;
    }

    __m256d mean_vec = _mm256_set1_pd(mean);
    __m256d sd_vec = _mm256_set1_pd(sd);
    size_t i = 0;

    for (; i + 4 <= n; i += 4) {
        __m256d v = _mm256_loadu_pd(&data[i]);
        v = _mm256_sub_pd(v, mean_vec);
        v = _mm256_div_pd(v, sd_vec);
        _mm256_storeu_pd(&out[i], v);
    }

    for (; i < n; i++) {
        out[i] = (data[i] - mean) / sd;
    }
}

void simd_mean_variance_avx(const double *data, size_t n, double *mean, double *variance) {
    if (n == 0) {
        *mean = 0.0;
        *variance = 0.0;
        return;
    }

    if (n == 1) {
        *mean = data[0];
        *variance = 0.0;
        return;
    }

    // Pass 1: Mean
    __m256d sum_vec = _mm256_setzero_pd();
    size_t i = 0;

    for (; i + 4 <= n; i += 4) {
        __m256d v = _mm256_loadu_pd(&data[i]);
        sum_vec = _mm256_add_pd(sum_vec, v);
    }

    double sum_arr[4];
    _mm256_storeu_pd(sum_arr, sum_vec);
    double m = sum_arr[0] + sum_arr[1] + sum_arr[2] + sum_arr[3];

    for (; i < n; i++) {
        m += data[i];
    }
    m /= n;
    *mean = m;

    // Pass 2: Variance
    __m256d mean_vec = _mm256_set1_pd(m);
    __m256d var_vec = _mm256_setzero_pd();
    i = 0;

    for (; i + 4 <= n; i += 4) {
        __m256d v = _mm256_loadu_pd(&data[i]);
        v = _mm256_sub_pd(v, mean_vec);
        var_vec = _mm256_add_pd(var_vec, _mm256_mul_pd(v, v));
    }

    _mm256_storeu_pd(sum_arr, var_vec);
    double v_sum = sum_arr[0] + sum_arr[1] + sum_arr[2] + sum_arr[3];

    for (; i < n; i++) {
        double diff = data[i] - m;
        v_sum += diff * diff;
    }

    *variance = v_sum / (n - 1);
}
