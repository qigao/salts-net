/**
 * @file benchmark_json_parser.c
 * @brief Benchmark for JSON parser performance
 *
 * Tests parsing speed with various JSON sizes and structures.
 */

#include "json_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

typedef struct {
  const char *name;
  char *json;
  size_t json_len;
  int iterations;
} benchmark_t;

static char *generate_array_of_numbers(size_t count) {
  size_t buf_size = count * 12 + 3;
  char *buf = (char *)malloc(buf_size);
  if (!buf)
    return NULL;

  int pos = 0;
  pos += sprintf(buf + pos, "[");
  for (size_t i = 0; i < count; i++) {
    if (i > 0)
      pos += sprintf(buf + pos, ",");
    pos += sprintf(buf + pos, "%zu", i);
  }
  pos += sprintf(buf + pos, "]");
  return buf;
}

static char *generate_array_of_objects(size_t count) {
  size_t buf_size = count * 100 + 3;
  char *buf = (char *)malloc(buf_size);
  if (!buf)
    return NULL;

  int pos = 0;
  pos += sprintf(buf + pos, "[");
  for (size_t i = 0; i < count; i++) {
    if (i > 0)
      pos += sprintf(buf + pos, ",");
    pos +=
        sprintf(buf + pos, "{\"id\":%zu,\"name\":\"item_%zu\",\"value\":%zu.%zu,\"active\":true}",
                i, i, i * 10, i % 10);
  }
  pos += sprintf(buf + pos, "]");
  return buf;
}

static char *generate_nested_object(int depth) {
  size_t buf_size = depth * 50 + 100;
  char *buf = (char *)malloc(buf_size);
  if (!buf)
    return NULL;

  int pos = 0;
  for (int i = 0; i < depth; i++) {
    pos += sprintf(buf + pos, "{\"level%d\":", i);
  }
  pos += sprintf(buf + pos, "\"deep_value\"");
  for (int i = 0; i < depth; i++) {
    pos += sprintf(buf + pos, "}");
  }
  return buf;
}

static char *generate_string_heavy(size_t string_count, size_t string_len) {
  size_t buf_size = string_count * (string_len + 20) + 100;
  char *buf = (char *)malloc(buf_size);
  if (!buf)
    return NULL;

  char *value = (char *)malloc(string_len + 1);
  if (!value) {
    free(buf);
    return NULL;
  }
  memset(value, 'x', string_len);
  value[string_len] = '\0';

  int pos = 0;
  pos += sprintf(buf + pos, "{");
  for (size_t i = 0; i < string_count; i++) {
    if (i > 0)
      pos += sprintf(buf + pos, ",");
    pos += sprintf(buf + pos, "\"key%zu\":\"%s\"", i, value);
  }
  pos += sprintf(buf + pos, "}");

  free(value);
  return buf;
}

static char *generate_mqtt_proxy_config(size_t listeners, size_t upstreams, size_t filters) {
  size_t buf_size = (listeners + upstreams + filters) * 200 + 500;
  char *buf = (char *)malloc(buf_size);
  if (!buf)
    return NULL;

  int pos = 0;
  pos += sprintf(buf + pos, "{\"listeners\":[");
  for (size_t i = 0; i < listeners; i++) {
    if (i > 0)
      pos += sprintf(buf + pos, ",");
    pos +=
        sprintf(buf + pos, "{\"port\":%zu,\"transport\":\"tcp\",\"host\":\"0.0.0.0\"}", 1883 + i);
  }
  pos += sprintf(buf + pos, "],\"upstreams\":[");
  for (size_t i = 0; i < upstreams; i++) {
    if (i > 0)
      pos += sprintf(buf + pos, ",");
    pos += sprintf(buf + pos, "{\"host\":\"10.0.0.%zu\",\"port\":1883,\"weight\":%zu}", i + 1,
                   (i % 3) + 1);
  }
  pos += sprintf(buf + pos, "],\"filters\":[");
  for (size_t i = 0; i < filters; i++) {
    if (i > 0)
      pos += sprintf(buf + pos, ",");
    pos += sprintf(buf + pos, "{\"type\":\"topic\",\"action\":\"deny\",\"pattern\":\"$SYS/%zu/#\"}",
                   i);
  }
  pos += sprintf(buf + pos, "],\"settings\":{");
  pos += sprintf(buf + pos, "\"max_clients\":10000,");
  pos += sprintf(buf + pos, "\"connect_timeout_ms\":5000,");
  pos += sprintf(buf + pos, "\"hash_replicas\":150}}");

  return buf;
}

static void run_benchmark(benchmark_t *bench) {
  double start = get_time_ms();
  size_t total_bytes = 0;

  for (int i = 0; i < bench->iterations; i++) {
    json_value_t *v = json_parse(bench->json, bench->json_len);
    if (!v) {
      printf("  ERROR: Parse failed at iteration %d\n", i);
      return;
    }
    total_bytes += bench->json_len;
    json_free(v);
  }

  double elapsed = get_time_ms() - start;
  double ops_per_sec = bench->iterations / (elapsed / 1000.0);
  double mb_per_sec = (total_bytes / (1024.0 * 1024.0)) / (elapsed / 1000.0);

  printf("  %-30s %8zu bytes  %6d iter  %8.2f ms  %10.0f ops/s  %6.2f MB/s\n", bench->name,
         bench->json_len, bench->iterations, elapsed, ops_per_sec, mb_per_sec);
}

int main(int argc, char **argv) {
  int quick = (argc > 1 && strcmp(argv[1], "--quick") == 0);

  printf("JSON Parser Benchmark\n");
  printf("=====================\n\n");

  benchmark_t benchmarks[] = {
      // Small JSON
      {"tiny object", "{\"a\":1}", 0, quick ? 10000 : 100000},
      {"small object", "{\"name\":\"test\",\"value\":42,\"active\":true}", 0,
       quick ? 10000 : 100000},

      // Arrays
      {"array 100 numbers", NULL, 0, quick ? 1000 : 10000},
      {"array 1000 numbers", NULL, 0, quick ? 100 : 1000},
      {"array 10000 numbers", NULL, 0, quick ? 10 : 100},

      // Objects in arrays
      {"array 100 objects", NULL, 0, quick ? 1000 : 10000},
      {"array 1000 objects", NULL, 0, quick ? 100 : 1000},

      // Nested
      {"nested depth 10", NULL, 0, quick ? 10000 : 100000},
      {"nested depth 50", NULL, 0, quick ? 10000 : 50000},
      {"nested depth 100", NULL, 0, quick ? 5000 : 20000},

      // String heavy
      {"100 strings x 100 chars", NULL, 0, quick ? 1000 : 10000},
      {"100 strings x 1000 chars", NULL, 0, quick ? 100 : 1000},

      // Real-world: MQTT proxy config
      {"mqtt config small", NULL, 0, quick ? 10000 : 100000},
      {"mqtt config medium", NULL, 0, quick ? 1000 : 10000},
      {"mqtt config large", NULL, 0, quick ? 100 : 1000},
  };

  // Generate dynamic JSON
  benchmarks[2].json = generate_array_of_numbers(100);
  benchmarks[3].json = generate_array_of_numbers(1000);
  benchmarks[4].json = generate_array_of_numbers(10000);
  benchmarks[5].json = generate_array_of_objects(100);
  benchmarks[6].json = generate_array_of_objects(1000);
  benchmarks[7].json = generate_nested_object(10);
  benchmarks[8].json = generate_nested_object(50);
  benchmarks[9].json = generate_nested_object(100);
  benchmarks[10].json = generate_string_heavy(100, 100);
  benchmarks[11].json = generate_string_heavy(100, 1000);
  benchmarks[12].json = generate_mqtt_proxy_config(2, 3, 5);
  benchmarks[13].json = generate_mqtt_proxy_config(10, 20, 50);
  benchmarks[14].json = generate_mqtt_proxy_config(50, 100, 200);

  // Calculate lengths
  size_t num_benchmarks = sizeof(benchmarks) / sizeof(benchmarks[0]);
  for (size_t i = 0; i < num_benchmarks; i++) {
    if (benchmarks[i].json) {
      benchmarks[i].json_len = strlen(benchmarks[i].json);
    }
  }

  // Warmup
  printf("Warming up...\n");
  for (int i = 0; i < 1000; i++) {
    json_value_t *v = json_parse("{\"x\":1}", 7);
    json_free(v);
  }

  printf("\nResults (DOM Mode):\n");
  printf("  %-30s %14s  %10s  %12s  %14s  %10s\n", "Test", "Size", "Iterations", "Time",
         "Throughput", "Bandwidth");
  printf("  %s\n", "-------------------------------------------------------------------------------"
                   "-------------");

  for (size_t i = 0; i < num_benchmarks; i++) {
    run_benchmark(&benchmarks[i]);
  }

  // SAX benchmarks
  printf("\n  SAX/Stream Mode:\n");
  printf("  %s\n", "-------------------------------------------------------------------------------"
                   "-------------");

  json_sax_handler_t null_handler = {0}; // All NULL callbacks - just parse

  for (size_t i = 0; i < num_benchmarks; i++) {
    benchmark_t *bench = &benchmarks[i];
    double start = get_time_ms();
    size_t total_bytes = 0;

    for (int j = 0; j < bench->iterations; j++) {
      if (json_parse_sax(bench->json, bench->json_len, &null_handler, NULL) != 0) {
        printf("  ERROR: SAX parse failed\n");
        break;
      }
      total_bytes += bench->json_len;
    }

    double elapsed = get_time_ms() - start;
    double ops_per_sec = bench->iterations / (elapsed / 1000.0);
    double mb_per_sec = (total_bytes / (1024.0 * 1024.0)) / (elapsed / 1000.0);

    printf("  %-30s %8zu bytes  %6d iter  %8.2f ms  %10.0f ops/s  %6.2f MB/s\n", bench->name,
           bench->json_len, bench->iterations, elapsed, ops_per_sec, mb_per_sec);
  }

  // Cleanup dynamic JSON
  for (size_t i = 2; i < num_benchmarks; i++) {
    free(benchmarks[i].json);
  }

  printf("\nDone.\n");
  return 0;
}
