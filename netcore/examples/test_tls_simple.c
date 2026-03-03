/**
 * test_tls_simple.c - Simple TLS connection test with turbo_dns
 *
 * Tests basic TLS connectivity using turbo_dns for resolution.
 */

#include <stdio.h>
#include <string.h>
#include <uv.h>

#include "turbo_client.h"
#include "turbo_dns.h"

typedef struct {
  char resolved_ip[256];
  int status;
  int done;
} dns_result_t;

static void dns_callback(const char *hostname, const char *ip, int status, void *user_data) {
  (void)hostname;
  dns_result_t *result = (dns_result_t *)user_data;
  result->status = status;
  result->done = 1;
  if (status == 0 && ip) {
    strncpy(result->resolved_ip, ip, sizeof(result->resolved_ip) - 1);
    result->resolved_ip[sizeof(result->resolved_ip) - 1] = '\0';
    printf("   Resolved to: %s\n", ip);
  } else {
    printf("   DNS resolution failed: status=%d\n", status);
  }
}

int main(void) {
  printf("Simple TLS Connection Test\n");
  printf("===========================\n\n");

  printf("Test: TLS to httpbin.org:443\n");

  /* Resolve using turbo_dns with system default DNS */
  printf("  Resolving httpbin.org using turbo_dns (system DNS)...\n");

  dns_result_t dns_result = {0};

  uv_loop_t loop;
  if (uv_loop_init(&loop) != 0) {
    printf("   Failed to init uv loop\n");
    return 1;
  }

  int rc = turbo_dns_init();
  if (rc != 0) {
    printf("   Failed to init turbo_dns: %d (%s)\n", rc, uv_strerror(rc));
    uv_loop_close(&loop);
    return 1;
  }

  /* Use turbo_dns with system default DNS (no custom servers configured) */
  rc = turbo_dns_resolve_async(&loop, "httpbin.org", TURBO_DNS_ANY, dns_callback, &dns_result);
  if (rc != 0) {
    printf("   Failed to start DNS resolution: %d (%s)\n", rc, uv_strerror(rc));
    turbo_dns_cleanup();
    uv_loop_close(&loop);
    return 1;
  }

  /* Run event loop until DNS completes */
  while (!dns_result.done) {
    uv_run(&loop, UV_RUN_ONCE);
  }

  turbo_dns_cleanup();
  uv_run(&loop, UV_RUN_NOWAIT);
  uv_loop_close(&loop);

  if (dns_result.status != 0) {
    printf("   DNS resolution failed\n");
    return 1;
  }

  /* Now connect via TLS */
  printf("  Connecting via TLS to %s...\n", dns_result.resolved_ip);

  turbo_client_t *client = turbo_client_create_with_transport(SYNC_CLIENT_TRANSPORT_TLS);
  if (!client) {
    printf("   Failed to create TLS client\n");
    return 1;
  }

  turbo_client_status_t status = turbo_client_connect_timeout(client, "tls://httpbin.org:443", 5000);
  if (status == SYNC_CLIENT_STATUS_OK) {
    printf("   Connected successfully\n");

    /* Try sending a simple HTTP request */
    const char *request = "GET / HTTP/1.1\r\nHost: httpbin.org\r\nConnection: close\r\n\r\n";
    printf("  Sending HTTP request...\n");
    status = turbo_client_send(client, request, strlen(request));
    if (status == SYNC_CLIENT_STATUS_OK) {
      printf("   Send successful\n");

      /* Try receiving response (may need multiple receives) */
      printf("  Receiving response...\n");
      size_t total_received = 0;
      int receive_count = 0;

      while (receive_count < 10) { /* Max 10 chunks */
        char *response_data = NULL;
        size_t response_len = 0;
        status = turbo_client_receive_timeout(client, &response_data, &response_len, 2000);

        if (status != SYNC_CLIENT_STATUS_OK || !response_data || response_len == 0) {
          free(response_data);
          printf("  No more data (status=%d, len=%zu)\n", status, response_len);
          break;
        }

        receive_count++;
        total_received += response_len;
        printf("  Chunk %d: %zu bytes\n", receive_count, response_len);

        if (receive_count == 1) {
          printf("  First 100 bytes: %.100s\n", response_data);
        }

        free(response_data);
      }

      printf("   Total received: %zu bytes in %d chunks\n", total_received, receive_count);
    } else {
      printf("   Send failed: %s\n", turbo_client_last_message(client));
    }
  } else {
    printf("   Connection failed: %s\n", turbo_client_last_message(client));
  }

  turbo_client_destroy(client);
  printf("\n");

  printf("TLS tests completed\n");
  return 0;
}
