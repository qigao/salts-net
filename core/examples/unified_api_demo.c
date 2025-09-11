/**
 * TurboNet Unified API Demo
 * "The same code works for TCP, TLS, UDP - that's good taste!" - Linus
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "turbonet.h"

// Global state for demo
static turbo_handle_t g_client;
static turbo_req_t g_write_req;
static char g_message[] = "Hello from TurboNet!";

// Forward declarations
static void client_demo_url(const char* url);
static void server_demo_url(const char* url);
static void server_connection_cb(turbo_handle_t* server, int status);
static void server_alloc_cb(turbo_handle_t* handle,
                            size_t size,
                            turbo_buf_t* buf);
static void server_read_cb(turbo_handle_t* handle,
                           ssize_t nread,
                           const turbo_buf_t* buf);

// Callback functions for client demo
static void client_alloc_cb(turbo_handle_t* handle,
                            size_t size,
                            turbo_buf_t* buf)
{
  buf->base = malloc(size);
  buf->len = buf->base ? size : 0;
}

static void client_close_cb(turbo_handle_t* handle)
{
  printf("[CLOSED] CONNECTION CLOSED: %s\n",
         turbo_transport_name(handle->transport));
  printf("   [STATS] Final stats - Read: %llu bytes, Written: %llu bytes\n",
         (unsigned long long)handle->bytes_read,
         (unsigned long long)handle->bytes_written);
}

static void client_read_cb(turbo_handle_t* handle,
                           ssize_t nread,
                           const turbo_buf_t* buf)
{
  if (nread > 0) {
    printf("[SUCCESS] CLIENT RECEIVED: %zd bytes via %s\n",
           nread,
           turbo_transport_name(handle->transport));
    printf("   [RECEIVED] Data: \"%.*s\"\n", (int)nread, buf->base);
    printf("   [STATS] Total bytes read: %llu\n",
           (unsigned long long)handle->bytes_read);
    printf("   [SUCCESS] Echo received successfully! Closing connection...\n");

    // Now close the connection after receiving the echo
    turbo_close(handle, client_close_cb);
  } else if (nread < 0) {
    printf("[ERROR] CLIENT READ ERROR with code: %d\n", (int)nread);
    turbo_close(handle, client_close_cb);
  }
  if (buf->base) {
    free(buf->base);
  }
}

static void client_write_cb(turbo_req_t* req, int status)
{
  turbo_handle_t* handle = req->handle;
  if (status == 0) {
    printf("[SUCCESS] CLIENT SENT: Data via %s successfully!\n",
           turbo_transport_name(handle->transport));
    printf("   [STATS] Total bytes written: %llu\n",
           (unsigned long long)handle->bytes_written);
    printf("   [WAITING] Waiting for echo response...\n");
  } else {
    printf("[ERROR] CLIENT SEND FAILED with error code: %d\n", status);
    // Close connection on error
    turbo_close(req->handle, client_close_cb);
  }

  // Don't close immediately - let the read callback handle closing after
  // receiving echo
}

static void client_connect_cb(turbo_handle_t* handle, int status)
{
  if (status != 0) {
    printf("[ERROR] CONNECT FAILED with error code: %d\n", status);
    return;
  }

  printf("[STARTED] CONNECTED: Successfully via %s to %s:%d!\n",
         turbo_transport_name(handle->transport),
         handle->remote_ip,
         handle->remote_port);

  // Send data - same API for all transports
  turbo_buf_t buf = turbo_buf_init(g_message, strlen(g_message));
  printf("[SENDING] CLIENT SENDING: \"%s\" (%zu bytes)\n",
         g_message,
         strlen(g_message));

  turbo_write(&g_write_req, handle, &buf, 1, client_write_cb);

  // Start reading - same API for all transports
  turbo_read_start(handle, client_alloc_cb, client_read_cb);
}

// Client example - AUTONOMOUS URL-first approach!
static void client_demo_url(const char* url)
{
  printf("\n=== Autonomous URL-First Client Demo: %s ===\n", url);

  // Initialize handle according to public API documentation
  turbo_global_init(&g_client);

  printf("Connecting to %s\n", url);

  // AUTONOMOUS: Connect with URL (auto-detects AND initializes transport!)
  int err = turbo_connect_url(&g_client, url, client_connect_cb);
  if (err != 0) {
    printf("Connect failed with error code: %d\n", err);
    turbo_close(&g_client, NULL);
    return;
  }

  printf("Connection initiated to %s\n", url);

  // For TLS demo, set options AFTER connecting but BEFORE handshake completes
  if (g_client.transport == TURBO_TLS) {
    printf(
        "[DEMO] Configuring TLS to disable peer verification for self-signed "
        "cert.\n");

    int verify_err =
        turbo_tls_set_verify(&g_client, 0);  // 0 = DISABLE verification
    if (verify_err != 0) {
      printf("[ERROR] Failed to set TLS verification with error code: %d\n", verify_err);
      return;
    }
    printf("[SUCCESS] TLS peer verification disabled\n");
  }

  // Run event loop with TurboNet's global loop
  turbo_run();
}

// Client example - same code works for ALL transports!
static void client_demo(turbo_transport_t transport)
{
  // Convert transport to URL for the new approach
  const char* urls[] = {
      "tcp://127.0.0.1:8080",  // TURBO_TCP
      "tls://127.0.0.1:8443",  // TURBO_TLS
      "kcp://127.0.0.1:8888",  // TURBO_KCP
      "udp://127.0.0.1:8080",  // TURBO_UDP
#ifdef _WIN32
      "pipe://./turbonet_demo",  // TURBO_PIPE - Windows named pipe
#else
      "pipe:///tmp/turbonet_demo.pipe",  // TURBO_PIPE - Unix domain socket
#endif
      "quic://127.0.0.1:8443"  // TURBO_QUIC
  };

  if (transport >= 0 && transport < TURBO_TRANSPORT_MAX) {
    client_demo_url(urls[transport]);
  } else {
    printf("Unknown transport type: %d\n", transport);
  }
}

// Server example - AUTONOMOUS URL-first approach!
static void server_demo_url(const char* url)
{
  printf("\n=== Autonomous URL-First Server Demo: %s ===\n", url);

  turbo_handle_t server;

  // AUTONOMOUS WAY: Initialize handle according to public API
  turbo_global_init(&server);

  // AUTONOMOUS: Bind with URL (auto-detects AND initializes everything!)
  int err = turbo_bind_url(&server, url);
  if (err != 0) {
    printf("turbo_bind_url failed with error code: %d\n", err);
    return;
  }

  printf("Transport auto-detected and initialized: %s\n",
         turbo_transport_name(server.transport));

  // Configure TLS with secure defaults AFTER initialization
  if (server.transport == TURBO_TLS) {
    printf("Configuring TLS with secure defaults...\n");

    // Certificate is REQUIRED for TLS server
    int cert_err = turbo_tls_set_cert(&server, "server.pem", "server.key");
    printf("Certificate setup result: %d\n", cert_err);
    if (cert_err != 0) {
      printf(
          "ERROR: Certificate setup failed! TLS server cannot work without "
          "certificates.\n");
      return;
    }

    int defaults_err = turbo_tls_set_secure_defaults(
        &server);  // This should update existing SSL_CTX
    printf("Secure defaults setup result: %d\n", defaults_err);

    log_debug("Applied TLS secure defaults after initialization");
  }

  // Listen - same for all transports (except UDP)
  if (server.transport != TURBO_UDP) {
    err = turbo_listen(&server, 128, server_connection_cb);
  } else {
    // UDP server just starts reading
    turbo_read_start(&server, server_alloc_cb, server_read_cb);
  }

  if (err != 0) {
    printf("Listen/Read failed with error code: %d\n", err);
    return;
  }

  printf("[STARTED] %s server listening on %s\n",
         turbo_transport_name(server.transport),
         url);
  printf("[READY] Server ready to accept connections...\n");

  // Run event loop with TurboNet's global loop
  turbo_run();
}

// Server callback functions
static void server_alloc_cb(turbo_handle_t* handle,
                            size_t size,
                            turbo_buf_t* buf)
{
  buf->base = malloc(size);
  buf->len = buf->base ? size : 0;
}

static void echo_write_cb(turbo_req_t* req, int status)
{
  if (status == 0) {
    printf("[SUCCESS] SERVER ECHO SENT: Successfully echoed data\n");
  } else {
    printf("[ERROR] SERVER ECHO FAILED with error code: %d\n", status);
  }
  free(req);
}

static void client_close_cb2(turbo_handle_t* handle)
{
  printf("[CLOSED] SERVER: Client connection closed\n");
  printf("   [STATS] Client stats - Read: %llu bytes, Written: %llu bytes\n",
         (unsigned long long)handle->bytes_read,
         (unsigned long long)handle->bytes_written);
  free(handle);
}

static void server_read_cb(turbo_handle_t* handle,
                           ssize_t nread,
                           const turbo_buf_t* buf)
{
  if (nread > 0) {
    printf("[SUCCESS] SERVER RECEIVED: %zd bytes via %s\n",
           nread,
           turbo_transport_name(handle->transport));
    printf("   [RECEIVED] Data: \"%.*s\"\n", (int)nread, buf->base);
    printf("   [STATS] Total server bytes read: %llu\n",
           (unsigned long long)handle->bytes_read);

    // Echo back - same write API for all transports
    turbo_req_t* echo_req = malloc(sizeof(turbo_req_t));
    turbo_buf_t echo_buf = turbo_buf_init(buf->base, nread);

    printf("[ECHOING] SERVER ECHOING: Sending back %zd bytes\n", nread);
    turbo_write(echo_req, handle, &echo_buf, 1, echo_write_cb);
  } else if (nread < 0) {
    printf("[ERROR] SERVER: Client disconnected with error code: %d\n",
           (int)nread);
    turbo_close(handle, client_close_cb2);
  }
}

static void server_connection_cb(turbo_handle_t* server, int status)
{
  if (status != 0) {
    printf("[ERROR] SERVER ERROR: Listen error with code: %d\n",
           status);
    return;
  }

  printf("[STARTED] SERVER: New connection on %s server\n",
         turbo_transport_name(server->transport));

  // KCP and QUIC servers don't use accept model - connections are auto-created
  if (server->transport == TURBO_KCP || server->transport == TURBO_QUIC) {
    // For KCP and QUIC, we just start reading - the server handle itself
    // becomes the client
    turbo_read_start(server, server_alloc_cb, server_read_cb);
    return;
  }

  // Accept client - AUTONOMOUS (auto-initializes client)
  turbo_handle_t* client = malloc(sizeof(turbo_handle_t));

  // AUTONOMOUS WAY: Initialize client according to public API
  memset(client, 0, sizeof(*client));
  client->loop = server->loop;

  // AUTONOMOUS: Accept auto-initializes client with same transport as server
  int err = turbo_accept(server, client);
  if (err != 0) {
    printf("[ERROR] SERVER ERROR: Accept failed with error code: %d\n", err);
    free(client);
    return;
  }

  printf("[SUCCESS] SERVER: Client connected via %s from %s:%d\n",
         turbo_transport_name(client->transport),
         client->remote_ip,
         client->remote_port);

  // Echo server - read and write back
  turbo_read_start(client, server_alloc_cb, server_read_cb);
}

// Server example - also works for all transports!
static void server_demo(turbo_transport_t transport)
{
  // Convert transport to URL for the new approach
  const char* urls[] = {
      "tcp://0.0.0.0:8080",  // TURBO_TCP
      "tls://0.0.0.0:8443",  // TURBO_TLS
      "kcp://0.0.0.0:8888",  // TURBO_KCP
      "udp://0.0.0.0:8080",  // TURBO_UDP
#ifdef _WIN32
      "pipe://./turbonet_demo",  // TURBO_PIPE - Windows named pipe
#else
      "pipe:///tmp/turbonet_demo.pipe",  // TURBO_PIPE - Unix domain socket
#endif
      "quic://0.0.0.0:8443"  // TURBO_QUIC
  };

  if (transport >= 0 && transport < TURBO_TRANSPORT_MAX) {
    server_demo_url(urls[transport]);
  } else {
    printf("Unknown transport type: %d\n", transport);
  }
}

// Transport comparison demo - shows the beauty of URL-first API
static void transport_comparison_demo()
{
  printf("\n=== Transport Comparison Demo ===\n");
  printf("URL-first approach - shows different transport types:\n\n");

  const char* test_urls[] = {"tcp://example.com:80",
                             "tls://secure.example.com:443",
                             "udp://dns.example.com:53",
#ifdef _WIN32
                             "pipe://./example_demo",  // Windows named pipe
#else
                             "pipe:///tmp/example.sock",  // Unix domain socket
#endif
                             "kcp://game.example.com:8888",
                             "quic://http3.example.com:443"};

  const char* descriptions[] = {"Reliable, connection-oriented",
                                "Secure, encrypted TCP",
                                "Fast, connectionless",
                                "Local IPC via named pipes",
                                "Fast reliable UDP (KCP)",
                                "Next-gen HTTP/3 protocol (QUIC)"};

  for (int i = 0; i < 6; i++) {
    printf("URL: %s\n", test_urls[i]);
    printf("  Description: %s\n", descriptions[i]);

    // Initialize handle according to public API
    turbo_handle_t handle;
    turbo_global_init(&handle);

    printf("  [OK] Handle initialized for URL connection\n");
    printf("  [INFO] Ready for turbo_connect_url() or turbo_bind_url()\n");
    printf("\n");
  }
}

int main(int argc, char* argv[])
{
  // Initialize TurboNet
  log_set_level(0);  // Enable debug logging to see TLS configuration
  printf("Log level set to DEBUG (%d)\n", LOG_DEBUG);

  printf("\n[CONFIG] TurboNet Autonomous Transport Demo\n");
  printf("==========================================\n");
  printf(
      "[AUTONOMOUS] \"Zero initialization - URLs do everything!\" - Ultimate "
      "simplicity\n");
  printf("[UNIFIED] Same code works for TCP, TLS, UDP, PIPE, KCP, QUIC\n");

  if (argc < 2) {
    printf("\nUsage: %s <mode> [transport]\n", argv[0]);
    printf("Modes:\n");
    printf("  compare     - Show transport comparison\n");
    printf("  client      - Run client demo\n");
    printf("  server      - Run server demo\n");
    printf("\nTransports: tcp, udp, tls, pipe, kcp, quic\n");
    printf("Examples:\n");
    printf("  %s compare\n", argv[0]);
    printf("  %s server tcp\n", argv[0]);
    printf("  %s client quic\n", argv[0]);
    return 1;
  }

  const char* mode = argv[1];

  if (strcmp(mode, "compare") == 0) {
    transport_comparison_demo();
  } else {
    turbo_transport_t transport = TURBO_TCP;  // default

    if (argc > 2) {
      const char* transport_name = argv[2];
      if (strcmp(transport_name, "tcp") == 0) {
        transport = TURBO_TCP;
      } else if (strcmp(transport_name, "udp") == 0) {
        transport = TURBO_UDP;
      } else if (strcmp(transport_name, "tls") == 0) {
        transport = TURBO_TLS;
      } else if (strcmp(transport_name, "pipe") == 0) {
        transport = TURBO_PIPE;
      } else if (strcmp(transport_name, "kcp") == 0) {
        transport = TURBO_KCP;
      } else if (strcmp(transport_name, "quic") == 0) {
        transport = TURBO_QUIC;
      } else {
        printf("Unknown transport: %s\n", transport_name);
        return 1;
      }
    }

    if (strcmp(mode, "server") == 0) {
      server_demo(transport);
    } else if (strcmp(mode, "client") == 0) {
      client_demo(transport);
    } else {
      printf("Unknown mode: %s\n", mode);
      return 1;
    }
  }
  return 0;
}
