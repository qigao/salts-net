#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
 

#include "error_recovery.h"
#include "router.h"
#include "router_adapter.h"
#include "tlog.h"
#include "turbo_async_server.h"

#define READ_BUF_SIZE 8192

/* Connection context structure */
typedef struct {
  async_server_connection_t *connection;
  char buffer[READ_BUF_SIZE];
  size_t buffer_used;
  int keep_alive;
  time_t created_time;
  int request_count;
  void *middleware_data;                  /* For middleware-specific connection data */
  void (*middleware_cleanup)(void *data); /* Cleanup function for middleware data */
} iris_connection_ctx_t;

/* Forward declarations */
static void cleanup_connection_context(iris_connection_ctx_t *ctx);
static void cleanup_all_connections(void);

/* Global server state */
static async_server_t *g_server = NULL;
static int g_shutdown_requested = 0;
static void (*g_app_shutdown_hook)(void) = NULL;

/* Global shutdown flag for external modules (like pquv) */
int shutdown_requested = 0;

void shutdown_hook(void (*hook)(void)) { g_app_shutdown_hook = hook; }

/* Signal handler for graceful shutdown */
static void signal_handler(int signum) {
  if (g_shutdown_requested) {

    TLOG_INFO("Received signal {:d}, shutting down...", signum);
    return;
  }

  g_shutdown_requested = 1;
  shutdown_requested = 1; /* Also set global flag for external modules */

  /* Call application cleanup hook */
  if (g_app_shutdown_hook) {
    g_app_shutdown_hook();
  }

  /* Stop the server */
  if (g_server) {
    async_server_stop(g_server);
  }
}

/* Server event callback */
static void server_event_cb(async_server_t *server, const async_server_event_t *event,
                            void *user_data) {
  (void)user_data;

  switch (event->type) {
  case ASYNC_SERVER_EVENT_LISTENING:
    /* Server is now listening - nothing to do */
    break;

  case ASYNC_SERVER_EVENT_CONNECTION: {
    /* New connection established */
    iris_connection_ctx_t *ctx = (iris_connection_ctx_t *)calloc(1, sizeof(iris_connection_ctx_t));
    if (!ctx) {
      iris_error_context_t error_ctx =
          IRIS_ERROR_CONTEXT(IRIS_ERROR_OUT_OF_MEMORY, -1, "Failed to allocate connection context");
      iris_handle_error(&error_ctx, IRIS_RECOVERY_REJECT_REQUEST);
      async_server_close_connection(server, event->connection);
      return;
    }

    ctx->connection = event->connection;
    ctx->buffer_used = 0;
    ctx->keep_alive = 1;
    ctx->created_time = time(NULL);
    ctx->request_count = 0;
    ctx->middleware_data = NULL;
    ctx->middleware_cleanup = NULL;

    /* Attach context to connection */
    async_server_connection_set_user_data(event->connection, ctx);
    break;
  }

  case ASYNC_SERVER_EVENT_DATA: {
    /* Data received from connection */
    if (!event->connection || !event->data || event->length == 0)
      break;

    iris_connection_ctx_t *ctx =
        (iris_connection_ctx_t *)async_server_connection_get_user_data(event->connection);
    if (!ctx)
      break;

    /* Increment request count for this connection */
    ctx->request_count++;

    /* Process the HTTP request through the router adapter */
    int should_close =
        router_process_request(server, event->connection, event->data, event->length);

    /* Close connection if requested */
    if (should_close) {
      async_server_close_connection(server, event->connection);
    }
    break;
  }

  case ASYNC_SERVER_EVENT_DISCONNECTION: {
    /* Connection closed */
    if (!event->connection)
      break;

    iris_connection_ctx_t *ctx =
        (iris_connection_ctx_t *)async_server_connection_get_user_data(event->connection);
    if (ctx) {
      cleanup_connection_context(ctx);
      async_server_connection_set_user_data(event->connection, NULL);
    }
    break;
  }

  case ASYNC_SERVER_EVENT_CLOSED:
    /* Server closed */
    TLOG_INFO("Server closed");
    break;

  case ASYNC_SERVER_EVENT_ERROR:
    /* Error occurred */
    if (event->message) {
      iris_error_context_t ctx =
          IRIS_ERROR_CONTEXT(IRIS_ERROR_PROTOCOL_ERROR, event->status, event->message);
      iris_handle_error(&ctx, IRIS_RECOVERY_CONTINUE);
    } else {
      char error_msg[64];
      snprintf(error_msg, sizeof(error_msg), "Server error: status=%d", event->status);
      iris_error_context_t ctx =
          IRIS_ERROR_CONTEXT(IRIS_ERROR_PROTOCOL_ERROR, event->status, error_msg);
      iris_handle_error(&ctx, IRIS_RECOVERY_CONTINUE);
    }
    break;
  }
}

/* Server startup function */
int ecewo(unsigned short PORT) {
  /* Initialize error recovery system */
  if (iris_error_recovery_init() != 0) {
    TLOG_ERROR("Failed to initialize error recovery system");
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

  /* Create async server - transport determined from URL */
  g_server = async_server_create(server_event_cb, NULL);
  if (!g_server) {
    iris_error_context_t ctx =
        IRIS_ERROR_CONTEXT(IRIS_ERROR_SERVER_INIT_FAILED, -1, "Failed to create server");
    iris_handle_error(&ctx, IRIS_RECOVERY_GRACEFUL_SHUTDOWN);
    iris_error_recovery_cleanup();
    return -1;
  }

  /* Start listening - URL format determines transport (tcp://) */
  char listen_url[64];
  snprintf(listen_url, sizeof(listen_url), "tcp://0.0.0.0:%d", PORT);
  async_server_status_t status = async_server_listen(g_server, listen_url, 128);
  if (status != ASYNC_SERVER_STATUS_OK) {
    char error_msg[256];
    snprintf(error_msg, sizeof(error_msg), "Failed to start listening: %s",
             async_server_status_to_string(status));
    iris_error_context_t ctx = IRIS_ERROR_CONTEXT(IRIS_ERROR_LISTEN_FAILED, (int)status, error_msg);
    iris_handle_error(&ctx, IRIS_RECOVERY_GRACEFUL_SHUTDOWN);
    async_server_destroy(g_server);
    g_server = NULL;
    iris_error_recovery_cleanup();
    return -1;
  }

  TLOG_INFO("Server is running on http://localhost:{:d}", PORT);

  /* Wait for shutdown signal */
  while (!g_shutdown_requested) {
    turbo_sleep_ms(100);
  }

  /* Cleanup all active connections before destroying server */
  cleanup_all_connections();

  /* Cleanup */
  TLOG_INFO("Shutting down server...");
  if (g_server) {
    async_server_destroy(g_server);
    g_server = NULL;
  }

  TLOG_INFO("Server shutdown complete");

  /* Cleanup error recovery system */
  iris_error_recovery_cleanup();

  return 0;
}

/* ============================================================================
 * Connection Context Cleanup Functions
 * ============================================================================ */

/**
 * @brief Clean up connection context and associated resources
 * @param ctx Connection context to clean up
 */
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
    TLOG_DEBUG("Connection closed: duration={:d} seconds, requests={:d}", connection_duration,
               ctx->request_count);
  }

  /* Free the context structure */
  free(ctx);
}

/**
 * @brief Clean up all active connections during server shutdown
 *
 * This function is called during server shutdown to ensure all connection
 * contexts are properly cleaned up. Since we don't maintain a list of
 * active connections, this function serves as a placeholder for future
 * enhancements where we might need to track and cleanup active connections.
 */
static void cleanup_all_connections(void) {
  /* Note: NetCore's async_server_destroy() will handle closing all connections
   * and triggering ASYNC_SERVER_EVENT_DISCONNECTION events, which will call
   * cleanup_connection_context() for each connection. This function is here
   * for future enhancements where we might need additional cleanup logic.
   */

  TLOG_INFO("Cleaning up all active connections...");

  /* Future enhancement: If we maintain a list of active connections,
   * we would iterate through them here and ensure cleanup */
}
