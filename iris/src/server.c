#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"
#include "async.h"
#include "error_recovery.h"
#include "router.h"
#include "tlog.h"
#include "netcore/turbo_coro_server.h" /* Use coroutine server */
#include "netcore/turbo_coro_client.h"
#include "turbo_thread.h"

#define READ_BUF_SIZE 8192

/* Connection context structure - matching definition in router.c */
typedef struct {
  turbo_coro_client_t *client;
  char buffer[READ_BUF_SIZE];
  size_t buffer_used;
  int keep_alive;
  time_t created_time;
  int request_count;
  void *middleware_data;                  /* For middleware-specific connection data */
  void (*middleware_cleanup)(void *data); /* Cleanup function for middleware data */
} iris_connection_ctx_t;

/* Global server state */
static turbo_coro_server_t *g_server = NULL;
static turbo_coro_context_t *g_coro_ctx = NULL;
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
    turbo_coro_context_stop(g_coro_ctx);
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
static void server_handler(turbo_coro_client_t *client, void *arg) {
  (void)arg;

  /* Initialize connection context */
  iris_connection_ctx_t *ctx = (iris_connection_ctx_t *)calloc(1, sizeof(iris_connection_ctx_t));
  if (!ctx) {
    TLOG_ERROR("Failed to allocate connection context");
    return;
  }

  ctx->client = client;
  ctx->keep_alive = 1;
  ctx->created_time = time(NULL);
  ctx->request_count = 0;
  
  /* Attach context to client used_data */
  turbo_coro_client_set_user_data(client, ctx);

  char *data = NULL;
  size_t len = 0;
  int r;

  /* Read loop */
  while (1) {
    /* Receive data (yields until data available) */
    r = turbo_coro_client_recv(client, &data, &len);
    
    if (r != 0) {
      if (r != TURBO_EOF) {
        TLOG_DEBUG("Receive error: {}", r);
      }
      break; /* Connection closed or error */
    }

    if (data && len > 0) {
      ctx->request_count++;
      
      /* Process request via router */
      /* Router returns 1 to close, 0 to keep alive */
      int should_close = router(client, data, len);
      
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
  turbo_coro_client_set_user_data(client, NULL);
}

/* Server startup function */
/* Server startup function - decoupled for testing */
turbo_coro_server_t* iris_server_start(turbo_coro_context_t *ctx, unsigned short port) {
  if (!ctx) return NULL;

  g_coro_ctx = ctx;

  turbo_coro_server_t *server = turbo_coro_server_create(ctx);
  if (!server) {
    TLOG_ERROR("Failed to create server");
    g_coro_ctx = NULL;
    return NULL;
  }

  char listen_url[64];
  snprintf(listen_url, sizeof(listen_url), "tcp://0.0.0.0:%d", port);

  int r = turbo_coro_server_listen(server, listen_url, server_handler, NULL);
  if (r != 0) {
    TLOG_ERROR("Failed to start listening: {}", r);
    turbo_coro_server_destroy(server);
    g_coro_ctx = NULL;
    return NULL;
  }

  return server;
}

int iris_server_run(unsigned short PORT) {
  /* Initialize error recovery system */
  if (iris_error_recovery_init() != 0) {
    TLOG_ERROR("Failed to initialize error recovery system");
    return -1;
  }

  g_coro_ctx = turbo_coro_context_create(NULL);
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

  /* Start server */
  g_server = iris_server_start(g_coro_ctx, PORT);
  if (!g_server) {
    turbo_coro_context_destroy(g_coro_ctx);
    g_coro_ctx = NULL;
    iris_error_recovery_cleanup();
    return -1;
  }

  /* Initialize thread pool for iris_await() */
  iris_async_init(0);

  TLOG_INFO("Server is running on http://localhost:{}", PORT);

  /* Run the loop */
  turbo_coro_context_run(g_coro_ctx, TURBO_RUN_DEFAULT);

  /* Cleanup */
  TLOG_INFO("Shutting down server...");

  iris_async_shutdown();

  if (g_server) {
    turbo_coro_server_destroy(g_server);
    g_server = NULL;
  }

  if (g_coro_ctx) {
    turbo_coro_context_destroy(g_coro_ctx);
    g_coro_ctx = NULL;
  }

  TLOG_INFO("Server shutdown complete");

  /* Cleanup error recovery system */
  iris_error_recovery_cleanup();

  return 0;
}
