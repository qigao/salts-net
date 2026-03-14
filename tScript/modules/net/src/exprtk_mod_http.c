/**
 * @file exprtk_mod_http.c
 * @brief HTTP and WebSocket module: http.* and ws.* for TurboScript.
 */
#include "net_ctx.h"
#include <CoroNet.h>
#include <turbo_protocol.h>
#include <stdlib.h>
#include <stdio.h>

#define NET_ONE ((exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = 1.0})

/* == Lifecycle ============================================================ */

void *net_ctx_create(void) {
  return calloc(1, sizeof(net_ctx_t));
}

void net_ctx_destroy(void *p) {
  net_ctx_t *ctx = (net_ctx_t *)p;
  if (!ctx) return;
  if (ctx->client) http_client_destroy(ctx->client);
  if (ctx->ws_client) coro_socket_destroy(ctx->ws_client);
  free(ctx);
}

/* == Helpers ============================================================== */

static exprtk_value_t copy_response_body(http_response_t *resp, exprtk_env_t *env) {
  if (resp && resp->status_code >= 200 && resp->status_code < 300 && resp->body) {
    char *buf = mem_alloc(&env->arena, resp->body_len + 1);
    if (buf) {
      memcpy(buf, resp->body, resp->body_len);
      buf[resp->body_len] = '\0';
      return (exprtk_value_t){EXPRTK_VAL_STRING,
                              .data.string = tstr_v_from_buf(buf, resp->body_len)};
    }
  }
  return NET_ZERO;
}

static void net_set_error(net_ctx_t *ctx, const char *msg) {
  if (!ctx) return;
  snprintf(ctx->error_msg, sizeof(ctx->error_msg), "%s", msg ? msg : "");
}

static coro_socket_t *net_ctx_recreate_ws_client(net_ctx_t *ctx) {
  if (!ctx) return NULL;
  if (ctx->ws_client) {
    coro_socket_destroy(ctx->ws_client);
    ctx->ws_client = NULL;
  }
  
  coro_context_t *cctx = coro_context_current();
  if (!cctx) {
      net_set_error(ctx, "no coroutine context");
      return NULL;
  }
  
  ctx->ws_client = coro_socket_create_tcpv4(cctx);
  return ctx->ws_client;
}

/* == Script functions: HTTP ============================================== */

static exprtk_value_t fn_http_get(size_t argc, exprtk_value_t *args, void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;
  if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
    return NET_ZERO;

  http_client_t *c = net_ctx_ensure_client(ud->ctx);
  if (!c) return NET_ZERO;

  char *url = net_arena_cstr(ud->scratch, args[0].data.string);
  http_response_t *resp = http_get(c, url);
  exprtk_value_t ret = copy_response_body(resp, ud->env);
  if (resp) http_response_free(resp);
  return ret;
}

static exprtk_value_t fn_http_post(size_t argc, exprtk_value_t *args, void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;
  if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING)
    return NET_ZERO;

  http_client_t *c = net_ctx_ensure_client(ud->ctx);
  if (!c) return NET_ZERO;

  char *url = net_arena_cstr(ud->scratch, args[0].data.string);
  tstr_v body = args[1].data.string;
  http_response_t *resp = http_post(c, url, body.data, body.len);
  exprtk_value_t ret = copy_response_body(resp, ud->env);
  if (resp) http_response_free(resp);
  return ret;
}

/* == Script functions: WebSocket ========================================= */

static exprtk_value_t fn_ws_connect(size_t argc, exprtk_value_t *args, void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;
  if (!ud || !ud->ctx || argc < 1 || argc > 2 || args[0].type != EXPRTK_VAL_STRING)
    return NET_ZERO;
  if (argc == 2 && args[1].type != EXPRTK_VAL_STRING)
    return NET_ZERO;

  coro_socket_t *client = net_ctx_recreate_ws_client(ud->ctx);
  if (!client) {
    return NET_ZERO;
  }

  char *url = net_arena_cstr(ud->scratch, args[0].data.string);
  if (!url) {
    net_set_error(ud->ctx, "url alloc failed");
    return NET_ZERO;
  }

  /* Note: In CoroNet, WebSocket path is handled automatically by coro_socket_connect 
     or can be set via URL. The argc==2 path argument from exprtk is currently ignored
     or should be appended to URL. For now we follow the URL. */

  coro_socket_set_timeout(client, 10000);
  int r = coro_socket_connect(client, url);
  if (r != 0) {
    net_set_error(ud->ctx, "ws connect failed");
    return NET_ZERO;
  }
  
  net_set_error(ud->ctx, "");
  return NET_ONE;
}

static exprtk_value_t fn_ws_send(size_t argc, exprtk_value_t *args, void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;
  if (!ud || !ud->ctx || argc != 1 || args[0].type != EXPRTK_VAL_STRING)
    return NET_ZERO;
  if (!ud->ctx->ws_client) {
    net_set_error(ud->ctx, "ws client not connected");
    return NET_ZERO;
  }
  tstr_v payload = args[0].data.string;
  int r = coro_socket_send(ud->ctx->ws_client, payload.data, payload.len);
  if (r < 0) {
    net_set_error(ud->ctx, "ws send failed");
    return NET_ZERO;
  }
  net_set_error(ud->ctx, "");
  return NET_ONE;
}

static exprtk_value_t fn_ws_recv(size_t argc, exprtk_value_t *args, void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;
  if (!ud || !ud->ctx || argc > 1)
    return NET_ZERO;
  if (argc == 1 && args[0].type != EXPRTK_VAL_NUMBER)
    return NET_ZERO;
  if (!ud->ctx->ws_client) {
    net_set_error(ud->ctx, "ws client not connected");
    return NET_ZERO;
  }

  int timeout_ms = 5000;
  if (argc == 1)
    timeout_ms = (int)args[0].data.number;

  coro_socket_set_timeout(ud->ctx->ws_client, (uint64_t)timeout_ms);

  char *resp = NULL;
  size_t len = 0;
  int r = coro_socket_recv(ud->ctx->ws_client, &resp, &len);
  
  if (r != 0 || !resp) {
    net_set_error(ud->ctx, "ws recv failed or timeout");
    if (resp) coro_socket_free_recv(resp);
    return NET_ZERO;
  }

  char *buf = mem_alloc(&ud->env->arena, len + 1);
  if (!buf) {
    coro_socket_free_recv(resp);
    net_set_error(ud->ctx, "response alloc failed");
    return NET_ZERO;
  }
  memcpy(buf, resp, len);
  buf[len] = '\0';
  coro_socket_free_recv(resp);
  net_set_error(ud->ctx, "");
  return (exprtk_value_t){EXPRTK_VAL_STRING, .data.string = tstr_v_from_buf(buf, len)};
}

static exprtk_value_t fn_ws_close(size_t argc, exprtk_value_t *args, void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;
  (void)args;
  if (!ud || !ud->ctx || argc != 0)
    return NET_ZERO;
  if (ud->ctx->ws_client) {
    coro_socket_destroy(ud->ctx->ws_client);
    ud->ctx->ws_client = NULL;
  }
  net_set_error(ud->ctx, "");
  return NET_ONE;
}

/* == Loader =============================================================== */

void net_load(void *p, void *e, void *s) {
  net_ctx_t *ctx = (net_ctx_t *)p;
  exprtk_env_t *env = (exprtk_env_t *)e;
  mem_pool_t *scratch = (mem_pool_t *)s;
  if (!ctx || !env) return;
  http_ud_t *ud = mem_alloc(&env->arena, sizeof(*ud));
  if (!ud) return;
  ud->ctx = ctx;
  ud->env = env;
  ud->scratch = scratch;
  exprtk_env_register_func(env, "http.get", fn_http_get, ud);
  exprtk_env_register_func(env, "http.post", fn_http_post, ud);
  exprtk_env_register_func(env, "ws.connect", fn_ws_connect, ud);
  exprtk_env_register_func(env, "ws.send", fn_ws_send, ud);
  exprtk_env_register_func(env, "ws.recv", fn_ws_recv, ud);
  exprtk_env_register_func(env, "ws.close", fn_ws_close, ud);
}
