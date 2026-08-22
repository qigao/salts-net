#include "turbo_coro.h"
#include "turbo_coro_socket.h"
#include "tinytest.h"
#include "turbo_error.h"

#include <stdio.h>

typedef struct connect_policy_audit_s {
  int calls;
  int result;
  size_t result_count;
  int family;
  char hostname[64];
  char ip[64];
} connect_policy_audit_t;

static int audit_connect_policy(const char *hostname, int port,
                                const turbo_dns_result_t *results, size_t result_count,
                                void *user_data) {
  connect_policy_audit_t *audit = (connect_policy_audit_t *)user_data;
  (void)port;
  audit->calls++;
  audit->result_count = result_count;
  if (hostname) snprintf(audit->hostname, sizeof(audit->hostname), "%s", hostname);
  if (result_count > 0) {
    audit->family = results[0].family;
    snprintf(audit->ip, sizeof(audit->ip), "%s", results[0].ip);
  }
  return audit->result;
}

spec("CoroNet resolved-address connect policy") {
  it("reports one numeric address and permits clearing after connect") {
    coro_context_t *ctx = coro_context_create(NULL);
    coro_socket_t *socket;
    connect_policy_audit_t audit = {0};

    check_not_null(ctx);
    socket = coro_socket_create_udpv4(ctx);
    check_not_null(socket);
    check_equal(coro_socket_set_connect_policy(socket, audit_connect_policy, &audit), 0);
    check_equal(coro_socket_connect(socket, "127.0.0.1", 9), 0);
    check_equal(audit.calls, 1);
    check_equal(audit.result_count, 1);
    check_equal(audit.hostname, "127.0.0.1");
    check_equal(audit.ip, "127.0.0.1");
    check_equal(coro_socket_set_connect_policy(socket, NULL, NULL), 0);

    coro_socket_destroy(socket);
    coro_context_destroy(ctx);
  }

  it("propagates rejection before a backend connect") {
    coro_context_t *ctx = coro_context_create(NULL);
    coro_socket_t *socket;
    connect_policy_audit_t audit = {0};

    check_not_null(ctx);
    socket = coro_socket_create(ctx, CORO_SOCKET_TCP_V4);
    check_not_null(socket);
    audit.result = TURBO_EPERM;
    check_equal(coro_socket_set_connect_policy(socket, audit_connect_policy, &audit), 0);
    check_equal(coro_socket_connect(socket, "127.0.0.1", 1), TURBO_EPERM);
    check_equal(audit.calls, 1);
    check_equal(audit.result_count, 1);
    check_equal(coro_socket_set_connect_policy(socket, NULL, NULL), 0);

    coro_socket_destroy(socket);
    coro_context_destroy(ctx);
  }
}
