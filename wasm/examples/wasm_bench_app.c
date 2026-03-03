#include "turbo_wasm.h"

#include "coremark_minimal.wasm.h"
#include "fib32.wasm.h"
#include "platform.h"
#include "tinytest.h"
#include "turbo_thread.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#if defined(__SANITIZE_ADDRESS__) || defined(_DEBUG)
// ASan / Debug builds are typically slower
#define THRESH_CALL_ONLY_MS 5.00
#define THRESH_LOAD_EXEC_DESTROY_MS 5.00
#define THRESH_CONCURRENT_MS 2.50
#define THRESH_LARGE_LOAD_MS 1.50
#else
// Release builds
#define THRESH_CALL_ONLY_MS 1.00
#define THRESH_LOAD_EXEC_DESTROY_MS 1.10
#define THRESH_CONCURRENT_MS 0.35
#define THRESH_LARGE_LOAD_MS 0.30
#endif

typedef struct bench_metric_s {
  double per_ms;
  int64_t last_value;
  int64_t checksum;
  int ok_count;
} bench_metric_t;

static int run_call_only_bench(int iters, bench_metric_t *out) {
  turbo_wasm_config_t cfg = turbo_wasm_config_default();
  turbo_wasm_vm_t *vm;
  uint64_t t0;
  uint64_t t1;
  int i;
  int64_t last = 0;

  vm = turbo_wasm_vm_create(&cfg);
  if (!vm)
    return -1;
  if (turbo_wasm_vm_load_bytes(vm, fib32_wasm, fib32_wasm_len) != 0) {
    turbo_wasm_vm_destroy(vm);
    return -1;
  }

  turbo_wasm_func_t fib_func = turbo_wasm_vm_find_func(vm, "fib");
  if (!fib_func) {
    turbo_wasm_vm_destroy(vm);
    return -1;
  }

  t0 = turbo_monotonic_ms();
  for (i = 0; i < iters; ++i) {
    int32_t arg_val = 20;
    const void *raw_args[1] = {&arg_val};
    if (turbo_wasm_vm_call_func(vm, fib_func, 1, raw_args) != 0) {
      turbo_wasm_vm_destroy(vm);
      return -1;
    }
    if (turbo_wasm_vm_last_result_i64(vm, &last) != 0) {
      turbo_wasm_vm_destroy(vm);
      return -1;
    }
  }
  t1 = turbo_monotonic_ms();
  turbo_wasm_vm_destroy(vm);

  out->per_ms = (double)(t1 - t0) / (double)iters;
  out->last_value = last;
  return 0;
}

static int run_load_exec_destroy_bench(int iters, bench_metric_t *out) {
  uint64_t t0 = turbo_monotonic_ms();
  uint64_t t1;
  int i;
  int64_t last = 0;

  for (i = 0; i < iters; ++i) {
    turbo_wasm_config_t cfg = turbo_wasm_config_default();
    turbo_wasm_vm_t *vm;
    const char *argv_call[1] = {"20"};

    vm = turbo_wasm_vm_create(&cfg);
    if (!vm)
      return -1;
    if (turbo_wasm_vm_load_bytes(vm, fib32_wasm, fib32_wasm_len) != 0) {
      turbo_wasm_vm_destroy(vm);
      return -1;
    }
    if (turbo_wasm_vm_call_argv(vm, "fib", 1, argv_call) != 0) {
      turbo_wasm_vm_destroy(vm);
      return -1;
    }
    if (turbo_wasm_vm_last_result_i64(vm, &last) != 0) {
      turbo_wasm_vm_destroy(vm);
      return -1;
    }
    turbo_wasm_vm_destroy(vm);
  }

  t1 = turbo_monotonic_ms();
  out->per_ms = (double)(t1 - t0) / (double)iters;
  out->last_value = last;
  return 0;
}

typedef struct worker_arg_s {
  int iters;
  int fail;
  int64_t checksum;
} worker_arg_t;

static void bench_worker(void *argp) {
  worker_arg_t *arg = (worker_arg_t *)argp;
  int i;
  int64_t sum = 0;
  for (i = 0; i < arg->iters; ++i) {
    turbo_wasm_config_t cfg = turbo_wasm_config_default();
    turbo_wasm_vm_t *vm = turbo_wasm_vm_create(&cfg);
    const char *argv_call[1] = {"20"};
    int64_t out = 0;

    if (!vm) {
      arg->fail = 1;
      return;
    }
    if (turbo_wasm_vm_load_bytes(vm, fib32_wasm, fib32_wasm_len) != 0) {
      arg->fail = 1;
      turbo_wasm_vm_destroy(vm);
      return;
    }
    if (turbo_wasm_vm_call_argv(vm, "fib", 1, argv_call) != 0) {
      arg->fail = 1;
      turbo_wasm_vm_destroy(vm);
      return;
    }
    if (turbo_wasm_vm_last_result_i64(vm, &out) != 0) {
      arg->fail = 1;
      turbo_wasm_vm_destroy(vm);
      return;
    }
    sum += out;
    turbo_wasm_vm_destroy(vm);
  }
  arg->checksum = sum;
}

static int run_concurrent_create_destroy_bench(int threads, int iters_per_thread,
                                               bench_metric_t *out) {
  turbo_thread_t tids[32];
  worker_arg_t args[32];
  uint64_t t0;
  uint64_t t1;
  int i;
  int64_t checksum = 0;
  int total_ops;

  if (threads <= 0 || threads > 32)
    return -1;
  total_ops = threads * iters_per_thread;

  memset(tids, 0, sizeof(tids));
  memset(args, 0, sizeof(args));
  for (i = 0; i < threads; ++i)
    args[i].iters = iters_per_thread;

  t0 = turbo_monotonic_ms();
  for (i = 0; i < threads; ++i) {
    if (turbo_thread_create(&tids[i], bench_worker, &args[i]) != 0)
      return -1;
  }
  for (i = 0; i < threads; ++i)
    turbo_thread_join(&tids[i]);
  t1 = turbo_monotonic_ms();

  for (i = 0; i < threads; ++i) {
    if (args[i].fail)
      return -1;
    checksum += args[i].checksum;
  }

  out->per_ms = (double)(t1 - t0) / (double)total_ops;
  out->checksum = checksum;
  return 0;
}

static int write_wasm_file(const char *path, const uint8_t *bytes, size_t len) {
  FILE *f = fopen(path, "wb");
  size_t nw;
  if (!f)
    return -1;
  nw = fwrite(bytes, 1, len, f);
  fclose(f);
  return (nw == len) ? 0 : -1;
}

static int run_large_wasm_load_bench(const char *path, int iters, bench_metric_t *out) {
  uint64_t t0;
  uint64_t t1;
  int i;
  int ok = 0;

  t0 = turbo_monotonic_ms();
  for (i = 0; i < iters; ++i) {
    turbo_wasm_config_t cfg = turbo_wasm_config_default();
    turbo_wasm_vm_t *vm = turbo_wasm_vm_create(&cfg);
    if (!vm)
      return -1;
    if (turbo_wasm_vm_load_file(vm, path) != 0) {
      turbo_wasm_vm_destroy(vm);
      return -1;
    }
    ++ok;
    turbo_wasm_vm_destroy(vm);
  }
  t1 = turbo_monotonic_ms();
  out->per_ms = (double)(t1 - t0) / (double)iters;
  out->ok_count = ok;
  return 0;
}

suite("wasm bench regression") {
  before() { printf("[turbo_wasm] mode=arena-only (internal arena per VM)\n"); }

  bench("call_only should stay under threshold") {
    bench_metric_t m = {0};
    int rc = -1;
    benchmark("call_only.internal_arena", 1) { rc = run_call_only_bench(1000, &m); }
    check_int_eq(rc, 0);
    printf("[call_only.internal_arena] iters=1000 per_call=%.3fms last=%" PRId64 "\n",
           m.per_ms, m.last_value);
    check_float_le(m.per_ms, THRESH_CALL_ONLY_MS);
    check_int_eq((int)m.last_value, 6765);
  }

  bench("load_exec_destroy should stay under threshold") {
    bench_metric_t m = {0};
    int rc = -1;
    benchmark("load_exec_destroy.internal_arena", 1) {
      rc = run_load_exec_destroy_bench(300, &m);
    }
    check_int_eq(rc, 0);
    printf("[load_exec_destroy.internal_arena] iters=300 per_iter=%.3fms last=%" PRId64
           "\n",
           m.per_ms, m.last_value);
    check_float_le(m.per_ms, THRESH_LOAD_EXEC_DESTROY_MS);
    check_int_eq((int)m.last_value, 6765);
  }

  bench("concurrent create/destroy should stay under threshold") {
    bench_metric_t m = {0};
    int rc = -1;
    benchmark("concurrent.create_destroy", 1) {
      rc = run_concurrent_create_destroy_bench(4, 120, &m);
    }
    check_int_eq(rc, 0);
    printf("[concurrent] threads=4 iters/thread=120 total_ops=480 per_op=%.3fms checksum=%" PRId64
           "\n",
           m.per_ms, m.checksum);
    check_float_le(m.per_ms, THRESH_CONCURRENT_MS);
    check_true(m.checksum > 0);
  }

  bench("large wasm load should stay under threshold") {
    const char *large_wasm_file = "wasm_bench_coremark_tmp.wasm";
    bench_metric_t m = {0};
    int rc = write_wasm_file(large_wasm_file, coremark_minimal_wasm,
                             coremark_minimal_wasm_len);
    check_int_eq(rc, 0);
    benchmark("large_load.coremark", 1) { rc = run_large_wasm_load_bench(large_wasm_file, 200, &m); }
    check_int_eq(rc, 0);
    printf("[large_load] file=%s size=%uB iters=200 per_load=%.3fms ok=%d\n",
           large_wasm_file, (unsigned)coremark_minimal_wasm_len, m.per_ms, m.ok_count);
    check_float_le(m.per_ms, THRESH_LARGE_LOAD_MS);
    check_int_eq(m.ok_count, 200);
    remove(large_wasm_file);
  }
}
