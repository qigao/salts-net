/**
 * tproxy_example.c - TProxy transparent proxy example
 *
 * This example demonstrates a transparent proxy server that can intercept
 * connections and forward them to their original destinations.
 *
 * Usage:
 *   ./tproxy_example [options]
 *
 * Options:
 *   --listen <host:port>  Listen address (default: 0.0.0.0:1080)
 *   --upstream <host:port> Upstream proxy (optional, default: direct)
 *   --socks5              Enable SOCKS5 protocol
 *
 * Example:
 *   ./tproxy_example --listen 0.0.0.0:1080 --socks5
 *   ./tproxy_example --listen 0.0.0.0:1080 --upstream 127.0.0.1:1080
 */
#if defined(_MSC_VER) && !defined(__clang__)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
 

#include "turbo_tproxy.h" 

static int g_running = 1;
static uint64_t g_total_connections = 0;
static uint64_t g_total_bytes = 0;

static void signal_handler(uv_signal_t *handle, int signum) {
  (void)handle;
  (void)signum;
  printf("\nShutting down...\n");
  g_running = 0;
}

static void on_tproxy_event(tproxy_server_t *server, const tproxy_event_t *event,
                            void *user_data) {
  (void)server;
  (void)user_data;

  switch (event->type) {
    case TPROXY_EVENT_LISTENING: {
      printf("TProxy server is listening...\n");
      break;
    }

    case TPROXY_EVENT_CONNECTION: {
      g_total_connections++;
      char client_host[64];
      int client_port;
      tproxy_connection_get_client_addr(server, event->connection, client_host, sizeof(client_host), &client_port);

      char target_host[256];
      int target_port;
      tproxy_connection_get_original_dest(server, event->connection, target_host, sizeof(target_host), &target_port);

      printf("[%lu] New connection from %s:%d to %s:%d\n",
             g_total_connections, client_host, client_port, target_host, target_port);
      break;
    }

    case TPROXY_EVENT_DATA: {
      g_total_bytes += event->length;
      break;
    }

    case TPROXY_EVENT_UPSTREAM_CONNECTED: {
      printf("Upstream connected for connection %p\n", (void *)event->connection);
      break;
    }

    case TPROXY_EVENT_DISCONNECT: {
      printf("Connection %p closed\n", (void *)event->connection);
      break;
    }

    case TPROXY_EVENT_ERROR: {
      fprintf(stderr, "Error: %s (status=%d)\n", event->message ? event->message : "unknown", event->status);
      break;
    }

    case TPROXY_EVENT_CLOSED: {
      printf("TProxy server closed\n");
      break;
    }
  }
}

static void print_help(const char *prog_name) {
  printf("Usage: %s [options]\n", prog_name);
  printf("\nOptions:\n");
  printf("  --listen <host:port>  Listen address (default: 0.0.0.0:1080)\n");
  printf("  --upstream <host:port> Upstream proxy (optional, default: direct)\n");
  printf("  --socks5              Enable SOCKS5 protocol\n");
  printf("  --help                Show this help\n");
}

int main(int argc, char *argv[]) {
  tproxy_config_t config = {0};

  /* Default configuration */
  config.listen_host = "0.0.0.0";
  config.listen_port = 1080;
  config.upstream_host = NULL;
  config.upstream_port = 0;
  config.max_connections = 1024;
  config.enable_socks5 = 0;
  config.enable_udp = 0;
  config.buffer_size = 64 * 1024;

  /* Parse command line arguments */
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0) {
      print_help(argv[0]);
      return 0;
    } else if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
      char *host_port = argv[++i];
      char *colon = strrchr(host_port, ':');
      if (colon) {
        *colon = '\0';
        config.listen_host = host_port;
        config.listen_port = atoi(colon + 1);
      } else {
        config.listen_host = host_port;
        config.listen_port = 1080;
      }
    } else if (strcmp(argv[i], "--upstream") == 0 && i + 1 < argc) {
      char *host_port = argv[++i];
      char *colon = strrchr(host_port, ':');
      if (colon) {
        *colon = '\0';
        config.upstream_host = host_port;
        config.upstream_port = atoi(colon + 1);
      } else {
        config.upstream_host = host_port;
        config.upstream_port = 1080;
      }
    } else if (strcmp(argv[i], "--socks5") == 0) {
      config.enable_socks5 = 1;
    } else if (strcmp(argv[i], "--udp") == 0) {
      config.enable_udp = 1;
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      print_help(argv[0]);
      return 1;
    }
  }

  /* Create TProxy server */
  tproxy_server_t *server = tproxy_server_create(&config, on_tproxy_event, NULL);
  if (!server) {
    fprintf(stderr, "Failed to create TProxy server\n");
    return 1;
  }

  /* Set up signal handlers */
  uv_signal_t sigint, sigterm;
  uv_signal_init(uv_default_loop(), &sigint);
  uv_signal_start(&sigint, signal_handler, SIGINT);
  uv_signal_init(uv_default_loop(), &sigterm);
  uv_signal_start(&sigterm, signal_handler, SIGTERM);

  /* Start server */
  turbo_client_status_t status = tproxy_server_start(server);
  if (status != TURBO_CLIENT_STATUS_OK) {
    fprintf(stderr, "Failed to start server: %d\n", status);
    tproxy_server_destroy(server);
    return 1;
  }

  printf("TProxy Transparent Proxy Server\n");
  printf("================================\n");
  printf("Listening on: %s:%d\n", config.listen_host, config.listen_port);
  if (config.upstream_host) {
    printf("Upstream proxy: %s:%d\n", config.upstream_host, config.upstream_port);
  } else {
    printf("Mode: Direct connection\n");
  }
  printf("SOCKS5 support: %s\n", config.enable_socks5 ? "Enabled" : "Disabled");
  printf("UDP support: %s\n", config.enable_udp ? "Enabled" : "Disabled");
  printf("\nPress Ctrl+C to stop\n\n");

  /* Main loop */
  while (g_running) {
    uv_run(uv_default_loop(), UV_RUN_ONCE);
  }

  /* Cleanup */
  tproxy_server_stop(server);
  tproxy_server_destroy(server);

  printf("\nStatistics:\n");
  printf("  Total connections: %lu\n", g_total_connections);
  printf("  Total bytes: %lu\n", g_total_bytes);

  return 0;
}
