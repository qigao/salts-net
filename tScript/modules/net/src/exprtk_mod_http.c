/**
 * @file exprtk_mod_http.c
 * @brief HTTP and WebSocket module: http.* and ws.* for TurboScript.
 */
#include "net_ctx.h"

#include <stdio.h>

#define NET_ONE ((exprtk_value_t){exprtk_VAL_NUMBER, .data.number = 1.0})

/* == Lifecycle ============================================================ */

void *net_ctx_create(void) {
  return calloc(1, sizeof(net_ctx_t));
}

void net_ctx_destroy(void *p) {
  net_ctx_t *ctx = (net_ctx_t *)p;
  if (!ctx) return;
  if (ctx->client) http_client_destroy(ctx->client);
  if (ctx->ws_client) turbo_client_destroy(ctx->ws_client);
  free(ctx);
}

/* == Helpers ============================================================== */

static exprtk_value_t copy_response_body(http_response_t *resp, exprtk_env_t *env) {
  if (resp && resp->status_code >= 200 && resp->status_code < 300 && resp->body) {
    char *buf = turbo_arena_alloc(&env->arena, resp->body_len + 1);
    if (buf) {
      memcpy(buf, resp->body, resp->body_len);
      buf[resp->body_len] = '\0';
      return (exprtk_value_t){exprtk_VAL_STRING,
                              .data.string = tstr_v_from_buf(buf, resp->body_len)};
    }
  }
  return NET_ZERO;
}

static void net_set_error(net_ctx_t *ctx, const char *msg) {
  if (!ctx) return;
  snprintf(ctx->error_msg, sizeof(ctx->error_msg), "%s", msg ? msg : "");
}

static turbo_client_t *net_ctx_recreate_ws_client(net_ctx_t *ctx) {
  if (!ctx) return NULL;
  if (ctx->ws_client) {
    turbo_client_destroy(ctx->ws_client);
    ctx->ws_client = NULL;
  }
  ctx->ws_client = turbo_client_create_with_transport(SYNC_CLIENT_TRANSPORT_WEBSOCKET);
  return ctx->ws_client;
}

/* == Script functions: HTTP ============================================== */

static exprtk_value_t fn_http_get(size_t argc, exprtk_value_t *args, void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;
  if (argc != 1 || args[0].type != exprtk_VAL_STRING)
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
  if (argc != 2 || args[0].type != exprtk_VAL_STRING || args[1].type != exprtk_VAL_STRING)
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
  if (!ud || !ud->ctx || argc < 1 || argc > 2 || args[0].type != exprtk_VAL_STRING)
    return NET_ZERO;
  if (argc == 2 && args[1].type != exprtk_VAL_STRING)
    return NET_ZERO;

  turbo_client_t *client = net_ctx_recreate_ws_client(ud->ctx);
  if (!client) {
    net_set_error(ud->ctx, "ws client create failed");
    return NET_ZERO;
  }

  char *url = net_arena_cstr(ud->scratch, args[0].data.string);
  if (!url) {
    net_set_error(ud->ctx, "url alloc failed");
    return NET_ZERO;
  }

  turbo_client_ws_config_t ws_config = {0};
  ws_config.path = "/";
  ws_config.use_tls = (strncmp(url, "wss://", 6) == 0) ? 1 : 0;
  if (argc == 2) {
    char *path = net_arena_cstr(ud->scratch, args[1].data.string);
    if (!path) {
      net_set_error(ud->ctx, "path alloc failed");
      return NET_ZERO;
    }
    ws_config.path = path;
  }

  turbo_client_status_t st = turbo_client_set_ws_config(client, &ws_config);
  if (st != SYNC_CLIENT_STATUS_OK) {
    net_set_error(ud->ctx, turbo_client_status_to_string(st));
    return NET_ZERO;
  }

  st = turbo_client_connect(client, url);
  if (st != SYNC_CLIENT_STATUS_OK) {
    net_set_error(ud->ctx, turbo_client_last_message(client));
    return NET_ZERO;
  }
  net_set_error(ud->ctx, "");
  return NET_ONE;
}

static exprtk_value_t fn_ws_send(size_t argc, exprtk_value_t *args, void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;
  if (!ud || !ud->ctx || argc != 1 || args[0].type != exprtk_VAL_STRING)
    return NET_ZERO;
  if (!ud->ctx->ws_client) {
    net_set_error(ud->ctx, "ws client not connected");
    return NET_ZERO;
  }
  tstr_v payload = args[0].data.string;
  turbo_client_status_t st = turbo_client_send(ud->ctx->ws_client, payload.data, payload.len);
  if (st != SYNC_CLIENT_STATUS_OK) {
    net_set_error(ud->ctx, turbo_client_last_message(ud->ctx->ws_client));
    return NET_ZERO;
  }
  net_set_error(ud->ctx, "");
  return NET_ONE;
}

static exprtk_value_t fn_ws_recv(size_t argc, exprtk_value_t *args, void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;
  if (!ud || !ud->ctx || argc > 1)
    return NET_ZERO;
  if (argc == 1 && args[0].type != exprtk_VAL_NUMBER)
    return NET_ZERO;
  if (!ud->ctx->ws_client) {
    net_set_error(ud->ctx, "ws client not connected");
    return NET_ZERO;
  }

  int timeout_ms = 5000;
  if (argc == 1)
    timeout_ms = (int)args[0].data.number;

  char *resp = NULL;
  size_t len = 0;
  turbo_client_status_t st =
      turbo_client_receive_timeout(ud->ctx->ws_client, &resp, &len, timeout_ms);
  if (st != SYNC_CLIENT_STATUS_OK || !resp) {
    net_set_error(ud->ctx, turbo_client_status_to_string(st));
    if (resp) free(resp);
    return NET_ZERO;
  }

  char *buf = turbo_arena_alloc(&ud->env->arena, len + 1);
  if (!buf) {
    free(resp);
    net_set_error(ud->ctx, "response alloc failed");
    return NET_ZERO;
  }
  memcpy(buf, resp, len);
  buf[len] = '\0';
  free(resp);
  net_set_error(ud->ctx, "");
  return (exprtk_value_t){exprtk_VAL_STRING, .data.string = tstr_v_from_buf(buf, len)};
}

static exprtk_value_t fn_ws_close(size_t argc, exprtk_value_t *args, void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;
  (void)args;
  if (!ud || !ud->ctx || argc != 0)
    return NET_ZERO;
  if (ud->ctx->ws_client) {
    turbo_client_destroy(ud->ctx->ws_client);
    ud->ctx->ws_client = NULL;
  }
  net_set_error(ud->ctx, "");
  return NET_ONE;
}

/* == Loader =============================================================== */

void net_load(void *p, void *e, void *s) {
  net_ctx_t *ctx = (net_ctx_t *)p;
  exprtk_env_t *env = (exprtk_env_t *)e;
  turbo_arena_t *scratch = (turbo_arena_t *)s;
  if (!ctx || !env) return;
  http_ud_t *ud = turbo_arena_alloc(&env->arena, sizeof(*ud));
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
