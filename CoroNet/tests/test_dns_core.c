#include "CoroNet.h"
#include "turbo_dns.h"
#include "tinytest.h"

#include <string.h>

typedef struct {
  int called;
  int status;
  char ip[INET6_ADDRSTRLEN];
} dns_cb_state_t;

typedef struct {
  int called;
  int status;
  size_t count;
  turbo_dns_result_t results[TURBO_DNS_MAX_RESULTS];
} dns_results_state_t;

static void on_dns_resolved(const char *hostname, const char *ip, int status, void *user_data) {
  dns_cb_state_t *state = (dns_cb_state_t *)user_data;
  (void)hostname;
  state->called = 1;
  state->status = status;
  if (ip) {
    strncpy(state->ip, ip, sizeof(state->ip) - 1);
    state->ip[sizeof(state->ip) - 1] = '\0';
  }
}

static void on_dns_results(const char *hostname, const turbo_dns_result_t *results,
                           size_t count, int status, void *user_data) {
  dns_results_state_t *state = (dns_results_state_t *)user_data;
  (void)hostname;
  state->called = 1;
  state->status = status;
  state->count = count;
  if (results && count > 0) {
    memcpy(state->results, results, count * sizeof(results[0]));
  }
}

spec("dns_core") {
  it("should resolve literal ipv4 synchronously") {
    struct sockaddr_storage addr;
    int addr_len = 0;

    check_int_eq(turbo_dns_resolve(NULL, "127.0.0.1", 8080, &addr, &addr_len), 0);
    check_int_eq(addr.ss_family, AF_INET);
    check_int_eq(addr_len, (int)sizeof(struct sockaddr_in));
    check_int_eq(ntohs(((struct sockaddr_in *)&addr)->sin_port), 8080);
  }

  it("should resolve literal ipv4 asynchronously without background dns") {
    dns_cb_state_t state = {0};

    check_int_eq(turbo_dns_resolve_async(NULL, "127.0.0.1", TURBO_DNS_ANY,
                                         on_dns_resolved, &state), 0);
    check_int_eq(state.called, 1);
    check_int_eq(state.status, 0);
    check_str_eq(state.ip, "127.0.0.1");
  }

  it("should resolve literal ipv6 to ordered results immediately") {
    dns_results_state_t state = {0};

    check_int_eq(turbo_dns_resolve_async_results(NULL, "::1", TURBO_DNS_ANY,
                                                 on_dns_results, &state), 0);
    check_int_eq(state.called, 1);
    check_int_eq(state.status, 0);
    check_int_eq((int)state.count, 1);
    check_int_eq(state.results[0].family, AF_INET6);
    check_str_eq(state.results[0].ip, "::1");
  }

  it("should store and read configured dns servers") {
    const char *servers[] = {"1.1.1.1", "8.8.8.8"};
    char stored[2][46];
    int count = 0;

    check_int_eq(turbo_dns_init(), 0);
    check_int_eq(turbo_dns_set_servers(servers, 2), 0);
    check_int_eq(turbo_dns_get_servers(stored, 2, &count), 0);
    check_int_eq(count, 2);
    check_str_eq(stored[0], "1.1.1.1");
    check_str_eq(stored[1], "8.8.8.8");
    turbo_dns_cleanup();
  }
}
