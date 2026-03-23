/**
 * @file coro_tls_cert_demo_simple.c
 * @brief Coroutine TLS demo with ECDSA P-256 certificate generation.
 *
 * This example keeps the original certificate-generation focus but uses the
 * active coroutine TLS client API instead of the removed libuv server API.
 */

#include "CoroNet.h"
#include "turbo_coro.h"
#include "turbo_coro_socket.h"
#include <asn1/x509_generate.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  coro_context_t *ctx;
  const char *host;
  int port;
  const char *request;
} tls_demo_config_t;

static void print_pem_preview(const char *label, const char *pem) {
  const char *line_end;

  if (!label || !pem) return;

  printf("%s\n", label);
  line_end = strchr(pem, '\n');
  if (!line_end) {
    printf("  %s\n", pem);
    return;
  }

  printf("  %.*s\n", (int)(line_end - pem), pem);
  if (line_end[1] != '\0') {
    const char *second_start = line_end + 1;
    const char *second_end = strchr(second_start, '\n');
    if (second_end) {
      printf("  %.*s\n", (int)(second_end - second_start), second_start);
    }
  }
  printf("  ...\n");
}

static void tls_demo_task(coro_t *co, void *arg) {
  tls_demo_config_t *cfg = (tls_demo_config_t *)arg;
  coro_socket_t *client;
  char *data = NULL;
  size_t len = 0;
  int rc;

  UNUSED(co);

  client = coro_socket_create(cfg->ctx, CORO_SOCKET_TLS);
  if (!client) {
    printf("[Coro] failed to create TLS socket\n");
    return;
  }

  coro_socket_set_timeout(client, 15000);

  printf("[Coro] connecting to %s:%d over TLS...\n", cfg->host, cfg->port);
  rc = coro_socket_connect(client, cfg->host, cfg->port);
  if (rc != 0) {
    printf("[Coro] connect failed: %d (%s)\n", rc, turbo_strerror(rc));
    coro_socket_destroy(client);
    return;
  }

  printf("[Coro] connected, sending HTTP request...\n");
  rc = coro_socket_send(client, cfg->request, strlen(cfg->request));
  if (rc != 0) {
    printf("[Coro] send failed: %d (%s)\n", rc, turbo_strerror(rc));
    coro_socket_destroy(client);
    return;
  }

  rc = coro_socket_recv(client, &data, &len);
  if (rc == 0 && data) {
    size_t preview_len = len > 400 ? 400 : len;
    printf("[Coro] received %zu bytes\n", len);
    printf("----- response preview -----\n%.*s\n----- end preview -----\n",
           (int)preview_len, data);
    coro_socket_free_recv(data);
  } else {
    printf("[Coro] recv failed: %d (%s)\n", rc, turbo_strerror(rc));
  }

  coro_socket_destroy(client);
}

int main(void) {
  char *cert_pem = NULL;
  char *key_pem = NULL;
  size_t cert_len = 0;
  size_t key_len = 0;
  int rc;
  coro_context_t *ctx;
  tls_demo_config_t cfg = {
      .ctx = NULL,
      .host = "www.google.com",
      .port = 443,
      .request =
          "GET / HTTP/1.1\r\n"
          "Host: www.google.com\r\n"
          "Connection: close\r\n"
          "\r\n"};

  printf("Coroutine TLS Demo with ECDSA P-256 Certificate Generation\n");
  printf("==========================================================\n\n");

  printf("1. generating ECDSA P-256 self-signed certificate...\n");
  rc = x509_generate_tls_cert_pem_ecdsa("localhost", 365, &cert_pem, &cert_len,
                                        &key_pem, &key_len);
  if (rc != 0) {
    printf("   certificate generation failed: %d\n", rc);
    return 1;
  }

  printf("   certificate: %zu bytes\n", cert_len);
  printf("   private key: %zu bytes\n", key_len);
  print_pem_preview("   certificate preview:", cert_pem);

  printf("\n2. running coroutine TLS client demo...\n");
  ctx = coro_context_create(NULL);
  if (!ctx) {
    printf("   failed to create coroutine context\n");
    memset(key_pem, 0, key_len);
    free(key_pem);
    free(cert_pem);
    return 1;
  }

  cfg.ctx = ctx;
  rc = coro_context_spawn(ctx, tls_demo_task, &cfg);
  if (rc != 0) {
    printf("   failed to spawn coroutine: %d\n", rc);
    coro_context_destroy(ctx);
    memset(key_pem, 0, key_len);
    free(key_pem);
    free(cert_pem);
    return 1;
  }

  coro_context_run(ctx, TURBO_RUN_DEFAULT);
  coro_context_destroy(ctx);

  printf("\n3. cleaning up generated key material...\n");
  memset(key_pem, 0, key_len);
  free(key_pem);
  free(cert_pem);

  printf("done.\n");
  return 0;
}
