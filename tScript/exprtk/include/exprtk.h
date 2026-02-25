/**
 * @file exprtk.h
 * @brief exprtk-like Parser Public API
 */

#ifndef exprtk_H
#define exprtk_H

#include "exprtk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Parse the input string into an AST (simplified API, creates internal arena if needed)
exprtk_node_t *exprtk_parse(const char *input, size_t length);

// Extended parse API allowing custom arena and error reporting
exprtk_node_t *exprtk_parse_ext(const char *input, size_t length,
                                 turbo_arena_t *arena, int *error,
                                 char *error_msg, size_t error_msg_len);

// Clean up (frees the arena)
void exprtk_free(exprtk_node_t *node);

// Evaluate AST
exprtk_value_t exprtk_eval(const exprtk_node_t *node, exprtk_env_t *env);

// Complexity checks
size_t exprtk_node_count(const exprtk_node_t *node);
size_t exprtk_node_depth(const exprtk_node_t *node);

// Node allocation (internal/parser use)
exprtk_node_t *exprtk_node_create(turbo_arena_t *arena, exprtk_node_type_t type);

// Environment management
void exprtk_env_init(exprtk_env_t *env);
void exprtk_env_free(exprtk_env_t *env);
void exprtk_env_set(exprtk_env_t *env, const char *name, exprtk_value_t value);
exprtk_value_t exprtk_env_get(exprtk_env_t *env, const char *name);
void exprtk_env_register_func(exprtk_env_t *env, const char *name, exprtk_native_fn fn, void *user_data);
void exprtk_env_set_constant(exprtk_env_t *env, const char *name, exprtk_value_t value);
void exprtk_env_add_module(exprtk_env_t *env, const exprtk_module_t *mod);

// Math Functions
double exprtk_median(const double *data, size_t n, turbo_arena_t *arena);
double exprtk_percentile(const double *data, size_t n, double p, turbo_arena_t *arena);
double exprtk_geometric_mean(const double *data, size_t n);
double exprtk_harmonic_mean(const double *data, size_t n);
double exprtk_skewness(const double *data, size_t n);
double exprtk_kurtosis(const double *data, size_t n);

double exprtk_fibonacci(int n);
long long exprtk_gcd(long long u, long long v);
double exprtk_normal_rand(double mu, double sigma);

double exprtk_det2(const double *A);
double exprtk_det3(const double *A);
int    exprtk_inv2(const double *A, double *out);
int    exprtk_inv3(const double *A, double *out);
void   exprtk_matmul(const double *A, const double *B, size_t m, size_t k, size_t n, double *out);
void   exprtk_transpose(const double *A, size_t rows, size_t cols, double *out);
int    exprtk_eig2(const double *A, double *ev);
int    exprtk_eig3(const double *A, double *ev);
double exprtk_trace2(const double *A);

#ifdef __cplusplus
}
#endif

#endif // exprtk_H

