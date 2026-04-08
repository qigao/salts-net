/**
 * @file benchmark_query.c
 * @brief Benchmark for json_object_get() query performance
 *
 * Tests the real-world impact of O(n) linear search in json_object_get()
 * to determine if hash table optimization is worth the complexity.
 */

#include "json_parser.h"
#include <fmt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
static double get_time_ms(void) {
  LARGE_INTEGER freq, counter;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&counter);
  return (double)counter.QuadPart * 1000.0 / (double)freq.QuadPart;
}
#else
  #include <sys/time.h>
static double get_time_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}
#endif

// Generate JSON object with N keys
static char *generate_object_with_keys(size_t num_keys) {
  size_t buf_size = num_keys * 50 + 100;
  char *buf = (char *)malloc(buf_size);
  if (!buf)
    return NULL;

  int pos = 0;
  pos += fmt(buf + pos, buf_size - (size_t)pos, "{{");
  for (size_t i = 0; i < num_keys; i++) {
    if (i > 0)
      pos += fmt(buf + pos, buf_size - (size_t)pos, ",");
    pos += fmt(buf + pos, buf_size - (size_t)pos, "\"key_{}\":{}", i, i);
  }
  pos += fmt(buf + pos, buf_size - (size_t)pos, "}}");
  return buf;
}

// Generate MQTT proxy config with configurable sizes
static char *generate_mqtt_config(size_t listeners, size_t upstreams, size_t filters) {
  size_t buf_size = (listeners + upstreams + filters) * 200 + 500;
  char *buf = (char *)malloc(buf_size);
  if (!buf)
    return NULL;

  int pos = 0;
  pos += fmt(buf + pos, buf_size - (size_t)pos, "{{\"listeners\":[");
  for (size_t i = 0; i < listeners; i++) {
    if (i > 0)
      pos += fmt(buf + pos, buf_size - (size_t)pos, ",");
    pos += fmt(buf + pos, buf_size - (size_t)pos,
               "{{\"port\":{},\"transport\":\"tcp\",\"host\":\"0.0.0.0\"}}", 1883 + i);
  }
  pos += fmt(buf + pos, buf_size - (size_t)pos, "],\"upstreams\":[");
  for (size_t i = 0; i < upstreams; i++) {
    if (i > 0)
      pos += fmt(buf + pos, buf_size - (size_t)pos, ",");
    pos += fmt(buf + pos, buf_size - (size_t)pos,
               "{{\"host\":\"10.0.0.{}\",\"port\":1883,\"weight\":{}}}", i + 1, (i % 3) + 1);
  }
  pos += fmt(buf + pos, buf_size - (size_t)pos, "],\"filters\":[");
  for (size_t i = 0; i < filters; i++) {
    if (i > 0)
      pos += fmt(buf + pos, buf_size - (size_t)pos, ",");
    pos += fmt(buf + pos, buf_size - (size_t)pos,
               "{{\"type\":\"topic\",\"action\":\"deny\",\"pattern\":\"$SYS/{}/#\"}}", i);
  }
  pos += fmt(buf + pos, buf_size - (size_t)pos, "],\"max_clients\":10000,");
  pos += fmt(buf + pos, buf_size - (size_t)pos, "\"connect_timeout_ms\":5000,");
  pos += fmt(buf + pos, buf_size - (size_t)pos, "\"keepalive_sec\":60,");
  pos += fmt(buf + pos, buf_size - (size_t)pos, "\"max_packet_size\":268435456,");
  pos += fmt(buf + pos, buf_size - (size_t)pos, "\"hash_replicas\":150}}");

  return buf;
}

typedef struct {
  const char *name;
  json_value_t *obj;
  size_t num_keys;
} query_test_t;

// Test 1: Sequential key access (best case - keys at start)
static void test_sequential_access(query_test_t *test, int iterations) {
  double start = get_time_ms();
  size_t total_queries = 0;

  for (int i = 0; i < iterations; i++) {
    // Access first 10 keys
    for (size_t k = 0; k < 10 && k < test->num_keys; k++) {
      char key[32];
      fmt(key, sizeof(key), "key_{}", k);
      json_value_t *val = json_object_get(test->obj, key);
      (void)val;
      total_queries++;
    }
  }

  double elapsed = get_time_ms() - start;
  double queries_per_sec = total_queries / (elapsed / 1000.0);

  printf("    Sequential (first 10):   %8.2f ms  %12.0f queries/s\n", elapsed, queries_per_sec);
}

// Test 2: Random key access (average case)
static void test_random_access(query_test_t *test, int iterations) {
  double start = get_time_ms();
  size_t total_queries = 0;

  // Pre-generate random indices
  size_t *indices = (size_t *)malloc(iterations * 10 * sizeof(size_t));
  for (int i = 0; i < iterations * 10; i++) {
    indices[i] = rand() % test->num_keys;
  }

  start = get_time_ms();
  for (int i = 0; i < iterations; i++) {
    for (int k = 0; k < 10; k++) {
      char key[32];
      fmt(key, sizeof(key), "key_{}", indices[i * 10 + k]);
      json_value_t *val = json_object_get(test->obj, key);
      (void)val;
      total_queries++;
    }
  }

  double elapsed = get_time_ms() - start;
  double queries_per_sec = total_queries / (elapsed / 1000.0);

  printf("    Random (10 per iter):    %8.2f ms  %12.0f queries/s\n", elapsed, queries_per_sec);
  free(indices);
}

// Test 3: Worst case - always access last key
static void test_worst_case(query_test_t *test, int iterations) {
  double start = get_time_ms();
  size_t total_queries = 0;

  char last_key[32];
  fmt(last_key, sizeof(last_key), "key_{}", test->num_keys - 1);

  for (int i = 0; i < iterations; i++) {
    for (int k = 0; k < 10; k++) {
      json_value_t *val = json_object_get(test->obj, last_key);
      (void)val;
      total_queries++;
    }
  }

  double elapsed = get_time_ms() - start;
  double queries_per_sec = total_queries / (elapsed / 1000.0);

  printf("    Worst case (last key):   %8.2f ms  %12.0f queries/s\n", elapsed, queries_per_sec);
}

// Test 4: MQTT config realistic access pattern
static void test_mqtt_pattern(json_value_t *config, int iterations) {
  const char *common_keys[] = {"max_clients", "connect_timeout_ms", "keepalive_sec",
                                "max_packet_size", "hash_replicas", "listeners", "upstreams",
                                "filters"};
  size_t num_common = sizeof(common_keys) / sizeof(common_keys[0]);

  double start = get_time_ms();
  size_t total_queries = 0;

  for (int i = 0; i < iterations; i++) {
    for (size_t k = 0; k < num_common; k++) {
      json_value_t *val = json_object_get(config, common_keys[k]);
      (void)val;
      total_queries++;
    }
  }

  double elapsed = get_time_ms() - start;
  double queries_per_sec = total_queries / (elapsed / 1000.0);

  printf("    MQTT realistic pattern:  %8.2f ms  %12.0f queries/s\n", elapsed, queries_per_sec);
}

// Test 5: Parse + Query combined (real-world scenario)
static void test_parse_and_query(const char *json_str, size_t json_len, int iterations) {
  double start = get_time_ms();
  size_t total_queries = 0;

  for (int i = 0; i < iterations; i++) {
    json_value_t *obj = json_parse(json_str, json_len);
    if (!obj) {
      printf("Parse failed\n");
      return;
    }

    // Simulate real usage: parse, then query a few keys
    json_object_get(obj, "max_clients");
    json_object_get(obj, "connect_timeout_ms");
    json_object_get(obj, "listeners");
    total_queries += 3;

    json_free(obj);
  }

  double elapsed = get_time_ms() - start;
  double ops_per_sec = iterations / (elapsed / 1000.0);

  printf("    Parse+Query combined:    %8.2f ms  %12.0f ops/s (%.2f us per parse+query)\n",
         elapsed, ops_per_sec, elapsed * 1000.0 / iterations);
}

int main(int argc, char **argv) {
  int quick = (argc > 1 && strcmp(argv[1], "--quick") == 0);
  srand(42); // Deterministic random

  printf("JSON Query Benchmark\n");
  printf("====================\n\n");
  printf("Purpose: Measure json_object_get() performance to determine if O(n)\n");
  printf("         linear search is a real bottleneck worth optimizing.\n\n");

  // Test configurations
  size_t key_counts[] = {10, 50, 100, 500, 1000};
  int base_iterations = quick ? 10000 : 100000;

  printf("Test 1: Pure Query Performance (parse once, query many times)\n");
  printf("----------------------------------------------------------------\n");

  for (size_t i = 0; i < sizeof(key_counts) / sizeof(key_counts[0]); i++) {
    size_t num_keys = key_counts[i];
    char *json_str = generate_object_with_keys(num_keys);
    if (!json_str)
      continue;

    size_t json_len = strlen(json_str);
    json_value_t *obj = json_parse(json_str, json_len);
    if (!obj) {
      free(json_str);
      continue;
    }

    // Scale iterations based on object size
    int iterations = base_iterations / (num_keys / 10 + 1);

    query_test_t test = {.name = "simple object", .obj = obj, .num_keys = num_keys};

    printf("  Object with %zu keys (%d iterations):\n", num_keys, iterations);
    test_sequential_access(&test, iterations);
    test_random_access(&test, iterations);
    test_worst_case(&test, iterations);
    printf("\n");

    json_free(obj);
    free(json_str);
  }

  printf("Test 2: MQTT Config Realistic Scenarios\n");
  printf("----------------------------------------------------------------\n");

  struct {
    const char *name;
    size_t listeners;
    size_t upstreams;
    size_t filters;
  } mqtt_configs[] = {
      {"Small (2/3/5)", 2, 3, 5}, {"Medium (10/20/50)", 10, 20, 50}, {"Large (50/100/200)", 50, 100, 200}};

  for (size_t i = 0; i < sizeof(mqtt_configs) / sizeof(mqtt_configs[0]); i++) {
    char *json_str =
        generate_mqtt_config(mqtt_configs[i].listeners, mqtt_configs[i].upstreams, mqtt_configs[i].filters);
    if (!json_str)
      continue;

    size_t json_len = strlen(json_str);
    json_value_t *config = json_parse(json_str, json_len);
    if (!config) {
      free(json_str);
      continue;
    }

    size_t total_keys = json_object_size(config);
    printf("  %s - Total keys: %zu\n", mqtt_configs[i].name, total_keys);
    test_mqtt_pattern(config, quick ? 10000 : 100000);
    printf("\n");

    json_free(config);
    free(json_str);
  }

  printf("Test 3: Parse + Query Combined (Real-world usage)\n");
  printf("----------------------------------------------------------------\n");
  printf("  Simulates: Load config -> Query a few keys -> Discard\n\n");

  for (size_t i = 0; i < sizeof(mqtt_configs) / sizeof(mqtt_configs[0]); i++) {
    char *json_str =
        generate_mqtt_config(mqtt_configs[i].listeners, mqtt_configs[i].upstreams, mqtt_configs[i].filters);
    if (!json_str)
      continue;

    size_t json_len = strlen(json_str);
    printf("  %s:\n", mqtt_configs[i].name);
    test_parse_and_query(json_str, json_len, quick ? 1000 : 10000);
    printf("\n");

    free(json_str);
  }

  printf("Test 4: Query Cost as Percentage of Total Time\n");
  printf("----------------------------------------------------------------\n");
  printf("  Measures: Is query time significant compared to parse time?\n\n");

  char *json_str = generate_mqtt_config(50, 100, 200);
  size_t json_len = strlen(json_str);

  // Measure parse-only
  double start = get_time_ms();
  for (int i = 0; i < (quick ? 100 : 1000); i++) {
    json_value_t *obj = json_parse(json_str, json_len);
    json_free(obj);
  }
  double parse_only = get_time_ms() - start;

  // Measure parse + 10 queries
  start = get_time_ms();
  for (int i = 0; i < (quick ? 100 : 1000); i++) {
    json_value_t *obj = json_parse(json_str, json_len);
    json_object_get(obj, "max_clients");
    json_object_get(obj, "connect_timeout_ms");
    json_object_get(obj, "keepalive_sec");
    json_object_get(obj, "max_packet_size");
    json_object_get(obj, "hash_replicas");
    json_object_get(obj, "listeners");
    json_object_get(obj, "upstreams");
    json_object_get(obj, "filters");
    json_object_get(obj, "max_clients"); // repeat
    json_object_get(obj, "listeners");   // repeat
    json_free(obj);
  }
  double parse_and_query = get_time_ms() - start;

  double query_time = parse_and_query - parse_only;
  double query_percentage = (query_time / parse_and_query) * 100.0;

  printf("  Large MQTT config (355 keys):\n");
  printf("    Parse only:              %8.2f ms\n", parse_only);
  printf("    Parse + 10 queries:      %8.2f ms\n", parse_and_query);
  printf("    Query overhead:          %8.2f ms (%.1f%% of total)\n", query_time, query_percentage);
  printf("\n");

  free(json_str);

  printf("Conclusion:\n");
  printf("----------------------------------------------------------------\n");
  printf("If query overhead is < 5%%, hash table optimization is NOT worth it.\n");
  printf("If query overhead is > 20%%, consider optimization.\n");
  printf("\nNote: This measures worst-case O(n) search. Most real-world configs\n");
  printf("      have frequently-accessed keys at the beginning, reducing impact.\n");

  return 0;
}
