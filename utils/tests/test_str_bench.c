/**
 * @file test_str_bench.c
 * @brief Micro-benchmarks for tstr_t and tstr_v (tinytest)
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tinytest.h"
#include "turbo_str.h"
#include "turbo_str_view.h"

#define BENCH_ITERS 200000
#define BENCH_ITERS_LONG 100000

static volatile size_t sink_size = 0;
static volatile int sink_int = 0;

spec("String Bench") {
  const char *sample = "The quick brown fox jumps over the lazy dog";
  const char *sample_upper = "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG";
  const char *sample_long =
      "Lorem ipsum dolor sit amet, consectetur adipiscing elit. "
      "Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. "
      "Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris.";

  bench("tstr (owned)") {

      benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
    benchmark("dup", BENCH_ITERS, 1) {
      tstr_t s = tstr_dup(sample);
      sink_size += tstr_len(s);
      tstr_free(s);
    }

    benchmark("cat", BENCH_ITERS, 1) {
      tstr_t s = tstr_new();
      s = tstr_cat(s, "hello");
      s = tstr_cat(s, " world");
      sink_size += tstr_len(s);
      tstr_free(s);
    }

    benchmark("cat_fmt", BENCH_ITERS_LONG, 1) {
      tstr_t s = tstr_new();
      s = tstr_cat_fmt(s, "num=%d str=%s", 12345, "test");
      sink_size += tstr_len(s);
      tstr_free(s);
    }

    benchmark("cmp", BENCH_ITERS, 1) {
      tstr_t a = tstr_dup(sample);
      tstr_t b = tstr_dup(sample);
      sink_int += tstr_cmp(a, b);
      tstr_free(a);
      tstr_free(b);
    }

    benchmark("casecmp", BENCH_ITERS, 1) {
      sink_int += tstr_casecmp(sample, sample_upper);
    }

    benchmark("starts_with", BENCH_ITERS, 1) {
      sink_int += tstr_starts_with(sample, "The quick");
    }

    benchmark("ends_with", BENCH_ITERS, 1) {
      sink_int += tstr_ends_with(sample, "lazy dog");
    }

    benchmark("contains", BENCH_ITERS, 1) {
      sink_int += tstr_contains(sample, "brown fox");
    }

    benchmark("trim", BENCH_ITERS, 1) {
      tstr_t s = tstr_dup("   hello world   ");
      s = tstr_trim(s, " ");
      sink_size += tstr_len(s);
      tstr_free(s);
    }

    benchmark("split", BENCH_ITERS_LONG, 1) {
      tstr_t s = tstr_dup("alpha,beta,gamma,delta,epsilon,zeta");
      int count = 0;
      tstr_t *parts = tstr_split(s, ",", &count);
      sink_int += count;
      tstr_free_split(parts, count);
      tstr_free(s);
    }
  }

  bench("tstr_v (view)") {

      benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
    tstr_v sv = tstr_v_from_cstr(sample);
    tstr_v sv_long = tstr_v_from_cstr(sample_long);
    tstr_v needle = tstr_v_from_cstr("brown fox");
    tstr_v needle_long = tstr_v_from_cstr("exercitation");

    benchmark("eq", BENCH_ITERS, 1) {
      sink_int += tstr_v_eq(sv, sv);
    }

    benchmark("ieq", BENCH_ITERS, 1) {
      tstr_v upper = tstr_v_from_cstr(sample_upper);
      sink_int += tstr_v_ieq(sv, upper);
    }

    benchmark("find(short)", BENCH_ITERS, 1) {
      sink_size += tstr_v_find(sv, needle);
    }

    benchmark("find(long)", BENCH_ITERS, 1) {
      sink_size += tstr_v_find(sv_long, needle_long);
    }

    benchmark("find_char", BENCH_ITERS, 1) {
      sink_size += tstr_v_find_char(sv_long, 'x');
    }

    benchmark("rfind", BENCH_ITERS, 1) {
      sink_size += tstr_v_rfind(sv_long, needle_long);
    }

    benchmark("rfind_char", BENCH_ITERS, 1) {
      sink_size += tstr_v_rfind_char(sv_long, 'e');
    }

    benchmark("contains", BENCH_ITERS, 1) {
      sink_int += tstr_v_contains(sv, needle);
    }

    benchmark("starts_with", BENCH_ITERS, 1) {
      sink_int += tstr_v_starts_with(sv, tstr_v_from_cstr("The quick"));
    }

    benchmark("ends_with", BENCH_ITERS, 1) {
      sink_int += tstr_v_ends_with(sv, tstr_v_from_cstr("lazy dog"));
    }

    benchmark("trim", BENCH_ITERS, 1) {
      tstr_v padded = tstr_v_from_cstr("   hello world   ");
      tstr_v trimmed = tstr_v_trim(padded, " ");
      sink_size += trimmed.len;
    }

    benchmark("split", BENCH_ITERS_LONG, 1) {
      tstr_v csv = tstr_v_from_cstr("alpha,beta,gamma,delta,epsilon,zeta");
      tstr_v delim = tstr_v_from_cstr(",");
      tstr_v part;
      int count = 0;
      while ((part = tstr_v_split_next(&csv, delim)).data || csv.len > 0) {
        count++;
        if (!part.data) break;
      }
      sink_int += count;
    }
  }

  bench("tstr vs tstr_v") {

      benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
    tstr_t ts = tstr_dup(sample_long);
    tstr_v sv_long = tstr_v_from_cstr(sample_long);
    tstr_v needle_v = tstr_v_from_cstr("exercitation");

    benchmark("tstr:contains", BENCH_ITERS, 1) {
      sink_int += tstr_contains(sample_long, "exercitation");
    }

    benchmark("tstr:contains_v", BENCH_ITERS, 1) {
      sink_int += tstr_contains_v(ts, needle_v);
    }

    benchmark("view:contains", BENCH_ITERS, 1) {
      sink_int += tstr_v_contains(sv_long, needle_v);
    }

    benchmark("tstr:find_v", BENCH_ITERS, 1) {
      sink_size += tstr_find_v(ts, needle_v);
    }

    benchmark("view:find", BENCH_ITERS, 1) {
      sink_size += tstr_v_find(sv_long, needle_v);
    }

    benchmark("tstr:split", BENCH_ITERS_LONG, 1) {
      tstr_t s = tstr_dup("alpha,beta,gamma,delta,epsilon,zeta");
      int count = 0;
      tstr_t *parts = tstr_split(s, ",", &count);
      sink_int += count;
      tstr_free_split(parts, count);
      tstr_free(s);
    }

    benchmark("view:split", BENCH_ITERS_LONG, 1) {
      tstr_v csv = tstr_v_from_cstr("alpha,beta,gamma,delta,epsilon,zeta");
      tstr_v delim = tstr_v_from_cstr(",");
      tstr_v part;
      int count = 0;
      while ((part = tstr_v_split_next(&csv, delim)).data || csv.len > 0) {
        count++;
        if (!part.data) break;
      }
      sink_int += count;
    }

    tstr_free(ts);
  }
}
