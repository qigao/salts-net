#ifndef TURBO_WASM_H
#define TURBO_WASM_H

#include "platform.h"
#include <stdint.h>

#if defined(_WIN32)
  #if defined(TURBO_WASM_BUILD_SHARED)
    #define TURBO_WASM_API __declspec(dllexport)
  #else
    #define TURBO_WASM_API __declspec(dllimport)
  #endif
#elif defined(__GNUC__) || defined(__clang__)
  #define TURBO_WASM_API __attribute__((visibility("default")))
#else
  #define TURBO_WASM_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_wasm_vm_s turbo_wasm_vm_t;
typedef void *turbo_wasm_func_t;

typedef enum turbo_wasm_value_type_e {
  TURBO_WASM_VAL_NONE = 0,
  TURBO_WASM_VAL_I32 = 1,
  TURBO_WASM_VAL_I64 = 2,
  TURBO_WASM_VAL_F32 = 3,
  TURBO_WASM_VAL_F64 = 4
} turbo_wasm_value_type_t;

typedef struct turbo_wasm_config_s {
  uint32_t stack_size_bytes;
  int link_libc;
  void (*link_cb)(struct turbo_wasm_vm_s *vm, void *m3_module, void *userdata);
  void *link_userdata;
  /* Reserved for future ABI-safe extension. Keep zero. */
  uint32_t reserved;
} turbo_wasm_config_t;

TURBO_WASM_API turbo_wasm_config_t turbo_wasm_config_default(void);

TURBO_WASM_API turbo_wasm_vm_t *
turbo_wasm_vm_create(const turbo_wasm_config_t *cfg);
TURBO_WASM_API void turbo_wasm_vm_destroy(turbo_wasm_vm_t *vm);

TURBO_WASM_API int turbo_wasm_vm_load_bytes(turbo_wasm_vm_t *vm,
                                            const uint8_t *bytes, size_t len);
TURBO_WASM_API int turbo_wasm_vm_load_file(turbo_wasm_vm_t *vm,
                                           const char *path);

TURBO_WASM_API int turbo_wasm_vm_call_argv(turbo_wasm_vm_t *vm,
                                           const char *func_name, uint32_t argc,
                                           const char *argv[]);

TURBO_WASM_API turbo_wasm_func_t
turbo_wasm_vm_find_func(turbo_wasm_vm_t *vm, const char *func_name);

TURBO_WASM_API int turbo_wasm_vm_call_func(turbo_wasm_vm_t *vm,
                                           turbo_wasm_func_t func, uint32_t argc,
                                           const void *args[]);

TURBO_WASM_API turbo_wasm_value_type_t
turbo_wasm_vm_last_result_type(const turbo_wasm_vm_t *vm);
TURBO_WASM_API int turbo_wasm_vm_last_result_i64(const turbo_wasm_vm_t *vm,
                                                 int64_t *out_value);
TURBO_WASM_API int turbo_wasm_vm_last_result_f64(const turbo_wasm_vm_t *vm,
                                                 double *out_value);

TURBO_WASM_API const char *turbo_wasm_vm_last_error(const turbo_wasm_vm_t *vm);

TURBO_WASM_API int
turbo_wasm_vm_link_raw_func(turbo_wasm_vm_t *vm, void *m3_module,
                            const char *module_name, const char *function_name,
                            const char *signature, const void *raw_func,
                            void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_WASM_H */
