/**
 * @file simd_helpers.h
 * @brief SIMD-optimized vector operations using AVX/AVX2
 */

#ifndef SIMD_HELPERS_H
#define SIMD_HELPERS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Vectorized sum using AVX (processes 4 doubles at a time)
 * @param data Input array
 * @param n Array size
 * @return Sum of all elements
 */
double simd_sum_avx(const double *data, size_t n);

/**
 * @brief Vectorized dot product using AVX
 * @param a First vector
 * @param b Second vector
 * @param n Vector size
 * @return Dot product a·b
 */
double simd_dot_avx(const double *a, const double *b, size_t n);

/**
 * @brief Vectorized scalar multiplication using AVX
 * @param in Input array
 * @param out Output array
 * @param scalar Scalar multiplier
 * @param n Array size
 */
void simd_scale_avx(const double *in, double *out, double scalar, size_t n);

/**
 * @brief Vectorized element-wise addition using AVX
 * @param a First vector
 * @param b Second vector
 * @param out Output vector (a + b)
 * @param n Vector size
 */
void simd_add_avx(const double *a, const double *b, double *out, size_t n);

/**
 * @brief Vectorized element-wise subtraction using AVX
 * @param a First vector
 * @param b Second vector
 * @param out Output vector (a - b)
 * @param n Vector size
 */
void simd_sub_avx(const double *a, const double *b, double *out, size_t n);

/**
 * @brief Vectorized element-wise multiplication using AVX
 * @param a First vector
 * @param b Second vector
 * @param out Output vector (a * b)
 * @param n Vector size
 */
void simd_mul_avx(const double *a, const double *b, double *out, size_t n);

/**
 * @brief Vectorized z-score normalization using AVX
 * @param in Input array
 * @param out Output array (z-scores)
 * @param n Array size
 * @param mean Mean value
 * @param sd Standard deviation
 */
void simd_zscore_avx(const double *in, double *out, size_t n, double mean, double sd);

/**
 * @brief Vectorized mean and variance calculation (single pass)
 * @param data Input array
 * @param n Array size
 * @param out_mean Output: mean value
 * @param out_variance Output: variance value
 */
void simd_mean_variance_avx(const double *data, size_t n, double *out_mean, double *out_variance);

/**
 * @brief Check if AVX is available at runtime
 * @return 1 if AVX is supported, 0 otherwise
 */
int simd_has_avx(void);

/**
 * @brief Check if AVX2 is available at runtime
 * @return 1 if AVX2 is supported, 0 otherwise
 */
int simd_has_avx2(void);

#ifdef __cplusplus
}
#endif

#endif /* SIMD_HELPERS_H */
