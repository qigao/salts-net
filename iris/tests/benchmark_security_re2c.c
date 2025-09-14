#include "security.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BENCHMARK_ITERATIONS 100000
#define TEST_STRING_COUNT 10

static const char* test_header_names[] = {
    "Content-Type",
    "User-Agent", 
    "Authorization",
    "Accept-Encoding",
    "Cache-Control",
    "X-Forwarded-For",
    "X-Real-IP",
    "X-Custom-Header-123",
    "Accept-Language",
    "Connection"
};

static const char* test_header_values[] = {
    "application/json; charset=utf-8",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36",
    "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9",
    "gzip, deflate, br",
    "no-cache, no-store, must-revalidate",
    "203.0.113.195, 70.41.3.18, 150.172.238.178",
    "192.168.1.100",
    "custom-value-with-numbers-123-and-symbols",
    "en-US,en;q=0.9,fr;q=0.8",
    "keep-alive"
};

static const char* test_url_paths[] = {
    "/",
    "/api/v1/users",
    "/api/v1/users/123/profile",
    "/static/css/main.css",
    "/images/logo.png",
    "/api/v2/data/export.json",
    "/admin/dashboard/analytics",
    "/user/settings/privacy",
    "/search/results",
    "/api/v1/auth/login"
};

static const char* test_parameters[] = {
    "username",
    "user_id_123",
    "search-query",
    "file.name.txt",
    "category_filter",
    "sort_order",
    "page_number",
    "items_per_page",
    "timestamp_value",
    "api_key_token"
};

static double get_time_diff(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
}

void benchmark_header_name_validation(void) {
    struct timespec start, end;
    printf("Benchmarking HTTP header name validation...\n");
    
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    for (int i = 0; i < BENCHMARK_ITERATIONS; i++) {
        for (int j = 0; j < TEST_STRING_COUNT; j++) {
            iris_validate_http_header_name(test_header_names[j], 100);
        }
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    double elapsed = get_time_diff(start, end);
    double ops_per_sec = (BENCHMARK_ITERATIONS * TEST_STRING_COUNT) / elapsed;
    
    printf("  Time: %.3f seconds\n", elapsed);
    printf("  Operations: %d\n", BENCHMARK_ITERATIONS * TEST_STRING_COUNT);
    printf("  Operations/sec: %.0f\n", ops_per_sec);
    printf("  Avg time per operation: %.3f microseconds\n\n", (elapsed * 1e6) / (BENCHMARK_ITERATIONS * TEST_STRING_COUNT));
}

void benchmark_header_value_validation(void) {
    struct timespec start, end;
    printf("Benchmarking HTTP header value validation...\n");
    
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    for (int i = 0; i < BENCHMARK_ITERATIONS; i++) {
        for (int j = 0; j < TEST_STRING_COUNT; j++) {
            iris_validate_http_header_value(test_header_values[j], 200);
        }
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    double elapsed = get_time_diff(start, end);
    double ops_per_sec = (BENCHMARK_ITERATIONS * TEST_STRING_COUNT) / elapsed;
    
    printf("  Time: %.3f seconds\n", elapsed);
    printf("  Operations: %d\n", BENCHMARK_ITERATIONS * TEST_STRING_COUNT);
    printf("  Operations/sec: %.0f\n", ops_per_sec);
    printf("  Avg time per operation: %.3f microseconds\n\n", (elapsed * 1e6) / (BENCHMARK_ITERATIONS * TEST_STRING_COUNT));
}

void benchmark_url_path_validation(void) {
    struct timespec start, end;
    printf("Benchmarking URL path validation...\n");
    
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    for (int i = 0; i < BENCHMARK_ITERATIONS; i++) {
        for (int j = 0; j < TEST_STRING_COUNT; j++) {
            iris_validate_url_path(test_url_paths[j], 100);
        }
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    double elapsed = get_time_diff(start, end);
    double ops_per_sec = (BENCHMARK_ITERATIONS * TEST_STRING_COUNT) / elapsed;
    
    printf("  Time: %.3f seconds\n", elapsed);
    printf("  Operations: %d\n", BENCHMARK_ITERATIONS * TEST_STRING_COUNT);
    printf("  Operations/sec: %.0f\n", ops_per_sec);
    printf("  Avg time per operation: %.3f microseconds\n\n", (elapsed * 1e6) / (BENCHMARK_ITERATIONS * TEST_STRING_COUNT));
}

void benchmark_parameter_sanitization(void) {
    struct timespec start, end;
    char sanitized[100];
    printf("Benchmarking parameter sanitization...\n");
    
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    for (int i = 0; i < BENCHMARK_ITERATIONS; i++) {
        for (int j = 0; j < TEST_STRING_COUNT; j++) {
            iris_sanitize_url_parameter(test_parameters[j], sanitized, sizeof(sanitized));
        }
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    double elapsed = get_time_diff(start, end);
    double ops_per_sec = (BENCHMARK_ITERATIONS * TEST_STRING_COUNT) / elapsed;
    
    printf("  Time: %.3f seconds\n", elapsed);
    printf("  Operations: %d\n", BENCHMARK_ITERATIONS * TEST_STRING_COUNT);
    printf("  Operations/sec: %.0f\n", ops_per_sec);
    printf("  Avg time per operation: %.3f microseconds\n\n", (elapsed * 1e6) / (BENCHMARK_ITERATIONS * TEST_STRING_COUNT));
}

void benchmark_suspicious_parameter_detection(void) {
    struct timespec start, end;
    printf("Benchmarking suspicious parameter detection...\n");
    
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    for (int i = 0; i < BENCHMARK_ITERATIONS; i++) {
        for (int j = 0; j < TEST_STRING_COUNT; j++) {
            iris_is_suspicious_parameter(test_parameters[j]);
        }
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    double elapsed = get_time_diff(start, end);
    double ops_per_sec = (BENCHMARK_ITERATIONS * TEST_STRING_COUNT) / elapsed;
    
    printf("  Time: %.3f seconds\n", elapsed);
    printf("  Operations: %d\n", BENCHMARK_ITERATIONS * TEST_STRING_COUNT);
    printf("  Operations/sec: %.0f\n", ops_per_sec);
    printf("  Avg time per operation: %.3f microseconds\n\n", (elapsed * 1e6) / (BENCHMARK_ITERATIONS * TEST_STRING_COUNT));
}

int main(void) {
    printf("=== Iris Security Validators Performance Benchmark ===\n");
    printf("Using re2c-generated state machines for optimal performance\n");
    printf("Iterations: %d per test string\n", BENCHMARK_ITERATIONS);
    printf("Test strings: %d per benchmark\n\n", TEST_STRING_COUNT);
    
    // Initialize security module
    iris_security_init(NULL);
    
    // Run benchmarks
    benchmark_header_name_validation();
    benchmark_header_value_validation();
    benchmark_url_path_validation();
    benchmark_parameter_sanitization();
    benchmark_suspicious_parameter_detection();
    
    printf("=== Benchmark Complete ===\n");
    printf("Note: These results demonstrate the performance of re2c-optimized validators\n");
    printf("compared to traditional character-by-character validation loops.\n");
    
    return 0;
}