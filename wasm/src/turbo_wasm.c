#include "turbo_wasm.h"

#include "turbo_buffer.h"
#include "m3_api_libc.h"
#include "m3_core.h"
#include "turbo_str_view.h"
#include "wasm3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct turbo_wasm_vm_s {
  turbo_wasm_config_t cfg;
  turbo_pool_t owned_arena;
  IM3Environment env;
  IM3Runtime runtime;
  uint8_t *wasm_bytes;
  size_t wasm_len;
  char last_error[256];
  turbo_wasm_value_type_t last_type;
  union {
    int64_t i64;
    double f64;
  } last_value;
} turbo_wasm_vm_t;

static void wasm_set_err(turbo_wasm_vm_t *vm, const char *msg) {
  if (!vm)
    return;
  if (!msg)
    msg = "unknown wasm error";
  snprintf(vm->last_error, sizeof(vm->last_error), "%s", msg);
}

static void wasm_clear_err(turbo_wasm_vm_t *vm) {
  if (!vm)
    return;
  vm->last_error[0] = '\0';
}

static void wasm_release_bytes(turbo_wasm_vm_t *vm) {
  if (!vm)
    return;
  vm->wasm_bytes = NULL;
  vm->wasm_len = 0;
}

static int wasm_set_bytes_copy(turbo_wasm_vm_t *vm, const uint8_t *bytes, size_t len) {
  uint8_t *dst;

  if (!vm || !bytes || len == 0)
    return -1;

  dst = (uint8_t *)turbo_pool_alloc(&vm->owned_arena, len);
  if (!dst) {
    wasm_set_err(vm, "out of memory");
    return -1;
  }

  memcpy(dst, bytes, len);
  wasm_release_bytes(vm);
  vm->wasm_bytes = dst;
  vm->wasm_len = len;
  return 0;
}

static void wasm_set_bytes_take(turbo_wasm_vm_t *vm, uint8_t *bytes, size_t len) {
  wasm_release_bytes(vm);
  vm->wasm_bytes = bytes;
  vm->wasm_len = len;
}

static IM3Runtime wasm_make_runtime(turbo_wasm_vm_t *vm) {
  uint32_t stack_size = vm->cfg.stack_size_bytes ? vm->cfg.stack_size_bytes : 65536U;
  m3_SetThreadArena(&vm->owned_arena);
  IM3Runtime rt = m3_NewRuntime(vm->env, stack_size, vm);
  m3_SetThreadArena(NULL);
  return rt;
}

static void wasm_reset_runtime(turbo_wasm_vm_t *vm) {
  if (vm->runtime) {
    m3_SetThreadArena(&vm->owned_arena);
    m3_FreeRuntime(vm->runtime);
    m3_SetThreadArena(NULL);
    vm->runtime = NULL;
  }
  vm->runtime = wasm_make_runtime(vm);
}

static int wasm_load_into_runtime(turbo_wasm_vm_t *vm) {
  IM3Module module = NULL;
  M3Result r;

  if (!vm || !vm->runtime || !vm->wasm_bytes || vm->wasm_len == 0) {
    wasm_set_err(vm, "invalid vm state");
    return -1;
  }

  m3_SetThreadArena(&vm->owned_arena);
  r = m3_ParseModule(vm->env, &module, vm->wasm_bytes, (uint32_t)vm->wasm_len);
  m3_SetThreadArena(NULL);
  if (r) {
    wasm_set_err(vm, r);
    return -1;
  }

  m3_SetThreadArena(&vm->owned_arena);
  r = m3_LoadModule(vm->runtime, module);
  m3_SetThreadArena(NULL);
  if (r) {
    wasm_set_err(vm, r);
    m3_SetThreadArena(&vm->owned_arena);
    m3_FreeModule(module);
    m3_SetThreadArena(NULL);
    return -1;
  }

  if (vm->cfg.link_libc) {
    m3_SetThreadArena(&vm->owned_arena);
    r = m3_LinkLibC(module);
    m3_SetThreadArena(NULL);
    if (r) {
      wasm_set_err(vm, r);
      return -1;
    }
  }

  if (vm->cfg.link_cb) {
    vm->cfg.link_cb(vm, module, vm->cfg.link_userdata);
  }

  return 0;
}

turbo_wasm_config_t turbo_wasm_config_default(void) {
  turbo_wasm_config_t cfg;
  cfg.stack_size_bytes = 64U * 1024U;
  cfg.link_libc = 1;
  cfg.link_cb = NULL;
  cfg.link_userdata = NULL;
  cfg.reserved = 0;
  return cfg;
}

turbo_wasm_vm_t *turbo_wasm_vm_create(const turbo_wasm_config_t *cfg) {
  turbo_wasm_vm_t *vm = (turbo_wasm_vm_t *)calloc(1, sizeof(*vm));
  if (!vm)
    return NULL;

  vm->cfg = cfg ? *cfg : turbo_wasm_config_default();
  if (turbo_pool_init(&vm->owned_arena, 1U << 20) != 0) {
    wasm_set_err(vm, "failed to initialize owned arena");
    free(vm);
    return NULL;
  }
  vm->last_type = TURBO_WASM_VAL_NONE;

  m3_SetThreadArena(&vm->owned_arena);
  vm->env = m3_NewEnvironment();
  m3_SetThreadArena(NULL);
  if (!vm->env) {
    wasm_set_err(vm, "m3_NewEnvironment failed");
    turbo_wasm_vm_destroy(vm);
    return NULL;
  }

  vm->runtime = wasm_make_runtime(vm);
  if (!vm->runtime) {
    wasm_set_err(vm, "m3_NewRuntime failed");
    turbo_wasm_vm_destroy(vm);
    return NULL;
  }

  return vm;
}

void turbo_wasm_vm_destroy(turbo_wasm_vm_t *vm) {
  if (!vm)
    return;

  if (vm->runtime) {
    m3_SetThreadArena(&vm->owned_arena);
    m3_FreeRuntime(vm->runtime);
    m3_SetThreadArena(NULL);
  }

  if (vm->env) {
    m3_SetThreadArena(&vm->owned_arena);
    m3_FreeEnvironment(vm->env);
    m3_SetThreadArena(NULL);
  }

  wasm_release_bytes(vm);
  turbo_pool_free(&vm->owned_arena);
  free(vm);
}

int turbo_wasm_vm_load_bytes(turbo_wasm_vm_t *vm, const uint8_t *bytes, size_t len) {
  if (!vm || !bytes || len == 0) {
    wasm_set_err(vm, "invalid load args");
    return -1;
  }
  if (wasm_set_bytes_copy(vm, bytes, len) != 0)
    return -1;
  vm->last_type = TURBO_WASM_VAL_NONE;
  wasm_clear_err(vm);

  wasm_reset_runtime(vm);
  if (!vm->runtime) {
    wasm_set_err(vm, "m3_NewRuntime failed");
    return -1;
  }

  return wasm_load_into_runtime(vm);
}

int turbo_wasm_vm_load_file(turbo_wasm_vm_t *vm, const char *path) {
  FILE *fp;
  tstr_v path_view;
  const char *path_cstr;
  long file_size;
  uint8_t *buf;
  size_t nread;

  if (!vm || !path) {
    wasm_set_err(vm, "invalid load_file args");
    return -1;
  }
  path_view = tstr_v_from_cstr(path);
  if (tstr_v_empty(path_view)) {
    wasm_set_err(vm, "empty wasm path");
    return -1;
  }

  path_cstr = path;
  {
    char *arena_path = tstr_v_to_arena(path_view, &vm->owned_arena);
    if (!arena_path) {
      wasm_set_err(vm, "out of memory");
      return -1;
    }
    path_cstr = arena_path;
  }

  fp = fopen(path_cstr, "rb");
  if (!fp) {
    wasm_set_err(vm, "failed to open wasm file");
    return -1;
  }

  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    wasm_set_err(vm, "failed to seek wasm file");
    return -1;
  }
  file_size = ftell(fp);
  if (file_size <= 0) {
    fclose(fp);
    wasm_set_err(vm, "empty wasm file");
    return -1;
  }
  if (fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    wasm_set_err(vm, "failed to rewind wasm file");
    return -1;
  }

  buf = (uint8_t *)turbo_pool_alloc(&vm->owned_arena, (size_t)file_size);
  if (!buf) {
    fclose(fp);
    wasm_set_err(vm, "out of memory");
    return -1;
  }

  nread = fread(buf, 1, (size_t)file_size, fp);
  fclose(fp);
  if (nread != (size_t)file_size) {
    wasm_set_err(vm, "failed to read wasm file");
    return -1;
  }

  wasm_set_bytes_take(vm, buf, nread);
  vm->last_type = TURBO_WASM_VAL_NONE;
  wasm_clear_err(vm);

  wasm_reset_runtime(vm);
  if (!vm->runtime) {
    wasm_set_err(vm, "m3_NewRuntime failed");
    return -1;
  }
  return wasm_load_into_runtime(vm);
}

static int get_call_results(turbo_wasm_vm_t *vm, IM3Function f) {
  uint32_t retc;
  M3ValueType rt;
  M3Result r;

  retc = m3_GetRetCount(f);
  if (retc == 0) {
    vm->last_type = TURBO_WASM_VAL_NONE;
    return 0;
  }

  if (retc != 1) {
    wasm_set_err(vm, "only single return value is supported");
    return -1;
  }

  rt = m3_GetRetType(f, 0);
  switch (rt) {
  case c_m3Type_i32: {
    int32_t v = 0;
    const void *outv[1] = {&v};
    r = m3_GetResults(f, 1, outv);
    if (r) {
      wasm_set_err(vm, r);
      return -1;
    }
    vm->last_type = TURBO_WASM_VAL_I32;
    vm->last_value.i64 = (int64_t)v;
    return 0;
  }
  case c_m3Type_i64: {
    int64_t v = 0;
    const void *outv[1] = {&v};
    r = m3_GetResults(f, 1, outv);
    if (r) {
      wasm_set_err(vm, r);
      return -1;
    }
    vm->last_type = TURBO_WASM_VAL_I64;
    vm->last_value.i64 = v;
    return 0;
  }
  case c_m3Type_f32: {
    float v = 0.0f;
    const void *outv[1] = {&v};
    r = m3_GetResults(f, 1, outv);
    if (r) {
      wasm_set_err(vm, r);
      return -1;
    }
    vm->last_type = TURBO_WASM_VAL_F32;
    vm->last_value.f64 = (double)v;
    return 0;
  }
  case c_m3Type_f64: {
    double v = 0.0;
    const void *outv[1] = {&v};
    r = m3_GetResults(f, 1, outv);
    if (r) {
      wasm_set_err(vm, r);
      return -1;
    }
    vm->last_type = TURBO_WASM_VAL_F64;
    vm->last_value.f64 = v;
    return 0;
  }
  default:
    wasm_set_err(vm, "unsupported return type");
    return -1;
  }
}

int turbo_wasm_vm_call_argv(turbo_wasm_vm_t *vm, const char *func_name, uint32_t argc,
                            const char *argv[]) {
  IM3Function f = NULL;
  M3Result r;
  uint32_t retc;
  M3ValueType rt;

  if (!vm || !vm->runtime || !func_name) {
    wasm_set_err(vm, "invalid call args");
    return -1;
  }

  wasm_clear_err(vm);
  vm->last_type = TURBO_WASM_VAL_NONE;

  m3_SetThreadArena(&vm->owned_arena);
  r = m3_FindFunction(&f, vm->runtime, func_name);
  m3_SetThreadArena(NULL);
  if (r) {
    wasm_set_err(vm, r);
    return -1;
  }

  m3_SetThreadArena(&vm->owned_arena);
  r = m3_CallArgv(f, argc, argv);
  m3_SetThreadArena(NULL);
  if (r) {
    wasm_set_err(vm, r);
    return -1;
  }

  return get_call_results(vm, f);
}

turbo_wasm_func_t turbo_wasm_vm_find_func(turbo_wasm_vm_t *vm, const char *func_name) {
  IM3Function f = NULL;
  M3Result r;
  if (!vm || !vm->runtime || !func_name) {
    wasm_set_err(vm, "invalid find_func args");
    return NULL;
  }
  wasm_clear_err(vm);
  m3_SetThreadArena(&vm->owned_arena);
  r = m3_FindFunction(&f, vm->runtime, func_name);
  m3_SetThreadArena(NULL);
  if (r) {
    wasm_set_err(vm, r);
    return NULL;
  }
  return (turbo_wasm_func_t)f;
}

int turbo_wasm_vm_call_func(turbo_wasm_vm_t *vm, turbo_wasm_func_t func, uint32_t argc, const void *args[]) {
  M3Result r;
  IM3Function f = (IM3Function)func;

  if (!vm || !f) {
    wasm_set_err(vm, "invalid call args");
    return -1;
  }

  wasm_clear_err(vm);
  vm->last_type = TURBO_WASM_VAL_NONE;

  m3_SetThreadArena(&vm->owned_arena);
  r = m3_Call(f, argc, args);
  m3_SetThreadArena(NULL);

  if (r) {
    wasm_set_err(vm, r);
    return -1;
  }

  return get_call_results(vm, f);
}

turbo_wasm_value_type_t turbo_wasm_vm_last_result_type(const turbo_wasm_vm_t *vm) {
  if (!vm)
    return TURBO_WASM_VAL_NONE;
  return vm->last_type;
}

int turbo_wasm_vm_last_result_i64(const turbo_wasm_vm_t *vm, int64_t *out_value) {
  if (!vm || !out_value)
    return -1;
  if (vm->last_type != TURBO_WASM_VAL_I32 && vm->last_type != TURBO_WASM_VAL_I64)
    return -1;
  *out_value = vm->last_value.i64;
  return 0;
}

int turbo_wasm_vm_last_result_f64(const turbo_wasm_vm_t *vm, double *out_value) {
  if (!vm || !out_value)
    return -1;
  if (vm->last_type != TURBO_WASM_VAL_F32 && vm->last_type != TURBO_WASM_VAL_F64)
    return -1;
  *out_value = vm->last_value.f64;
  return 0;
}

const char *turbo_wasm_vm_last_error(const turbo_wasm_vm_t *vm) {
  if (!vm || vm->last_error[0] == '\0')
    return NULL;
  return vm->last_error;
}

int turbo_wasm_vm_link_raw_func(turbo_wasm_vm_t *vm, void *m3_module,
                                const char *module_name, const char *function_name,
                                const char *signature, const void *raw_func,
                                void *userdata) {
  M3Result r;
  if (!vm || !m3_module || !module_name || !function_name || !signature || !raw_func) {
    wasm_set_err(vm, "invalid link function arguments");
    return -1;
  }

  m3_SetThreadArena(&vm->owned_arena);
  r = m3_LinkRawFunctionEx((IM3Module)m3_module, module_name, function_name,
                           signature, (M3RawCall)raw_func, userdata);
  m3_SetThreadArena(NULL);

  if (r) {
    char err_buf[256];
    snprintf(err_buf, sizeof(err_buf), "link %s.%s failed: %s", module_name, function_name, r);
    wasm_set_err(vm, err_buf);
    return -1;
  }
  return 0;
}
