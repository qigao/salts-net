#include <signal.h>
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
  char buffer[READ_BUF_SIZE];
  size_t buffer_used;
  int keep_alive;
  time_t created_time;
  int request_count;
  void *middleware_data;                  /* For middleware-specific connection data */
  void (*middleware_cleanup)(void *data); /* Cleanup function for middleware data */
} iris_connection_ctx_t;

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
      ctx->request_count++;
      
      /* Process request via app-aware router */
      /* iris_app_execute returns 1 to close, 0 to keep alive */
      int should_close = iris_app_execute(app, client, data, len);
      
      free(data);
      data = NULL;

      if (should_close) {
        break;
      }
    } else {
      if (data) free(data);
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

  char listen_url[64];
  snprintf(listen_url, sizeof(listen_url), "tcp://0.0.0.0:%d", port);

  int r = coro_socket_listen_url(server, listen_url, server_handler, app);
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
  iris_async_init(0);

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
