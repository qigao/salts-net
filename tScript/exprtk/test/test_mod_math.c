/**
 * @file test_mod_math.c
 * @brief Unit tests for SIMD-optimized math functions in mod_math.c
 */

#include "tinytest.h"
#include "exprtk_module.h" 
#include "simd_helpers.h"
#include "turbo_buffer.h"
#include <stdlib.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TEST_TOLERANCE 1e-10

// Forward declarations


// Helper function
static void fill_random(double *arr, size_t n, double min, double max) {
    for (size_t i = 0; i < n; i++) {
        arr[i] = min + (max - min) * ((double)rand() / RAND_MAX);
    }
}

spec("SIMD Math Module") {
    describe("OLS Regression") {
        it("should fit perfect linear relationship") {
            // y = 2x + 3
            double x[] = {1, 2, 3, 4, 5};
            double y[] = {5, 7, 9, 11, 13};

            ols_result_t result = ols_fit(y, x, 5);

            check_double_eq(result.slope, 2.0, TEST_TOLERANCE);
            check_double_eq(result.intercept, 3.0, TEST_TOLERANCE);
            check_double_eq(result.res_var, 0.0, TEST_TOLERANCE);
        }

        it("should handle large datasets") {
            const size_t n = 1000;
            double *x = malloc(n * sizeof(double));
            double *y = malloc(n * sizeof(double));

            // y = 1.5x + 2.0 + noise
            for (size_t i = 0; i < n; i++) {
                x[i] = (double)i;
                y[i] = 1.5 * x[i] + 2.0 + ((double)rand() / RAND_MAX - 0.5) * 0.1;
            }

            ols_result_t result = ols_fit(y, x, n);

            check_double_eq(result.slope, 1.5, 0.01);
            check_double_eq(result.intercept, 2.0, 0.1);

            free(x);
            free(y);
        }
    }

    describe("Skewness") {
        it("should be near zero for symmetric data") {
            double data[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
            double skew = exprtk_skewness(data, 9);
            check(fabs(skew) < 0.1);
        }

        it("should handle large datasets") {
            const size_t n = 1000;
            double *data = malloc(n * sizeof(double));
            fill_random(data, n, 0.0, 100.0);

            double skew = exprtk_skewness(data, n);
            check(fabs(skew) < 2.0);

            free(data);
        }
    }

    describe("Kurtosis") {
        it("should be reasonable for normal-like data") {
            const size_t n = 1000;
            double *data = malloc(n * sizeof(double));

            // Box-Muller transform for normal distribution
            for (size_t i = 0; i < n; i += 2) {
                double u1 = (double)rand() / RAND_MAX;
                double u2 = (double)rand() / RAND_MAX;
                data[i] = sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
                if (i + 1 < n) {
                    data[i + 1] = sqrt(-2.0 * log(u1)) * sin(2.0 * M_PI * u2);
                }
            }

            double kurt = exprtk_kurtosis(data, n);
            check(fabs(kurt) < 1.0);

            free(data);
        }
    }

    describe("Geometric Mean") {
        it("should calculate correctly") {
            double data[] = {1, 2, 4, 8, 16};
            double gm = exprtk_geometric_mean(data, 5);
            double expected = pow(1.0 * 2.0 * 4.0 * 8.0 * 16.0, 1.0 / 5.0);
            check_double_eq(gm, expected, TEST_TOLERANCE);
        }

        it("should handle large datasets") {
            const size_t n = 500;
            double *data = malloc(n * sizeof(double));
            fill_random(data, n, 1.0, 100.0);

            double gm = exprtk_geometric_mean(data, n);
            check(gm > 0 && gm < 100.0);

            free(data);
        }
    }

    describe("Harmonic Mean") {
        it("should calculate correctly") {
            double data[] = {1, 2, 4};
            double hm = exprtk_harmonic_mean(data, 3);
            double expected = 3.0 / (1.0/1.0 + 1.0/2.0 + 1.0/4.0);
            check_double_eq(hm, expected, TEST_TOLERANCE);
        }

        it("should handle large datasets") {
            const size_t n = 500;
            double *data = malloc(n * sizeof(double));
            fill_random(data, n, 1.0, 100.0);

            double hm = exprtk_harmonic_mean(data, n);
            check(hm > 0 && hm < 100.0);

            free(data);
        }
    }

    describe("Matrix Multiplication") {
        it("should multiply by identity correctly") {
            double A[] = {1, 2, 3, 4};  // 2x2
            double I[] = {1, 0, 0, 1};  // 2x2 identity
            double result[4];

            exprtk_matmul(A, I, 2, 2, 2, result);

            for (int i = 0; i < 4; i++) {
                check_double_eq(result[i], A[i], TEST_TOLERANCE);
            }
        }

        it("should multiply matrices correctly") {
            // [1 2]   [5 6]   [19 22]
            // [3 4] * [7 8] = [43 50]
            double A[] = {1, 2, 3, 4};
            double B[] = {5, 6, 7, 8};
            double expected[] = {19, 22, 43, 50};
            double result[4];

            exprtk_matmul(A, B, 2, 2, 2, result);

            for (int i = 0; i < 4; i++) {
                check_double_eq(result[i], expected[i], TEST_TOLERANCE);
            }
        }

        it("should handle large matrices") {
            const size_t n = 16;
            double *A = malloc(n * n * sizeof(double));
            double *B = malloc(n * n * sizeof(double));
            double *C = malloc(n * n * sizeof(double));

            fill_random(A, n * n, -10.0, 10.0);
            fill_random(B, n * n, -10.0, 10.0);

            exprtk_matmul(A, B, n, n, n, C);

            // Verify one element manually
            double expected = 0;
            for (size_t k = 0; k < n; k++) {
                expected += A[k] * B[k * n];
            }
            check_double_eq(C[0], expected, 1e-8);

            free(A);
            free(B);
            free(C);
        }
    }

    describe("Matrix Transpose") {
        it("should transpose square matrix") {
            double A[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};  // 3x3
            double expected[] = {1, 4, 7, 2, 5, 8, 3, 6, 9};
            double result[9];

            exprtk_transpose(A, 3, 3, result);

            for (int i = 0; i < 9; i++) {
                check_double_eq(result[i], expected[i], TEST_TOLERANCE);
            }
        }

        it("should transpose rectangular matrix") {
            double A[] = {1, 2, 3, 4, 5, 6};  // 2x3
            double expected[] = {1, 4, 2, 5, 3, 6};  // 3x2
            double result[6];

            exprtk_transpose(A, 2, 3, result);

            for (int i = 0; i < 6; i++) {
                check_double_eq(result[i], expected[i], TEST_TOLERANCE);
            }
        }

        it("should handle large matrices") {
            const size_t rows = 32, cols = 24;
            double *A = malloc(rows * cols * sizeof(double));
            double *B = malloc(rows * cols * sizeof(double));

            fill_random(A, rows * cols, -10.0, 10.0);
            exprtk_transpose(A, rows, cols, B);

            // Verify: B[j * rows + i] = A[i * cols + j]
            check_double_eq(B[0 * rows + 0], A[0 * cols + 0], TEST_TOLERANCE);
            check_double_eq(B[1 * rows + 0], A[0 * cols + 1], TEST_TOLERANCE);
            check_double_eq(B[(cols-1) * rows + (rows-1)], A[(rows-1) * cols + (cols-1)], TEST_TOLERANCE);

            free(A);
            free(B);
        }
    }

    describe("Gauss-Jordan Inversion") {
        it("should invert identity matrix") {
            turbo_pool_t arena;
            turbo_pool_init(&arena, 4096);

            double I[] = {1, 0, 0, 1};  // 2x2 identity
            double result[4];
            memcpy(result, I, sizeof(I));

            int success = gauss_jordan_invert(result, 2, &arena);
            check(success);

            for (int i = 0; i < 4; i++) {
                check_double_eq(result[i], I[i], TEST_TOLERANCE);
            }

            turbo_pool_free(&arena);
        }

        it("should invert 2x2 matrix") {
            turbo_pool_t arena;
            turbo_pool_init(&arena, 4096);

            // [4 7]  inverse is [0.6 -0.7]
            // [2 6]             [-0.2 0.4]
            double A[] = {4, 7, 2, 6};
            double expected[] = {0.6, -0.7, -0.2, 0.4};

            int success = gauss_jordan_invert(A, 2, &arena);
            check(success);

            for (int i = 0; i < 4; i++) {
                check_double_eq(A[i], expected[i], 1e-9);
            }

            turbo_pool_free(&arena);
        }
    }

    describe("Performance Benchmarks") {
        static volatile double sink = 0.0;

        bench("Statistical Functions") {
            const size_t n = 1000;
            double *data = malloc(n * sizeof(double));
            double *x = malloc(n * sizeof(double));
            double *y = malloc(n * sizeof(double));
            fill_random(data, n, 1.0, 100.0);
            fill_random(x, n, 0.0, 100.0);
            fill_random(y, n, 0.0, 100.0);

            benchmark("ols_fit", 1000) {
                ols_result_t r = ols_fit(y, x, n);
                sink += r.slope;
            }

            benchmark("skewness", 1000) {
                sink += exprtk_skewness(data, n);
            }

            benchmark("kurtosis", 1000) {
                sink += exprtk_kurtosis(data, n);
            }

            benchmark("geometric_mean", 1000) {
                sink += exprtk_geometric_mean(data, n);
            }

            benchmark("harmonic_mean", 1000) {
                sink += exprtk_harmonic_mean(data, n);
            }

            free(data);
            free(x);
            free(y);
        }

        bench("Matrix Operations") {
            const size_t n = 16;
            double *A = malloc(n * n * sizeof(double));
            double *B = malloc(n * n * sizeof(double));
            double *C = malloc(n * n * sizeof(double));
            fill_random(A, n * n, -10.0, 10.0);
            fill_random(B, n * n, -10.0, 10.0);

            benchmark("matmul 16x16", 1000) {
                exprtk_matmul(A, B, n, n, n, C);
                sink += C[0];
            }

            benchmark("transpose 16x16", 5000) {
                exprtk_transpose(A, n, n, C);
                sink += C[0];
            }

            free(A);
            free(B);
            free(C);
        }

        bench("Large Matrix Operations") {
            const size_t n = 64;
            double *A = malloc(n * n * sizeof(double));
            double *B = malloc(n * n * sizeof(double));
            double *C = malloc(n * n * sizeof(double));
            fill_random(A, n * n, -10.0, 10.0);
            fill_random(B, n * n, -10.0, 10.0);

            benchmark("matmul 64x64", 100) {
                exprtk_matmul(A, B, n, n, n, C);
                sink += C[0];
            }

            benchmark("transpose 64x64", 1000) {
                exprtk_transpose(A, n, n, C);
                sink += C[0];
            }

            free(A);
            free(B);
            free(C);
        }
    }
}
