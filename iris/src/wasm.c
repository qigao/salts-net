#include "wasm.h"

#include "middleware.h"
#include "router.h"
#include "turbo_error.h"
#include "turbo_fs.h"
#include "turbo_str.h"
#include "turbo_wasm3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct iris_wasm_mount_s {
  iris_app_t *app;
  char *method;
  char *path;
  char *wasm_path;
  char *module_name;
  char *handler_name;
  uint8_t *wasm_bytes;
  uint32_t wasm_size;
  uint32_t stack_size;
  size_t socket_capacity;
  int stream_body;
  int enable_turbonet_host;
  int enable_http_host;
  int enable_sqlite_db;
  struct iris_wasm_mount_s *next;
} iris_wasm_mount_t;

typedef struct iris_wasm_request_state_s {
  const iris_wasm_mount_t *mount;
  Req *req;
  Res *res;
  int32_t status;
  char *content_type;
  uint8_t *body;
  size_t body_len;
  size_t body_offset;
} iris_wasm_request_state_t;

static iris_wasm_mount_t *g_iris_wasm_mounts = NULL;

static M3Result iris_wasm_route_linker(turbo_wasm3_vm_t *vm, IM3Module module,
                                       void *user_data);
static void iris_wasm_route_handler(Req *req, Res *res);

static int iris_wasm_path_matches(const char *pattern, const char *path);

static M3Result iris_wasm_suppress_lookup_failure(M3Result result) {
  if (result == m3Err_functionLookupFailed) {
    return m3Err_none;
  }
  return result;
}

static char *iris_wasm_strdup(const char *value) {
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

static char *iris_wasm_copy_guest_bytes(const uint8_t *data, uint32_t len) {
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

static int iris_wasm_write_string(const char *value, char *buffer,
                                  uint32_t buffer_size,
                                  uint32_t *out_written) {
  size_t len = value ? strlen(value) : 0;

  if (!out_written) {
    return TURBO_EINVAL;
  }

  if (!buffer && buffer_size != 0) {
    return TURBO_EINVAL;
  }

  if (buffer_size == 0) {
    *out_written = (uint32_t)len;
    return 0;
  }

  if (!buffer) {
    return TURBO_EINVAL;
  }

  if (len >= buffer_size) {
    size_t copy_len = (size_t)buffer_size - 1;

    if (copy_len != 0 && value) {
      memcpy(buffer, value, copy_len);
    }
    buffer[copy_len] = '\0';
    *out_written = (uint32_t)copy_len;
    return TURBO_EMSGSIZE;
  }

  if (len != 0 && value) {
    memcpy(buffer, value, len);
  }
  buffer[len] = '\0';
  *out_written = (uint32_t)len;
  return 0;
}

static int iris_wasm_write_bytes(const void *value, size_t value_len,
                                 uint8_t *buffer, uint32_t buffer_size,
                                 uint32_t *out_written) {
  if (!out_written) {
    return TURBO_EINVAL;
  }
  if (!buffer && buffer_size != 0) {
    return TURBO_EINVAL;
  }

  if (buffer_size == 0) {
    *out_written = (uint32_t)value_len;
    return 0;
  }

  if (value_len > buffer_size) {
    if (buffer_size != 0 && value && value_len != 0) {
      memcpy(buffer, value, buffer_size);
    }
    *out_written = buffer_size;
    return TURBO_EMSGSIZE;
  }

  if (value_len != 0 && value) {
    memcpy(buffer, value, value_len);
  }
  *out_written = (uint32_t)value_len;
  return 0;
}

static int iris_wasm_set_content_type(iris_wasm_request_state_t *state,
                                      const char *content_type) {
  char *copy;

  if (!state || !content_type) {
    return TURBO_EINVAL;
  }

  copy = iris_wasm_strdup(content_type);
  if (!copy) {
    return TURBO_ENOMEM;
  }

  free(state->content_type);
  state->content_type = copy;
  return 0;
}

static int iris_wasm_set_body(iris_wasm_request_state_t *state,
                              const uint8_t *body, uint32_t body_len) {
  uint8_t *copy = NULL;

  if (!state) {
    return TURBO_EINVAL;
  }
  if (!body && body_len != 0) {
    return TURBO_EINVAL;
  }

  if (body_len != 0) {
    copy = (uint8_t *)malloc(body_len);
    if (!copy) {
      return TURBO_ENOMEM;
    }
    memcpy(copy, body, body_len);
  }

  free(state->body);
  state->body = copy;
  state->body_len = body_len;
  return 0;
}

static void iris_wasm_free_request_state(iris_wasm_request_state_t *state) {
  if (!state) {
    return;
  }

  free(state->content_type);
  state->content_type = NULL;
  free(state->body);
  state->body = NULL;
  state->body_len = 0;
}

static void iris_wasm_free_mount(iris_wasm_mount_t *mount) {
  if (!mount) {
    return;
  }

  free(mount->method);
  free(mount->path);
  free(mount->wasm_path);
  free(mount->module_name);
  free(mount->handler_name);
  free(mount->wasm_bytes);
  free(mount);
}

static const iris_wasm_mount_t *iris_wasm_find_mount(const Req *req) {
  iris_wasm_mount_t *mount = g_iris_wasm_mounts;

  if (!req || !req->app || !req->method || !req->path) {
    return NULL;
  }

  while (mount) {
    if (mount->app == req->app && strcmp(mount->method, req->method) == 0 &&
        iris_wasm_path_matches(mount->path, req->path)) {
      return mount;
    }
    mount = mount->next;
  }

  return NULL;
}

static const char *iris_wasm_next_segment(const char *cursor,
                                          const char **segment_start,
                                          size_t *segment_len) {
  const char *start = cursor;

  while (*start == '/') {
    ++start;
  }

  cursor = start;
  while (*cursor != '\0' && *cursor != '/') {
    ++cursor;
  }

  *segment_start = start;
  *segment_len = (size_t)(cursor - start);
  while (*cursor == '/') {
    ++cursor;
  }
  return cursor;
}

static int iris_wasm_path_matches(const char *pattern, const char *path) {
  const char *pattern_cursor;
  const char *path_cursor;

  if (!pattern || !path) {
    return 0;
  }

  pattern_cursor = pattern;
  path_cursor = path;

  while (1) {
    const char *pattern_segment = NULL;
    const char *path_segment = NULL;
    size_t pattern_len = 0;
    size_t path_len = 0;

    pattern_cursor =
        iris_wasm_next_segment(pattern_cursor, &pattern_segment, &pattern_len);
    path_cursor = iris_wasm_next_segment(path_cursor, &path_segment, &path_len);

    if (pattern_len == 0 && path_len == 0) {
      return 1;
    }

    if (pattern_len == 1 && pattern_segment[0] == '*') {
      return 1;
    }

    if (pattern_len == 0 || path_len == 0) {
      return 0;
    }

    if (pattern_segment[0] == ':') {
      continue;
    }

    if (pattern_len != path_len ||
        memcmp(pattern_segment, path_segment, pattern_len) != 0) {
      return 0;
    }
  }
}

static int iris_wasm_request_value(const char *value, char *buffer,
                                   uint32_t buffer_size,
                                   uint32_t *out_written) {
  return iris_wasm_write_string(value ? value : "", buffer, buffer_size,
                                out_written);
}

m3ApiRawFunction(iris_wasm_request_method) {
  iris_wasm_request_state_t *state = (iris_wasm_request_state_t *)_ctx->userdata;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!state || !state->req || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  rc = iris_wasm_write_string(state->req->method, buffer, buffer_size,
                              out_written);
  m3ApiReturn(rc);
}

m3ApiRawFunction(iris_wasm_request_path) {
  iris_wasm_request_state_t *state = (iris_wasm_request_state_t *)_ctx->userdata;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!state || !state->req || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  rc = iris_wasm_write_string(state->req->path, buffer, buffer_size,
                              out_written);
  m3ApiReturn(rc);
}

m3ApiRawFunction(iris_wasm_request_body) {
  iris_wasm_request_state_t *state = (iris_wasm_request_state_t *)_ctx->userdata;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArgMem(uint8_t *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!state || !state->req || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  rc = iris_wasm_write_bytes(state->req->body, state->req->body_len, buffer,
                             buffer_size, out_written);
  m3ApiReturn(rc);
}

m3ApiRawFunction(iris_wasm_request_header) {
  iris_wasm_request_state_t *state = (iris_wasm_request_state_t *)_ctx->userdata;
  char *name = NULL;
  const char *value;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArgMem(const uint8_t *, name_data)
  m3ApiGetArg(uint32_t, name_len)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!state || !state->req || !out_written || name_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  m3ApiCheckMem(name_data, name_len);
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  name = iris_wasm_copy_guest_bytes(name_data, name_len);
  if (!name) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  value = get_headers(state->req, name);
  free(name);
  if (!value) {
    m3ApiReturn(TURBO_ENOENT);
  }

  rc = iris_wasm_write_string(value, buffer, buffer_size, out_written);
  m3ApiReturn(rc);
}

m3ApiRawFunction(iris_wasm_request_param) {
  iris_wasm_request_state_t *state = (iris_wasm_request_state_t *)_ctx->userdata;
  char *name = NULL;
  const char *value = NULL;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArgMem(const uint8_t *, name_data)
  m3ApiGetArg(uint32_t, name_len)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!state || !state->req || !out_written || name_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  m3ApiCheckMem(name_data, name_len);
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  name = iris_wasm_copy_guest_bytes(name_data, name_len);
  if (!name) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  value = get_params(state->req, name);
  free(name);
  if (!value) {
    m3ApiReturn(TURBO_ENOENT);
  }

  rc = iris_wasm_request_value(value, buffer, buffer_size, out_written);
  m3ApiReturn(rc);
}

m3ApiRawFunction(iris_wasm_request_query) {
  iris_wasm_request_state_t *state = (iris_wasm_request_state_t *)_ctx->userdata;
  char *name = NULL;
  const char *value = NULL;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArgMem(const uint8_t *, name_data)
  m3ApiGetArg(uint32_t, name_len)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!state || !state->req || !out_written || name_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  m3ApiCheckMem(name_data, name_len);
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  name = iris_wasm_copy_guest_bytes(name_data, name_len);
  if (!name) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  value = get_query(state->req, name);
  free(name);
  if (!value) {
    m3ApiReturn(TURBO_ENOENT);
  }

  rc = iris_wasm_request_value(value, buffer, buffer_size, out_written);
  m3ApiReturn(rc);
}

m3ApiRawFunction(iris_wasm_request_is_body_stream) {
  iris_wasm_request_state_t *state = (iris_wasm_request_state_t *)_ctx->userdata;

  m3ApiReturnType(int32_t)

  if (!state || !state->req) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(req_is_body_stream(state->req) ? 1 : 0);
}

m3ApiRawFunction(iris_wasm_request_body_read) {
  iris_wasm_request_state_t *state = (iris_wasm_request_state_t *)_ctx->userdata;
  uint32_t read_len = 0;
  size_t available = 0;

  m3ApiReturnType(int32_t)
  m3ApiGetArgMem(uint8_t *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_read)

  if (!state || !state->req || !buffer || buffer_size == 0 || !out_read) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(buffer, buffer_size);
  m3ApiCheckMem(out_read, sizeof(uint32_t));
  m3ApiWriteMem32(out_read, 0);

  if (req_is_body_stream(state->req)) {
    read_len = (uint32_t)req_read_body(state->req, (char *)buffer, buffer_size);
    m3ApiWriteMem32(out_read, read_len);
    m3ApiReturn(0);
  }

  if (!state->req->body || state->body_offset >= state->req->body_len) {
    m3ApiReturn(0);
  }

  available = state->req->body_len - state->body_offset;
  if (available > buffer_size) {
    available = buffer_size;
  }

  memcpy(buffer, state->req->body + state->body_offset, available);
  state->body_offset += available;
  m3ApiWriteMem32(out_read, (uint32_t)available);
  m3ApiReturn(0);
}

m3ApiRawFunction(iris_wasm_response_set_status) {
  iris_wasm_request_state_t *state = (iris_wasm_request_state_t *)_ctx->userdata;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(int32_t, status)

  if (!state) {
    m3ApiReturn(TURBO_EINVAL);
  }

  state->status = status;
  m3ApiReturn(0);
}

m3ApiRawFunction(iris_wasm_response_set_content_type) {
  iris_wasm_request_state_t *state = (iris_wasm_request_state_t *)_ctx->userdata;
  char *content_type = NULL;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArgMem(const uint8_t *, content_type_data)
  m3ApiGetArg(uint32_t, content_type_len)

  if (!state || content_type_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(content_type_data, content_type_len);
  content_type = iris_wasm_copy_guest_bytes(content_type_data, content_type_len);
  if (!content_type) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = iris_wasm_set_content_type(state, content_type);
  free(content_type);
  m3ApiReturn(rc);
}

m3ApiRawFunction(iris_wasm_response_set_header) {
  iris_wasm_request_state_t *state = (iris_wasm_request_state_t *)_ctx->userdata;
  char *name = NULL;
  char *value = NULL;
  int rc = 0;

  m3ApiReturnType(int32_t)
  m3ApiGetArgMem(const uint8_t *, name_data)
  m3ApiGetArg(uint32_t, name_len)
  m3ApiGetArgMem(const uint8_t *, value_data)
  m3ApiGetArg(uint32_t, value_len)

  if (!state || !state->res || name_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(name_data, name_len);
  if (value_len != 0) {
    m3ApiCheckMem(value_data, value_len);
  }

  name = iris_wasm_copy_guest_bytes(name_data, name_len);
  value = iris_wasm_copy_guest_bytes(value_data, value_len);
  if (!name || (value_len != 0 && !value)) {
    free(name);
    free(value);
    m3ApiReturn(TURBO_ENOMEM);
  }

  if (tstr_casecmp(name, "Content-Type") == 0) {
    rc = iris_wasm_set_content_type(state, value ? value : "");
  } else {
    set_header(state->res, name, value ? value : "");
  }

  free(name);
  free(value);
  m3ApiReturn(rc);
}

m3ApiRawFunction(iris_wasm_response_set_body) {
  iris_wasm_request_state_t *state = (iris_wasm_request_state_t *)_ctx->userdata;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArgMem(const uint8_t *, body_data)
  m3ApiGetArg(uint32_t, body_len)

  if (!state) {
    m3ApiReturn(TURBO_EINVAL);
  }

  if (body_len != 0) {
    m3ApiCheckMem(body_data, body_len);
  }

  rc = iris_wasm_set_body(state, body_data, body_len);
  m3ApiReturn(rc);
}

static M3Result iris_wasm_route_linker(turbo_wasm3_vm_t *vm, IM3Module module,
                                       void *user_data) {
  M3Result result = m3Err_none;
  const char *mod = "iris";

  (void)vm;

  result = iris_wasm_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "request_method", "i(*i*)", &iris_wasm_request_method,
      user_data));
  if (result) {
    return result;
  }

  result = iris_wasm_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "request_path", "i(*i*)", &iris_wasm_request_path,
      user_data));
  if (result) {
    return result;
  }

  result = iris_wasm_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "request_body", "i(*i*)", &iris_wasm_request_body,
      user_data));
  if (result) {
    return result;
  }

  result = iris_wasm_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "request_header", "i(*i*i*)", &iris_wasm_request_header,
      user_data));
  if (result) {
    return result;
  }

  result = iris_wasm_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "request_param", "i(*i*i*)", &iris_wasm_request_param,
      user_data));
  if (result) {
    return result;
  }

  result = iris_wasm_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "request_query", "i(*i*i*)", &iris_wasm_request_query,
      user_data));
  if (result) {
    return result;
  }

  result = iris_wasm_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "request_is_body_stream", "i()",
      &iris_wasm_request_is_body_stream, user_data));
  if (result) {
    return result;
  }

  result = iris_wasm_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "request_body_read", "i(*i*)", &iris_wasm_request_body_read,
      user_data));
  if (result) {
    return result;
  }

  result = iris_wasm_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "response_set_status", "i(i)",
      &iris_wasm_response_set_status, user_data));
  if (result) {
    return result;
  }

  result = iris_wasm_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "response_set_content_type", "i(*i)",
      &iris_wasm_response_set_content_type, user_data));
  if (result) {
    return result;
  }

  result = iris_wasm_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "response_set_header", "i(*i*i)",
      &iris_wasm_response_set_header, user_data));
  if (result) {
    return result;
  }

  result = iris_wasm_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "response_set_body", "i(*i)",
      &iris_wasm_response_set_body, user_data));
  if (result) {
    return result;
  }

  return m3Err_none;
}

static void iris_wasm_execute_mount(const iris_wasm_mount_t *mount, Req *req,
                                    Res *res) {
  turbo_wasm3_vm_t *vm = NULL;
  iris_wasm_request_state_t state;
  IM3Module module = NULL;
  IM3Function handler = NULL;
  int32_t guest_rc = 0;
  M3Result result = m3Err_none;
  const char *content_type;

  if (!mount || !req || !res) {
    send_text(res, 500, "wasm mount missing");
    return;
  }

  memset(&state, 0, sizeof(state));
  state.mount = mount;
  state.req = req;
  state.res = res;
  state.status = 200;

  vm = turbo_wasm3_vm_create(mount->stack_size, NULL, mount->socket_capacity);
  if (!vm) {
    send_text(res, 500, "failed to create wasm vm");
    return;
  }

  if (mount->enable_sqlite_db && turbo_wasm3_vm_enable_sqlite_db(vm) != 0) {
    send_text(res, 500, "failed to enable wasm sqlite");
    goto done;
  }
  if (mount->enable_turbonet_host &&
      turbo_wasm3_vm_enable_host(vm) != 0) {
    send_text(res, 500, "failed to enable wasm host");
    goto done;
  }
  if (mount->enable_http_host && turbo_wasm3_vm_enable_http_host(vm) != 0) {
    send_text(res, 500, "failed to enable wasm http host");
    goto done;
  }
  if (turbo_wasm3_vm_add_host_linker(vm, iris_wasm_route_linker, &state) != 0) {
    send_text(res, 500, "failed to add iris wasm linker");
    goto done;
  }

  result = turbo_wasm3_vm_load_module(vm, mount->wasm_bytes, mount->wasm_size,
                                      mount->module_name, &module);
  if (result) {
    send_text(res, 500, result);
    goto done;
  }

  result =
      m3_FindFunction(&handler, turbo_wasm3_vm_get_runtime(vm), mount->handler_name);
  if (result) {
    send_text(res, 500, result);
    goto done;
  }

  result = m3_CallV(handler);
  if (result) {
    send_text(res, 500, result);
    goto done;
  }

  result = m3_GetResultsV(handler, &guest_rc);
  if (result) {
    send_text(res, 500, result);
    goto done;
  }

  if (guest_rc != 0) {
    if (!state.body) {
      char errbuf[64];
      fprintf(stderr, "iris wasm guest rc=%d for %s %s\n", guest_rc,
              req->method ? req->method : "?", req->path ? req->path : "?");
      snprintf(errbuf, sizeof(errbuf), "wasm handler returned %d", guest_rc);
      send_text(res, 500, errbuf);
    } else {
      content_type = state.content_type ? state.content_type : "text/plain";
      reply(res, state.status > 0 ? state.status : 500, content_type, state.body,
            state.body_len);
    }
    goto done;
  }

  content_type = state.content_type ? state.content_type : "text/plain";
  reply(res, state.status > 0 ? state.status : 200, content_type, state.body,
        state.body_len);

done:
  turbo_wasm3_vm_destroy(vm);
  iris_wasm_free_request_state(&state);
  (void)module;
}

static void iris_wasm_route_handler(Req *req, Res *res) {
  const iris_wasm_mount_t *mount = iris_wasm_find_mount(req);

  if (!mount) {
    send_text(res, 500, "wasm route not mounted");
    return;
  }

  iris_wasm_execute_mount(mount, req, res);
}

int iris_wasm_mount(iris_app_t *app, const char *method, const char *path,
                    const char *wasm_path,
                    const iris_wasm_options_t *options) {
  iris_wasm_mount_t *mount;
  iris_wasm_options_t resolved = IRIS_WASM_OPTIONS_DEFAULT;

  if (options) {
    resolved = *options;
  }

  if (!app || !method || !path || !wasm_path || !resolved.module_name ||
      !resolved.handler_name || resolved.stack_size == 0 ||
      resolved.socket_capacity == 0) {
    return TURBO_EINVAL;
  }

  mount = (iris_wasm_mount_t *)calloc(1, sizeof(*mount));
  if (!mount) {
    return TURBO_ENOMEM;
  }

  mount->app = app;
  mount->method = iris_wasm_strdup(method);
  mount->path = iris_wasm_strdup(path);
  mount->wasm_path = iris_wasm_strdup(wasm_path);
  mount->module_name = iris_wasm_strdup(resolved.module_name);
  mount->handler_name = iris_wasm_strdup(resolved.handler_name);
  if (!mount->method || !mount->path || !mount->wasm_path || !mount->module_name ||
      !mount->handler_name) {
    iris_wasm_free_mount(mount);
    return TURBO_ENOMEM;
  }

  mount->stack_size = resolved.stack_size;
  mount->socket_capacity = resolved.socket_capacity;
  mount->stream_body = resolved.stream_body;
  mount->enable_turbonet_host = resolved.enable_turbonet_host;
  mount->enable_http_host = resolved.enable_http_host;
  mount->enable_sqlite_db = resolved.enable_sqlite_db;

  {
    turbo_fs_buf_t wasm = {0};
    if (turbo_fs_read_file(wasm_path, &wasm) != 0 || wasm.len == 0 ||
        wasm.len > UINT32_MAX) {
      turbo_fs_buf_free(&wasm);
      iris_wasm_free_mount(mount);
      return TURBO_EINVAL;
    }

    mount->wasm_bytes = (uint8_t *)malloc(wasm.len);
    if (!mount->wasm_bytes) {
      turbo_fs_buf_free(&wasm);
      iris_wasm_free_mount(mount);
      return TURBO_ENOMEM;
    }

    memcpy(mount->wasm_bytes, wasm.base, wasm.len);
    mount->wasm_size = (uint32_t)wasm.len;
    turbo_fs_buf_free(&wasm);
  }

  mount->next = g_iris_wasm_mounts;
  g_iris_wasm_mounts = mount;

  if (mount->stream_body) {
    iris_app_route_stream(app, method, path, NO_MW, iris_wasm_route_handler);
  } else {
    iris_app_route(app, method, path, NO_MW, iris_wasm_route_handler);
  }
  return 0;
}

void iris_wasm_reset(void) {
  iris_wasm_mount_t *mount = g_iris_wasm_mounts;

  while (mount) {
    iris_wasm_mount_t *next = mount->next;
    iris_wasm_free_mount(mount);
    mount = next;
  }

  g_iris_wasm_mounts = NULL;
}
