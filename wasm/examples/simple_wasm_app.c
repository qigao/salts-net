#include "turbo_wasm.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "fib32.wasm.h"

static int run_default_fib(void) {
  turbo_wasm_config_t cfg = turbo_wasm_config_default();
  turbo_wasm_vm_t *vm = turbo_wasm_vm_create(&cfg);
  const char *argv_call[1] = {"20"};
  int64_t out = 0;

  if (!vm) {
    fprintf(stderr, "turbo_wasm_vm_create failed\n");
    return 1;
  }

  if (turbo_wasm_vm_load_bytes(vm, fib32_wasm, fib32_wasm_len) != 0) {
    fprintf(stderr, "load default fib wasm failed: %s\n", turbo_wasm_vm_last_error(vm));
    turbo_wasm_vm_destroy(vm);
    return 1;
  }

  if (turbo_wasm_vm_call_argv(vm, "fib", 1, argv_call) != 0) {
    fprintf(stderr, "call fib failed: %s\n", turbo_wasm_vm_last_error(vm));
    turbo_wasm_vm_destroy(vm);
    return 1;
  }

  if (turbo_wasm_vm_last_result_i64(vm, &out) != 0) {
    fprintf(stderr, "result type is not integer\n");
    turbo_wasm_vm_destroy(vm);
    return 1;
  }

  printf("fib(20) = %" PRId64 "\n", out);
  turbo_wasm_vm_destroy(vm);
  return 0;
}

static int run_file(const char *path, const char *func, const char *arg) {
  turbo_wasm_config_t cfg = turbo_wasm_config_default();
  turbo_wasm_vm_t *vm = turbo_wasm_vm_create(&cfg);
  const char *argv_call[1] = {arg};
  turbo_wasm_value_type_t t;
  int64_t ires = 0;
  double fres = 0.0;

  if (!vm) {
    fprintf(stderr, "turbo_wasm_vm_create failed\n");
    return 1;
  }

  if (turbo_wasm_vm_load_file(vm, path) != 0) {
    fprintf(stderr, "load file failed: %s\n", turbo_wasm_vm_last_error(vm));
    turbo_wasm_vm_destroy(vm);
    return 1;
  }

  if (turbo_wasm_vm_call_argv(vm, func, 1, argv_call) != 0) {
    fprintf(stderr, "call %s failed: %s\n", func, turbo_wasm_vm_last_error(vm));
    turbo_wasm_vm_destroy(vm);
    return 1;
  }

  t = turbo_wasm_vm_last_result_type(vm);
  if (t == TURBO_WASM_VAL_I32 || t == TURBO_WASM_VAL_I64) {
    if (turbo_wasm_vm_last_result_i64(vm, &ires) == 0)
      printf("%s(%s) = %" PRId64 "\n", func, arg, ires);
  } else if (t == TURBO_WASM_VAL_F32 || t == TURBO_WASM_VAL_F64) {
    if (turbo_wasm_vm_last_result_f64(vm, &fres) == 0)
      printf("%s(%s) = %.12g\n", func, arg, fres);
  } else {
    printf("%s called, no return value\n", func);
  }

  turbo_wasm_vm_destroy(vm);
  return 0;
}

int main(int argc, char **argv) {
  printf("[turbo_wasm] mode=arena-only (internal arena managed by VM)\n");

  if (argc == 1)
    return run_default_fib();

  if (argc != 4) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s\n", argv[0]);
    fprintf(stderr, "  %s <wasm_file> <func_name> <arg0>\n", argv[0]);
    return 1;
  }

  return run_file(argv[1], argv[2], argv[3]);
}
