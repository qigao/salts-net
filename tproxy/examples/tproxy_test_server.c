/**
 * tproxy_test_server.c - Simple test server for TProxy
 *
 * This is a simple HTTP server that can be used to test the TProxy implementation.
 * It responds with information about the incoming connection.
 *
 * Usage:
 *   ./tproxy_test_server [port]
 */
#if defined(_MSC_VER) && !defined(__clang__)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h> 

#include "turbo_async_server.h"
#include "turbo_async_client.h"
#define STB_SPRINTF_IMPLEMENTATION
#include <stb_sprintf.h>

typedef struct {
  uv_tcp_t handle;
  char buffer[4096];
  int request_count;
} client_context_t;

static void after_write(uv_write_t *req, int status) {
  (void)status;
  free(req);
}

static void after_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  (void)buf;

  if (nread <= 0) {
    uv_close((uv_handle_t *)stream, NULL);
    free(stream->data);
    return;
  }

  client_context_t *ctx = (client_context_t *)stream->data;

  /* Simple HTTP response */
  char response[4096];
  int response_len = stbsp_snprintf(response, sizeof(response),
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/plain\r\n"
      "Connection: close\r\n"
      "\r\n"
      "Hello from TProxy test server!\n"
      "Request #%d\n"
      "Received %zd bytes\n",
      ++ctx->request_count, nread);

  uv_write_t *write_req = (uv_write_t *)malloc(sizeof(uv_write_t));
  uv_buf_t write_buf = uv_buf_init(response, response_len);
  uv_write(write_req, stream, &write_buf, 1, after_write);
}

static void on_new_connection(uv_stream_t *server, int status) {
  if (status != 0) {
    return;
  }

  uv_tcp_t *client = (uv_tcp_t *)malloc(sizeof(uv_tcp_t));
  uv_tcp_init(uv_default_loop(), client);

  if (uv_accept(server, (uv_stream_t *)client) == 0) {
    client->data = calloc(1, sizeof(client_context_t));
    uv_read_start((uv_stream_t *)client,
                  (uv_alloc_cb)malloc,
                  after_read);
  } else {
    uv_close((uv_handle_t *)client, NULL);
    free(client);
  }
}

int main(int argc, char *argv[]) {
  int port = (argc > 1) ? atoi(argv[1]) : 8080;

  uv_tcp_t server;
  uv_tcp_init(uv_default_loop(), &server);

  struct sockaddr_in addr;
  uv_ip4_addr("0.0.0.0", port, &addr);

  int r = uv_tcp_bind(&server, (const struct sockaddr *)&addr, 0);
  if (r != 0) {
    fprintf(stderr, "Bind error: %s\n", uv_strerror(r));
    return 1;
  }

  r = uv_listen((uv_stream_t *)&server, 128, on_new_connection);
  if (r != 0) {
    fprintf(stderr, "Listen error: %s\n", uv_strerror(r));
    return 1;
  }

  printf("Test server listening on port %d\n", port);
  uv_run(uv_default_loop(), UV_RUN_DEFAULT);

  return 0;
}
