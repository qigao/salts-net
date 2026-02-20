/**
 * @file coro_http_example.c
 * @brief Demonstrates the coroutine HTTP client — sequential GET + POST JSON.
 *
 * Must be run inside a coroutine (uses turbo_coro_client under the hood).
 */

#include "http_coro_client.h"
#include <turbo_coro.h>
#include <stdio.h>
#include <stdlib.h>

static turbo_coro_context_t *g_ctx;

static void print_response(const char *label, http_coro_response_t *resp) {
  printf("\n=== %s ===\n", label);
  if (resp->error_code != HTTP_ERROR_NONE) {
    printf("  ERROR [%d]: %s\n", resp->error_code, resp->error ? resp->error : "unknown");
    return;
  }
  printf("  Status: %d\n", resp->status_code);
  if (resp->body && resp->body_len > 0) {
    size_t preview = resp->body_len < 512 ? resp->body_len : 512;
    printf("  Body (%zu bytes):\n%.* s\n", resp->body_len, (int)preview, resp->body);
  }
}

static void http_demo(turbo_coro_t *co, void *arg) {
  (void)co;
  (void)arg;

  http_coro_client_t *client = http_coro_client_create(g_ctx);
  if (!client) {
    printf("Failed to create HTTP coro client\n");
    return;
  }

  http_coro_client_set_timeout(client, 10000);
  http_coro_client_set_user_agent(client, "CoroHTTP-Example/1.0");

  /* ── GET request ──────────────────────────────────────────────── */
  printf("Sending GET request to httpbin.org/get ...\n");
  http_coro_response_t *resp = http_coro_get(client, "https://httpbin.org/get");
  print_response("GET /get", resp);
  http_coro_response_free(resp);

  /* ── POST JSON ────────────────────────────────────────────────── */
  printf("\nSending POST JSON to httpbin.org/post ...\n");
  const char *json = "{\"message\": \"hello from coroutine\", \"version\": 1}";
  resp = http_coro_post_json(client, "https://httpbin.org/post", json);
  print_response("POST /post (JSON)", resp);

  if (http_coro_response_is_json(resp)) {
    printf("  Response is JSON ✓\n");
  }
  http_coro_response_free(resp);

  /* ── GET with auth ────────────────────────────────────────────── */
  printf("\nSending GET with Bearer token ...\n");
  http_coro_client_set_bearer_token(client, "test-token-123");
  resp = http_coro_get(client, "https://httpbin.org/bearer");
  print_response("GET /bearer", resp);
  http_coro_response_free(resp);
  http_coro_client_clear_auth(client);

  /* ── Print stats ──────────────────────────────────────────────── */
  http_async_client_stats_t stats;
  http_coro_client_get_stats(client, &stats);
  printf("\n=== Stats ===\n");
  printf("  Total: %llu  Success: %llu  Failed: %llu\n",
         (unsigned long long)stats.total_requests,
         (unsigned long long)stats.successful_requests,
         (unsigned long long)stats.failed_requests);
  printf("  Bytes sent: %llu  received: %llu\n",
         (unsigned long long)stats.bytes_sent,
         (unsigned long long)stats.bytes_received);

  http_coro_client_destroy(client);
  printf("\nDone.\n");
}

int main(void) {
  g_ctx = turbo_coro_context_create();

  turbo_coro_scheduler_t *sched = turbo_coro_scheduler_create();
  turbo_coro_spawn(sched, http_demo, NULL);
  turbo_coro_scheduler_run(sched);
  turbo_coro_scheduler_destroy(sched);

  turbo_coro_context_destroy(g_ctx);
  return 0;
}
