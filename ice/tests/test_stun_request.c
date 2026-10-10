#include "stun_test_server.h"
#include "ice/salts_turn.h"
#include <salts/clock.h>
#include <salts/error_codes.h>
#include <tinytest.h>

spec("public STUN request ownership") {
  static stun_test_server_t server;
  static cmeta_thread_t thread;
  static stun_request_t *request;
  static salts_turn_client_t *client;
  static int server_open;
  before_each() {
    memset(&server, 0, sizeof(server));
    server.socket = STUN_TEST_INVALID_SOCKET;
    thread = NULL;
    request = NULL;
    client = NULL;
    server_open = 0;
  }
  after_each() {
    int cleanup = SALTS_OK, joined = 0;
    if (thread) {
      joined = cmeta_thread_join(&thread);
      cmeta_thread_destroy(&thread);
    }
    if (request) {
      cleanup = stun_request_stop(request, 1000u);
      if (cleanup == SALTS_OK) cleanup = stun_request_destroy(&request);
    }
    if (client) {
      int status = turn_client_destroy_checked(&client, 1000u);
      if (cleanup == SALTS_OK) cleanup = status;
    }
    if (server_open) stun_test_server_close(&server);
    check_equal(joined, 0);
    check_equal(cleanup, SALTS_OK);
  }
  for (int dropped = 0; dropped <= 1; ++dropped) {
    it("copies input, matches responses and retains result after stop (drops=%d)", dropped) {
      stun_request_t *saved;
      stun_mapped_address_t mapped = {0};
      char host[] = "127.0.0.1";
      stun_client_config_t config = {host, 0u, 100, dropped + 1};
      uint16_t port;
      uint64_t deadline;
      int status, result = SALTS_EBUSY;
      server_open = 1;
      check_equal(stun_test_server_open(&server, &port), 0);
      config.server_port = port;
      server.drop_requests = dropped;
      server.send_mismatched_first = 1;
      check_equal(cmeta_thread_create(&thread, stun_test_server_run, &server), 0);
      check_equal(stun_request_create(&request), SALTS_OK);
      saved = request;
      check_equal(stun_request_start(request, &config), SALTS_OK);
      memset(host, 'x', sizeof(host) - 1u);
      check_equal(stun_request_start(request, &config), SALTS_EALREADY);
      check_equal(stun_request_destroy(&request), SALTS_EBUSY);
      check(request == saved);
      deadline = cmeta_monotonic_ms() + STUN_TEST_TIMEOUT_MS;
      do {
        check(cmeta_monotonic_ms() < deadline);
        check_equal(stun_request_progress(request, 10u), SALTS_OK);
        status = stun_request_result(request, &result, &mapped);
      } while (status == SALTS_EBUSY);
      check_equal(status, SALTS_OK);
      check_equal(result, SALTS_OK);
      check_equal(mapped.ip_str, "203.0.113.17");
      check_equal(mapped.port, 45678u);
      check_equal(stun_request_stop(request, 1000u), SALTS_OK);
      check_equal(stun_request_result(request, &result, NULL), SALTS_OK);
      check_equal(result, SALTS_OK);
      check_equal(stun_request_destroy(&request), SALTS_OK);
      check(request == NULL);
      check_equal(cmeta_thread_join(&thread), 0);
      cmeta_thread_destroy(&thread);
      stun_test_server_close(&server);
      server_open = 0;
      check_equal(server.status, 0);
      check_equal(server.received_requests, dropped + 1);
    }
  }

  it("retains cancellation ownership when the cleanup budget is exhausted") {
    stun_request_t *saved;
    stun_client_config_t config = {"127.0.0.1", 0u, 1000, 1};
    stun_mapped_address_t mapped = {0};
    uint16_t port;
    int status, result;
    server_open = 1;
    check_equal(stun_test_server_open(&server, &port), 0);
    config.server_port = port;
    check_equal(stun_request_create(&request), SALTS_OK);
    saved = request;
    check_equal(stun_request_start(request, &config), SALTS_OK);
    /* Drive send completion and arm a receive with no responder. */
    for (int i = 0; i < 4; ++i) check_equal(stun_request_progress(request, 1u), SALTS_OK);
    check_equal(stun_request_result(request, &result, NULL), SALTS_EBUSY);
    status = stun_request_stop(request, 0u);
    check(status == SALTS_OK || status == SALTS_ETIMEDOUT);
    check(request == saved);
    if (status != SALTS_OK) {
      check_equal(stun_request_destroy(&request), SALTS_EBUSY);
      check(request == saved);
    }
    check_equal(stun_request_progress(request, 0u), SALTS_ESHUTDOWN);
    mapped.port = 123u;
    check_equal(stun_request_result(request, &result, &mapped), SALTS_OK);
    check_equal(result, SALTS_ECANCELED);
    check_equal(mapped.port, 123u);
    check_equal(stun_request_stop(request, 1000u), SALTS_OK);
    check_equal(stun_request_stop(request, 0u), SALTS_OK);
    check_equal(stun_request_destroy(&request), SALTS_OK);
    check(request == NULL);
    stun_test_server_close(&server);
    server_open = 0;
  }

  it("publishes timeout separately from cleanup and leaves mapped output untouched") {
    stun_client_config_t config = {"127.0.0.1", 0u, 10, 1};
    stun_mapped_address_t mapped = {0};
    int status, result = SALTS_OK;
    uint64_t deadline;
    server_open = 1;
    check_equal(stun_test_server_open(&server, &config.server_port), 0);
    check_equal(stun_request_create(&request), SALTS_OK);
    check_equal(stun_request_start(request, &config), SALTS_OK);
    mapped.port = 123u;
    deadline = cmeta_monotonic_ms() + 1000u;
    do {
      check(cmeta_monotonic_ms() < deadline);
      check_equal(stun_request_progress(request, 10u), SALTS_OK);
      status = stun_request_result(request, &result, &mapped);
    } while (status == SALTS_EBUSY);
    check_equal(status, SALTS_OK);
    check_equal(result, SALTS_ETIMEDOUT);
    check_equal(mapped.port, 123u);
    check_equal(stun_request_destroy(&request), SALTS_EBUSY);
    check_equal(stun_request_stop(request, 1000u), SALTS_OK);
    check_equal(stun_request_result(request, &result, NULL), SALTS_OK);
    check_equal(result, SALTS_ETIMEDOUT);
    check_equal(stun_request_destroy(&request), SALTS_OK);
  }

  it("rejects invalid inputs without taking ownership and cleans unstarted requests") {
    stun_client_config_t config = {"localhost", 3478u, 10, 1};
    int result = 17;
    check_equal(stun_request_create(NULL), SALTS_EINVAL);
    check_equal(stun_request_destroy(NULL), SALTS_EINVAL);
    check_equal(stun_request_destroy(&request), SALTS_OK);
    check_equal(stun_request_create(&request), SALTS_OK);
    check_equal(stun_request_create(&request), SALTS_EALREADY);
    check_equal(stun_request_start(request, &config), SALTS_EINVAL);
    check_equal(stun_request_result(request, &result, NULL), SALTS_EINVAL);
    check_equal(result, 17);
    check_equal(stun_request_stop(request, 0u), SALTS_OK);
    check_equal(stun_request_start(request, &config), SALTS_ESHUTDOWN);
    check_equal(stun_request_destroy(&request), SALTS_OK);
    check(request == NULL);
  }

  it("validates synchronous use before admission and never consumes its Owner") {
    stun_client_config_t config = {"localhost", 3478u, 10, 1};
    stun_mapped_address_t mapped = {0};
    stun_request_t *saved;
    int result = 17;
    mapped.port = 123u;
    check_equal(stun_binding_request(NULL, &config, &mapped), SALTS_EINVAL);
    check_equal(stun_request_create(&request), SALTS_OK);
    saved = request;
    check_equal(stun_binding_request(request, &config, NULL), SALTS_EINVAL);
    check_equal(stun_binding_request(request, NULL, &mapped), SALTS_EINVAL);
    check_equal(stun_binding_request(request, &config, &mapped), SALTS_EINVAL);
    check_equal(stun_request_result(request, &result, NULL), SALTS_EINVAL);
    check_equal(result, 17);
    check_equal(mapped.port, 123u);
    check_equal(stun_request_destroy(&request), SALTS_EBUSY);
    check(request == saved);
    check_equal(stun_request_stop(request, 0u), SALTS_OK);
    check_equal(stun_binding_request(request, &config, &mapped), SALTS_ESHUTDOWN);
    check_equal(stun_request_destroy(&request), SALTS_OK);
    check(request == NULL);
  }

  it("reports checked TURN destruction and clears only consumed handles") {
    turn_client_config_t config = {0};
    config.server_host = "127.0.0.1";
    config.server_port = STUN_DEFAULT_PORT;
    config.username = "test";
    config.password = "test";
    client = turn_client_create(&config);
    check_not_null(client);
    check_equal(turn_client_destroy_checked(&client, 1000u), SALTS_OK);
    check(client == NULL);
    check_equal(turn_client_destroy_checked(&client, 0u), SALTS_OK);
    check_equal(turn_client_destroy_checked(NULL, 0u), SALTS_EINVAL);
  }
}
