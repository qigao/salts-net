#include "turbo_wasm3.h"

#include "CoroNet/turbo_coro_socket.h"

#if defined(_MSC_VER)
#define _Static_assert(...)
#define __attribute__(...)
#define _Noreturn
#endif

#include "extra/wasi_core.h"
#include "http_client.h"
#include "redis_client.h"
#include "turbo_error.h"
#include "turbo_fs.h"
#include "turbo_str.h"
#include "sqlite3.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct turbo_wasm3_socket_entry_s {
  uint32_t handle;
  coro_socket_t *socket;
} turbo_wasm3_socket_entry_t;

typedef struct turbo_wasm3_blob_s {
  uint8_t *bytes;
  uint32_t size;
} turbo_wasm3_blob_t;

typedef struct turbo_wasm3_db_entry_s {
  uint32_t handle;
  void *db;
  char *last_error;
} turbo_wasm3_db_entry_t;

typedef struct turbo_wasm3_db_stmt_entry_s {
  uint32_t handle;
  uint32_t db_handle;
  void *stmt;
  char *last_error;
} turbo_wasm3_db_stmt_entry_t;

typedef struct turbo_wasm3_http_client_entry_s {
  uint32_t handle;
  http_client_t *client;
  char *last_error;
} turbo_wasm3_http_client_entry_t;

typedef struct turbo_wasm3_http_response_entry_s {
  uint32_t handle;
  uint32_t client_handle;
  http_response_t *response;
  char *last_error;
  turbo_wasm3_blob_t *chunks;
  size_t chunk_count;
  size_t chunk_capacity;
  int is_stream;
  int is_sse;
} turbo_wasm3_http_response_entry_t;

typedef struct turbo_wasm3_redis_client_entry_s {
  uint32_t handle;
  coro_context_t *ctx;
  redis_client_t *client;
  char *last_error;
} turbo_wasm3_redis_client_entry_t;

typedef struct turbo_wasm3_redis_reply_entry_s {
  uint32_t handle;
  uint32_t client_handle;
  redis_reply_t *reply;
} turbo_wasm3_redis_reply_entry_t;

typedef struct turbo_wasm3_host_linker_entry_s {
  turbo_wasm3_host_linker_fn linker;
  void *user_data;
} turbo_wasm3_host_linker_entry_t;

typedef struct turbo_wasm3_parser_entry_s {
  uint32_t handle;
  int type;
  void *doc;
} turbo_wasm3_parser_entry_t;

struct turbo_wasm3_socket_registry_s {
  uint32_t next_handle;
  size_t capacity;
  size_t count;
  turbo_wasm3_socket_entry_t *entries;
};

struct turbo_wasm3_db_registry_s {
  uint32_t next_handle;
  uint32_t next_stmt_handle;
  size_t capacity;
  size_t count;
  turbo_wasm3_db_entry_t *entries;
  size_t stmt_capacity;
  size_t stmt_count;
  turbo_wasm3_db_stmt_entry_t *stmt_entries;
  const turbo_wasm3_db_ops_t *ops;
  void *user_data;
};

struct turbo_wasm3_http_registry_s {
  uint32_t next_client_handle;
  uint32_t next_response_handle;
  size_t client_capacity;
  size_t client_count;
  turbo_wasm3_http_client_entry_t *clients;
  size_t response_capacity;
  size_t response_count;
  turbo_wasm3_http_response_entry_t *responses;
};

struct turbo_wasm3_redis_registry_s {
  uint32_t next_client_handle;
  uint32_t next_reply_handle;
  size_t client_capacity;
  size_t client_count;
  turbo_wasm3_redis_client_entry_t *clients;
  size_t reply_capacity;
  size_t reply_count;
  turbo_wasm3_redis_reply_entry_t *replies;
};

struct turbo_wasm3_parser_registry_s {
  uint32_t next_handle;
  size_t capacity;
  size_t count;
  turbo_wasm3_parser_entry_t *entries;
};

struct turbo_wasm3_vm_s {
  IM3Environment env;
  IM3Runtime runtime;
  m3_wasi_context_t *wasi_context;
  turbo_wasm3_socket_registry_t *socket_registry;
  turbo_wasm3_db_registry_t *db_registry;
  turbo_wasm3_http_registry_t *http_registry;
  turbo_wasm3_redis_registry_t *redis_registry;
  turbo_wasm3_parser_registry_t *parser_registry;
  void *host_user_data;
  char **wasi_argv;
  uint32_t wasi_argc;
  turbo_wasm3_blob_t *blobs;
  size_t blob_count;
  size_t blob_capacity;
  turbo_wasm3_host_linker_entry_t *host_linkers;
  size_t host_linker_count;
  size_t host_linker_capacity;
};

typedef struct turbo_wasm3_redis_command_capture_s {
  redis_reply_t *reply;
} turbo_wasm3_redis_command_capture_t;

typedef struct turbo_wasm3_redis_open_task_state_s {
  const char *host;
  uint16_t port;
  coro_socket_t *socket;
  int rc;
  int done;
} turbo_wasm3_redis_open_task_state_t;

typedef struct turbo_wasm3_redis_command_task_state_s {
  turbo_wasm3_redis_client_entry_t *entry;
  uint32_t argc;
  const char *const *argv;
  const uint32_t *argv_lens;
  redis_reply_t *reply;
  int rc;
  int done;
} turbo_wasm3_redis_command_task_state_t;

static char *turbo_wasm3_strdup(const char *value) {
  size_t len;
  char *copy;

  if (!value) {
    return NULL;
  }

  len = strlen(value) + 1;
  copy = (char *)malloc(len);
  if (!copy) {
    return NULL;
  }

  memcpy(copy, value, len);
  return copy;
}

static void turbo_wasm3_vm_clear_wasi_args(turbo_wasm3_vm_t *vm) {
  uint32_t i;

  if (!vm) {
    return;
  }

  if (vm->wasi_argv) {
    for (i = 0; i < vm->wasi_argc; ++i) {
      free(vm->wasi_argv[i]);
    }
    free(vm->wasi_argv);
  }

  vm->wasi_argv = NULL;
  vm->wasi_argc = 0;

  if (vm->wasi_context) {
    vm->wasi_context->argc = 0;
    vm->wasi_context->argv = NULL;
  }
}

static uint16_t turbo_wasm3_error_to_wasi(int err) {
  switch (err) {
  case 0: return __WASI_ERRNO_SUCCESS;
  case TURBO_EBADF: return __WASI_ERRNO_BADF;
  case TURBO_ECONNABORTED: return __WASI_ERRNO_CONNABORTED;
  case TURBO_ECONNREFUSED: return __WASI_ERRNO_CONNREFUSED;
  case TURBO_ECONNRESET: return __WASI_ERRNO_CONNRESET;
  case TURBO_EINTR: return __WASI_ERRNO_INTR;
  case TURBO_EINVAL: return __WASI_ERRNO_INVAL;
  case TURBO_EIO: return __WASI_ERRNO_IO;
  case TURBO_EISCONN: return __WASI_ERRNO_ISCONN;
  case TURBO_EMSGSIZE: return __WASI_ERRNO_MSGSIZE;
  case TURBO_ENOBUFS: return __WASI_ERRNO_NOBUFS;
  case TURBO_ENOMEM: return __WASI_ERRNO_NOMEM;
  case TURBO_ENOSYS: return __WASI_ERRNO_NOSYS;
  case TURBO_ENOTCONN: return __WASI_ERRNO_NOTCONN;
  case TURBO_ENOTSOCK: return __WASI_ERRNO_NOTSOCK;
  case TURBO_ENOTSUP: return __WASI_ERRNO_NOTSUP;
  case TURBO_EPIPE: return __WASI_ERRNO_PIPE;
  case TURBO_ESHUTDOWN: return __WASI_ERRNO_NOTCONN;
  case TURBO_ETIMEDOUT: return __WASI_ERRNO_TIMEDOUT;
  default: return __WASI_ERRNO_IO;
  }
}

static turbo_wasm3_socket_entry_t *
turbo_wasm3_socket_registry_find_entry(turbo_wasm3_socket_registry_t *registry,
                                       uint32_t wasi_fd) {
  size_t i;

  if (!registry) {
    return NULL;
  }

  for (i = 0; i < registry->count; ++i) {
    if (registry->entries[i].handle == wasi_fd) {
      return &registry->entries[i];
    }
  }

  return NULL;
}

static turbo_wasm3_db_entry_t *
turbo_wasm3_db_registry_find_entry(turbo_wasm3_db_registry_t *registry,
                                   uint32_t db_handle) {
  size_t i;

  if (!registry) {
    return NULL;
  }

  for (i = 0; i < registry->count; ++i) {
    if (registry->entries[i].handle == db_handle) {
      return &registry->entries[i];
    }
  }

  return NULL;
}

static turbo_wasm3_db_stmt_entry_t *
turbo_wasm3_db_registry_find_stmt_entry(turbo_wasm3_db_registry_t *registry,
                                        uint32_t stmt_handle) {
  size_t i;

  if (!registry) {
    return NULL;
  }

  for (i = 0; i < registry->stmt_count; ++i) {
    if (registry->stmt_entries[i].handle == stmt_handle) {
      return &registry->stmt_entries[i];
    }
  }

  return NULL;
}

static turbo_wasm3_redis_client_entry_t *
turbo_wasm3_redis_registry_find_client(turbo_wasm3_redis_registry_t *registry,
                                       uint32_t client_handle) {
  size_t i;

  if (!registry) {
    return NULL;
  }

  for (i = 0; i < registry->client_count; ++i) {
    if (registry->clients[i].handle == client_handle) {
      return &registry->clients[i];
    }
  }

  return NULL;
}

static turbo_wasm3_redis_reply_entry_t *
turbo_wasm3_redis_registry_find_reply(turbo_wasm3_redis_registry_t *registry,
                                      uint32_t reply_handle) {
  size_t i;

  if (!registry) {
    return NULL;
  }

  for (i = 0; i < registry->reply_count; ++i) {
    if (registry->replies[i].handle == reply_handle) {
      return &registry->replies[i];
    }
  }

  return NULL;
}

static int
turbo_wasm3_socket_registry_reserve(turbo_wasm3_socket_registry_t *registry,
                                    size_t required) {
  turbo_wasm3_socket_entry_t *entries;
  size_t new_capacity;

  if (!registry) {
    return TURBO_EINVAL;
  }
  if (required <= registry->capacity) {
    return 0;
  }

  new_capacity = registry->capacity ? registry->capacity : 16;
  while (new_capacity < required) {
    new_capacity *= 2;
  }

  entries = (turbo_wasm3_socket_entry_t *)realloc(
      registry->entries, new_capacity * sizeof(*entries));
  if (!entries) {
    return TURBO_ENOMEM;
  }

  registry->entries = entries;
  registry->capacity = new_capacity;
  return 0;
}

static int turbo_wasm3_db_registry_reserve(turbo_wasm3_db_registry_t *registry,
                                           size_t required) {
  turbo_wasm3_db_entry_t *entries;
  size_t new_capacity;

  if (!registry) {
    return TURBO_EINVAL;
  }
  if (required <= registry->capacity) {
    return 0;
  }

  new_capacity = registry->capacity ? registry->capacity : 4;
  while (new_capacity < required) {
    new_capacity *= 2;
  }

  entries = (turbo_wasm3_db_entry_t *)realloc(
      registry->entries, new_capacity * sizeof(*entries));
  if (!entries) {
    return TURBO_ENOMEM;
  }

  registry->entries = entries;
  registry->capacity = new_capacity;
  return 0;
}

static int
turbo_wasm3_db_registry_reserve_statements(turbo_wasm3_db_registry_t *registry,
                                           size_t required) {
  turbo_wasm3_db_stmt_entry_t *entries;
  size_t new_capacity;

  if (!registry) {
    return TURBO_EINVAL;
  }
  if (required <= registry->stmt_capacity) {
    return 0;
  }

  new_capacity = registry->stmt_capacity ? registry->stmt_capacity : 4;
  while (new_capacity < required) {
    new_capacity *= 2;
  }

  entries = (turbo_wasm3_db_stmt_entry_t *)realloc(
      registry->stmt_entries, new_capacity * sizeof(*entries));
  if (!entries) {
    return TURBO_ENOMEM;
  }

  registry->stmt_entries = entries;
  registry->stmt_capacity = new_capacity;
  return 0;
}

static int turbo_wasm3_redis_registry_reserve_clients(
    turbo_wasm3_redis_registry_t *registry, size_t required) {
  turbo_wasm3_redis_client_entry_t *entries;
  size_t new_capacity;

  if (!registry) {
    return TURBO_EINVAL;
  }
  if (required <= registry->client_capacity) {
    return 0;
  }

  new_capacity = registry->client_capacity ? registry->client_capacity : 4;
  while (new_capacity < required) {
    new_capacity *= 2;
  }

  entries = (turbo_wasm3_redis_client_entry_t *)realloc(
      registry->clients, new_capacity * sizeof(*entries));
  if (!entries) {
    return TURBO_ENOMEM;
  }

  registry->clients = entries;
  registry->client_capacity = new_capacity;
  return 0;
}

static int turbo_wasm3_redis_registry_reserve_replies(
    turbo_wasm3_redis_registry_t *registry, size_t required) {
  turbo_wasm3_redis_reply_entry_t *entries;
  size_t new_capacity;

  if (!registry) {
    return TURBO_EINVAL;
  }
  if (required <= registry->reply_capacity) {
    return 0;
  }

  new_capacity = registry->reply_capacity ? registry->reply_capacity : 4;
  while (new_capacity < required) {
    new_capacity *= 2;
  }

  entries = (turbo_wasm3_redis_reply_entry_t *)realloc(
      registry->replies, new_capacity * sizeof(*entries));
  if (!entries) {
    return TURBO_ENOMEM;
  }

  registry->replies = entries;
  registry->reply_capacity = new_capacity;
  return 0;
}

static int turbo_wasm3_vm_reserve_blobs(turbo_wasm3_vm_t *vm, size_t required) {
  turbo_wasm3_blob_t *blobs;
  size_t new_capacity;

  if (!vm) {
    return TURBO_EINVAL;
  }
  if (required <= vm->blob_capacity) {
    return 0;
  }

  new_capacity = vm->blob_capacity ? vm->blob_capacity : 4;
  while (new_capacity < required) {
    new_capacity *= 2;
  }

  blobs = (turbo_wasm3_blob_t *)realloc(vm->blobs, new_capacity * sizeof(*blobs));
  if (!blobs) {
    return TURBO_ENOMEM;
  }

  vm->blobs = blobs;
  vm->blob_capacity = new_capacity;
  return 0;
}

static int
turbo_wasm3_vm_reserve_host_linkers(turbo_wasm3_vm_t *vm, size_t required) {
  turbo_wasm3_host_linker_entry_t *entries;
  size_t new_capacity;

  if (!vm) {
    return TURBO_EINVAL;
  }
  if (required <= vm->host_linker_capacity) {
    return 0;
  }

  new_capacity = vm->host_linker_capacity ? vm->host_linker_capacity : 4;
  while (new_capacity < required) {
    new_capacity *= 2;
  }

  entries = (turbo_wasm3_host_linker_entry_t *)realloc(
      vm->host_linkers, new_capacity * sizeof(*entries));
  if (!entries) {
    return TURBO_ENOMEM;
  }

  vm->host_linkers = entries;
  vm->host_linker_capacity = new_capacity;
  return 0;
}

static M3Result turbo_wasm3_vm_store_blob(turbo_wasm3_vm_t *vm,
                                          const uint8_t *wasm_bytes,
                                          uint32_t wasm_size,
                                          const uint8_t **stored_bytes) {
  uint8_t *copy;
  int rc;

  if (!vm || !wasm_bytes || !wasm_size || !stored_bytes) {
    return m3Err_wasmMalformed;
  }

  rc = turbo_wasm3_vm_reserve_blobs(vm, vm->blob_count + 1);
  if (rc != 0) {
    return m3Err_mallocFailed;
  }

  copy = (uint8_t *)malloc(wasm_size);
  if (!copy) {
    return m3Err_mallocFailed;
  }

  memcpy(copy, wasm_bytes, wasm_size);
  vm->blobs[vm->blob_count].bytes = copy;
  vm->blobs[vm->blob_count].size = wasm_size;
  *stored_bytes = copy;
  vm->blob_count++;
  return m3Err_none;
}

static M3Result turbo_wasm3_vm_bind_wasi(turbo_wasm3_vm_t *vm) {
  if (!vm) {
    return m3Err_wasmMalformed;
  }

  if (!vm->wasi_context) {
    return "wasm3 wasi context unavailable";
  }

  if (vm->socket_registry) {
    int rc = turbo_wasm3_socket_registry_bind_wasi(vm->socket_registry,
                                                   vm->wasi_context);
    if (rc != 0) {
      return "failed to bind TurboNet socket registry";
    }
  }

  return m3Err_none;
}

static M3Result turbo_wasm3_suppress_lookup_failure(M3Result result) {
  if (result == m3Err_functionLookupFailed) {
    return m3Err_none;
  }

  return result;
}

/* Parser host function forward declarations */
m3ApiRawFunction(turbo_wasm3_host_parser_json_parse);
m3ApiRawFunction(turbo_wasm3_host_parser_csv_parse);
m3ApiRawFunction(turbo_wasm3_host_parser_xml_parse);
m3ApiRawFunction(turbo_wasm3_host_parser_ini_parse);
m3ApiRawFunction(turbo_wasm3_host_parser_free);
m3ApiRawFunction(turbo_wasm3_host_json_get_string);
m3ApiRawFunction(turbo_wasm3_host_json_get_int);
m3ApiRawFunction(turbo_wasm3_host_json_get_bool);
m3ApiRawFunction(turbo_wasm3_host_json_array_size);
m3ApiRawFunction(turbo_wasm3_host_csv_row_count);
m3ApiRawFunction(turbo_wasm3_host_csv_column_count);
m3ApiRawFunction(turbo_wasm3_host_csv_get_cell);
m3ApiRawFunction(turbo_wasm3_host_csv_find_column);
m3ApiRawFunction(turbo_wasm3_host_xml_root_name);
m3ApiRawFunction(turbo_wasm3_host_xml_get_text);
m3ApiRawFunction(turbo_wasm3_host_xml_count);
m3ApiRawFunction(turbo_wasm3_host_ini_get_string);
m3ApiRawFunction(turbo_wasm3_host_ini_get_int);
m3ApiRawFunction(turbo_wasm3_host_ini_get_bool);
m3ApiRawFunction(turbo_wasm3_host_ini_get_double);

/* Parser registry forward declarations */
turbo_wasm3_parser_registry_t *turbo_wasm3_parser_registry_create(size_t initial_capacity);
void turbo_wasm3_parser_registry_destroy(turbo_wasm3_parser_registry_t *registry);
turbo_wasm3_redis_registry_t *turbo_wasm3_redis_registry_create(size_t initial_capacity);
void turbo_wasm3_redis_registry_destroy(turbo_wasm3_redis_registry_t *registry);

static char *turbo_wasm3_copy_guest_bytes(const uint8_t *data, uint32_t len) {
  char *copy;

  if (!data && len != 0) {
    return NULL;
  }

  copy = (char *)malloc((size_t)len + 1);
  if (!copy) {
    return NULL;
  }

  if (len != 0) {
    memcpy(copy, data, len);
  }
  copy[len] = '\0';
  return copy;
}

static void turbo_wasm3_error_slot_clear(char **slot) {
  if (!slot) {
    return;
  }

  free(*slot);
  *slot = NULL;
}

static int turbo_wasm3_error_slot_set(char **slot, const char *message,
                                      uint32_t message_len) {
  char *copy;

  if (!slot) {
    return TURBO_EINVAL;
  }

  turbo_wasm3_error_slot_clear(slot);
  if (!message || message_len == 0) {
    return 0;
  }

  copy = (char *)malloc((size_t)message_len + 1);
  if (!copy) {
    return TURBO_ENOMEM;
  }

  memcpy(copy, message, message_len);
  copy[message_len] = '\0';
  *slot = copy;
  return 0;
}

static int turbo_wasm3_error_buffer_write(const char *message, char *buffer,
                                          size_t buffer_size,
                                          uint32_t *out_len) {
  size_t len;

  if (!buffer || buffer_size == 0 || !out_len) {
    return TURBO_EINVAL;
  }

  if (!message) {
    buffer[0] = '\0';
    *out_len = 0;
    return 0;
  }

  len = strlen(message);
  if (len >= buffer_size) {
    len = buffer_size - 1;
  }
  memcpy(buffer, message, len);
  buffer[len] = '\0';
  *out_len = (uint32_t)len;
  return 0;
}

static void turbo_wasm3_db_entry_clear_error(turbo_wasm3_db_entry_t *entry) {
  if (!entry) {
    return;
  }

  turbo_wasm3_error_slot_clear(&entry->last_error);
}

static int turbo_wasm3_db_entry_set_error(turbo_wasm3_db_entry_t *entry,
                                          const char *message,
                                          uint32_t message_len) {
  if (!entry) {
    return TURBO_EINVAL;
  }

  return turbo_wasm3_error_slot_set(&entry->last_error, message, message_len);
}

static void turbo_wasm3_db_stmt_entry_clear_error(
    turbo_wasm3_db_stmt_entry_t *entry) {
  if (!entry) {
    return;
  }

  turbo_wasm3_error_slot_clear(&entry->last_error);
}

static int turbo_wasm3_db_stmt_entry_set_error(
    turbo_wasm3_db_stmt_entry_t *entry, const char *message,
    uint32_t message_len) {
  if (!entry) {
    return TURBO_EINVAL;
  }

  return turbo_wasm3_error_slot_set(&entry->last_error, message, message_len);
}

static void turbo_wasm3_db_entry_capture_provider_error(
    turbo_wasm3_db_registry_t *registry, turbo_wasm3_db_entry_t *entry) {
  char buffer[256];
  uint32_t out_len = 0;

  if (!registry || !entry || !registry->ops || !registry->ops->error) {
    return;
  }

  if (registry->ops->error(registry->user_data, entry->db, buffer, sizeof(buffer),
                           &out_len) != 0) {
    return;
  }

  turbo_wasm3_db_entry_set_error(entry, buffer, out_len);
}

static void turbo_wasm3_db_stmt_entry_capture_provider_error(
    turbo_wasm3_db_registry_t *registry, turbo_wasm3_db_stmt_entry_t *stmt_entry,
    turbo_wasm3_db_entry_t *db_entry) {
  if (!stmt_entry) {
    return;
  }

  if (db_entry) {
    turbo_wasm3_db_entry_capture_provider_error(registry, db_entry);
    if (db_entry->last_error) {
      turbo_wasm3_db_stmt_entry_set_error(stmt_entry, db_entry->last_error,
                                          (uint32_t)strlen(db_entry->last_error));
      return;
    }
  }

  turbo_wasm3_db_stmt_entry_clear_error(stmt_entry);
}

static void turbo_wasm3_http_client_entry_clear_error(
    turbo_wasm3_http_client_entry_t *entry) {
  if (!entry) {
    return;
  }

  turbo_wasm3_error_slot_clear(&entry->last_error);
}

static int turbo_wasm3_http_client_entry_set_error(
    turbo_wasm3_http_client_entry_t *entry, const char *message,
    uint32_t message_len) {
  if (!entry) {
    return TURBO_EINVAL;
  }

  return turbo_wasm3_error_slot_set(&entry->last_error, message, message_len);
}

static void turbo_wasm3_http_response_entry_clear_error(
    turbo_wasm3_http_response_entry_t *entry) {
  if (!entry) {
    return;
  }

  turbo_wasm3_error_slot_clear(&entry->last_error);
}

static int turbo_wasm3_http_response_entry_set_error(
    turbo_wasm3_http_response_entry_t *entry, const char *message,
    uint32_t message_len) {
  if (!entry) {
    return TURBO_EINVAL;
  }

  return turbo_wasm3_error_slot_set(&entry->last_error, message, message_len);
}

static void turbo_wasm3_http_response_entry_clear_chunks(
    turbo_wasm3_http_response_entry_t *entry) {
  size_t i;

  if (!entry) {
    return;
  }

  for (i = 0; i < entry->chunk_count; ++i) {
    free(entry->chunks[i].bytes);
  }
  free(entry->chunks);
  entry->chunks = NULL;
  entry->chunk_count = 0;
  entry->chunk_capacity = 0;
  entry->is_stream = 0;
  entry->is_sse = 0;
}

static void turbo_wasm3_http_response_entry_reset(
    turbo_wasm3_http_response_entry_t *entry) {
  if (!entry) {
    return;
  }

  turbo_wasm3_http_response_entry_clear_error(entry);
  turbo_wasm3_http_response_entry_clear_chunks(entry);
  if (entry->response) {
    http_response_free(entry->response);
  }
  memset(entry, 0, sizeof(*entry));
}

static int turbo_wasm3_http_copy_buffer(const void *src, size_t src_len, void *buffer,
                                        size_t buffer_size, uint32_t *out_len) {
  if (!buffer || buffer_size == 0 || !out_len) {
    return TURBO_EINVAL;
  }

  if (!src || src_len == 0) {
    *out_len = 0;
    return 0;
  }

  if (src_len > buffer_size) {
    memcpy(buffer, src, buffer_size);
    *out_len = (uint32_t)buffer_size;
    return TURBO_EMSGSIZE;
  }

  memcpy(buffer, src, src_len);
  *out_len = (uint32_t)src_len;
  return 0;
}

static void turbo_wasm3_redis_client_entry_clear_error(
    turbo_wasm3_redis_client_entry_t *entry) {
  if (!entry) {
    return;
  }

  turbo_wasm3_error_slot_clear(&entry->last_error);
}

static int turbo_wasm3_redis_client_entry_set_error(
    turbo_wasm3_redis_client_entry_t *entry, const char *message,
    uint32_t message_len) {
  if (!entry) {
    return TURBO_EINVAL;
  }

  return turbo_wasm3_error_slot_set(&entry->last_error, message, message_len);
}

static redis_reply_t *turbo_wasm3_redis_reply_clone(const redis_reply_t *reply) {
  redis_reply_t *copy;
  size_t i;

  if (!reply) {
    return NULL;
  }

  copy = (redis_reply_t *)calloc(1, sizeof(*copy));
  if (!copy) {
    return NULL;
  }

  copy->type = reply->type;
  copy->integer = reply->integer;
  copy->len = reply->len;
  copy->element_count = reply->element_count;

  if (reply->str && reply->len != 0) {
    copy->str = tstr_dup_len(reply->str, reply->len);
    if (!copy->str) {
      free(copy);
      return NULL;
    }
  } else if (reply->str) {
    copy->str = tstr_dup("");
    if (!copy->str) {
      free(copy);
      return NULL;
    }
  }

  if (reply->element_count != 0) {
    copy->elements = (redis_reply_t **)calloc(reply->element_count,
                                              sizeof(*copy->elements));
    if (!copy->elements) {
      redis_reply_free(copy);
      return NULL;
    }

    for (i = 0; i < reply->element_count; ++i) {
      copy->elements[i] = turbo_wasm3_redis_reply_clone(reply->elements[i]);
      if (!copy->elements[i]) {
        redis_reply_free(copy);
        return NULL;
      }
    }
  }

  return copy;
}

static void turbo_wasm3_redis_reply_entry_reset(
    turbo_wasm3_redis_reply_entry_t *entry) {
  if (!entry) {
    return;
  }

  if (entry->reply) {
    redis_reply_free(entry->reply);
  }
  memset(entry, 0, sizeof(*entry));
}

static void turbo_wasm3_redis_capture_reply_cb(redis_client_t *client,
                                               redis_reply_t *reply,
                                               void *user_data) {
  turbo_wasm3_redis_command_capture_t *capture =
      (turbo_wasm3_redis_command_capture_t *)user_data;

  (void)client;
  if (!capture || capture->reply || !reply) {
    return;
  }

  capture->reply = turbo_wasm3_redis_reply_clone(reply);
}

static void turbo_wasm3_redis_open_task(coro_t *co, void *arg) {
  turbo_wasm3_redis_open_task_state_t *state =
      (turbo_wasm3_redis_open_task_state_t *)arg;
  coro_context_t *ctx = coro_context_current();

  (void)co;
  state->rc = TURBO_EIO;
  state->socket = NULL;
  if (!ctx || !state || !state->host || state->port == 0) {
    state->done = 1;
    return;
  }

  state->socket = coro_socket_create_tcpv4(ctx);
  if (!state->socket) {
    state->rc = TURBO_ENOMEM;
    state->done = 1;
    return;
  }

  coro_socket_set_timeout(state->socket, 5000);
  state->rc = coro_socket_connect(state->socket, state->host, (int)state->port);
  if (state->rc != 0) {
    coro_socket_destroy(state->socket);
    state->socket = NULL;
  }
  state->done = 1;
}

static void turbo_wasm3_redis_command_task(coro_t *co, void *arg) {
  turbo_wasm3_redis_command_task_state_t *state =
      (turbo_wasm3_redis_command_task_state_t *)arg;
  size_t *arg_lens = NULL;
  turbo_wasm3_redis_command_capture_t capture = {0};
  uint32_t i;

  (void)co;
  state->rc = TURBO_EIO;
  if (!state || !state->entry || !state->entry->client || state->argc == 0 ||
      !state->argv || !state->argv_lens) {
    state->done = 1;
    return;
  }

  arg_lens = (size_t *)calloc(state->argc, sizeof(*arg_lens));
  if (!arg_lens) {
    state->rc = TURBO_ENOMEM;
    state->done = 1;
    return;
  }

  for (i = 0; i < state->argc; ++i) {
    arg_lens[i] = (size_t)state->argv_lens[i];
  }

  state->rc = redis_commandv(state->entry->client, (int)state->argc, state->argv,
                             arg_lens, turbo_wasm3_redis_capture_reply_cb,
                             &capture);
  free(arg_lens);
  if (state->rc == 0) {
    state->reply = capture.reply;
  }
  state->done = 1;
}

static int turbo_wasm3_redis_run_until_done(coro_context_t *ctx, int *done) {
  uint64_t deadline_ms;

  if (!ctx || !done) {
    return TURBO_EINVAL;
  }

  deadline_ms = turbo_monotonic_ms() + 2000;
  while (!*done && turbo_monotonic_ms() < deadline_ms) {
    coro_context_run(ctx, TURBO_RUN_ONCE);
  }

  return *done ? 0 : TURBO_ETIMEDOUT;
}

static int turbo_wasm3_redis_registry_store_reply(
    turbo_wasm3_redis_registry_t *registry, uint32_t client_handle,
    redis_reply_t *reply, uint32_t *out_reply_handle) {
  turbo_wasm3_redis_reply_entry_t *entry;
  int rc;

  if (!registry || !reply || !out_reply_handle) {
    return TURBO_EINVAL;
  }

  rc = turbo_wasm3_redis_registry_reserve_replies(registry,
                                                  registry->reply_count + 1);
  if (rc != 0) {
    return rc;
  }

  entry = &registry->replies[registry->reply_count];
  entry->handle = registry->next_reply_handle++;
  entry->client_handle = client_handle;
  entry->reply = reply;
  *out_reply_handle = entry->handle;
  registry->reply_count++;
  return 0;
}

static int turbo_wasm3_http_copy_string(const char *src, char *buffer,
                                        size_t buffer_size, uint32_t *out_len) {
  size_t len = src ? strlen(src) : 0;

  if (!buffer || buffer_size == 0 || !out_len) {
    return TURBO_EINVAL;
  }

  if (!src) {
    buffer[0] = '\0';
    *out_len = 0;
    return 0;
  }

  if (len >= buffer_size) {
    memcpy(buffer, src, buffer_size - 1);
    buffer[buffer_size - 1] = '\0';
    *out_len = (uint32_t)(buffer_size - 1);
    return TURBO_EMSGSIZE;
  }

  if (len != 0) {
    memcpy(buffer, src, len);
  }
  buffer[len] = '\0';
  *out_len = (uint32_t)len;
  return 0;
}

typedef struct turbo_wasm3_http_header_block_s {
  char *storage;
  const char **lines;
  int count;
} turbo_wasm3_http_header_block_t;

static void
turbo_wasm3_http_header_block_clear(turbo_wasm3_http_header_block_t *block) {
  if (!block) {
    return;
  }

  free((void *)block->lines);
  free(block->storage);
  block->storage = NULL;
  block->lines = NULL;
  block->count = 0;
}

static int turbo_wasm3_http_parse_headers(const char *headers, size_t headers_len,
                                          turbo_wasm3_http_header_block_t *out_block) {
  char *storage = NULL;
  const char **lines = NULL;
  size_t i;
  size_t line_count = 0;
  char *cursor;
  int index = 0;

  if (!out_block) {
    return TURBO_EINVAL;
  }

  memset(out_block, 0, sizeof(*out_block));
  if (!headers || headers_len == 0) {
    return 0;
  }

  storage = (char *)malloc(headers_len + 1);
  if (!storage) {
    return TURBO_ENOMEM;
  }

  memcpy(storage, headers, headers_len);
  storage[headers_len] = '\0';

  for (i = 0; i < headers_len; ++i) {
    if (storage[i] == '\r') {
      storage[i] = '\n';
    }
  }

  cursor = storage;
  while (*cursor != '\0') {
    char *line_start = cursor;

    while (*cursor != '\0' && *cursor != '\n') {
      ++cursor;
    }
    if (*cursor == '\n') {
      *cursor++ = '\0';
    }
    if (*line_start != '\0') {
      ++line_count;
    }
  }

  if (line_count == 0) {
    out_block->storage = storage;
    return 0;
  }

  lines = (const char **)calloc(line_count, sizeof(*lines));
  if (!lines) {
    free(storage);
    return TURBO_ENOMEM;
  }

  cursor = storage;
  while (cursor < storage + headers_len) {
    if (*cursor != '\0') {
      lines[index++] = cursor;
      cursor += strlen(cursor) + 1;
      continue;
    }
    ++cursor;
  }

  out_block->storage = storage;
  out_block->lines = lines;
  out_block->count = (int)line_count;
  return 0;
}

static turbo_wasm3_http_client_entry_t *
turbo_wasm3_http_registry_find_client(turbo_wasm3_http_registry_t *registry,
                                      uint32_t client_handle) {
  size_t i;

  if (!registry) {
    return NULL;
  }

  for (i = 0; i < registry->client_count; ++i) {
    if (registry->clients[i].handle == client_handle) {
      return &registry->clients[i];
    }
  }

  return NULL;
}

static turbo_wasm3_http_response_entry_t *
turbo_wasm3_http_registry_find_response(turbo_wasm3_http_registry_t *registry,
                                        uint32_t response_handle) {
  size_t i;

  if (!registry) {
    return NULL;
  }

  for (i = 0; i < registry->response_count; ++i) {
    if (registry->responses[i].handle == response_handle) {
      return &registry->responses[i];
    }
  }

  return NULL;
}

static int turbo_wasm3_http_registry_reserve_clients(
    turbo_wasm3_http_registry_t *registry, size_t required) {
  turbo_wasm3_http_client_entry_t *entries;
  size_t new_capacity;

  if (!registry) {
    return TURBO_EINVAL;
  }
  if (required <= registry->client_capacity) {
    return 0;
  }

  new_capacity = registry->client_capacity ? registry->client_capacity : 4;
  while (new_capacity < required) {
    new_capacity *= 2;
  }

  entries = (turbo_wasm3_http_client_entry_t *)realloc(
      registry->clients, new_capacity * sizeof(*entries));
  if (!entries) {
    return TURBO_ENOMEM;
  }

  registry->clients = entries;
  registry->client_capacity = new_capacity;
  return 0;
}

static int turbo_wasm3_http_registry_reserve_responses(
    turbo_wasm3_http_registry_t *registry, size_t required) {
  turbo_wasm3_http_response_entry_t *entries;
  size_t new_capacity;

  if (!registry) {
    return TURBO_EINVAL;
  }
  if (required <= registry->response_capacity) {
    return 0;
  }

  new_capacity = registry->response_capacity ? registry->response_capacity : 4;
  while (new_capacity < required) {
    new_capacity *= 2;
  }

  entries = (turbo_wasm3_http_response_entry_t *)realloc(
      registry->responses, new_capacity * sizeof(*entries));
  if (!entries) {
    return TURBO_ENOMEM;
  }

  registry->responses = entries;
  registry->response_capacity = new_capacity;
  return 0;
}

static int turbo_wasm3_http_response_entry_reserve_chunks(
    turbo_wasm3_http_response_entry_t *entry, size_t required) {
  turbo_wasm3_blob_t *chunks;
  size_t new_capacity;

  if (!entry) {
    return TURBO_EINVAL;
  }
  if (required <= entry->chunk_capacity) {
    return 0;
  }

  new_capacity = entry->chunk_capacity ? entry->chunk_capacity : 4;
  while (new_capacity < required) {
    new_capacity *= 2;
  }

  chunks = (turbo_wasm3_blob_t *)realloc(entry->chunks,
                                         new_capacity * sizeof(*chunks));
  if (!chunks) {
    return TURBO_ENOMEM;
  }

  entry->chunks = chunks;
  entry->chunk_capacity = new_capacity;
  return 0;
}

static int turbo_wasm3_http_response_entry_append_chunk(
    turbo_wasm3_http_response_entry_t *entry, const void *data, size_t len) {
  uint8_t *copy = NULL;
  int rc;

  if (!entry) {
    return TURBO_EINVAL;
  }
  if (len == 0) {
    return 0;
  }

  rc = turbo_wasm3_http_response_entry_reserve_chunks(entry,
                                                      entry->chunk_count + 1);
  if (rc != 0) {
    return rc;
  }

  copy = (uint8_t *)malloc(len);
  if (!copy) {
    return TURBO_ENOMEM;
  }

  memcpy(copy, data, len);
  entry->chunks[entry->chunk_count].bytes = copy;
  entry->chunks[entry->chunk_count].size = (uint32_t)len;
  entry->chunk_count++;
  entry->is_stream = 1;
  return 0;
}

static int turbo_wasm3_http_response_entry_finalize_stream(
    turbo_wasm3_http_response_entry_t *entry) {
  size_t total = 0;
  size_t i;
  char *body;

  if (!entry || !entry->response) {
    return TURBO_EINVAL;
  }
  if (entry->chunk_count == 0 || entry->response->body != NULL) {
    return 0;
  }

  for (i = 0; i < entry->chunk_count; ++i) {
    total += entry->chunks[i].size;
  }

  body = (char *)malloc(total + 1);
  if (!body) {
    return TURBO_ENOMEM;
  }

  total = 0;
  for (i = 0; i < entry->chunk_count; ++i) {
    if (entry->chunks[i].size != 0) {
      memcpy(body + total, entry->chunks[i].bytes, entry->chunks[i].size);
      total += entry->chunks[i].size;
    }
  }
  body[total] = '\0';
  entry->response->body = body;
  entry->response->body_len = total;
  return 0;
}

typedef struct turbo_wasm3_http_capture_ctx_s {
  turbo_wasm3_http_response_entry_t *entry;
  int rc;
} turbo_wasm3_http_capture_ctx_t;

static void turbo_wasm3_http_capture_chunk_cb(const char *data, size_t len,
                                              void *user_data) {
  turbo_wasm3_http_capture_ctx_t *ctx =
      (turbo_wasm3_http_capture_ctx_t *)user_data;
  int rc;

  if (!ctx || !ctx->entry || ctx->rc != 0) {
    return;
  }

  rc = turbo_wasm3_http_response_entry_append_chunk(ctx->entry, data, len);
  if (rc != 0) {
    ctx->rc = rc;
  }
}

static void turbo_wasm3_http_capture_client_error(
    turbo_wasm3_http_client_entry_t *entry, const char *message) {
  if (!entry) {
    return;
  }

  turbo_wasm3_http_client_entry_set_error(entry, message ? message : "http client error",
                                          (uint32_t)strlen(message ? message
                                                                   : "http client error"));
}

static void turbo_wasm3_http_capture_response_error(
    turbo_wasm3_http_response_entry_t *response_entry, http_response_t *response) {
  const char *message = NULL;

  if (!response_entry) {
    return;
  }

  if (response && response->error && response->error[0] != '\0') {
    message = response->error;
  } else {
    message = "http response error";
  }

  turbo_wasm3_http_response_entry_set_error(response_entry, message,
                                            (uint32_t)strlen(message));
}

static int turbo_wasm3_http_registry_request_core(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle, int method,
    const char *url, const char **headers, int header_count, const void *body,
    size_t body_len, int use_stream_api, int use_sse_api,
    uint32_t *response_handle) {
  turbo_wasm3_http_client_entry_t *client_entry;
  turbo_wasm3_http_response_entry_t *entry;
  turbo_wasm3_http_capture_ctx_t capture_ctx = {0};
  http_response_t *response = NULL;
  int rc;

  if (!registry || !url || !response_handle) {
    return TURBO_EINVAL;
  }

  client_entry = turbo_wasm3_http_registry_find_client(registry, client_handle);
  if (!client_entry) {
    return TURBO_EBADF;
  }

  rc = turbo_wasm3_http_registry_reserve_responses(registry,
                                                   registry->response_count + 1);
  if (rc != 0) {
    return rc;
  }

  entry = &registry->responses[registry->response_count];
  memset(entry, 0, sizeof(*entry));
  entry->handle = registry->next_response_handle++;
  entry->client_handle = client_handle;
  entry->is_stream = use_stream_api ? 1 : 0;
  entry->is_sse = use_sse_api ? 1 : 0;

  turbo_wasm3_http_client_entry_clear_error(client_entry);
  if (use_sse_api) {
    capture_ctx.entry = entry;
    response = http_sse_get(client_entry->client, url,
                            turbo_wasm3_http_capture_chunk_cb, &capture_ctx);
  } else if (use_stream_api) {
    capture_ctx.entry = entry;
    response = http_receive_stream_get(client_entry->client, url,
                                       turbo_wasm3_http_capture_chunk_cb,
                                       &capture_ctx);
  } else {
    response = http_request(client_entry->client, method, url, headers,
                            header_count, (const char *)body, body_len);
  }

  if (!response) {
    turbo_wasm3_http_capture_client_error(client_entry,
                                          "http request returned null");
    return TURBO_EIO;
  }

  entry->response = response;
  if (capture_ctx.rc != 0) {
    rc = capture_ctx.rc;
    turbo_wasm3_http_capture_response_error(entry, response);
    turbo_wasm3_http_response_entry_reset(entry);
    return rc;
  }

  rc = turbo_wasm3_http_response_entry_finalize_stream(entry);
  if (rc != 0) {
    turbo_wasm3_http_capture_response_error(entry, response);
    turbo_wasm3_http_response_entry_reset(entry);
    return rc;
  }

  if (response->error && response->error[0] != '\0') {
    turbo_wasm3_http_capture_response_error(entry, response);
  }

  *response_handle = entry->handle;
  registry->response_count++;
  return 0;
}

static int turbo_wasm3_sqlite_column_type_value(void *stmt, uint32_t index,
                                                int32_t *out_type) {
  int type;

  if (!stmt || !out_type) {
    return TURBO_EINVAL;
  }

  type = sqlite3_column_type((sqlite3_stmt *)stmt, (int)index);
  switch (type) {
    case SQLITE_INTEGER:
      *out_type = TURBO_WASM3_DB_TYPE_INT64;
      return 0;
    case SQLITE_FLOAT:
      *out_type = TURBO_WASM3_DB_TYPE_DOUBLE;
      return 0;
    case SQLITE_TEXT:
      *out_type = TURBO_WASM3_DB_TYPE_TEXT;
      return 0;
    case SQLITE_BLOB:
      *out_type = TURBO_WASM3_DB_TYPE_BLOB;
      return 0;
    case SQLITE_NULL:
      *out_type = TURBO_WASM3_DB_TYPE_NULL;
      return 0;
    default:
      return TURBO_EIO;
  }
}

static int turbo_wasm3_sqlite_open(void *user_data, const char *target,
                                   void **out_db) {
  sqlite3 *db = NULL;
  int rc;

  (void)user_data;

  if (!target || !out_db) {
    return TURBO_EINVAL;
  }

  rc = sqlite3_open(target, &db);
  if (rc != SQLITE_OK) {
    if (db) {
      sqlite3_close(db);
    }
    return TURBO_EIO;
  }

  *out_db = db;
  return 0;
}

static int turbo_wasm3_sqlite_close(void *user_data, void *db) {
  (void)user_data;

  if (!db) {
    return TURBO_EINVAL;
  }

  return sqlite3_close((sqlite3 *)db) == SQLITE_OK ? 0 : TURBO_EBUSY;
}

static int turbo_wasm3_sqlite_exec(void *user_data, void *db, const char *sql,
                                   uint64_t *changes) {
  char *errmsg = NULL;
  int rc;

  (void)user_data;

  if (!db || !sql || !changes) {
    return TURBO_EINVAL;
  }

  *changes = 0;
  rc = sqlite3_exec((sqlite3 *)db, sql, NULL, NULL, &errmsg);
  if (errmsg) {
    sqlite3_free(errmsg);
  }
  if (rc != SQLITE_OK) {
    return TURBO_EINVAL;
  }

  *changes = (uint64_t)sqlite3_changes64((sqlite3 *)db);
  return 0;
}

static int turbo_wasm3_sqlite_error(void *user_data, void *db, char *buffer,
                                    size_t buffer_size, uint32_t *out_len) {
  const char *msg;
  size_t len;

  (void)user_data;

  if (!db || !buffer || buffer_size == 0 || !out_len) {
    return TURBO_EINVAL;
  }

  msg = sqlite3_errmsg((sqlite3 *)db);
  if (!msg) {
    buffer[0] = '\0';
    *out_len = 0;
    return 0;
  }

  len = strlen(msg);
  if (len >= buffer_size) {
    len = buffer_size - 1;
  }

  memcpy(buffer, msg, len);
  buffer[len] = '\0';
  *out_len = (uint32_t)len;
  return 0;
}

static int turbo_wasm3_sqlite_prepare(void *user_data, void *db, const char *sql,
                                      void **out_stmt) {
  sqlite3_stmt *stmt = NULL;
  int rc;

  (void)user_data;

  if (!db || !sql || !out_stmt) {
    return TURBO_EINVAL;
  }

  rc = sqlite3_prepare_v2((sqlite3 *)db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    if (stmt) {
      sqlite3_finalize(stmt);
    }
    return TURBO_EINVAL;
  }

  *out_stmt = stmt;
  return 0;
}

static int turbo_wasm3_sqlite_finalize(void *user_data, void *stmt) {
  (void)user_data;

  if (!stmt) {
    return TURBO_EINVAL;
  }

  return sqlite3_finalize((sqlite3_stmt *)stmt) == SQLITE_OK ? 0 : TURBO_EIO;
}

static int turbo_wasm3_sqlite_reset(void *user_data, void *stmt) {
  int rc;

  (void)user_data;

  if (!stmt) {
    return TURBO_EINVAL;
  }

  rc = sqlite3_reset((sqlite3_stmt *)stmt);
  sqlite3_clear_bindings((sqlite3_stmt *)stmt);
  return rc == SQLITE_OK ? 0 : TURBO_EIO;
}

static int turbo_wasm3_sqlite_step(void *user_data, void *stmt,
                                   int32_t *out_state) {
  int rc;

  (void)user_data;

  if (!stmt || !out_state) {
    return TURBO_EINVAL;
  }

  rc = sqlite3_step((sqlite3_stmt *)stmt);
  if (rc == SQLITE_ROW) {
    *out_state = TURBO_WASM3_DB_STEP_ROW;
    return 0;
  }
  if (rc == SQLITE_DONE) {
    *out_state = TURBO_WASM3_DB_STEP_DONE;
    return 0;
  }

  return TURBO_EIO;
}

static int turbo_wasm3_sqlite_bind_int64(void *user_data, void *stmt,
                                         uint32_t index, int64_t value) {
  (void)user_data;

  if (!stmt || index == 0) {
    return TURBO_EINVAL;
  }

  return sqlite3_bind_int64((sqlite3_stmt *)stmt, (int)index, (sqlite3_int64)value) ==
                 SQLITE_OK
             ? 0
             : TURBO_EINVAL;
}

static int turbo_wasm3_sqlite_bind_double(void *user_data, void *stmt,
                                          uint32_t index, double value) {
  (void)user_data;

  if (!stmt || index == 0) {
    return TURBO_EINVAL;
  }

  return sqlite3_bind_double((sqlite3_stmt *)stmt, (int)index, value) == SQLITE_OK
             ? 0
             : TURBO_EINVAL;
}

static int turbo_wasm3_sqlite_bind_null(void *user_data, void *stmt,
                                        uint32_t index) {
  (void)user_data;

  if (!stmt || index == 0) {
    return TURBO_EINVAL;
  }

  return sqlite3_bind_null((sqlite3_stmt *)stmt, (int)index) == SQLITE_OK ? 0
                                                                           : TURBO_EINVAL;
}

static int turbo_wasm3_sqlite_bind_blob(void *user_data, void *stmt,
                                        uint32_t index, const void *value,
                                        size_t value_len) {
  (void)user_data;

  if (!stmt || index == 0 || (!value && value_len != 0)) {
    return TURBO_EINVAL;
  }

  return sqlite3_bind_blob((sqlite3_stmt *)stmt, (int)index, value, (int)value_len,
                           SQLITE_TRANSIENT) == SQLITE_OK
             ? 0
             : TURBO_EINVAL;
}

static int turbo_wasm3_sqlite_bind_text(void *user_data, void *stmt,
                                        uint32_t index, const char *value,
                                        size_t value_len) {
  (void)user_data;

  if (!stmt || index == 0 || (!value && value_len != 0)) {
    return TURBO_EINVAL;
  }

  return sqlite3_bind_text((sqlite3_stmt *)stmt, (int)index, value,
                           (int)value_len, SQLITE_TRANSIENT) == SQLITE_OK
             ? 0
             : TURBO_EINVAL;
}

static int turbo_wasm3_sqlite_column_type(void *user_data, void *stmt,
                                          uint32_t index, int32_t *out_type) {
  (void)user_data;

  return turbo_wasm3_sqlite_column_type_value(stmt, index, out_type);
}

static int turbo_wasm3_sqlite_column_int64(void *user_data, void *stmt,
                                           uint32_t index, int64_t *out_value) {
  (void)user_data;

  if (!stmt || !out_value) {
    return TURBO_EINVAL;
  }

  *out_value = (int64_t)sqlite3_column_int64((sqlite3_stmt *)stmt, (int)index);
  return 0;
}

static int turbo_wasm3_sqlite_column_double(void *user_data, void *stmt,
                                            uint32_t index, double *out_value) {
  (void)user_data;

  if (!stmt || !out_value) {
    return TURBO_EINVAL;
  }

  *out_value = sqlite3_column_double((sqlite3_stmt *)stmt, (int)index);
  return 0;
}

static int turbo_wasm3_sqlite_column_blob(void *user_data, void *stmt,
                                          uint32_t index, void *buffer,
                                          size_t buffer_size,
                                          uint32_t *out_len) {
  const void *blob;
  int len;
  int32_t type;

  (void)user_data;

  if (!stmt || !buffer || buffer_size == 0 || !out_len) {
    return TURBO_EINVAL;
  }

  if (turbo_wasm3_sqlite_column_type_value(stmt, index, &type) != 0) {
    return TURBO_EIO;
  }
  if (type == TURBO_WASM3_DB_TYPE_NULL) {
    *out_len = 0;
    return 0;
  }

  blob = sqlite3_column_blob((sqlite3_stmt *)stmt, (int)index);
  if (!blob) {
    *out_len = 0;
    return 0;
  }

  len = sqlite3_column_bytes((sqlite3_stmt *)stmt, (int)index);
  if ((size_t)len > buffer_size) {
    memcpy(buffer, blob, buffer_size);
    *out_len = (uint32_t)buffer_size;
    return TURBO_EMSGSIZE;
  }

  if (len != 0) {
    memcpy(buffer, blob, (size_t)len);
  }
  *out_len = (uint32_t)len;
  return 0;
}

static int turbo_wasm3_sqlite_column_text(void *user_data, void *stmt,
                                          uint32_t index, char *buffer,
                                          size_t buffer_size,
                                          uint32_t *out_len) {
  const unsigned char *text;
  int len;

  (void)user_data;

  if (!stmt || !buffer || buffer_size == 0 || !out_len) {
    return TURBO_EINVAL;
  }

  text = sqlite3_column_text((sqlite3_stmt *)stmt, (int)index);
  if (!text) {
    buffer[0] = '\0';
    *out_len = 0;
    return 0;
  }

  len = sqlite3_column_bytes((sqlite3_stmt *)stmt, (int)index);
  if ((size_t)len >= buffer_size) {
    memcpy(buffer, text, buffer_size - 1);
    buffer[buffer_size - 1] = '\0';
    *out_len = (uint32_t)(buffer_size - 1);
    return TURBO_EMSGSIZE;
  }

  memcpy(buffer, text, (size_t)len);
  buffer[len] = '\0';
  *out_len = (uint32_t)len;
  return 0;
}

static const turbo_wasm3_db_ops_t turbo_wasm3_sqlite_db_ops = {
    turbo_wasm3_sqlite_open,
    turbo_wasm3_sqlite_close,
    turbo_wasm3_sqlite_exec,
    turbo_wasm3_sqlite_error,
    turbo_wasm3_sqlite_prepare,
    turbo_wasm3_sqlite_finalize,
    turbo_wasm3_sqlite_reset,
    turbo_wasm3_sqlite_step,
    turbo_wasm3_sqlite_bind_int64,
    turbo_wasm3_sqlite_bind_double,
    turbo_wasm3_sqlite_bind_null,
    turbo_wasm3_sqlite_bind_blob,
    turbo_wasm3_sqlite_bind_text,
    turbo_wasm3_sqlite_column_type,
    turbo_wasm3_sqlite_column_int64,
    turbo_wasm3_sqlite_column_double,
    turbo_wasm3_sqlite_column_blob,
    turbo_wasm3_sqlite_column_text,
};

static uint16_t
turbo_wasm3_socket_send_cb(void *user_data, uint32_t handle, const uint8_t *data,
                           uint32_t data_len, uint16_t si_flags,
                           uint32_t *so_datalen) {
  turbo_wasm3_socket_registry_t *registry =
      (turbo_wasm3_socket_registry_t *)user_data;
  turbo_wasm3_socket_entry_t *entry;
  int rc;

  if (si_flags != 0 || !so_datalen) {
    return __WASI_ERRNO_INVAL;
  }

  entry = turbo_wasm3_socket_registry_find_entry(registry, handle);
  if (!entry) {
    return __WASI_ERRNO_BADF;
  }

  rc = coro_socket_send(entry->socket, (const char *)data, (size_t)data_len);
  if (rc != 0) {
    return turbo_wasm3_error_to_wasi(rc);
  }

  *so_datalen = data_len;
  return __WASI_ERRNO_SUCCESS;
}

static uint16_t
turbo_wasm3_socket_recv_cb(void *user_data, uint32_t handle, uint8_t *data,
                           uint32_t data_len, uint16_t ri_flags,
                           uint32_t *ro_datalen, uint16_t *ro_flags) {
  turbo_wasm3_socket_registry_t *registry =
      (turbo_wasm3_socket_registry_t *)user_data;
  turbo_wasm3_socket_entry_t *entry;
  char *recv_data = NULL;
  size_t recv_len = 0;
  int rc;

  if (!ro_datalen || !ro_flags) {
    return __WASI_ERRNO_INVAL;
  }

  entry = turbo_wasm3_socket_registry_find_entry(registry, handle);
  if (!entry) {
    return __WASI_ERRNO_BADF;
  }

  if ((ri_flags & ~((uint16_t)0x0003)) != 0) {
    return __WASI_ERRNO_INVAL;
  }

  rc = coro_socket_recv(entry->socket, &recv_data, &recv_len);
  if (rc != 0) {
    if (recv_data) {
      coro_socket_free_recv(recv_data);
    }
    return turbo_wasm3_error_to_wasi(rc);
  }

  *ro_flags = 0;
  if (recv_len > data_len) {
    recv_len = data_len;
    *ro_flags = __WASI_ROFLAGS_RECV_DATA_TRUNCATED;
  }

  if (recv_len != 0 && data && recv_data) {
    memcpy(data, recv_data, recv_len);
  }
  if (recv_data) {
    coro_socket_free_recv(recv_data);
  }

  *ro_datalen = (uint32_t)recv_len;
  return __WASI_ERRNO_SUCCESS;
}

static uint16_t
turbo_wasm3_socket_shutdown_cb(void *user_data, uint32_t handle, uint8_t how) {
  turbo_wasm3_socket_registry_t *registry =
      (turbo_wasm3_socket_registry_t *)user_data;
  turbo_wasm3_socket_entry_t *entry;

  (void)how;

  entry = turbo_wasm3_socket_registry_find_entry(registry, handle);
  if (!entry) {
    return __WASI_ERRNO_BADF;
  }

  return __WASI_ERRNO_NOSYS;
}

static const m3_wasi_socket_ops_t turbo_wasm3_socket_ops = {
    turbo_wasm3_socket_send_cb,
    turbo_wasm3_socket_recv_cb,
    turbo_wasm3_socket_shutdown_cb,
};

static turbo_wasm3_vm_t *turbo_wasm3_import_vm(IM3ImportContext ctx) {
  return ctx ? (turbo_wasm3_vm_t *)ctx->userdata : NULL;
}

m3ApiRawFunction(turbo_wasm3_host_abi_version) {
  m3ApiReturnType(uint32_t)

  m3ApiReturn(TURBO_WASM3_HOST_ABI_VERSION);
}

m3ApiRawFunction(turbo_wasm3_host_clock_time_ms) {
  m3ApiReturnType(uint64_t)

  m3ApiReturn(turbo_realtime_ms());
}

m3ApiRawFunction(turbo_wasm3_host_socket_send) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  turbo_wasm3_socket_entry_t *entry;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(const uint8_t *, data)
  m3ApiGetArg(uint32_t, data_len)
  m3ApiGetArgMem(uint32_t *, sent_len)

  if (!vm || !vm->socket_registry || !sent_len) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(sent_len, sizeof(uint32_t));
  m3ApiWriteMem32(sent_len, 0);
  entry = turbo_wasm3_socket_registry_find_entry(vm->socket_registry, handle);
  if (!entry) {
    m3ApiReturn(TURBO_EBADF);
  }
  if (data_len == 0) {
    m3ApiReturn(0);
  }

  m3ApiCheckMem(data, data_len);
  {
    int rc = coro_socket_send(entry->socket, (const char *)data, (size_t)data_len);
    if (rc != 0) {
      m3ApiReturn(rc);
    }
  }

  m3ApiWriteMem32(sent_len, data_len);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_socket_recv) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  turbo_wasm3_socket_entry_t *entry;
  char *recv_data = NULL;
  size_t recv_len = 0;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(uint8_t *, data)
  m3ApiGetArg(uint32_t, data_len)
  m3ApiGetArgMem(uint32_t *, recv_len_out)

  if (!vm || !vm->socket_registry || !recv_len_out) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(recv_len_out, sizeof(uint32_t));
  m3ApiWriteMem32(recv_len_out, 0);
  entry = turbo_wasm3_socket_registry_find_entry(vm->socket_registry, handle);
  if (!entry) {
    m3ApiReturn(TURBO_EBADF);
  }
  if (data_len != 0) {
    m3ApiCheckMem(data, data_len);
  }

  {
    int rc = coro_socket_recv(entry->socket, &recv_data, &recv_len);
    size_t copy_len = recv_len;

    if (rc != 0) {
      if (recv_data) {
        coro_socket_free_recv(recv_data);
      }
      m3ApiReturn(rc);
    }

    if (copy_len > data_len) {
      copy_len = data_len;
    }
    if (copy_len != 0 && data && recv_data) {
      memcpy(data, recv_data, copy_len);
    }
    if (recv_data) {
      coro_socket_free_recv(recv_data);
    }

    m3ApiWriteMem32(recv_len_out, (uint32_t)copy_len);
    if (recv_len > data_len) {
      m3ApiReturn(TURBO_EMSGSIZE);
    }
  }

  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_socket_release) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)

  if (!vm || !vm->socket_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(
      turbo_wasm3_socket_registry_unregister(vm->socket_registry, handle));
}

m3ApiRawFunction(turbo_wasm3_host_db_open) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *target = NULL;
  int rc;
  uint32_t handle = 0;

  m3ApiReturnType(int32_t)
  m3ApiGetArgMem(const uint8_t *, target_data)
  m3ApiGetArg(uint32_t, target_len)
  m3ApiGetArgMem(uint32_t *, out_handle)

  if (!vm || !vm->db_registry || !out_handle) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_handle, sizeof(uint32_t));
  if (target_len != 0) {
    m3ApiCheckMem(target_data, target_len);
  }
  m3ApiWriteMem32(out_handle, 0);

  target = turbo_wasm3_copy_guest_bytes(target_data, target_len);
  if (!target) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_wasm3_db_registry_open(vm->db_registry, target, &handle);
  free(target);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_handle, handle);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_db_close) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)

  if (!vm || !vm->db_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(turbo_wasm3_db_registry_close(vm->db_registry, handle));
}

m3ApiRawFunction(turbo_wasm3_host_db_exec) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *sql = NULL;
  uint64_t changes = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(const uint8_t *, sql_data)
  m3ApiGetArg(uint32_t, sql_len)
  m3ApiGetArgMem(uint64_t *, out_changes)

  if (!vm || !vm->db_registry || !out_changes) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_changes, sizeof(uint64_t));
  if (sql_len != 0) {
    m3ApiCheckMem(sql_data, sql_len);
  }
  m3ApiWriteMem64(out_changes, 0);

  sql = turbo_wasm3_copy_guest_bytes(sql_data, sql_len);
  if (!sql) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_wasm3_db_registry_exec(vm->db_registry, handle, sql, &changes);
  free(sql);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem64(out_changes, changes);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_db_error) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  uint32_t written = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->db_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  rc = turbo_wasm3_db_registry_error(vm->db_registry, handle, buffer,
                                     (size_t)buffer_size, &written);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_written, written);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_db_stmt_error) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  uint32_t written = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->db_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  rc = turbo_wasm3_db_registry_stmt_error(vm->db_registry, stmt_handle, buffer,
                                          (size_t)buffer_size, &written);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_written, written);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_db_prepare) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *sql = NULL;
  uint32_t stmt_handle = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, db_handle)
  m3ApiGetArgMem(const uint8_t *, sql_data)
  m3ApiGetArg(uint32_t, sql_len)
  m3ApiGetArgMem(uint32_t *, out_stmt_handle)

  if (!vm || !vm->db_registry || !out_stmt_handle) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_stmt_handle, sizeof(uint32_t));
  if (sql_len != 0) {
    m3ApiCheckMem(sql_data, sql_len);
  }
  m3ApiWriteMem32(out_stmt_handle, 0);

  sql = turbo_wasm3_copy_guest_bytes(sql_data, sql_len);
  if (!sql) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_wasm3_db_registry_prepare(vm->db_registry, db_handle, sql,
                                       &stmt_handle);
  free(sql);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_stmt_handle, stmt_handle);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_db_finalize) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)

  if (!vm || !vm->db_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(turbo_wasm3_db_registry_finalize(vm->db_registry, stmt_handle));
}

m3ApiRawFunction(turbo_wasm3_host_db_reset) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)

  if (!vm || !vm->db_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(turbo_wasm3_db_registry_reset(vm->db_registry, stmt_handle));
}

m3ApiRawFunction(turbo_wasm3_host_db_step) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  int32_t state = TURBO_WASM3_DB_STEP_DONE;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)
  m3ApiGetArgMem(int32_t *, out_state)

  if (!vm || !vm->db_registry || !out_state) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_state, sizeof(int32_t));
  m3ApiWriteMem32(out_state, (uint32_t)TURBO_WASM3_DB_STEP_DONE);

  rc = turbo_wasm3_db_registry_step(vm->db_registry, stmt_handle, &state);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_state, (uint32_t)state);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_db_bind_i64) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)
  m3ApiGetArg(uint32_t, index)
  m3ApiGetArg(int64_t, value)

  if (!vm || !vm->db_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(
      turbo_wasm3_db_registry_bind_int64(vm->db_registry, stmt_handle, index, value));
}

m3ApiRawFunction(turbo_wasm3_host_db_bind_f64) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)
  m3ApiGetArg(uint32_t, index)
  m3ApiGetArg(double, value)

  if (!vm || !vm->db_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(
      turbo_wasm3_db_registry_bind_double(vm->db_registry, stmt_handle, index, value));
}

m3ApiRawFunction(turbo_wasm3_host_db_bind_null) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)
  m3ApiGetArg(uint32_t, index)

  if (!vm || !vm->db_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(turbo_wasm3_db_registry_bind_null(vm->db_registry, stmt_handle, index));
}

m3ApiRawFunction(turbo_wasm3_host_db_bind_blob) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *blob = NULL;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)
  m3ApiGetArg(uint32_t, index)
  m3ApiGetArgMem(const uint8_t *, blob_data)
  m3ApiGetArg(uint32_t, blob_len)

  if (!vm || !vm->db_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  if (blob_len != 0) {
    m3ApiCheckMem(blob_data, blob_len);
  }

  blob = turbo_wasm3_copy_guest_bytes(blob_data, blob_len);
  if (!blob) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_wasm3_db_registry_bind_blob(vm->db_registry, stmt_handle, index, blob,
                                         blob_len);
  free(blob);
  m3ApiReturn(rc);
}

m3ApiRawFunction(turbo_wasm3_host_db_bind_text) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *text = NULL;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)
  m3ApiGetArg(uint32_t, index)
  m3ApiGetArgMem(const uint8_t *, text_data)
  m3ApiGetArg(uint32_t, text_len)

  if (!vm || !vm->db_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  if (text_len != 0) {
    m3ApiCheckMem(text_data, text_len);
  }

  text = turbo_wasm3_copy_guest_bytes(text_data, text_len);
  if (!text) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_wasm3_db_registry_bind_text(vm->db_registry, stmt_handle, index, text,
                                         text_len);
  free(text);
  m3ApiReturn(rc);
}

m3ApiRawFunction(turbo_wasm3_host_db_column_type) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  int32_t type = TURBO_WASM3_DB_TYPE_NULL;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)
  m3ApiGetArg(uint32_t, index)
  m3ApiGetArgMem(int32_t *, out_type)

  if (!vm || !vm->db_registry || !out_type) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_type, sizeof(int32_t));
  m3ApiWriteMem32(out_type, (uint32_t)TURBO_WASM3_DB_TYPE_NULL);

  rc = turbo_wasm3_db_registry_column_type(vm->db_registry, stmt_handle, index, &type);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_type, (uint32_t)type);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_db_column_i64) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  int64_t value = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)
  m3ApiGetArg(uint32_t, index)
  m3ApiGetArgMem(int64_t *, out_value)

  if (!vm || !vm->db_registry || !out_value) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_value, sizeof(int64_t));
  m3ApiWriteMem64(out_value, 0);

  rc = turbo_wasm3_db_registry_column_int64(vm->db_registry, stmt_handle, index,
                                            &value);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem64(out_value, (uint64_t)value);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_db_column_f64) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  double value = 0.0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)
  m3ApiGetArg(uint32_t, index)
  m3ApiGetArgMem(double *, out_value)

  if (!vm || !vm->db_registry || !out_value) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_value, sizeof(double));
  memset(out_value, 0, sizeof(double));

  rc = turbo_wasm3_db_registry_column_double(vm->db_registry, stmt_handle, index,
                                             &value);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  memcpy(out_value, &value, sizeof(value));
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_db_column_blob) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  uint32_t written = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)
  m3ApiGetArg(uint32_t, index)
  m3ApiGetArgMem(uint8_t *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->db_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  rc = turbo_wasm3_db_registry_column_blob(vm->db_registry, stmt_handle, index,
                                           buffer, (size_t)buffer_size, &written);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_written, written);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_db_column_text) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  uint32_t written = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)
  m3ApiGetArg(uint32_t, index)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->db_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  rc = turbo_wasm3_db_registry_column_text(vm->db_registry, stmt_handle, index,
                                           buffer, buffer_size, &written);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_written, written);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_client_open) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *base_url = NULL;
  uint32_t client_handle = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArgMem(const uint8_t *, base_url_data)
  m3ApiGetArg(uint32_t, base_url_len)
  m3ApiGetArgMem(uint32_t *, out_client_handle)

  if (!vm || !vm->http_registry || !out_client_handle) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_client_handle, sizeof(uint32_t));
  if (base_url_len != 0) {
    m3ApiCheckMem(base_url_data, base_url_len);
  }
  m3ApiWriteMem32(out_client_handle, 0);

  if (base_url_len != 0) {
    base_url = turbo_wasm3_copy_guest_bytes(base_url_data, base_url_len);
    if (!base_url) {
      m3ApiReturn(TURBO_ENOMEM);
    }
  }

  rc = turbo_wasm3_http_registry_open_client(vm->http_registry, base_url,
                                             &client_handle);
  free(base_url);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_client_handle, client_handle);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_client_close) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)

  if (!vm || !vm->http_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(
      turbo_wasm3_http_registry_close_client(vm->http_registry, client_handle));
}

m3ApiRawFunction(turbo_wasm3_host_http_client_set_timeout) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)
  m3ApiGetArg(int32_t, timeout_ms)

  if (!vm || !vm->http_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(turbo_wasm3_http_registry_set_timeout(vm->http_registry,
                                                    client_handle, timeout_ms));
}

m3ApiRawFunction(turbo_wasm3_host_http_client_set_default_header) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *name = NULL;
  char *value = NULL;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)
  m3ApiGetArgMem(const uint8_t *, name_data)
  m3ApiGetArg(uint32_t, name_len)
  m3ApiGetArgMem(const uint8_t *, value_data)
  m3ApiGetArg(uint32_t, value_len)

  if (!vm || !vm->http_registry || name_len == 0 || value_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(name_data, name_len);
  m3ApiCheckMem(value_data, value_len);

  name = turbo_wasm3_copy_guest_bytes(name_data, name_len);
  value = turbo_wasm3_copy_guest_bytes(value_data, value_len);
  if (!name || !value) {
    free(value);
    free(name);
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_wasm3_http_registry_set_default_header(vm->http_registry,
                                                    client_handle, name, value);
  free(value);
  free(name);
  m3ApiReturn(rc);
}

m3ApiRawFunction(turbo_wasm3_host_http_client_remove_default_header) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *name = NULL;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)
  m3ApiGetArgMem(const uint8_t *, name_data)
  m3ApiGetArg(uint32_t, name_len)

  if (!vm || !vm->http_registry || name_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(name_data, name_len);
  name = turbo_wasm3_copy_guest_bytes(name_data, name_len);
  if (!name) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_wasm3_http_registry_remove_default_header(vm->http_registry,
                                                       client_handle, name);
  free(name);
  m3ApiReturn(rc);
}

m3ApiRawFunction(turbo_wasm3_host_http_client_clear_default_headers) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)

  if (!vm || !vm->http_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(turbo_wasm3_http_registry_clear_default_headers(
      vm->http_registry, client_handle));
}

m3ApiRawFunction(turbo_wasm3_host_http_client_set_basic_auth) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *username = NULL;
  char *password = NULL;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)
  m3ApiGetArgMem(const uint8_t *, username_data)
  m3ApiGetArg(uint32_t, username_len)
  m3ApiGetArgMem(const uint8_t *, password_data)
  m3ApiGetArg(uint32_t, password_len)

  if (!vm || !vm->http_registry || username_len == 0 || password_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(username_data, username_len);
  m3ApiCheckMem(password_data, password_len);

  username = turbo_wasm3_copy_guest_bytes(username_data, username_len);
  password = turbo_wasm3_copy_guest_bytes(password_data, password_len);
  if (!username || !password) {
    free(password);
    free(username);
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_wasm3_http_registry_set_basic_auth(vm->http_registry, client_handle,
                                                username, password);
  free(password);
  free(username);
  m3ApiReturn(rc);
}

m3ApiRawFunction(turbo_wasm3_host_http_client_set_bearer_token) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *token = NULL;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)
  m3ApiGetArgMem(const uint8_t *, token_data)
  m3ApiGetArg(uint32_t, token_len)

  if (!vm || !vm->http_registry || token_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(token_data, token_len);
  token = turbo_wasm3_copy_guest_bytes(token_data, token_len);
  if (!token) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_wasm3_http_registry_set_bearer_token(vm->http_registry,
                                                  client_handle, token);
  free(token);
  m3ApiReturn(rc);
}

m3ApiRawFunction(turbo_wasm3_host_http_client_clear_auth) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)

  if (!vm || !vm->http_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(
      turbo_wasm3_http_registry_clear_auth(vm->http_registry, client_handle));
}

m3ApiRawFunction(turbo_wasm3_host_http_client_set_proxy) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *host = NULL;
  char *username = NULL;
  char *password = NULL;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)
  m3ApiGetArgMem(const uint8_t *, host_data)
  m3ApiGetArg(uint32_t, host_len)
  m3ApiGetArg(uint32_t, port)
  m3ApiGetArgMem(const uint8_t *, username_data)
  m3ApiGetArg(uint32_t, username_len)
  m3ApiGetArgMem(const uint8_t *, password_data)
  m3ApiGetArg(uint32_t, password_len)

  if (!vm || !vm->http_registry || host_len == 0 || port == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(host_data, host_len);
  if (username_len != 0) {
    m3ApiCheckMem(username_data, username_len);
  }
  if (password_len != 0) {
    m3ApiCheckMem(password_data, password_len);
  }

  host = turbo_wasm3_copy_guest_bytes(host_data, host_len);
  if (!host) {
    m3ApiReturn(TURBO_ENOMEM);
  }
  if (username_len != 0) {
    username = turbo_wasm3_copy_guest_bytes(username_data, username_len);
    if (!username) {
      free(host);
      m3ApiReturn(TURBO_ENOMEM);
    }
  }
  if (password_len != 0) {
    password = turbo_wasm3_copy_guest_bytes(password_data, password_len);
    if (!password) {
      free(username);
      free(host);
      m3ApiReturn(TURBO_ENOMEM);
    }
  }

  rc = turbo_wasm3_http_registry_set_proxy(vm->http_registry, client_handle, host,
                                           (uint16_t)port, username, password);
  free(password);
  free(username);
  free(host);
  m3ApiReturn(rc);
}

m3ApiRawFunction(turbo_wasm3_host_http_client_clear_proxy) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)

  if (!vm || !vm->http_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(
      turbo_wasm3_http_registry_clear_proxy(vm->http_registry, client_handle));
}

m3ApiRawFunction(turbo_wasm3_host_http_request) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *url = NULL;
  char *body = NULL;
  uint32_t response_handle = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)
  m3ApiGetArg(int32_t, method)
  m3ApiGetArgMem(const uint8_t *, url_data)
  m3ApiGetArg(uint32_t, url_len)
  m3ApiGetArgMem(const uint8_t *, body_data)
  m3ApiGetArg(uint32_t, body_len)
  m3ApiGetArgMem(uint32_t *, out_response_handle)

  if (!vm || !vm->http_registry || !out_response_handle || !url_len) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_response_handle, sizeof(uint32_t));
  m3ApiCheckMem(url_data, url_len);
  if (body_len != 0) {
    m3ApiCheckMem(body_data, body_len);
  }
  m3ApiWriteMem32(out_response_handle, 0);

  url = turbo_wasm3_copy_guest_bytes(url_data, url_len);
  if (!url) {
    m3ApiReturn(TURBO_ENOMEM);
  }
  if (body_len != 0) {
    body = turbo_wasm3_copy_guest_bytes(body_data, body_len);
    if (!body) {
      free(url);
      m3ApiReturn(TURBO_ENOMEM);
    }
  }

  rc = turbo_wasm3_http_registry_request(vm->http_registry, client_handle, method,
                                         url, body, body_len, &response_handle);
  free(body);
  free(url);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_response_handle, response_handle);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_request_with_headers) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *url = NULL;
  char *headers = NULL;
  char *body = NULL;
  uint32_t response_handle = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)
  m3ApiGetArg(int32_t, method)
  m3ApiGetArgMem(const uint8_t *, url_data)
  m3ApiGetArg(uint32_t, url_len)
  m3ApiGetArgMem(const uint8_t *, headers_data)
  m3ApiGetArg(uint32_t, headers_len)
  m3ApiGetArgMem(const uint8_t *, body_data)
  m3ApiGetArg(uint32_t, body_len)
  m3ApiGetArgMem(uint32_t *, out_response_handle)

  if (!vm || !vm->http_registry || !out_response_handle || !url_len) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_response_handle, sizeof(uint32_t));
  m3ApiCheckMem(url_data, url_len);
  if (headers_len != 0) {
    m3ApiCheckMem(headers_data, headers_len);
  }
  if (body_len != 0) {
    m3ApiCheckMem(body_data, body_len);
  }
  m3ApiWriteMem32(out_response_handle, 0);

  url = turbo_wasm3_copy_guest_bytes(url_data, url_len);
  if (!url) {
    m3ApiReturn(TURBO_ENOMEM);
  }
  if (headers_len != 0) {
    headers = turbo_wasm3_copy_guest_bytes(headers_data, headers_len);
    if (!headers) {
      free(url);
      m3ApiReturn(TURBO_ENOMEM);
    }
  }
  if (body_len != 0) {
    body = turbo_wasm3_copy_guest_bytes(body_data, body_len);
    if (!body) {
      free(headers);
      free(url);
      m3ApiReturn(TURBO_ENOMEM);
    }
  }

  rc = turbo_wasm3_http_registry_request_with_headers(
      vm->http_registry, client_handle, method, url, headers, headers_len, body,
      body_len, &response_handle);
  free(body);
  free(headers);
  free(url);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_response_handle, response_handle);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_stream_get) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *url = NULL;
  uint32_t response_handle = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)
  m3ApiGetArgMem(const uint8_t *, url_data)
  m3ApiGetArg(uint32_t, url_len)
  m3ApiGetArgMem(uint32_t *, out_response_handle)

  if (!vm || !vm->http_registry || !out_response_handle || url_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_response_handle, sizeof(uint32_t));
  m3ApiCheckMem(url_data, url_len);
  m3ApiWriteMem32(out_response_handle, 0);

  url = turbo_wasm3_copy_guest_bytes(url_data, url_len);
  if (!url) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_wasm3_http_registry_stream_get(vm->http_registry, client_handle, url,
                                            &response_handle);
  free(url);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_response_handle, response_handle);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_sse_get) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *url = NULL;
  uint32_t response_handle = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)
  m3ApiGetArgMem(const uint8_t *, url_data)
  m3ApiGetArg(uint32_t, url_len)
  m3ApiGetArgMem(uint32_t *, out_response_handle)

  if (!vm || !vm->http_registry || !out_response_handle || url_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_response_handle, sizeof(uint32_t));
  m3ApiCheckMem(url_data, url_len);
  m3ApiWriteMem32(out_response_handle, 0);

  url = turbo_wasm3_copy_guest_bytes(url_data, url_len);
  if (!url) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_wasm3_http_registry_sse_get(vm->http_registry, client_handle, url,
                                         &response_handle);
  free(url);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_response_handle, response_handle);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_response_status) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  int32_t status_code = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, response_handle)
  m3ApiGetArgMem(int32_t *, out_status)

  if (!vm || !vm->http_registry || !out_status) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_status, sizeof(int32_t));
  m3ApiWriteMem32(out_status, 0);

  rc = turbo_wasm3_http_registry_response_status(vm->http_registry, response_handle,
                                                 &status_code);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_status, (uint32_t)status_code);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_response_error_code) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  int32_t error_code = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, response_handle)
  m3ApiGetArgMem(int32_t *, out_error_code)

  if (!vm || !vm->http_registry || !out_error_code) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_error_code, sizeof(int32_t));
  m3ApiWriteMem32(out_error_code, 0);

  rc = turbo_wasm3_http_registry_response_error_code(vm->http_registry,
                                                     response_handle, &error_code);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_error_code, (uint32_t)error_code);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_response_is_sse) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  int32_t is_sse = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, response_handle)
  m3ApiGetArgMem(int32_t *, out_is_sse)

  if (!vm || !vm->http_registry || !out_is_sse) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_is_sse, sizeof(int32_t));
  m3ApiWriteMem32(out_is_sse, 0);

  rc = turbo_wasm3_http_registry_response_is_sse(vm->http_registry,
                                                 response_handle, &is_sse);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_is_sse, (uint32_t)is_sse);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_response_chunk_count) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  uint32_t count = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, response_handle)
  m3ApiGetArgMem(uint32_t *, out_count)

  if (!vm || !vm->http_registry || !out_count) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_count, sizeof(uint32_t));
  m3ApiWriteMem32(out_count, 0);

  rc = turbo_wasm3_http_registry_response_chunk_count(vm->http_registry,
                                                      response_handle, &count);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_count, count);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_response_chunk) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  uint32_t written = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, response_handle)
  m3ApiGetArg(uint32_t, chunk_index)
  m3ApiGetArgMem(uint8_t *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->http_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  rc = turbo_wasm3_http_registry_response_chunk(vm->http_registry,
                                                response_handle, chunk_index,
                                                buffer, (size_t)buffer_size,
                                                &written);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_written, written);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_response_header) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *header_name = NULL;
  uint32_t written = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, response_handle)
  m3ApiGetArgMem(const uint8_t *, header_name_data)
  m3ApiGetArg(uint32_t, header_name_len)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->http_registry || !out_written || !header_name_len) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  m3ApiCheckMem(header_name_data, header_name_len);
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  header_name = turbo_wasm3_copy_guest_bytes(header_name_data, header_name_len);
  if (!header_name) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_wasm3_http_registry_response_header(vm->http_registry, response_handle,
                                                 header_name, buffer,
                                                 (size_t)buffer_size, &written);
  free(header_name);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_written, written);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_response_headers) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  uint32_t written = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, response_handle)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->http_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  rc = turbo_wasm3_http_registry_response_headers(vm->http_registry, response_handle,
                                                  buffer, (size_t)buffer_size, &written);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_written, written);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_response_body) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  uint32_t written = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, response_handle)
  m3ApiGetArgMem(uint8_t *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->http_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  rc = turbo_wasm3_http_registry_response_body(vm->http_registry, response_handle,
                                               buffer, (size_t)buffer_size, &written);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_written, written);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_response_error) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  uint32_t written = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, response_handle)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->http_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  rc = turbo_wasm3_http_registry_response_error(vm->http_registry, response_handle,
                                                buffer, (size_t)buffer_size, &written);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_written, written);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_response_close) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, response_handle)

  if (!vm || !vm->http_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(
      turbo_wasm3_http_registry_close_response(vm->http_registry, response_handle));
}

m3ApiRawFunction(turbo_wasm3_host_redis_client_open) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *host = NULL;
  uint32_t client_handle = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArgMem(const uint8_t *, host_data)
  m3ApiGetArg(uint32_t, host_len)
  m3ApiGetArg(uint32_t, port)
  m3ApiGetArgMem(uint32_t *, out_client_handle)

  if (!vm || !vm->redis_registry || !out_client_handle || host_len == 0 ||
      port == 0 || port > 65535U) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_client_handle, sizeof(uint32_t));
  m3ApiCheckMem(host_data, host_len);
  m3ApiWriteMem32(out_client_handle, 0);

  host = turbo_wasm3_copy_guest_bytes(host_data, host_len);
  if (!host) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_wasm3_redis_registry_open_client(vm->redis_registry, host,
                                              (uint16_t)port, &client_handle);
  free(host);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_client_handle, client_handle);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_redis_client_close) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)

  if (!vm || !vm->redis_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(
      turbo_wasm3_redis_registry_close_client(vm->redis_registry, client_handle));
}

m3ApiRawFunction(turbo_wasm3_host_redis_client_error) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  uint32_t written = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->redis_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  rc = turbo_wasm3_redis_registry_client_error(vm->redis_registry, client_handle,
                                               buffer, (size_t)buffer_size,
                                               &written);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_written, written);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_redis_command) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  const uint32_t *argv_offsets = NULL;
  const uint32_t *argv_lens_mem = NULL;
  const char **argv = NULL;
  uint32_t *argv_lens = NULL;
  uint32_t reply_handle = 0;
  uint32_t i;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)
  m3ApiGetArg(uint32_t, argc)
  m3ApiGetArgMem(const uint32_t *, argv_offsets_arg)
  m3ApiGetArgMem(const uint32_t *, argv_lens_arg)
  m3ApiGetArgMem(uint32_t *, out_reply_handle)

  if (!vm || !vm->redis_registry || !out_reply_handle || argc == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_reply_handle, sizeof(uint32_t));
  m3ApiCheckMem(argv_offsets_arg, (size_t)argc * sizeof(uint32_t));
  m3ApiCheckMem(argv_lens_arg, (size_t)argc * sizeof(uint32_t));
  m3ApiWriteMem32(out_reply_handle, 0);

  argv_offsets = argv_offsets_arg;
  argv_lens_mem = argv_lens_arg;
  argv = (const char **)calloc(argc, sizeof(*argv));
  argv_lens = (uint32_t *)calloc(argc, sizeof(*argv_lens));
  if (!argv || !argv_lens) {
    free(argv_lens);
    free(argv);
    m3ApiReturn(TURBO_ENOMEM);
  }

  for (i = 0; i < argc; ++i) {
    uint32_t arg_len = m3ApiReadMem32(&argv_lens_mem[i]);
    uint32_t arg_offset = m3ApiReadMem32(&argv_offsets[i]);

    argv_lens[i] = arg_len;
    if (arg_len == 0) {
      argv[i] = "";
      continue;
    }

    argv[i] = (const char *)m3ApiOffsetToPtr(arg_offset);
    m3ApiCheckMem(argv[i], arg_len);
  }

  rc = turbo_wasm3_redis_registry_command(vm->redis_registry, client_handle, argc,
                                          argv, argv_lens, &reply_handle);
  free(argv_lens);
  free(argv);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_reply_handle, reply_handle);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_redis_reply_close) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, reply_handle)

  if (!vm || !vm->redis_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(
      turbo_wasm3_redis_registry_close_reply(vm->redis_registry, reply_handle));
}

m3ApiRawFunction(turbo_wasm3_host_redis_reply_type) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  int32_t type = TURBO_WASM3_REDIS_REPLY_NULL;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, reply_handle)
  m3ApiGetArgMem(int32_t *, out_type)

  if (!vm || !vm->redis_registry || !out_type) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_type, sizeof(int32_t));
  m3ApiWriteMem32(out_type, (uint32_t)TURBO_WASM3_REDIS_REPLY_NULL);

  rc = turbo_wasm3_redis_registry_reply_type(vm->redis_registry, reply_handle,
                                             &type);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_type, (uint32_t)type);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_redis_reply_i64) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  int64_t value = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, reply_handle)
  m3ApiGetArgMem(int64_t *, out_value)

  if (!vm || !vm->redis_registry || !out_value) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_value, sizeof(int64_t));
  m3ApiWriteMem64(out_value, 0);

  rc = turbo_wasm3_redis_registry_reply_int64(vm->redis_registry, reply_handle,
                                              &value);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem64(out_value, (uint64_t)value);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_redis_reply_text) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  uint32_t written = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, reply_handle)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->redis_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  rc = turbo_wasm3_redis_registry_reply_text(vm->redis_registry, reply_handle,
                                             buffer, (size_t)buffer_size,
                                             &written);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_written, written);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_redis_reply_array_len) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  uint32_t array_len = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, reply_handle)
  m3ApiGetArgMem(uint32_t *, out_len)

  if (!vm || !vm->redis_registry || !out_len) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_len, sizeof(uint32_t));
  m3ApiWriteMem32(out_len, 0);

  rc = turbo_wasm3_redis_registry_reply_array_len(vm->redis_registry,
                                                  reply_handle, &array_len);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_len, array_len);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_redis_reply_array_at) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  uint32_t child_handle = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, reply_handle)
  m3ApiGetArg(uint32_t, index)
  m3ApiGetArgMem(uint32_t *, out_child_reply_handle)

  if (!vm || !vm->redis_registry || !out_child_reply_handle) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_child_reply_handle, sizeof(uint32_t));
  m3ApiWriteMem32(out_child_reply_handle, 0);

  rc = turbo_wasm3_redis_registry_reply_array_at(
      vm->redis_registry, reply_handle, index, &child_handle);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_child_reply_handle, child_handle);
  m3ApiReturn(0);
}

static M3Result turbo_wasm3_vm_link_http_host_cb(turbo_wasm3_vm_t *vm,
                                                 IM3Module module,
                                                 void *user_data) {
  (void)user_data;
  return turbo_wasm3_vm_link_http_host(vm, module);
}

static M3Result turbo_wasm3_vm_link_redis_host_cb(turbo_wasm3_vm_t *vm,
                                                  IM3Module module,
                                                  void *user_data) {
  (void)user_data;
  return turbo_wasm3_vm_link_redis_host(vm, module);
}

static M3Result turbo_wasm3_vm_link_host_cb(turbo_wasm3_vm_t *vm,
                                            IM3Module module,
                                            void *user_data) {
  (void)user_data;
  return turbo_wasm3_vm_link_host(vm, module);
}

turbo_wasm3_vm_t *
turbo_wasm3_vm_create(uint32_t stack_size, void *runtime_user_data,
                      size_t socket_capacity) {
  turbo_wasm3_vm_t *vm;

  vm = (turbo_wasm3_vm_t *)calloc(1, sizeof(*vm));
  if (!vm) {
    return NULL;
  }

  vm->env = m3_NewEnvironment();
  if (!vm->env) {
    turbo_wasm3_vm_destroy(vm);
    return NULL;
  }

  vm->runtime = m3_NewRuntime(vm->env, stack_size ? stack_size : (64U * 1024U),
                              runtime_user_data);
  if (!vm->runtime) {
    turbo_wasm3_vm_destroy(vm);
    return NULL;
  }

  vm->socket_registry =
      turbo_wasm3_socket_registry_create(socket_capacity ? socket_capacity : 16);
  if (!vm->socket_registry) {
    turbo_wasm3_vm_destroy(vm);
    return NULL;
  }

  vm->db_registry = turbo_wasm3_db_registry_create(8);
  if (!vm->db_registry) {
    turbo_wasm3_vm_destroy(vm);
    return NULL;
  }

  vm->http_registry = turbo_wasm3_http_registry_create(4);
  if (!vm->http_registry) {
    turbo_wasm3_vm_destroy(vm);
    return NULL;
  }

  vm->redis_registry = turbo_wasm3_redis_registry_create(4);
  if (!vm->redis_registry) {
    turbo_wasm3_vm_destroy(vm);
    return NULL;
  }

  vm->parser_registry = turbo_wasm3_parser_registry_create(8);
  if (!vm->parser_registry) {
    turbo_wasm3_vm_destroy(vm);
    return NULL;
  }

  vm->wasi_context = m3_NewWasiContext();
  if (!vm->wasi_context) {
    turbo_wasm3_vm_destroy(vm);
    return NULL;
  }

  if (turbo_wasm3_vm_bind_wasi(vm) != m3Err_none) {
    turbo_wasm3_vm_destroy(vm);
    return NULL;
  }

  return vm;
}

void turbo_wasm3_vm_destroy(turbo_wasm3_vm_t *vm) {
  size_t i;

  if (!vm) {
    return;
  }

  if (vm->wasi_context) {
    turbo_wasm3_socket_registry_unbind_wasi(vm->wasi_context);
  }
  turbo_wasm3_vm_clear_wasi_args(vm);
  turbo_wasm3_vm_clear_host_linkers(vm);
  turbo_wasm3_http_registry_destroy(vm->http_registry);
  vm->http_registry = NULL;
  turbo_wasm3_redis_registry_destroy(vm->redis_registry);
  vm->redis_registry = NULL;
  turbo_wasm3_db_registry_destroy(vm->db_registry);
  vm->db_registry = NULL;
  turbo_wasm3_parser_registry_destroy(vm->parser_registry);
  vm->parser_registry = NULL;
  turbo_wasm3_socket_registry_destroy(vm->socket_registry);
  vm->socket_registry = NULL;

  if (vm->runtime) {
    m3_FreeRuntime(vm->runtime);
    vm->runtime = NULL;
  }
  if (vm->wasi_context) {
    m3_FreeWasiContext(vm->wasi_context);
    vm->wasi_context = NULL;
  }
  if (vm->env) {
    m3_FreeEnvironment(vm->env);
    vm->env = NULL;
  }

  for (i = 0; i < vm->blob_count; ++i) {
    free(vm->blobs[i].bytes);
  }
  free(vm->blobs);
  free(vm);
}

IM3Runtime turbo_wasm3_vm_get_runtime(turbo_wasm3_vm_t *vm) {
  return vm ? vm->runtime : NULL;
}

m3_wasi_context_t *turbo_wasm3_vm_get_wasi_context(turbo_wasm3_vm_t *vm) {
  return vm ? vm->wasi_context : NULL;
}

turbo_wasm3_db_registry_t *turbo_wasm3_vm_get_db_registry(turbo_wasm3_vm_t *vm) {
  return vm ? vm->db_registry : NULL;
}

turbo_wasm3_http_registry_t *turbo_wasm3_vm_get_http_registry(turbo_wasm3_vm_t *vm) {
  return vm ? vm->http_registry : NULL;
}

turbo_wasm3_redis_registry_t *turbo_wasm3_vm_get_redis_registry(turbo_wasm3_vm_t *vm) {
  return vm ? vm->redis_registry : NULL;
}

turbo_wasm3_parser_registry_t *turbo_wasm3_vm_get_parser_registry(turbo_wasm3_vm_t *vm) {
  return vm ? vm->parser_registry : NULL;
}

void turbo_wasm3_vm_set_host_user_data(turbo_wasm3_vm_t *vm,
                                       void *host_user_data) {
  if (!vm) {
    return;
  }

  vm->host_user_data = host_user_data;
}

void *turbo_wasm3_vm_get_host_user_data(turbo_wasm3_vm_t *vm) {
  return vm ? vm->host_user_data : NULL;
}

int turbo_wasm3_vm_add_host_linker(turbo_wasm3_vm_t *vm,
                                   turbo_wasm3_host_linker_fn linker,
                                   void *user_data) {
  size_t i;
  int rc;

  if (!vm || !linker) {
    return TURBO_EINVAL;
  }

  for (i = 0; i < vm->host_linker_count; ++i) {
    if (vm->host_linkers[i].linker == linker &&
        vm->host_linkers[i].user_data == user_data) {
      return 0;
    }
  }

  rc = turbo_wasm3_vm_reserve_host_linkers(vm, vm->host_linker_count + 1);
  if (rc != 0) {
    return rc;
  }

  vm->host_linkers[vm->host_linker_count].linker = linker;
  vm->host_linkers[vm->host_linker_count].user_data = user_data;
  vm->host_linker_count++;
  return 0;
}

void turbo_wasm3_vm_clear_host_linkers(turbo_wasm3_vm_t *vm) {
  if (!vm) {
    return;
  }

  free(vm->host_linkers);
  vm->host_linkers = NULL;
  vm->host_linker_count = 0;
  vm->host_linker_capacity = 0;
}

M3Result turbo_wasm3_vm_link_host_modules(turbo_wasm3_vm_t *vm,
                                          IM3Module module) {
  size_t i;
  M3Result result = m3Err_none;

  if (!vm || !module) {
    return m3Err_wasmMalformed;
  }

  for (i = 0; i < vm->host_linker_count; ++i) {
    result = vm->host_linkers[i].linker(vm, module, vm->host_linkers[i].user_data);
    if (result) {
      return result;
    }
  }

  return m3Err_none;
}

int turbo_wasm3_vm_enable_host(turbo_wasm3_vm_t *vm) {
  return turbo_wasm3_vm_add_host_linker(vm, turbo_wasm3_vm_link_host_cb, NULL);
}

int turbo_wasm3_vm_enable_http_host(turbo_wasm3_vm_t *vm) {
  return turbo_wasm3_vm_add_host_linker(vm, turbo_wasm3_vm_link_http_host_cb, NULL);
}

int turbo_wasm3_vm_enable_redis_host(turbo_wasm3_vm_t *vm) {
  return turbo_wasm3_vm_add_host_linker(vm, turbo_wasm3_vm_link_redis_host_cb,
                                        NULL);
}

M3Result turbo_wasm3_vm_link_host(turbo_wasm3_vm_t *vm, IM3Module module) {
  const char *mod = "TurboNet";
  M3Result result = m3Err_none;

  if (!vm || !module) {
    return m3Err_wasmMalformed;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "abi_version", "i()", &turbo_wasm3_host_abi_version, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "clock_time_ms", "I()", &turbo_wasm3_host_clock_time_ms,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "socket_send", "i(i*i*)", &turbo_wasm3_host_socket_send,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "socket_recv", "i(i*i*)", &turbo_wasm3_host_socket_recv,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "socket_release", "i(i)",
      &turbo_wasm3_host_socket_release, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_open", "i(*i*)", &turbo_wasm3_host_db_open, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_close", "i(i)", &turbo_wasm3_host_db_close, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_exec", "i(i*i*)", &turbo_wasm3_host_db_exec, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_error", "i(i*i*)", &turbo_wasm3_host_db_error, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_stmt_error", "i(i*i*)", &turbo_wasm3_host_db_stmt_error,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_prepare", "i(i*i*)", &turbo_wasm3_host_db_prepare, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_finalize", "i(i)", &turbo_wasm3_host_db_finalize, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_reset", "i(i)", &turbo_wasm3_host_db_reset, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_step", "i(i*)", &turbo_wasm3_host_db_step, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_bind_i64", "i(iiI)", &turbo_wasm3_host_db_bind_i64, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_bind_f64", "i(iiF)", &turbo_wasm3_host_db_bind_f64, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_bind_null", "i(ii)", &turbo_wasm3_host_db_bind_null, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_bind_blob", "i(ii*i)", &turbo_wasm3_host_db_bind_blob,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_bind_text", "i(ii*i)", &turbo_wasm3_host_db_bind_text,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_column_type", "i(ii*)", &turbo_wasm3_host_db_column_type,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_column_i64", "i(ii*)", &turbo_wasm3_host_db_column_i64,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_column_f64", "i(ii*)", &turbo_wasm3_host_db_column_f64,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_column_blob", "i(ii*i*)",
      &turbo_wasm3_host_db_column_blob, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_column_text", "i(ii*i*)",
      &turbo_wasm3_host_db_column_text, vm));
  if (result) {
    return result;
  }

  /* Parser functions */
  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "parser_json_parse", "i(*i*)", &turbo_wasm3_host_parser_json_parse, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "parser_csv_parse", "i(*i*)", &turbo_wasm3_host_parser_csv_parse, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "parser_xml_parse", "i(*i*)", &turbo_wasm3_host_parser_xml_parse, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "parser_ini_parse", "i(*i*)", &turbo_wasm3_host_parser_ini_parse, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "parser_free", "i(i)", &turbo_wasm3_host_parser_free, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "json_get_string", "i(i*i*i*)", &turbo_wasm3_host_json_get_string, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "json_get_int", "i(i*i*)", &turbo_wasm3_host_json_get_int, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "json_get_bool", "i(i*i*)", &turbo_wasm3_host_json_get_bool, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "json_array_size", "i(i*i*)", &turbo_wasm3_host_json_array_size, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "csv_row_count", "i(i*)", &turbo_wasm3_host_csv_row_count, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "csv_column_count", "i(i*)", &turbo_wasm3_host_csv_column_count, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "csv_get_cell", "i(iii*i*)", &turbo_wasm3_host_csv_get_cell, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "csv_find_column", "i(i*i*)", &turbo_wasm3_host_csv_find_column, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "xml_root_name", "i(i*i*)", &turbo_wasm3_host_xml_root_name, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "xml_get_text", "i(i*i*i*)", &turbo_wasm3_host_xml_get_text, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "xml_count", "i(i*i*)", &turbo_wasm3_host_xml_count, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "ini_get_string", "i(i*i*i*i*)",
      &turbo_wasm3_host_ini_get_string, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "ini_get_int", "i(i*i*i*)", &turbo_wasm3_host_ini_get_int,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "ini_get_bool", "i(i*i*i*)", &turbo_wasm3_host_ini_get_bool,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "ini_get_double", "i(i*i*i*)",
      &turbo_wasm3_host_ini_get_double, vm));
  if (result) {
    return result;
  }

  return m3Err_none;
}

M3Result turbo_wasm3_vm_link_http_host(turbo_wasm3_vm_t *vm, IM3Module module) {
  const char *mod = "TurboNet";
  M3Result result = m3Err_none;

  if (!vm || !module) {
    return m3Err_wasmMalformed;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_client_open", "i(*i*)", &turbo_wasm3_host_http_client_open,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_client_close", "i(i)",
      &turbo_wasm3_host_http_client_close, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_client_set_timeout", "i(ii)",
      &turbo_wasm3_host_http_client_set_timeout, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_client_set_default_header", "i(i*i*i)",
      &turbo_wasm3_host_http_client_set_default_header, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_client_remove_default_header", "i(i*i)",
      &turbo_wasm3_host_http_client_remove_default_header, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_client_clear_default_headers", "i(i)",
      &turbo_wasm3_host_http_client_clear_default_headers, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_client_set_basic_auth", "i(i*i*i)",
      &turbo_wasm3_host_http_client_set_basic_auth, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_client_set_bearer_token", "i(i*i)",
      &turbo_wasm3_host_http_client_set_bearer_token, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_client_clear_auth", "i(i)",
      &turbo_wasm3_host_http_client_clear_auth, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_client_set_proxy", "i(i*ii*i*i)",
      &turbo_wasm3_host_http_client_set_proxy, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_client_clear_proxy", "i(i)",
      &turbo_wasm3_host_http_client_clear_proxy, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_request", "i(ii*i*i*)", &turbo_wasm3_host_http_request,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_request_with_headers", "i(ii*i*i*i*)",
      &turbo_wasm3_host_http_request_with_headers, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_stream_get", "i(i*i*)",
      &turbo_wasm3_host_http_stream_get, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_sse_get", "i(i*i*)",
      &turbo_wasm3_host_http_sse_get, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_response_status", "i(i*)",
      &turbo_wasm3_host_http_response_status, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_response_error_code", "i(i*)",
      &turbo_wasm3_host_http_response_error_code, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_response_is_sse", "i(i*)",
      &turbo_wasm3_host_http_response_is_sse, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_response_chunk_count", "i(i*)",
      &turbo_wasm3_host_http_response_chunk_count, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_response_chunk", "i(ii*i*)",
      &turbo_wasm3_host_http_response_chunk, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_response_header", "i(i*i*i*)",
      &turbo_wasm3_host_http_response_header, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_response_headers", "i(i*i*)",
      &turbo_wasm3_host_http_response_headers, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_response_body", "i(i*i*)",
      &turbo_wasm3_host_http_response_body, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_response_error", "i(i*i*)",
      &turbo_wasm3_host_http_response_error, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_response_close", "i(i)",
      &turbo_wasm3_host_http_response_close, vm));
  if (result) {
    return result;
  }

  return m3Err_none;
}

M3Result turbo_wasm3_vm_link_redis_host(turbo_wasm3_vm_t *vm, IM3Module module) {
  const char *mod = "TurboNet";
  M3Result result = m3Err_none;

  if (!vm || !module) {
    return m3Err_wasmMalformed;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "redis_client_open", "i(*ii*)",
      &turbo_wasm3_host_redis_client_open, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "redis_client_close", "i(i)",
      &turbo_wasm3_host_redis_client_close, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "redis_client_error", "i(i*i*)",
      &turbo_wasm3_host_redis_client_error, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "redis_command", "i(ii***)", &turbo_wasm3_host_redis_command,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "redis_reply_close", "i(i)",
      &turbo_wasm3_host_redis_reply_close, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "redis_reply_type", "i(i*)",
      &turbo_wasm3_host_redis_reply_type, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "redis_reply_i64", "i(i*)",
      &turbo_wasm3_host_redis_reply_i64, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "redis_reply_text", "i(i*i*)",
      &turbo_wasm3_host_redis_reply_text, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "redis_reply_array_len", "i(i*)",
      &turbo_wasm3_host_redis_reply_array_len, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "redis_reply_array_at", "i(ii*)",
      &turbo_wasm3_host_redis_reply_array_at, vm));
  if (result) {
    return result;
  }

  return m3Err_none;
}

/* Parser Registry */
static int
turbo_wasm3_parser_registry_reserve(turbo_wasm3_parser_registry_t *registry, size_t new_capacity) {
  turbo_wasm3_parser_entry_t *new_entries;

  if (!registry) {
    return TURBO_EINVAL;
  }

  new_entries = (turbo_wasm3_parser_entry_t *)realloc(registry->entries,
                                                       new_capacity * sizeof(*new_entries));
  if (!new_entries) {
    return TURBO_ENOMEM;
  }

  registry->entries = new_entries;
  registry->capacity = new_capacity;
  return 0;
}

turbo_wasm3_parser_registry_t *
turbo_wasm3_parser_registry_create(size_t initial_capacity) {
  turbo_wasm3_parser_registry_t *registry;
  int rc;

  registry = (turbo_wasm3_parser_registry_t *)calloc(1, sizeof(*registry));
  if (!registry) {
    return NULL;
  }

  registry->next_handle = 0x1000u;
  rc = turbo_wasm3_parser_registry_reserve(registry,
                                           initial_capacity ? initial_capacity : 8);
  if (rc != 0) {
    free(registry);
    return NULL;
  }

  return registry;
}

static void
turbo_wasm3_parser_entry_clear(turbo_wasm3_parser_entry_t *entry) {
  if (!entry) {
    return;
  }

  if (entry->doc) {
    switch (entry->type) {
    case TURBO_WASM3_PARSER_TYPE_JSON:
      turbo_free_json(&entry->doc);
      break;
    case TURBO_WASM3_PARSER_TYPE_CSV:
      turbo_free_csv(&entry->doc);
      break;
    case TURBO_WASM3_PARSER_TYPE_XML:
      turbo_free_xml(&entry->doc);
      break;
    case TURBO_WASM3_PARSER_TYPE_INI:
      turbo_free_ini(&entry->doc);
      break;
    case TURBO_WASM3_PARSER_TYPE_TOML:
      turbo_free_toml(&entry->doc);
      break;
    default:
      free(entry->doc);
      break;
    }
    entry->doc = NULL;
  }
  entry->type = TURBO_WASM3_PARSER_TYPE_NONE;
  entry->handle = 0;
}

void
turbo_wasm3_parser_registry_destroy(turbo_wasm3_parser_registry_t *registry) {
  size_t i;

  if (!registry) {
    return;
  }

  for (i = 0; i < registry->count; ++i) {
    turbo_wasm3_parser_entry_clear(&registry->entries[i]);
  }

  free(registry->entries);
  free(registry);
}

static int
turbo_wasm3_parser_registry_find(turbo_wasm3_parser_registry_t *registry, uint32_t handle) {
  size_t i;

  if (!registry) {
    return -1;
  }

  for (i = 0; i < registry->count; ++i) {
    if (registry->entries[i].handle == handle) {
      return (int)i;
    }
  }

  return -1;
}

static int
turbo_wasm3_parser_registry_add(turbo_wasm3_parser_registry_t *registry, int type, void *doc, uint32_t *out_handle) {
  int rc;
  size_t i;

  if (!registry || !doc || !out_handle) {
    return TURBO_EINVAL;
  }

  if (registry->count >= registry->capacity) {
    rc = turbo_wasm3_parser_registry_reserve(registry, registry->capacity * 2);
    if (rc != 0) {
      return rc;
    }
  }

  i = registry->count++;
  registry->entries[i].handle = registry->next_handle++;
  registry->entries[i].type = type;
  registry->entries[i].doc = doc;

  *out_handle = registry->entries[i].handle;
  return 0;
}

int
turbo_wasm3_parser_free(turbo_wasm3_vm_t *vm, uint32_t handle) {
  int idx;

  if (!vm || !vm->parser_registry) {
    return TURBO_EINVAL;
  }

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    return TURBO_EBADF;
  }

  turbo_wasm3_parser_entry_clear(&vm->parser_registry->entries[idx]);

  if (idx != (int)(vm->parser_registry->count - 1)) {
    vm->parser_registry->entries[idx] = vm->parser_registry->entries[vm->parser_registry->count - 1];
  }
  vm->parser_registry->count--;

  return 0;
}

/* JSON host functions */
m3ApiRawFunction(turbo_wasm3_host_parser_json_parse) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *data = NULL;
  int rc;
  uint32_t handle = 0;
  void *doc = NULL;

  m3ApiReturnType(int32_t)
  m3ApiGetArgMem(const uint8_t *, data_ptr)
  m3ApiGetArg(uint32_t, data_len)
  m3ApiGetArgMem(uint32_t *, out_handle)

  if (!vm || !vm->parser_registry || !out_handle) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_handle, sizeof(uint32_t));
  if (data_len != 0) {
    m3ApiCheckMem(data_ptr, data_len);
  }
  m3ApiWriteMem32(out_handle, 0);

  if (data_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  data = turbo_wasm3_copy_guest_bytes(data_ptr, data_len);
  if (!data) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_parse_json((const uint8_t *)data, data_len, &doc);
  free(data);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  rc = turbo_wasm3_parser_registry_add(vm->parser_registry, TURBO_WASM3_PARSER_TYPE_JSON, doc, &handle);
  if (rc != 0) {
    turbo_free_json(&doc);
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_handle, handle);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_parser_csv_parse) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *data = NULL;
  turbo_csv_options_t opts = {1, ',', '"', 1};
  int rc;
  uint32_t handle = 0;
  void *doc = NULL;

  m3ApiReturnType(int32_t)
  m3ApiGetArgMem(const uint8_t *, data_ptr)
  m3ApiGetArg(uint32_t, data_len)
  m3ApiGetArgMem(uint32_t *, out_handle)

  if (!vm || !vm->parser_registry || !out_handle) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_handle, sizeof(uint32_t));
  if (data_len != 0) {
    m3ApiCheckMem(data_ptr, data_len);
  }
  m3ApiWriteMem32(out_handle, 0);

  if (data_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  data = turbo_wasm3_copy_guest_bytes(data_ptr, data_len);
  if (!data) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_parse_csv_opts((const uint8_t *)data, data_len, &opts, &doc);
  free(data);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  rc = turbo_wasm3_parser_registry_add(vm->parser_registry, TURBO_WASM3_PARSER_TYPE_CSV, doc, &handle);
  if (rc != 0) {
    turbo_free_csv(&doc);
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_handle, handle);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_parser_xml_parse) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *data = NULL;
  int rc;
  uint32_t handle = 0;
  void *doc = NULL;

  m3ApiReturnType(int32_t)
  m3ApiGetArgMem(const uint8_t *, data_ptr)
  m3ApiGetArg(uint32_t, data_len)
  m3ApiGetArgMem(uint32_t *, out_handle)

  if (!vm || !vm->parser_registry || !out_handle) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_handle, sizeof(uint32_t));
  if (data_len != 0) {
    m3ApiCheckMem(data_ptr, data_len);
  }
  m3ApiWriteMem32(out_handle, 0);

  if (data_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  data = turbo_wasm3_copy_guest_bytes(data_ptr, data_len);
  if (!data) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_parse_xml((const uint8_t *)data, data_len, &doc);
  free(data);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  rc = turbo_wasm3_parser_registry_add(vm->parser_registry, TURBO_WASM3_PARSER_TYPE_XML, doc, &handle);
  if (rc != 0) {
    turbo_free_xml(&doc);
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_handle, handle);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_parser_ini_parse) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *data = NULL;
  int rc;
  uint32_t handle = 0;
  void *doc = NULL;

  m3ApiReturnType(int32_t)
  m3ApiGetArgMem(const uint8_t *, data_ptr)
  m3ApiGetArg(uint32_t, data_len)
  m3ApiGetArgMem(uint32_t *, out_handle)

  if (!vm || !vm->parser_registry || !out_handle) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_handle, sizeof(uint32_t));
  if (data_len != 0) {
    m3ApiCheckMem(data_ptr, data_len);
  }
  m3ApiWriteMem32(out_handle, 0);

  if (data_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  data = turbo_wasm3_copy_guest_bytes(data_ptr, data_len);
  if (!data) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_parse_ini((const uint8_t *)data, data_len, &doc);
  free(data);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  rc = turbo_wasm3_parser_registry_add(vm->parser_registry, TURBO_WASM3_PARSER_TYPE_INI, doc, &handle);
  if (rc != 0) {
    turbo_free_ini(&doc);
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_handle, handle);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_parser_free) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)

  if (!vm || !vm->parser_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(turbo_wasm3_parser_free(vm, handle));
}

/* JSON accessors */
m3ApiRawFunction(turbo_wasm3_host_json_get_string) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *path = NULL;
  int idx, rc;
  turbo_wasm3_parser_entry_t *entry;
  json_value_t *val = NULL, *result;
  char *out_str = NULL;
  size_t out_len = 0;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(const uint8_t *, path_ptr)
  m3ApiGetArg(uint32_t, path_len)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->parser_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (path_len != 0) {
    m3ApiCheckMem(path_ptr, path_len);
  }
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_JSON) {
    m3ApiReturn(TURBO_EINVAL);
  }

  if (path_len > 0) {
    path = turbo_wasm3_copy_guest_bytes(path_ptr, path_len);
    if (!path) {
      m3ApiReturn(TURBO_ENOMEM);
    }
    result = turbo_json_object_get((json_value_t *)entry->doc, path);
    free(path);
  } else {
    result = (json_value_t *)entry->doc;
  }

  if (!result) {
    m3ApiReturn(TURBO_ENOENT);
  }

  rc = turbo_json_type(result);
  if (rc == TURBO_JSON_STRING) {
    out_str = (char *)turbo_json_string(result);
    out_len = strlen(out_str);
  } else if (rc == TURBO_JSON_NUMBER) {
    char num_buf[64];
    snprintf(num_buf, sizeof(num_buf), "%g", turbo_json_number(result));
    out_str = num_buf;
    out_len = strlen(num_buf);
  } else if (rc == TURBO_JSON_BOOL) {
    out_str = turbo_json_bool(result) ? "true" : "false";
    out_len = strlen(out_str);
  } else if (rc == TURBO_JSON_NULL) {
    m3ApiReturn(0);
  } else {
    m3ApiReturn(TURBO_EINVAL);
  }

  if (buffer_size > 0) {
    size_t copy_len = out_len < buffer_size - 1 ? out_len : buffer_size - 1;
    memcpy(buffer, out_str, copy_len);
    buffer[copy_len] = '\0';
    m3ApiWriteMem32(out_written, (uint32_t)copy_len);
  } else {
    m3ApiWriteMem32(out_written, (uint32_t)out_len);
  }

  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_json_get_int) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *path = NULL;
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  json_value_t *result;
  double val = 0;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(const uint8_t *, path_ptr)
  m3ApiGetArg(uint32_t, path_len)
  m3ApiGetArgMem(int32_t *, out_value)

  if (!vm || !vm->parser_registry || !out_value) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_value, sizeof(int32_t));
  if (path_len != 0) {
    m3ApiCheckMem(path_ptr, path_len);
  }

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_JSON) {
    m3ApiReturn(TURBO_EINVAL);
  }

  if (path_len > 0) {
    path = turbo_wasm3_copy_guest_bytes(path_ptr, path_len);
    if (!path) {
      m3ApiReturn(TURBO_ENOMEM);
    }
    result = turbo_json_object_get((json_value_t *)entry->doc, path);
    free(path);
  } else {
    result = (json_value_t *)entry->doc;
  }

  if (!result) {
    m3ApiReturn(TURBO_ENOENT);
  }

  val = turbo_json_number(result);
  m3ApiWriteMem32(out_value, (int32_t)val);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_json_get_bool) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *path = NULL;
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  json_value_t *result;
  bool val = false;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(const uint8_t *, path_ptr)
  m3ApiGetArg(uint32_t, path_len)
  m3ApiGetArgMem(int32_t *, out_value)

  if (!vm || !vm->parser_registry || !out_value) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_value, sizeof(int32_t));
  if (path_len != 0) {
    m3ApiCheckMem(path_ptr, path_len);
  }

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_JSON) {
    m3ApiReturn(TURBO_EINVAL);
  }

  if (path_len > 0) {
    path = turbo_wasm3_copy_guest_bytes(path_ptr, path_len);
    if (!path) {
      m3ApiReturn(TURBO_ENOMEM);
    }
    result = turbo_json_object_get((json_value_t *)entry->doc, path);
    free(path);
  } else {
    result = (json_value_t *)entry->doc;
  }

  if (!result) {
    m3ApiReturn(TURBO_ENOENT);
  }

  val = turbo_json_bool(result);
  m3ApiWriteMem32(out_value, val ? 1 : 0);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_json_array_size) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *path = NULL;
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  json_value_t *result;
  size_t size = 0;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(const uint8_t *, path_ptr)
  m3ApiGetArg(uint32_t, path_len)
  m3ApiGetArgMem(uint32_t *, out_size)

  if (!vm || !vm->parser_registry || !out_size) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_size, sizeof(uint32_t));
  if (path_len != 0) {
    m3ApiCheckMem(path_ptr, path_len);
  }

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_JSON) {
    m3ApiReturn(TURBO_EINVAL);
  }

  if (path_len > 0) {
    path = turbo_wasm3_copy_guest_bytes(path_ptr, path_len);
    if (!path) {
      m3ApiReturn(TURBO_ENOMEM);
    }
    result = turbo_json_object_get((json_value_t *)entry->doc, path);
    free(path);
  } else {
    result = (json_value_t *)entry->doc;
  }

  if (!result) {
    m3ApiReturn(TURBO_ENOENT);
  }

  size = turbo_json_array_size(result);
  m3ApiWriteMem32(out_size, (uint32_t)size);
  m3ApiReturn(0);
}

/* CSV accessors */
m3ApiRawFunction(turbo_wasm3_host_csv_row_count) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  size_t count = 0;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(uint32_t *, out_count)

  if (!vm || !vm->parser_registry || !out_count) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_count, sizeof(uint32_t));

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_CSV) {
    m3ApiReturn(TURBO_EINVAL);
  }

  count = turbo_csv_row_count((turbo_csv_doc_t *)entry->doc);
  m3ApiWriteMem32(out_count, (uint32_t)count);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_csv_column_count) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  size_t count = 0;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(uint32_t *, out_count)

  if (!vm || !vm->parser_registry || !out_count) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_count, sizeof(uint32_t));

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_CSV) {
    m3ApiReturn(TURBO_EINVAL);
  }

  count = turbo_csv_column_count((turbo_csv_doc_t *)entry->doc);
  m3ApiWriteMem32(out_count, (uint32_t)count);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_csv_get_cell) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  const char *cell;
  size_t cell_len;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArg(uint32_t, row)
  m3ApiGetArg(uint32_t, col)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->parser_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_CSV) {
    m3ApiReturn(TURBO_EINVAL);
  }

  cell = turbo_csv_get((turbo_csv_doc_t *)entry->doc, row, col);
  if (!cell) {
    m3ApiReturn(TURBO_ENOENT);
  }

  cell_len = strlen(cell);
  if (buffer_size > 0) {
    size_t copy_len = cell_len < buffer_size - 1 ? cell_len : buffer_size - 1;
    memcpy(buffer, cell, copy_len);
    buffer[copy_len] = '\0';
    m3ApiWriteMem32(out_written, (uint32_t)copy_len);
  } else {
    m3ApiWriteMem32(out_written, (uint32_t)cell_len);
  }

  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_csv_find_column) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *name = NULL;
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  size_t col;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(const uint8_t *, name_ptr)
  m3ApiGetArg(uint32_t, name_len)
  m3ApiGetArgMem(uint32_t *, out_col)

  if (!vm || !vm->parser_registry || !out_col) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_col, sizeof(uint32_t));
  if (name_len != 0) {
    m3ApiCheckMem(name_ptr, name_len);
  }

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_CSV) {
    m3ApiReturn(TURBO_EINVAL);
  }

  if (name_len > 0) {
    name = turbo_wasm3_copy_guest_bytes(name_ptr, name_len);
    if (!name) {
      m3ApiReturn(TURBO_ENOMEM);
    }
    col = turbo_csv_find_column((turbo_csv_doc_t *)entry->doc, name);
    free(name);
  } else {
    m3ApiReturn(TURBO_EINVAL);
  }

  if (col == (size_t)-1) {
    m3ApiReturn(TURBO_ENOENT);
  }

  m3ApiWriteMem32(out_col, (uint32_t)col);
  m3ApiReturn(0);
}

/* XML accessors */
m3ApiRawFunction(turbo_wasm3_host_xml_root_name) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  turbo_xml_node_t *root;
  const char *name;
  size_t name_len;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->parser_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_XML) {
    m3ApiReturn(TURBO_EINVAL);
  }

  root = turbo_xml_root_element((turbo_xml_doc_t *)entry->doc);
  if (!root) {
    m3ApiReturn(TURBO_ENOENT);
  }

  name = turbo_xml_node_name(root);
  if (!name) {
    m3ApiReturn(TURBO_ENOENT);
  }

  name_len = strlen(name);
  if (buffer_size > 0) {
    size_t copy_len = name_len < buffer_size - 1 ? name_len : buffer_size - 1;
    memcpy(buffer, name, copy_len);
    buffer[copy_len] = '\0';
    m3ApiWriteMem32(out_written, (uint32_t)copy_len);
  } else {
    m3ApiWriteMem32(out_written, (uint32_t)name_len);
  }

  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_xml_get_text) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *xpath = NULL;
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  const char *text;
  size_t text_len;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(const uint8_t *, xpath_ptr)
  m3ApiGetArg(uint32_t, xpath_len)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->parser_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (xpath_len != 0) {
    m3ApiCheckMem(xpath_ptr, xpath_len);
  }
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_XML) {
    m3ApiReturn(TURBO_EINVAL);
  }

  if (xpath_len > 0) {
    xpath = turbo_wasm3_copy_guest_bytes(xpath_ptr, xpath_len);
    if (!xpath) {
      m3ApiReturn(TURBO_ENOMEM);
    }
    text = turbo_xml_get_text((turbo_xml_doc_t *)entry->doc, xpath);
    free(xpath);
  } else {
    m3ApiReturn(TURBO_EINVAL);
  }

  if (!text) {
    m3ApiReturn(TURBO_ENOENT);
  }

  text_len = strlen(text);
  if (buffer_size > 0) {
    size_t copy_len = text_len < buffer_size - 1 ? text_len : buffer_size - 1;
    memcpy(buffer, text, copy_len);
    buffer[copy_len] = '\0';
    m3ApiWriteMem32(out_written, (uint32_t)copy_len);
  } else {
    m3ApiWriteMem32(out_written, (uint32_t)text_len);
  }

  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_xml_count) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *xpath = NULL;
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  size_t count = 0;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(const uint8_t *, xpath_ptr)
  m3ApiGetArg(uint32_t, xpath_len)
  m3ApiGetArgMem(uint32_t *, out_count)

  if (!vm || !vm->parser_registry || !out_count) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_count, sizeof(uint32_t));
  if (xpath_len != 0) {
    m3ApiCheckMem(xpath_ptr, xpath_len);
  }

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_XML) {
    m3ApiReturn(TURBO_EINVAL);
  }

  if (xpath_len > 0) {
    xpath = turbo_wasm3_copy_guest_bytes(xpath_ptr, xpath_len);
    if (!xpath) {
      m3ApiReturn(TURBO_ENOMEM);
    }
    count = turbo_xml_count((turbo_xml_doc_t *)entry->doc, xpath);
    free(xpath);
  } else {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiWriteMem32(out_count, (uint32_t)count);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_ini_get_string) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *section = NULL;
  char *key = NULL;
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  const char *value;
  size_t value_len;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(const uint8_t *, section_ptr)
  m3ApiGetArg(uint32_t, section_len)
  m3ApiGetArgMem(const uint8_t *, key_ptr)
  m3ApiGetArg(uint32_t, key_len)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->parser_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (section_len != 0) {
    m3ApiCheckMem(section_ptr, section_len);
  }
  if (key_len != 0) {
    m3ApiCheckMem(key_ptr, key_len);
  }
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_INI) {
    m3ApiReturn(TURBO_EINVAL);
  }
  if (section_len == 0 || key_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  section = turbo_wasm3_copy_guest_bytes(section_ptr, section_len);
  if (!section) {
    m3ApiReturn(TURBO_ENOMEM);
  }
  key = turbo_wasm3_copy_guest_bytes(key_ptr, key_len);
  if (!key) {
    free(section);
    m3ApiReturn(TURBO_ENOMEM);
  }

  value = turbo_ini_get((const turbo_ini_t *)entry->doc, section, key);
  free(key);
  free(section);
  if (!value) {
    m3ApiReturn(TURBO_ENOENT);
  }

  value_len = strlen(value);
  if (buffer_size > 0) {
    size_t copy_len = value_len < buffer_size - 1 ? value_len : buffer_size - 1;
    memcpy(buffer, value, copy_len);
    buffer[copy_len] = '\0';
    m3ApiWriteMem32(out_written, (uint32_t)copy_len);
  } else {
    m3ApiWriteMem32(out_written, (uint32_t)value_len);
  }

  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_ini_get_int) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *section = NULL;
  char *key = NULL;
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  const char *value;
  int32_t parsed = 0;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(const uint8_t *, section_ptr)
  m3ApiGetArg(uint32_t, section_len)
  m3ApiGetArgMem(const uint8_t *, key_ptr)
  m3ApiGetArg(uint32_t, key_len)
  m3ApiGetArgMem(int32_t *, out_value)

  if (!vm || !vm->parser_registry || !out_value) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_value, sizeof(int32_t));
  if (section_len != 0) {
    m3ApiCheckMem(section_ptr, section_len);
  }
  if (key_len != 0) {
    m3ApiCheckMem(key_ptr, key_len);
  }
  m3ApiWriteMem32(out_value, 0);

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_INI) {
    m3ApiReturn(TURBO_EINVAL);
  }
  if (section_len == 0 || key_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  section = turbo_wasm3_copy_guest_bytes(section_ptr, section_len);
  if (!section) {
    m3ApiReturn(TURBO_ENOMEM);
  }
  key = turbo_wasm3_copy_guest_bytes(key_ptr, key_len);
  if (!key) {
    free(section);
    m3ApiReturn(TURBO_ENOMEM);
  }

  value = turbo_ini_get((const turbo_ini_t *)entry->doc, section, key);
  parsed = (int32_t)turbo_ini_get_int((const turbo_ini_t *)entry->doc, section,
                                      key, 0);
  free(key);
  free(section);
  if (!value) {
    m3ApiReturn(TURBO_ENOENT);
  }

  m3ApiWriteMem32(out_value, (uint32_t)parsed);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_ini_get_bool) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *section = NULL;
  char *key = NULL;
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  const char *value;
  int32_t parsed = 0;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(const uint8_t *, section_ptr)
  m3ApiGetArg(uint32_t, section_len)
  m3ApiGetArgMem(const uint8_t *, key_ptr)
  m3ApiGetArg(uint32_t, key_len)
  m3ApiGetArgMem(int32_t *, out_value)

  if (!vm || !vm->parser_registry || !out_value) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_value, sizeof(int32_t));
  if (section_len != 0) {
    m3ApiCheckMem(section_ptr, section_len);
  }
  if (key_len != 0) {
    m3ApiCheckMem(key_ptr, key_len);
  }
  m3ApiWriteMem32(out_value, 0);

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_INI) {
    m3ApiReturn(TURBO_EINVAL);
  }
  if (section_len == 0 || key_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  section = turbo_wasm3_copy_guest_bytes(section_ptr, section_len);
  if (!section) {
    m3ApiReturn(TURBO_ENOMEM);
  }
  key = turbo_wasm3_copy_guest_bytes(key_ptr, key_len);
  if (!key) {
    free(section);
    m3ApiReturn(TURBO_ENOMEM);
  }

  value = turbo_ini_get((const turbo_ini_t *)entry->doc, section, key);
  parsed = turbo_ini_get_bool((const turbo_ini_t *)entry->doc, section, key, false)
               ? 1
               : 0;
  free(key);
  free(section);
  if (!value) {
    m3ApiReturn(TURBO_ENOENT);
  }

  m3ApiWriteMem32(out_value, (uint32_t)parsed);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_ini_get_double) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *section = NULL;
  char *key = NULL;
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  const char *value;
  double parsed = 0.0;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(const uint8_t *, section_ptr)
  m3ApiGetArg(uint32_t, section_len)
  m3ApiGetArgMem(const uint8_t *, key_ptr)
  m3ApiGetArg(uint32_t, key_len)
  m3ApiGetArgMem(double *, out_value)

  if (!vm || !vm->parser_registry || !out_value) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_value, sizeof(double));
  if (section_len != 0) {
    m3ApiCheckMem(section_ptr, section_len);
  }
  if (key_len != 0) {
    m3ApiCheckMem(key_ptr, key_len);
  }
  memset(out_value, 0, sizeof(double));

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_INI) {
    m3ApiReturn(TURBO_EINVAL);
  }
  if (section_len == 0 || key_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  section = turbo_wasm3_copy_guest_bytes(section_ptr, section_len);
  if (!section) {
    m3ApiReturn(TURBO_ENOMEM);
  }
  key = turbo_wasm3_copy_guest_bytes(key_ptr, key_len);
  if (!key) {
    free(section);
    m3ApiReturn(TURBO_ENOMEM);
  }

  value = turbo_ini_get((const turbo_ini_t *)entry->doc, section, key);
  parsed =
      turbo_ini_get_double((const turbo_ini_t *)entry->doc, section, key, 0.0);
  free(key);
  free(section);
  if (!value) {
    m3ApiReturn(TURBO_ENOENT);
  }

  memcpy(out_value, &parsed, sizeof(parsed));
  m3ApiReturn(0);
}

int turbo_wasm3_vm_set_wasi_args(turbo_wasm3_vm_t *vm, uint32_t argc,
                                 const char *const *argv) {
  char **argv_copy = NULL;
  uint32_t i;

  if (!vm || !vm->wasi_context) {
    return TURBO_EINVAL;
  }
  if (argc != 0 && !argv) {
    return TURBO_EINVAL;
  }

  turbo_wasm3_vm_clear_wasi_args(vm);

  if (argc == 0) {
    return 0;
  }

  argv_copy = (char **)calloc(argc, sizeof(*argv_copy));
  if (!argv_copy) {
    return TURBO_ENOMEM;
  }

  for (i = 0; i < argc; ++i) {
    uint32_t j;

    if (!argv[i]) {
      for (j = 0; j < i; ++j) {
        free(argv_copy[j]);
      }
      free(argv_copy);
      return TURBO_EINVAL;
    }

    argv_copy[i] = turbo_wasm3_strdup(argv[i]);
    if (!argv_copy[i]) {
      for (j = 0; j < i; ++j) {
        free(argv_copy[j]);
      }
      free(argv_copy);
      return TURBO_ENOMEM;
    }
  }

  vm->wasi_argv = argv_copy;
  vm->wasi_argc = argc;
  vm->wasi_context->argc = argc;
  vm->wasi_context->argv = (ccstr_t *)vm->wasi_argv;
  return 0;
}

void turbo_wasm3_vm_reset_preopens(turbo_wasm3_vm_t *vm) {
  if (!vm || !vm->wasi_context) {
    return;
  }

  m3_wasi_context_reset_preopens(vm->wasi_context);
}

int turbo_wasm3_vm_set_preopen(turbo_wasm3_vm_t *vm, uint32_t fd,
                               const char *guest_path, const char *host_path) {
  if (!vm || !vm->wasi_context) {
    return TURBO_EINVAL;
  }

  return m3_wasi_context_set_preopen(vm->wasi_context, fd, guest_path, host_path);
}

int turbo_wasm3_vm_remove_preopen(turbo_wasm3_vm_t *vm, uint32_t fd) {
  if (!vm || !vm->wasi_context) {
    return TURBO_EINVAL;
  }

  return m3_wasi_context_remove_preopen(vm->wasi_context, fd);
}

M3Result turbo_wasm3_vm_load_module(turbo_wasm3_vm_t *vm, const uint8_t *wasm_bytes,
                                    uint32_t wasm_size,
                                    const char *module_name,
                                    IM3Module *out_module) {
  size_t blob_index;
  const uint8_t *owned_bytes = NULL;
  IM3Module module = NULL;
  M3Result result;

  if (!vm || !vm->env || !vm->runtime || !wasm_bytes || !wasm_size) {
    return m3Err_wasmMalformed;
  }

  blob_index = vm->blob_count;
  result = turbo_wasm3_vm_store_blob(vm, wasm_bytes, wasm_size, &owned_bytes);
  if (result) {
    return result;
  }

  result = m3_ParseModule(vm->env, &module, owned_bytes, wasm_size);
  if (result) {
    free(vm->blobs[blob_index].bytes);
    vm->blob_count = blob_index;
    return result;
  }

  result = m3_LoadModule(vm->runtime, module);
  if (result) {
    m3_FreeModule(module);
    free(vm->blobs[blob_index].bytes);
    vm->blob_count = blob_index;
    return result;
  }

  if (module_name && module_name[0] != '\0') {
    m3_SetModuleName(module, module_name);
  }

  result = m3_LinkWASIWithContext(module, vm->wasi_context);
  if (result) {
    return result;
  }

  result = turbo_wasm3_vm_bind_wasi(vm);
  if (result) {
    return result;
  }

  result = turbo_wasm3_vm_link_host_modules(vm, module);
  if (result) {
    return result;
  }

  if (out_module) {
    *out_module = module;
  }
  return m3Err_none;
}

M3Result turbo_wasm3_vm_load_module_file(turbo_wasm3_vm_t *vm, const char *path,
                                         const char *module_name,
                                         IM3Module *out_module) {
  turbo_fs_buf_t wasm = {0};
  M3Result result;

  if (!vm || !path) {
    return m3Err_wasmMalformed;
  }

  if (turbo_fs_read_file(path, &wasm) != 0) {
    return "failed to read wasm module";
  }
  if (wasm.len == 0 || wasm.len > UINT32_MAX) {
    turbo_fs_buf_free(&wasm);
    return m3Err_wasmOverrun;
  }

  result = turbo_wasm3_vm_load_module(vm, (const uint8_t *)wasm.base,
                                      (uint32_t)wasm.len, module_name,
                                      out_module);
  turbo_fs_buf_free(&wasm);
  return result;
}

turbo_wasm3_socket_registry_t *
turbo_wasm3_socket_registry_create(size_t initial_capacity) {
  turbo_wasm3_socket_registry_t *registry;
  int rc;

  registry = (turbo_wasm3_socket_registry_t *)calloc(1, sizeof(*registry));
  if (!registry) {
    return NULL;
  }

  registry->next_handle = 64;
  rc = turbo_wasm3_socket_registry_reserve(
      registry, initial_capacity ? initial_capacity : 16);
  if (rc != 0) {
    free(registry);
    return NULL;
  }

  return registry;
}

void turbo_wasm3_socket_registry_destroy(turbo_wasm3_socket_registry_t *registry) {
  if (!registry) {
    return;
  }

  free(registry->entries);
  free(registry);
}

int turbo_wasm3_socket_registry_bind_wasi(turbo_wasm3_socket_registry_t *registry,
                                          m3_wasi_context_t *wasi_context) {
  if (!registry || !wasi_context) {
    return TURBO_EINVAL;
  }

  m3_wasi_context_set_socket_ops(wasi_context, &turbo_wasm3_socket_ops, registry);
  return 0;
}

int turbo_wasm3_socket_registry_unbind_wasi(m3_wasi_context_t *wasi_context) {
  if (!wasi_context) {
    return TURBO_EINVAL;
  }

  m3_wasi_context_set_socket_ops(wasi_context, NULL, NULL);
  return 0;
}

int turbo_wasm3_socket_registry_register(turbo_wasm3_socket_registry_t *registry,
                                         coro_socket_t *socket,
                                         uint32_t *wasi_fd) {
  int rc;

  if (!registry || !socket || !wasi_fd) {
    return TURBO_EINVAL;
  }

  rc = turbo_wasm3_socket_registry_reserve(registry, registry->count + 1);
  if (rc != 0) {
    return rc;
  }

  registry->entries[registry->count].handle = registry->next_handle++;
  registry->entries[registry->count].socket = socket;
  *wasi_fd = registry->entries[registry->count].handle;
  registry->count++;
  return 0;
}

int turbo_wasm3_vm_register_socket(turbo_wasm3_vm_t *vm, coro_socket_t *socket,
                                   uint32_t *wasi_fd) {
  if (!vm) {
    return TURBO_EINVAL;
  }

  return turbo_wasm3_socket_registry_register(vm->socket_registry, socket,
                                              wasi_fd);
}

int turbo_wasm3_socket_registry_unregister(turbo_wasm3_socket_registry_t *registry,
                                           uint32_t wasi_fd) {
  size_t i;

  if (!registry) {
    return TURBO_EINVAL;
  }

  for (i = 0; i < registry->count; ++i) {
    if (registry->entries[i].handle == wasi_fd) {
      registry->entries[i] = registry->entries[registry->count - 1];
      registry->count--;
      return 0;
    }
  }

  return TURBO_EBADF;
}

coro_socket_t *
turbo_wasm3_socket_registry_lookup(turbo_wasm3_socket_registry_t *registry,
                                   uint32_t wasi_fd) {
  turbo_wasm3_socket_entry_t *entry =
      turbo_wasm3_socket_registry_find_entry(registry, wasi_fd);

  return entry ? entry->socket : NULL;
}

turbo_wasm3_db_registry_t *
turbo_wasm3_db_registry_create(size_t initial_capacity) {
  turbo_wasm3_db_registry_t *registry;
  int rc;

  registry = (turbo_wasm3_db_registry_t *)calloc(1, sizeof(*registry));
  if (!registry) {
    return NULL;
  }

  registry->next_handle = 1;
  registry->next_stmt_handle = 0x10000u;
  rc = turbo_wasm3_db_registry_reserve(registry,
                                       initial_capacity ? initial_capacity : 4);
  if (rc != 0) {
    free(registry);
    return NULL;
  }

  rc = turbo_wasm3_db_registry_reserve_statements(registry, 4);
  if (rc != 0) {
    free(registry->entries);
    free(registry);
    return NULL;
  }

  return registry;
}

void turbo_wasm3_db_registry_destroy(turbo_wasm3_db_registry_t *registry) {
  size_t i;

  if (!registry) {
    return;
  }

  if (registry->ops && registry->ops->finalize) {
    for (i = 0; i < registry->stmt_count; ++i) {
      turbo_wasm3_db_stmt_entry_clear_error(&registry->stmt_entries[i]);
      registry->ops->finalize(registry->user_data, registry->stmt_entries[i].stmt);
    }
  }

  if (registry->ops && registry->ops->close) {
    for (i = 0; i < registry->count; ++i) {
      turbo_wasm3_db_entry_clear_error(&registry->entries[i]);
      registry->ops->close(registry->user_data, registry->entries[i].db);
    }
  }

  free(registry->stmt_entries);
  free(registry->entries);
  free(registry);
}

int turbo_wasm3_db_registry_set_ops(turbo_wasm3_db_registry_t *registry,
                                    const turbo_wasm3_db_ops_t *ops,
                                    void *user_data) {
  if (!registry || !ops || !ops->open || !ops->close || !ops->exec ||
      !ops->error || !ops->prepare || !ops->finalize || !ops->reset ||
      !ops->step || !ops->bind_int64 || !ops->bind_double || !ops->bind_null ||
      !ops->bind_blob || !ops->bind_text || !ops->column_type ||
      !ops->column_int64 || !ops->column_double || !ops->column_blob ||
      !ops->column_text) {
    return TURBO_EINVAL;
  }
  if (registry->count != 0 || registry->stmt_count != 0) {
    return TURBO_EBUSY;
  }

  registry->ops = ops;
  registry->user_data = user_data;
  return 0;
}

int turbo_wasm3_db_registry_enable_sqlite(turbo_wasm3_db_registry_t *registry) {
  return turbo_wasm3_db_registry_set_ops(registry, &turbo_wasm3_sqlite_db_ops,
                                         NULL);
}

int turbo_wasm3_vm_enable_sqlite_db(turbo_wasm3_vm_t *vm) {
  if (!vm) {
    return TURBO_EINVAL;
  }

  return turbo_wasm3_db_registry_enable_sqlite(vm->db_registry);
}

int turbo_wasm3_db_registry_open(turbo_wasm3_db_registry_t *registry,
                                 const char *target, uint32_t *db_handle) {
  int rc;
  void *db = NULL;

  if (!registry || !target || !db_handle || !registry->ops || !registry->ops->open) {
    return TURBO_EINVAL;
  }

  rc = turbo_wasm3_db_registry_reserve(registry, registry->count + 1);
  if (rc != 0) {
    return rc;
  }

  rc = registry->ops->open(registry->user_data, target, &db);
  if (rc != 0) {
    return rc;
  }

  registry->entries[registry->count].handle = registry->next_handle++;
  registry->entries[registry->count].db = db;
  registry->entries[registry->count].last_error = NULL;
  *db_handle = registry->entries[registry->count].handle;
  registry->count++;
  return 0;
}

int turbo_wasm3_db_registry_close(turbo_wasm3_db_registry_t *registry,
                                  uint32_t db_handle) {
  turbo_wasm3_db_entry_t *entry;
  size_t index;
  size_t i;
  int rc;

  if (!registry || !registry->ops || !registry->ops->close) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_db_registry_find_entry(registry, db_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  index = (size_t)(entry - registry->entries);
  for (i = registry->stmt_count; i > 0; --i) {
    turbo_wasm3_db_stmt_entry_t *stmt_entry = &registry->stmt_entries[i - 1];
    if (stmt_entry->db_handle == db_handle) {
      rc = registry->ops->finalize(registry->user_data, stmt_entry->stmt);
      if (rc != 0) {
        turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, entry);
        return rc;
      }
      turbo_wasm3_db_stmt_entry_clear_error(stmt_entry);
      registry->stmt_entries[i - 1] = registry->stmt_entries[registry->stmt_count - 1];
      registry->stmt_count--;
    }
  }

  rc = registry->ops->close(registry->user_data, entry->db);
  if (rc != 0) {
    turbo_wasm3_db_entry_capture_provider_error(registry, entry);
    return rc;
  }

  turbo_wasm3_db_entry_clear_error(entry);
  registry->entries[index] = registry->entries[registry->count - 1];
  registry->count--;
  return 0;
}

int turbo_wasm3_db_registry_exec(turbo_wasm3_db_registry_t *registry,
                                 uint32_t db_handle, const char *sql,
                                 uint64_t *changes) {
  turbo_wasm3_db_entry_t *entry;

  if (!registry || !sql || !changes || !registry->ops || !registry->ops->exec) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_db_registry_find_entry(registry, db_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  turbo_wasm3_db_entry_clear_error(entry);
  {
    int rc = registry->ops->exec(registry->user_data, entry->db, sql, changes);
    if (rc != 0) {
      turbo_wasm3_db_entry_capture_provider_error(registry, entry);
      return rc;
    }
  }

  return 0;
}

int turbo_wasm3_db_registry_prepare(turbo_wasm3_db_registry_t *registry,
                                    uint32_t db_handle, const char *sql,
                                    uint32_t *stmt_handle) {
  turbo_wasm3_db_entry_t *entry;
  void *stmt = NULL;
  int rc;

  if (!registry || !sql || !stmt_handle || !registry->ops || !registry->ops->prepare) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_db_registry_find_entry(registry, db_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  rc = turbo_wasm3_db_registry_reserve_statements(registry, registry->stmt_count + 1);
  if (rc != 0) {
    return rc;
  }

  turbo_wasm3_db_entry_clear_error(entry);
  rc = registry->ops->prepare(registry->user_data, entry->db, sql, &stmt);
  if (rc != 0) {
    turbo_wasm3_db_entry_capture_provider_error(registry, entry);
    return rc;
  }

  registry->stmt_entries[registry->stmt_count].handle = registry->next_stmt_handle++;
  registry->stmt_entries[registry->stmt_count].db_handle = db_handle;
  registry->stmt_entries[registry->stmt_count].stmt = stmt;
  registry->stmt_entries[registry->stmt_count].last_error = NULL;
  *stmt_handle = registry->stmt_entries[registry->stmt_count].handle;
  registry->stmt_count++;
  return 0;
}

int turbo_wasm3_db_registry_finalize(turbo_wasm3_db_registry_t *registry,
                                     uint32_t stmt_handle) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;
  size_t index;
  int rc;

  if (!registry || !registry->ops || !registry->ops->finalize) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  index = (size_t)(stmt_entry - registry->stmt_entries);
  rc = registry->ops->finalize(registry->user_data, stmt_entry->stmt);
  if (rc != 0) {
    turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, db_entry);
    return rc;
  }

  turbo_wasm3_db_stmt_entry_clear_error(stmt_entry);
  registry->stmt_entries[index] = registry->stmt_entries[registry->stmt_count - 1];
  registry->stmt_count--;
  return 0;
}

int turbo_wasm3_db_registry_reset(turbo_wasm3_db_registry_t *registry,
                                  uint32_t stmt_handle) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;
  int rc;

  if (!registry || !registry->ops || !registry->ops->reset) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  turbo_wasm3_db_stmt_entry_clear_error(stmt_entry);
  if (db_entry) {
    turbo_wasm3_db_entry_clear_error(db_entry);
  }
  rc = registry->ops->reset(registry->user_data, stmt_entry->stmt);
  if (rc != 0) {
    turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, db_entry);
  }
  return rc;
}

int turbo_wasm3_db_registry_step(turbo_wasm3_db_registry_t *registry,
                                 uint32_t stmt_handle, int32_t *out_state) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;
  int rc;

  if (!registry || !out_state || !registry->ops || !registry->ops->step) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  turbo_wasm3_db_stmt_entry_clear_error(stmt_entry);
  if (db_entry) {
    turbo_wasm3_db_entry_clear_error(db_entry);
  }
  rc = registry->ops->step(registry->user_data, stmt_entry->stmt, out_state);
  if (rc != 0) {
    turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, db_entry);
  }
  return rc;
}

int turbo_wasm3_db_registry_bind_int64(turbo_wasm3_db_registry_t *registry,
                                       uint32_t stmt_handle, uint32_t index,
                                       int64_t value) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;
  int rc;

  if (!registry || !registry->ops || !registry->ops->bind_int64) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  turbo_wasm3_db_stmt_entry_clear_error(stmt_entry);
  if (db_entry) {
    turbo_wasm3_db_entry_clear_error(db_entry);
  }
  rc = registry->ops->bind_int64(registry->user_data, stmt_entry->stmt, index,
                                 value);
  if (rc != 0) {
    turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, db_entry);
  }
  return rc;
}

int turbo_wasm3_db_registry_bind_double(turbo_wasm3_db_registry_t *registry,
                                        uint32_t stmt_handle, uint32_t index,
                                        double value) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;
  int rc;

  if (!registry || !registry->ops || !registry->ops->bind_double) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  turbo_wasm3_db_stmt_entry_clear_error(stmt_entry);
  if (db_entry) {
    turbo_wasm3_db_entry_clear_error(db_entry);
  }
  rc = registry->ops->bind_double(registry->user_data, stmt_entry->stmt, index,
                                  value);
  if (rc != 0) {
    turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, db_entry);
  }
  return rc;
}

int turbo_wasm3_db_registry_bind_null(turbo_wasm3_db_registry_t *registry,
                                      uint32_t stmt_handle, uint32_t index) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;
  int rc;

  if (!registry || !registry->ops || !registry->ops->bind_null) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  turbo_wasm3_db_stmt_entry_clear_error(stmt_entry);
  if (db_entry) {
    turbo_wasm3_db_entry_clear_error(db_entry);
  }
  rc = registry->ops->bind_null(registry->user_data, stmt_entry->stmt, index);
  if (rc != 0) {
    turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, db_entry);
  }
  return rc;
}

int turbo_wasm3_db_registry_bind_blob(turbo_wasm3_db_registry_t *registry,
                                      uint32_t stmt_handle, uint32_t index,
                                      const void *value, size_t value_len) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;
  int rc;

  if (!registry || (!value && value_len != 0) || !registry->ops ||
      !registry->ops->bind_blob) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  turbo_wasm3_db_stmt_entry_clear_error(stmt_entry);
  if (db_entry) {
    turbo_wasm3_db_entry_clear_error(db_entry);
  }
  rc = registry->ops->bind_blob(registry->user_data, stmt_entry->stmt, index,
                                value, value_len);
  if (rc != 0) {
    turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, db_entry);
  }
  return rc;
}

int turbo_wasm3_db_registry_bind_text(turbo_wasm3_db_registry_t *registry,
                                      uint32_t stmt_handle, uint32_t index,
                                      const char *value, size_t value_len) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;
  int rc;

  if (!registry || (!value && value_len != 0) || !registry->ops ||
      !registry->ops->bind_text) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  turbo_wasm3_db_stmt_entry_clear_error(stmt_entry);
  if (db_entry) {
    turbo_wasm3_db_entry_clear_error(db_entry);
  }
  rc = registry->ops->bind_text(registry->user_data, stmt_entry->stmt, index,
                                value, value_len);
  if (rc != 0) {
    turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, db_entry);
  }
  return rc;
}

int turbo_wasm3_db_registry_column_type(turbo_wasm3_db_registry_t *registry,
                                        uint32_t stmt_handle, uint32_t index,
                                        int32_t *out_type) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;
  int rc;

  if (!registry || !out_type || !registry->ops || !registry->ops->column_type) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  rc = registry->ops->column_type(registry->user_data, stmt_entry->stmt, index,
                                  out_type);
  if (rc != 0) {
    turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, db_entry);
  }
  return rc;
}

int turbo_wasm3_db_registry_column_int64(turbo_wasm3_db_registry_t *registry,
                                         uint32_t stmt_handle, uint32_t index,
                                         int64_t *out_value) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;
  int rc;

  if (!registry || !out_value || !registry->ops || !registry->ops->column_int64) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  rc = registry->ops->column_int64(registry->user_data, stmt_entry->stmt, index,
                                   out_value);
  if (rc != 0) {
    turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, db_entry);
  }
  return rc;
}

int turbo_wasm3_db_registry_column_double(turbo_wasm3_db_registry_t *registry,
                                          uint32_t stmt_handle, uint32_t index,
                                          double *out_value) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;
  int rc;

  if (!registry || !out_value || !registry->ops || !registry->ops->column_double) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  rc = registry->ops->column_double(registry->user_data, stmt_entry->stmt, index,
                                    out_value);
  if (rc != 0) {
    turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, db_entry);
  }
  return rc;
}

int turbo_wasm3_db_registry_column_blob(turbo_wasm3_db_registry_t *registry,
                                        uint32_t stmt_handle, uint32_t index,
                                        void *buffer, size_t buffer_size,
                                        uint32_t *out_len) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;
  int rc;

  if (!registry || !buffer || buffer_size == 0 || !out_len || !registry->ops ||
      !registry->ops->column_blob) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  rc = registry->ops->column_blob(registry->user_data, stmt_entry->stmt, index,
                                  buffer, buffer_size, out_len);
  if (rc != 0 && rc != TURBO_EMSGSIZE) {
    turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, db_entry);
  }
  return rc;
}

int turbo_wasm3_db_registry_column_text(turbo_wasm3_db_registry_t *registry,
                                        uint32_t stmt_handle, uint32_t index,
                                        char *buffer, size_t buffer_size,
                                        uint32_t *out_len) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;
  int rc;

  if (!registry || !buffer || buffer_size == 0 || !out_len || !registry->ops ||
      !registry->ops->column_text) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  rc = registry->ops->column_text(registry->user_data, stmt_entry->stmt, index,
                                  buffer, buffer_size, out_len);
  if (rc != 0 && rc != TURBO_EMSGSIZE) {
    turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, db_entry);
  }
  return rc;
}

int turbo_wasm3_db_registry_error(turbo_wasm3_db_registry_t *registry,
                                  uint32_t db_handle, char *buffer,
                                  size_t buffer_size, uint32_t *out_len) {
  turbo_wasm3_db_entry_t *entry;

  if (!registry || !buffer || buffer_size == 0 || !out_len || !registry->ops ||
      !registry->ops->error) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_db_registry_find_entry(registry, db_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  if (entry->last_error) {
    return turbo_wasm3_error_buffer_write(entry->last_error, buffer, buffer_size,
                                          out_len);
  }

  return registry->ops->error(registry->user_data, entry->db, buffer,
                              buffer_size, out_len);
}

int turbo_wasm3_db_registry_stmt_error(turbo_wasm3_db_registry_t *registry,
                                       uint32_t stmt_handle, char *buffer,
                                       size_t buffer_size, uint32_t *out_len) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;

  if (!registry || !buffer || buffer_size == 0 || !out_len || !registry->ops ||
      !registry->ops->error) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  if (stmt_entry->last_error) {
    return turbo_wasm3_error_buffer_write(stmt_entry->last_error, buffer,
                                          buffer_size, out_len);
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  if (!db_entry) {
    return TURBO_EBADF;
  }

  if (db_entry->last_error) {
    return turbo_wasm3_error_buffer_write(db_entry->last_error, buffer, buffer_size,
                                          out_len);
  }

  return registry->ops->error(registry->user_data, db_entry->db, buffer,
                              buffer_size, out_len);
}

turbo_wasm3_http_registry_t *
turbo_wasm3_http_registry_create(size_t initial_capacity) {
  turbo_wasm3_http_registry_t *registry;
  int rc;

  registry = (turbo_wasm3_http_registry_t *)calloc(1, sizeof(*registry));
  if (!registry) {
    return NULL;
  }

  registry->next_client_handle = 1;
  registry->next_response_handle = 0x20000u;

  rc = turbo_wasm3_http_registry_reserve_clients(registry,
                                                 initial_capacity ? initial_capacity : 4);
  if (rc != 0) {
    free(registry);
    return NULL;
  }

  rc = turbo_wasm3_http_registry_reserve_responses(registry, 4);
  if (rc != 0) {
    free(registry->clients);
    free(registry);
    return NULL;
  }

  return registry;
}

void turbo_wasm3_http_registry_destroy(turbo_wasm3_http_registry_t *registry) {
  size_t i;

  if (!registry) {
    return;
  }

  for (i = 0; i < registry->response_count; ++i) {
    turbo_wasm3_http_response_entry_reset(&registry->responses[i]);
  }

  for (i = 0; i < registry->client_count; ++i) {
    turbo_wasm3_http_client_entry_clear_error(&registry->clients[i]);
    http_client_destroy(registry->clients[i].client);
  }

  free(registry->responses);
  free(registry->clients);
  free(registry);
}

int turbo_wasm3_http_registry_open_client(turbo_wasm3_http_registry_t *registry,
                                          const char *base_url,
                                          uint32_t *client_handle) {
  http_client_t *client;
  int rc;

  if (!registry || !client_handle) {
    return TURBO_EINVAL;
  }

  rc = turbo_wasm3_http_registry_reserve_clients(registry, registry->client_count + 1);
  if (rc != 0) {
    return rc;
  }

  client = http_client_create(base_url && base_url[0] != '\0' ? base_url : NULL);
  if (!client) {
    return TURBO_ENOMEM;
  }

  registry->clients[registry->client_count].handle = registry->next_client_handle++;
  registry->clients[registry->client_count].client = client;
  registry->clients[registry->client_count].last_error = NULL;
  *client_handle = registry->clients[registry->client_count].handle;
  registry->client_count++;
  return 0;
}

int turbo_wasm3_http_registry_close_client(turbo_wasm3_http_registry_t *registry,
                                           uint32_t client_handle) {
  turbo_wasm3_http_client_entry_t *entry;
  size_t index;
  size_t i;

  if (!registry) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_client(registry, client_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  index = (size_t)(entry - registry->clients);
  for (i = registry->response_count; i > 0; --i) {
    turbo_wasm3_http_response_entry_t *response_entry = &registry->responses[i - 1];
    if (response_entry->client_handle == client_handle) {
      turbo_wasm3_http_response_entry_reset(response_entry);
      registry->responses[i - 1] = registry->responses[registry->response_count - 1];
      registry->response_count--;
    }
  }

  turbo_wasm3_http_client_entry_clear_error(entry);
  http_client_destroy(entry->client);
  registry->clients[index] = registry->clients[registry->client_count - 1];
  registry->client_count--;
  return 0;
}

int turbo_wasm3_http_registry_set_timeout(turbo_wasm3_http_registry_t *registry,
                                          uint32_t client_handle,
                                          int timeout_ms) {
  turbo_wasm3_http_client_entry_t *entry;

  if (!registry) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_client(registry, client_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  turbo_wasm3_http_client_entry_clear_error(entry);
  http_client_set_timeout(entry->client, timeout_ms);
  return 0;
}

int turbo_wasm3_http_registry_set_default_header(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle,
    const char *name, const char *value) {
  turbo_wasm3_http_client_entry_t *entry;

  if (!registry || !name || !value) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_client(registry, client_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  turbo_wasm3_http_client_entry_clear_error(entry);
  http_client_set_default_header(entry->client, name, value);
  return 0;
}

int turbo_wasm3_http_registry_remove_default_header(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle,
    const char *name) {
  turbo_wasm3_http_client_entry_t *entry;

  if (!registry || !name) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_client(registry, client_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  turbo_wasm3_http_client_entry_clear_error(entry);
  http_client_remove_default_header(entry->client, name);
  return 0;
}

int turbo_wasm3_http_registry_clear_default_headers(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle) {
  turbo_wasm3_http_client_entry_t *entry;

  if (!registry) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_client(registry, client_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  turbo_wasm3_http_client_entry_clear_error(entry);
  http_client_clear_default_headers(entry->client);
  return 0;
}

int turbo_wasm3_http_registry_set_basic_auth(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle,
    const char *username, const char *password) {
  turbo_wasm3_http_client_entry_t *entry;

  if (!registry || !username || !password) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_client(registry, client_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  turbo_wasm3_http_client_entry_clear_error(entry);
  http_client_set_basic_auth(entry->client, username, password);
  return 0;
}

int turbo_wasm3_http_registry_set_bearer_token(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle,
    const char *token) {
  turbo_wasm3_http_client_entry_t *entry;

  if (!registry || !token) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_client(registry, client_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  turbo_wasm3_http_client_entry_clear_error(entry);
  http_client_set_bearer_token(entry->client, token);
  return 0;
}

int turbo_wasm3_http_registry_clear_auth(turbo_wasm3_http_registry_t *registry,
                                         uint32_t client_handle) {
  turbo_wasm3_http_client_entry_t *entry;

  if (!registry) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_client(registry, client_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  turbo_wasm3_http_client_entry_clear_error(entry);
  http_client_clear_auth(entry->client);
  return 0;
}

int turbo_wasm3_http_registry_set_proxy(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle,
    const char *host, uint16_t port, const char *username,
    const char *password) {
  turbo_wasm3_http_client_entry_t *entry;

  if (!registry || !host || port == 0) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_client(registry, client_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  turbo_wasm3_http_client_entry_clear_error(entry);
  http_client_set_proxy(entry->client, host, port, username, password);
  return 0;
}

int turbo_wasm3_http_registry_clear_proxy(turbo_wasm3_http_registry_t *registry,
                                          uint32_t client_handle) {
  turbo_wasm3_http_client_entry_t *entry;

  if (!registry) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_client(registry, client_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  turbo_wasm3_http_client_entry_clear_error(entry);
  http_client_clear_proxy(entry->client);
  return 0;
}

int turbo_wasm3_http_registry_request(turbo_wasm3_http_registry_t *registry,
                                      uint32_t client_handle, int method,
                                      const char *url, const void *body,
                                      size_t body_len, uint32_t *response_handle) {
  return turbo_wasm3_http_registry_request_with_headers(
      registry, client_handle, method, url, NULL, 0, body, body_len, response_handle);
}

int turbo_wasm3_http_registry_request_with_headers(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle, int method,
    const char *url, const char *headers, size_t headers_len, const void *body,
    size_t body_len, uint32_t *response_handle) {
  turbo_wasm3_http_header_block_t header_block = {0};
  int rc;

  if (!registry || !url || !response_handle) {
    return TURBO_EINVAL;
  }

  rc = turbo_wasm3_http_parse_headers(headers, headers_len, &header_block);
  if (rc != 0) {
    return rc;
  }

  rc = turbo_wasm3_http_registry_request_core(
      registry, client_handle, method, url, header_block.lines,
      header_block.count, body, body_len, 0, 0, response_handle);
  turbo_wasm3_http_header_block_clear(&header_block);
  return rc;
}

int turbo_wasm3_http_registry_stream_get(turbo_wasm3_http_registry_t *registry,
                                         uint32_t client_handle, const char *url,
                                         uint32_t *response_handle) {
  return turbo_wasm3_http_registry_request_core(registry, client_handle,
                                                TURBO_WASM3_HTTP_METHOD_GET, url,
                                                NULL, 0, NULL, 0, 1, 0,
                                                response_handle);
}

int turbo_wasm3_http_registry_sse_get(turbo_wasm3_http_registry_t *registry,
                                      uint32_t client_handle, const char *url,
                                      uint32_t *response_handle) {
  return turbo_wasm3_http_registry_request_core(registry, client_handle,
                                                TURBO_WASM3_HTTP_METHOD_GET, url,
                                                NULL, 0, NULL, 0, 1, 1,
                                                response_handle);
}

int turbo_wasm3_http_registry_response_status(turbo_wasm3_http_registry_t *registry,
                                              uint32_t response_handle,
                                              int32_t *out_status) {
  turbo_wasm3_http_response_entry_t *entry;

  if (!registry || !out_status) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_response(registry, response_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  *out_status = entry->response ? (int32_t)entry->response->status_code : 0;
  return 0;
}

int turbo_wasm3_http_registry_response_error_code(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle,
    int32_t *out_error_code) {
  turbo_wasm3_http_response_entry_t *entry;

  if (!registry || !out_error_code) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_response(registry, response_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  *out_error_code =
      entry->response ? (int32_t)entry->response->error_code : (int32_t)TURBO_EIO;
  return 0;
}

int turbo_wasm3_http_registry_response_is_sse(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle,
    int32_t *out_is_sse) {
  turbo_wasm3_http_response_entry_t *entry;

  if (!registry || !out_is_sse) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_response(registry, response_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  *out_is_sse = (int32_t)(entry->is_sse ? 1 : 0);
  return 0;
}

int turbo_wasm3_http_registry_response_chunk_count(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle,
    uint32_t *out_count) {
  turbo_wasm3_http_response_entry_t *entry;

  if (!registry || !out_count) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_response(registry, response_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  *out_count = (uint32_t)entry->chunk_count;
  return 0;
}

int turbo_wasm3_http_registry_response_chunk(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle,
    uint32_t chunk_index, void *buffer, size_t buffer_size, uint32_t *out_len) {
  turbo_wasm3_http_response_entry_t *entry;
  turbo_wasm3_blob_t *chunk;

  if (!registry || !buffer || buffer_size == 0 || !out_len) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_response(registry, response_handle);
  if (!entry) {
    return TURBO_EBADF;
  }
  if (chunk_index >= entry->chunk_count) {
    return TURBO_EINVAL;
  }

  chunk = &entry->chunks[chunk_index];
  return turbo_wasm3_http_copy_buffer(chunk->bytes, chunk->size, buffer,
                                      buffer_size, out_len);
}

int turbo_wasm3_http_registry_response_header(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle,
    const char *header_name, char *buffer, size_t buffer_size, uint32_t *out_len) {
  turbo_wasm3_http_response_entry_t *entry;
  char *value;
  int rc;

  if (!registry || !header_name || !buffer || buffer_size == 0 || !out_len) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_response(registry, response_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  if (!entry->response) {
    buffer[0] = '\0';
    *out_len = 0;
    return 0;
  }

  value = http_response_get_header(entry->response, header_name);
  if (!value) {
    buffer[0] = '\0';
    *out_len = 0;
    return 0;
  }

  rc = turbo_wasm3_http_copy_string(value, buffer, buffer_size, out_len);
  free(value);
  if (rc != 0 && rc != TURBO_EMSGSIZE) {
    turbo_wasm3_http_capture_response_error(entry, entry->response);
  }
  return rc;
}

int turbo_wasm3_http_registry_response_headers(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle, char *buffer,
    size_t buffer_size, uint32_t *out_len) {
  turbo_wasm3_http_response_entry_t *entry;
  int rc;

  if (!registry || !buffer || buffer_size == 0 || !out_len) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_response(registry, response_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  rc = turbo_wasm3_http_copy_string(
      entry->response ? entry->response->headers : NULL, buffer, buffer_size, out_len);
  if (rc != 0 && rc != TURBO_EMSGSIZE) {
    turbo_wasm3_http_capture_response_error(entry, entry->response);
  }
  return rc;
}

int turbo_wasm3_http_registry_response_body(turbo_wasm3_http_registry_t *registry,
                                            uint32_t response_handle, void *buffer,
                                            size_t buffer_size, uint32_t *out_len) {
  turbo_wasm3_http_response_entry_t *entry;
  int rc;

  if (!registry || !buffer || buffer_size == 0 || !out_len) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_response(registry, response_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  rc = turbo_wasm3_http_copy_buffer(entry->response ? entry->response->body : NULL,
                                    entry->response ? entry->response->body_len : 0,
                                    buffer, buffer_size, out_len);
  if (rc != 0 && rc != TURBO_EMSGSIZE) {
    turbo_wasm3_http_capture_response_error(entry, entry->response);
  }
  return rc;
}

int turbo_wasm3_http_registry_response_error(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle, char *buffer,
    size_t buffer_size, uint32_t *out_len) {
  turbo_wasm3_http_response_entry_t *entry;

  if (!registry || !buffer || buffer_size == 0 || !out_len) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_response(registry, response_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  if (entry->last_error) {
    return turbo_wasm3_http_copy_string(entry->last_error, buffer, buffer_size,
                                        out_len);
  }

  return turbo_wasm3_http_copy_string(
      entry->response ? entry->response->error : NULL, buffer, buffer_size, out_len);
}

int turbo_wasm3_http_registry_close_response(turbo_wasm3_http_registry_t *registry,
                                             uint32_t response_handle) {
  turbo_wasm3_http_response_entry_t *entry;
  size_t index;

  if (!registry) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_response(registry, response_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  index = (size_t)(entry - registry->responses);
  turbo_wasm3_http_response_entry_reset(entry);
  registry->responses[index] = registry->responses[registry->response_count - 1];
  registry->response_count--;
  return 0;
}

turbo_wasm3_redis_registry_t *
turbo_wasm3_redis_registry_create(size_t initial_capacity) {
  turbo_wasm3_redis_registry_t *registry;
  int rc;

  registry = (turbo_wasm3_redis_registry_t *)calloc(1, sizeof(*registry));
  if (!registry) {
    return NULL;
  }

  registry->next_client_handle = 0x3000u;
  registry->next_reply_handle = 0x4000u;

  rc = turbo_wasm3_redis_registry_reserve_clients(
      registry, initial_capacity ? initial_capacity : 4);
  if (rc != 0) {
    free(registry);
    return NULL;
  }

  rc = turbo_wasm3_redis_registry_reserve_replies(registry, 4);
  if (rc != 0) {
    free(registry->clients);
    free(registry);
    return NULL;
  }

  return registry;
}

void turbo_wasm3_redis_registry_destroy(turbo_wasm3_redis_registry_t *registry) {
  size_t i;

  if (!registry) {
    return;
  }

  for (i = 0; i < registry->reply_count; ++i) {
    turbo_wasm3_redis_reply_entry_reset(&registry->replies[i]);
  }
  for (i = 0; i < registry->client_count; ++i) {
    turbo_wasm3_redis_client_entry_clear_error(&registry->clients[i]);
    redis_client_destroy(registry->clients[i].client);
    coro_context_destroy(registry->clients[i].ctx);
  }

  free(registry->replies);
  free(registry->clients);
  free(registry);
}

int turbo_wasm3_redis_registry_open_client(turbo_wasm3_redis_registry_t *registry,
                                           const char *host, uint16_t port,
                                           uint32_t *client_handle) {
  coro_context_t *ctx = NULL;
  redis_client_t *client;
  turbo_wasm3_redis_open_task_state_t task_state;
  int rc = 0;

  if (!registry || !host || port == 0 || !client_handle) {
    return TURBO_EINVAL;
  }

  rc = turbo_wasm3_redis_registry_reserve_clients(registry,
                                                  registry->client_count + 1);
  if (rc != 0) {
    return rc;
  }

  client = redis_client_create(host, port);
  if (!client) {
    return TURBO_ENOMEM;
  }

  ctx = coro_context_create(NULL);
  if (!ctx) {
    redis_client_destroy(client);
    return TURBO_ENOMEM;
  }

  memset(&task_state, 0, sizeof(task_state));
  task_state.host = host;
  task_state.port = port;
  rc = coro_context_spawn(ctx, turbo_wasm3_redis_open_task, &task_state);
  if (rc != 0) {
    coro_context_destroy(ctx);
    redis_client_destroy(client);
    return rc;
  }
  rc = turbo_wasm3_redis_run_until_done(ctx, &task_state.done);
  if (rc != 0 || task_state.rc != 0 || !task_state.socket) {
    coro_context_destroy(ctx);
    redis_client_destroy(client);
    return task_state.rc != 0 ? task_state.rc : rc;
  }

  if (redis_client_attach_socket(client, ctx, task_state.socket, 1) != 0) {
    coro_socket_destroy(task_state.socket);
    coro_context_destroy(ctx);
    redis_client_destroy(client);
    return TURBO_EIO;
  }

  registry->clients[registry->client_count].handle = registry->next_client_handle++;
  registry->clients[registry->client_count].ctx = ctx;
  registry->clients[registry->client_count].client = client;
  registry->clients[registry->client_count].last_error = NULL;
  *client_handle = registry->clients[registry->client_count].handle;
  registry->client_count++;
  return 0;
}

int turbo_wasm3_redis_registry_close_client(turbo_wasm3_redis_registry_t *registry,
                                            uint32_t client_handle) {
  turbo_wasm3_redis_client_entry_t *entry;
  size_t i;
  size_t index;

  if (!registry) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_redis_registry_find_client(registry, client_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  for (i = 0; i < registry->reply_count;) {
    if (registry->replies[i].client_handle == client_handle) {
      turbo_wasm3_redis_reply_entry_reset(&registry->replies[i]);
      registry->replies[i] = registry->replies[registry->reply_count - 1];
      registry->reply_count--;
      continue;
    }
    ++i;
  }

  index = (size_t)(entry - registry->clients);
  turbo_wasm3_redis_client_entry_clear_error(entry);
  redis_client_destroy(entry->client);
  coro_context_destroy(entry->ctx);
  registry->clients[index] = registry->clients[registry->client_count - 1];
  registry->client_count--;
  return 0;
}

int turbo_wasm3_redis_registry_client_error(
    turbo_wasm3_redis_registry_t *registry, uint32_t client_handle, char *buffer,
    size_t buffer_size, uint32_t *out_len) {
  turbo_wasm3_redis_client_entry_t *entry;

  if (!registry || !buffer || buffer_size == 0 || !out_len) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_redis_registry_find_client(registry, client_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  return turbo_wasm3_error_buffer_write(entry->last_error, buffer, buffer_size,
                                        out_len);
}

int turbo_wasm3_redis_registry_command(turbo_wasm3_redis_registry_t *registry,
                                       uint32_t client_handle, uint32_t argc,
                                       const char *const *argv,
                                       const uint32_t *argv_lens,
                                       uint32_t *reply_handle) {
  turbo_wasm3_redis_client_entry_t *entry;
  turbo_wasm3_redis_command_task_state_t task_state;
  int rc;

  if (!registry || !reply_handle || argc == 0 || !argv || !argv_lens) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_redis_registry_find_client(registry, client_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  turbo_wasm3_redis_client_entry_clear_error(entry);
  memset(&task_state, 0, sizeof(task_state));
  task_state.entry = entry;
  task_state.argc = argc;
  task_state.argv = argv;
  task_state.argv_lens = argv_lens;
  rc = coro_context_spawn(entry->ctx, turbo_wasm3_redis_command_task, &task_state);
  if (rc != 0) {
    turbo_wasm3_redis_client_entry_set_error(entry, "redis command failed", 20);
    return rc;
  }
  rc = turbo_wasm3_redis_run_until_done(entry->ctx, &task_state.done);
  if (rc != 0 || task_state.rc != 0) {
    turbo_wasm3_redis_client_entry_set_error(entry, "redis command failed", 20);
    return task_state.rc != 0 ? task_state.rc : rc;
  }
  if (!task_state.reply) {
    turbo_wasm3_redis_client_entry_set_error(entry, "redis reply missing", 19);
    return TURBO_EIO;
  }

  return turbo_wasm3_redis_registry_store_reply(registry, client_handle,
                                                task_state.reply, reply_handle);
}

int turbo_wasm3_redis_registry_close_reply(turbo_wasm3_redis_registry_t *registry,
                                           uint32_t reply_handle) {
  turbo_wasm3_redis_reply_entry_t *entry;
  size_t index;

  if (!registry) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_redis_registry_find_reply(registry, reply_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  index = (size_t)(entry - registry->replies);
  turbo_wasm3_redis_reply_entry_reset(entry);
  registry->replies[index] = registry->replies[registry->reply_count - 1];
  registry->reply_count--;
  return 0;
}

int turbo_wasm3_redis_registry_reply_type(turbo_wasm3_redis_registry_t *registry,
                                          uint32_t reply_handle,
                                          int32_t *out_type) {
  turbo_wasm3_redis_reply_entry_t *entry;

  if (!registry || !out_type) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_redis_registry_find_reply(registry, reply_handle);
  if (!entry || !entry->reply) {
    return TURBO_EBADF;
  }

  *out_type = (int32_t)entry->reply->type;
  return 0;
}

int turbo_wasm3_redis_registry_reply_int64(turbo_wasm3_redis_registry_t *registry,
                                           uint32_t reply_handle,
                                           int64_t *out_value) {
  turbo_wasm3_redis_reply_entry_t *entry;

  if (!registry || !out_value) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_redis_registry_find_reply(registry, reply_handle);
  if (!entry || !entry->reply) {
    return TURBO_EBADF;
  }
  if (entry->reply->type != REDIS_REPLY_INTEGER) {
    return TURBO_EINVAL;
  }

  *out_value = entry->reply->integer;
  return 0;
}

int turbo_wasm3_redis_registry_reply_text(turbo_wasm3_redis_registry_t *registry,
                                          uint32_t reply_handle, char *buffer,
                                          size_t buffer_size,
                                          uint32_t *out_len) {
  turbo_wasm3_redis_reply_entry_t *entry;

  if (!registry || !buffer || buffer_size == 0 || !out_len) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_redis_registry_find_reply(registry, reply_handle);
  if (!entry || !entry->reply) {
    return TURBO_EBADF;
  }

  switch (entry->reply->type) {
  case REDIS_REPLY_STRING:
  case REDIS_REPLY_ERROR:
  case REDIS_REPLY_BULK_STRING:
    return turbo_wasm3_http_copy_buffer(entry->reply->str, entry->reply->len,
                                        buffer, buffer_size, out_len);
  case REDIS_REPLY_NULL:
    buffer[0] = '\0';
    *out_len = 0;
    return 0;
  default:
    return TURBO_EINVAL;
  }
}

int turbo_wasm3_redis_registry_reply_array_len(
    turbo_wasm3_redis_registry_t *registry, uint32_t reply_handle,
    uint32_t *out_len) {
  turbo_wasm3_redis_reply_entry_t *entry;

  if (!registry || !out_len) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_redis_registry_find_reply(registry, reply_handle);
  if (!entry || !entry->reply) {
    return TURBO_EBADF;
  }
  if (entry->reply->type != REDIS_REPLY_ARRAY) {
    return TURBO_EINVAL;
  }

  *out_len = (uint32_t)entry->reply->element_count;
  return 0;
}

int turbo_wasm3_redis_registry_reply_array_at(
    turbo_wasm3_redis_registry_t *registry, uint32_t reply_handle, uint32_t index,
    uint32_t *out_child_reply_handle) {
  turbo_wasm3_redis_reply_entry_t *entry;
  const redis_reply_t *child_reply;
  uint32_t client_handle;
  redis_reply_t *copy;

  if (!registry || !out_child_reply_handle) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_redis_registry_find_reply(registry, reply_handle);
  if (!entry || !entry->reply) {
    return TURBO_EBADF;
  }
  if (entry->reply->type != REDIS_REPLY_ARRAY ||
      index >= entry->reply->element_count) {
    return TURBO_EINVAL;
  }

  child_reply = entry->reply->elements[index];
  client_handle = entry->client_handle;
  copy = turbo_wasm3_redis_reply_clone(child_reply);
  if (!copy) {
    return TURBO_ENOMEM;
  }

  {
    int rc = turbo_wasm3_redis_registry_store_reply(
        registry, client_handle, copy, out_child_reply_handle);
    if (rc != 0) {
      redis_reply_free(copy);
    }
    return rc;
  }
}
