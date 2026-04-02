#include <signal.h>
#include <fmt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"
#include "iris_app.h"
#include "async.h"
#include "error_recovery.h"
#include "router.h"
#include "tlog.h"
#include "CoroNet.h" /* Use coroutine server */
#include "turbo_thread.h"

#define READ_BUF_SIZE 8192

/* Connection context structure - matching definition in router.c */
typedef struct {
  iris_app_t *app;
  coro_socket_t *client;
  char *buffer;
  size_t buffer_capacity;
  size_t buffer_used;
  size_t parsed_offset;
  int keep_alive;
  time_t created_time;
  int request_count;
  mem_pool_t request_arena;
  http_context_t *request_ctx;
  int request_arena_ready;
  int request_streaming_active;
  const char *pending_body_chunk;
  size_t pending_body_chunk_len;
  size_t pending_body_chunk_offset;
  void *middleware_data;                  /* For middleware-specific connection data */
  void (*middleware_cleanup)(void *data); /* Cleanup function for middleware data */
} iris_connection_ctx_t;

static void send_simple_error(coro_socket_t *client, int status, const char *reason) {
  char response[256];
  int written;

  if (!client || !reason) {
    return;
  }

  written = fmt(response, sizeof(response),
                "HTTP/1.1 {} {}\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n"
                "\r\n",
                status, reason);
  if (written <= 0 || (size_t)written >= sizeof(response)) {
    TLOG_ERROR("Failed to format simple error response");
    return;
  }
  if (coro_socket_send(client, response, (size_t)written) != 0) {
    TLOG_ERROR("Failed to send simple error response");
  }
}

static int ensure_connection_buffer_capacity(iris_connection_ctx_t *ctx, size_t needed) {
  size_t new_capacity;
  char *new_buffer;

  if (!ctx) {
    return -1;
  }

  if (needed <= ctx->buffer_capacity) {
    return 0;
  }

  new_capacity = ctx->buffer_capacity ? ctx->buffer_capacity : READ_BUF_SIZE;
  while (new_capacity < needed) {
    if (new_capacity > SIZE_MAX / 2) {
      return -1;
    }
    new_capacity *= 2;
  }

  new_buffer = (char *)realloc(ctx->buffer, new_capacity);
  if (!new_buffer) {
    return -1;
  }

  ctx->buffer = new_buffer;
  ctx->buffer_capacity = new_capacity;
  return 0;
}

static int append_connection_data(iris_connection_ctx_t *ctx, const char *data, size_t len) {
  if (!ctx || !data || len == 0) {
    return 0;
  }

  if (ensure_connection_buffer_capacity(ctx, ctx->buffer_used + len + 1) != 0) {
    return -1;
  }

  memcpy(ctx->buffer + ctx->buffer_used, data, len);
  ctx->buffer_used += len;
  ctx->buffer[ctx->buffer_used] = '\0';
  return 0;
}

static int init_request_parser(iris_connection_ctx_t *ctx) {
  if (!ctx) {
    return -1;
  }

  if (ctx->request_arena_ready) {
    return 0;
  }

  if (mem_init(&ctx->request_arena, 8192) != 0) {
    return -1;
  }

  ctx->request_ctx = mem_alloc(&ctx->request_arena, sizeof(*ctx->request_ctx));
  if (!ctx->request_ctx) {
    mem_destroy(&ctx->request_arena);
    return -1;
  }

  http_context_init(ctx->request_ctx, &ctx->request_arena);
  ctx->request_arena_ready = 1;
  return 0;
}

static void reset_request_parser(iris_connection_ctx_t *ctx) {
  if (!ctx || !ctx->request_arena_ready) {
    return;
  }

  mem_destroy(&ctx->request_arena);
  ctx->request_ctx = NULL;
  ctx->request_arena_ready = 0;
  ctx->request_streaming_active = 0;
  ctx->pending_body_chunk = NULL;
  ctx->pending_body_chunk_len = 0;
  ctx->pending_body_chunk_offset = 0;
}

static void finish_current_request(iris_connection_ctx_t *ctx) {
  if (!ctx) {
    return;
  }

  if (ctx->parsed_offset < ctx->buffer_used) {
    memmove(ctx->buffer, ctx->buffer + ctx->parsed_offset, ctx->buffer_used - ctx->parsed_offset);
  }
  ctx->buffer_used -= ctx->parsed_offset;
  ctx->buffer[ctx->buffer_used] = '\0';
  ctx->parsed_offset = 0;
  reset_request_parser(ctx);
}

static int request_body_fully_received(const http_context_t *ctx) {
  const char *content_length;
  char *end = NULL;
  unsigned long long expected = 0;

  if (!ctx) {
    return 1;
  }

  content_length = get_req(&ctx->headers, "Content-Length");
  if (!content_length || content_length[0] == '\0') {
    return 0;
  }

  expected = strtoull(content_length, &end, 10);
  if (end == content_length || (end && *end != '\0')) {
    return 0;
  }

  return ctx->body_length >= (size_t)expected;
}

static int drain_streaming_request_body(iris_connection_ctx_t *ctx) {
  while (ctx && ctx->request_ctx && !ctx->request_ctx->message_complete) {
    if (ctx->pending_body_chunk_len > ctx->pending_body_chunk_offset) {
      ctx->pending_body_chunk = NULL;
      ctx->pending_body_chunk_len = 0;
      ctx->pending_body_chunk_offset = 0;
      http_context_resume(ctx->request_ctx);
      continue;
    }

    if (ctx->parsed_offset < ctx->buffer_used) {
      size_t consumed = 0;
      int parse_result = http_context_execute(ctx->request_ctx, ctx->buffer + ctx->parsed_offset,
                                              ctx->buffer_used - ctx->parsed_offset, &consumed);
      ctx->parsed_offset += consumed;

      if (parse_result < 0) {
        return parse_result;
      }

      if (parse_result == 3) {
        ctx->pending_body_chunk = ctx->request_ctx->stream_chunk;
        ctx->pending_body_chunk_len = ctx->request_ctx->stream_chunk_len;
        ctx->pending_body_chunk_offset = 0;
        continue;
      }

      if (parse_result == 1) {
        return 1;
      }

      if (parse_result == 0) {
        continue;
      }
    }

    if (ctx->parsed_offset == ctx->buffer_used &&
        request_body_fully_received(ctx->request_ctx)) {
      ctx->request_ctx->message_complete = 1;
      return 1;
    }

    {
      char *data = NULL;
      size_t len = 0;
      int r = coro_socket_recv(ctx->client, &data, &len);

      if (r != 0 || !data || len == 0) {
        if (data) {
          coro_socket_free_recv(data);
        }
        return -400;
      }

      if (append_connection_data(ctx, data, len) != 0) {
        coro_socket_free_recv(data);
        return -500;
      }

      coro_socket_free_recv(data);
    }
  }

  return 1;
}

static int complete_buffered_request_body(iris_connection_ctx_t *ctx) {
  while (ctx && ctx->request_ctx && !ctx->request_ctx->message_complete) {
    if (ctx->parsed_offset < ctx->buffer_used) {
      size_t consumed = 0;
      int parse_result = http_context_execute(ctx->request_ctx, ctx->buffer + ctx->parsed_offset,
                                              ctx->buffer_used - ctx->parsed_offset, &consumed);
      ctx->parsed_offset += consumed;

      if (parse_result < 0) {
        return parse_result;
      }

      if (parse_result == 1) {
        return 1;
      }

      if (parse_result == 2) {
        http_context_resume(ctx->request_ctx);
        continue;
      }
    }

    {
      size_t flushed = 0;
      int flush_result = http_context_execute(ctx->request_ctx, "", 0, &flushed);
      (void)flushed;

      if (flush_result < 0) {
        return flush_result;
      }

      if (flush_result == 1) {
        return 1;
      }
    }

    {
      char *data = NULL;
      size_t len = 0;
      int r = coro_socket_recv(ctx->client, &data, &len);

      if (r != 0 || !data || len == 0) {
        if (data) {
          coro_socket_free_recv(data);
        }
        return -400;
      }

      if (append_connection_data(ctx, data, len) != 0) {
        coro_socket_free_recv(data);
        return -500;
      }

      coro_socket_free_recv(data);
    }
  }

  return 1;
}

/* Global server state */
static coro_socket_t *g_server = NULL;
static coro_context_t *g_coro_ctx = NULL;
static int g_shutdown_requested = 0;
static void (*g_app_shutdown_hook)(void) = NULL;

/* Global shutdown flag for external modules (like pquv) */
int shutdown_requested = 0;

void shutdown_hook(void (*hook)(void)) { g_app_shutdown_hook = hook; }

/* Signal handler for graceful shutdown */
static void signal_handler(int signum) {
  if (g_shutdown_requested) {
    TLOG_INFO("Received signal {}, shutting down...", signum);
    return;
  }

  g_shutdown_requested = 1;
  shutdown_requested = 1; /* Also set global flag for external modules */

  /* Call application cleanup hook */
  if (g_app_shutdown_hook) {
    g_app_shutdown_hook();
  }

  /* Stop the loop */
  if (g_coro_ctx) {
    coro_context_stop(g_coro_ctx);
  }
}

/* Connection Context Cleanup */
static void cleanup_connection_context(iris_connection_ctx_t *ctx) {
  if (!ctx) {
    return;
  }

  /* Call middleware cleanup if registered */
  if (ctx->middleware_data && ctx->middleware_cleanup) {
    ctx->middleware_cleanup(ctx->middleware_data);
    ctx->middleware_data = NULL;
    ctx->middleware_cleanup = NULL;
  }

  /* Log connection statistics for monitoring */
  time_t connection_duration = time(NULL) - ctx->created_time;
  if (connection_duration > 0) {
    TLOG_DEBUG("Connection closed: duration={} seconds, requests={}", (long)connection_duration,
               ctx->request_count);
  }

  /* Free the context structure */
  reset_request_parser(ctx);
  free(ctx->buffer);
  ctx->buffer = NULL;
  ctx->buffer_capacity = 0;
  free(ctx);
}

/* Coroutine Handler for each connection */
static void server_handler(coro_socket_t *client, void *arg) {
  iris_app_t *app = (iris_app_t *)arg;

  /* Initialize connection context */
  iris_connection_ctx_t *ctx = (iris_connection_ctx_t *)calloc(1, sizeof(iris_connection_ctx_t));
  if (!ctx) {
    TLOG_ERROR("Failed to allocate connection context");
    return;
  }

  ctx->app = app;
  ctx->client = client;
  ctx->keep_alive = 1;
  ctx->created_time = time(NULL);
  ctx->request_count = 0;
  ctx->parsed_offset = 0;
  ctx->buffer_capacity = READ_BUF_SIZE;
  ctx->buffer = (char *)malloc(ctx->buffer_capacity);
  if (!ctx->buffer) {
    free(ctx);
    TLOG_ERROR("Failed to allocate connection buffer");
    return;
  }
  ctx->buffer[0] = '\0';

  if (init_request_parser(ctx) != 0) {
    free(ctx->buffer);
    free(ctx);
    TLOG_ERROR("Failed to initialize request parser");
    return;
  }
  
  /* Attach context to client used_data */
  coro_socket_set_user_data(client, ctx);

  char *data = NULL;
  size_t len = 0;
  int r;

  /* Read loop */
  while (1) {
    /* Receive data (yields until data available) */
    r = coro_socket_recv(client, &data, &len);
    
    if (r != 0) {
      if (r != TURBO_EOF) {
        TLOG_DEBUG("Receive error: {}", r);
      }
      break; /* Connection closed or error */
    }

    if (data && len > 0) {
      int should_close = 0;

      if (append_connection_data(ctx, data, len) != 0) {
        coro_socket_free_recv(data);
        TLOG_ERROR("Connection buffer growth failed");
        break;
      }

      coro_socket_free_recv(data);
      data = NULL;

      while (ctx->buffer_used > 0) {
        size_t consumed = 0;
        int parse_result;

        if (!ctx->request_arena_ready && init_request_parser(ctx) != 0) {
          TLOG_ERROR("Request parser reset failed");
          send_simple_error(client, 500, "Internal Server Error");
          should_close = 1;
          break;
        }

        if (ctx->parsed_offset >= ctx->buffer_used) {
          break;
        }

        parse_result = http_context_execute(ctx->request_ctx, ctx->buffer + ctx->parsed_offset,
                                            ctx->buffer_used - ctx->parsed_offset, &consumed);
        ctx->parsed_offset += consumed;

        if (parse_result < 0) {
          send_simple_error(client, (parse_result == -413) ? 413 : 400,
                            (parse_result == -413) ? "Payload Too Large" : "Bad Request");
          should_close = 1;
          break;
        }

        if (parse_result == 0) {
          break;
        }

        if (parse_result == 2) {
          {
            int stream_mode = iris_app_route_uses_stream(app, &ctx->request_arena, ctx->request_ctx);
            if (stream_mode < 0) {
              send_simple_error(client, 500, "Internal Server Error");
              should_close = 1;
              break;
            }
            if (stream_mode) {
            int drain_result;

            ctx->request_ctx->stream_mode = 1;
            ctx->request_streaming_active = 1;
            http_context_resume(ctx->request_ctx);
            ctx->request_count++;
            should_close = iris_app_execute_parsed(app, client, &ctx->request_arena, ctx->request_ctx);
            drain_result = drain_streaming_request_body(ctx);
            if (drain_result < 0) {
              send_simple_error(client, (drain_result == -413) ? 413 : 400,
                                (drain_result == -413) ? "Payload Too Large" : "Bad Request");
              should_close = 1;
            }
            finish_current_request(ctx);
            if (should_close) {
              break;
            }
            continue;
          }
          }

          http_context_resume(ctx->request_ctx);
          {
            int complete_result = complete_buffered_request_body(ctx);
            if (complete_result < 0) {
              send_simple_error(client, (complete_result == -413) ? 413 : 400,
                                (complete_result == -413) ? "Payload Too Large" : "Bad Request");
              should_close = 1;
              break;
            }
          }
          ctx->request_count++;
          should_close = iris_app_execute_parsed(app, client, &ctx->request_arena, ctx->request_ctx);
          finish_current_request(ctx);
          if (should_close) {
            break;
          }
          continue;
        }

        send_simple_error(client, 400, "Bad Request");
        should_close = 1;
        break;
      }

      if (should_close) {
        break;
      }
    } else {
      if (data) coro_socket_free_recv(data);
      break;
    }
  }

  /* Cleanup */
  cleanup_connection_context(ctx);
  coro_socket_set_user_data(client, NULL);
}

/* Server startup function - decoupled for testing */
coro_socket_t* iris_server_start(iris_app_t *app, coro_context_t *ctx, unsigned short port) {
  if (!ctx) return NULL;

  g_coro_ctx = ctx;

  coro_socket_t *server = coro_socket_create(ctx, CORO_SOCKET_TCP_V4);
  if (!server) {
    TLOG_ERROR("Failed to create server");
    g_coro_ctx = NULL;
    return NULL;
  }

  int r = coro_socket_listen_on(server, "0.0.0.0", port, server_handler, app);
  if (r != 0) {
    TLOG_ERROR("Failed to start listening: {}", r);
    coro_socket_destroy(server);
    g_coro_ctx = NULL;
    return NULL;
  }

  return server;
}

int iris_app_run(iris_app_t *app, unsigned short port) {
  if (!app) return -1;

  /* Initialize error recovery system */
  if (iris_error_recovery_init() != 0) {
    TLOG_ERROR("Failed to initialize error recovery system");
    return -1;
  }

  g_coro_ctx = coro_context_create(NULL);
  if (!g_coro_ctx) {
    TLOG_ERROR("Failed to create coro context");
    iris_error_recovery_cleanup();
    return -1;
  }

  /* Install signal handlers */
  signal(SIGINT, signal_handler);
#ifndef _WIN32
  signal(SIGTERM, signal_handler);
  signal(SIGHUP, signal_handler);
#endif
#ifdef _WIN32
  signal(SIGBREAK, signal_handler);
  signal(SIGTERM, signal_handler);
#endif

  /* Start server with app context */
  g_server = iris_server_start(app, g_coro_ctx, port);
  if (!g_server) {
    coro_context_destroy(g_coro_ctx);
    g_coro_ctx = NULL;
    iris_error_recovery_cleanup();
    return -1;
  }

  /* Initialize thread pool for iris_await() */
  if (iris_async_init(0) != 0) {
    coro_socket_destroy(g_server);
    g_server = NULL;
    coro_context_destroy(g_coro_ctx);
    g_coro_ctx = NULL;
    iris_error_recovery_cleanup();
    return -1;
  }

  TLOG_INFO("Server is running on http://localhost:{}", port);

  /* Run the loop */
  coro_context_run(g_coro_ctx, TURBO_RUN_DEFAULT);

  /* Cleanup */
  TLOG_INFO("Shutting down server...");

  iris_async_shutdown();

  if (g_server) {
    coro_socket_destroy(g_server);
    g_server = NULL;
  }

  if (g_coro_ctx) {
    coro_context_destroy(g_coro_ctx);
    g_coro_ctx = NULL;
  }

  TLOG_INFO("Server shutdown complete");

  /* Cleanup error recovery system */
  iris_error_recovery_cleanup();

  return 0;
}

int iris_server_run(unsigned short PORT) {
  return iris_app_run(iris_app_default(), PORT);
}
