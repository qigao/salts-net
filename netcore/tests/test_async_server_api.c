#include <stdlib.h>
#include <string.h>

#include "turbo_async_server.h"
#include "unity.h"

/* Test fixtures */
static async_server_t *server = NULL;
static int event_count = 0;

void setUp(void) {
  event_count = 0;
  server = NULL;
}

void tearDown(void) {
  if (server) {
    async_server_destroy(server);
    server = NULL;
  }
}

/* Simple event callback for tests */
static void test_event_cb(async_server_t *s, const async_server_event_t *event, void *user_data) {
  (void)s;
  (void)event;
  (void)user_data;
  event_count++;
}

/* String conversion tests */
void test_status_to_string(void) {
  TEST_ASSERT_EQUAL_STRING("ok", async_server_status_to_string(ASYNC_SERVER_STATUS_OK));
  TEST_ASSERT_EQUAL_STRING("invalid parameter", async_server_status_to_string(ASYNC_SERVER_STATUS_INVALID_PARAM));
  TEST_ASSERT_EQUAL_STRING("allocation failure", async_server_status_to_string(ASYNC_SERVER_STATUS_ALLOC_FAILED));
  TEST_ASSERT_EQUAL_STRING("server not ready", async_server_status_to_string(ASYNC_SERVER_STATUS_NOT_READY));
  TEST_ASSERT_EQUAL_STRING("server shutting down", async_server_status_to_string(ASYNC_SERVER_STATUS_SHUTTING_DOWN));
  TEST_ASSERT_EQUAL_STRING("I/O error", async_server_status_to_string(ASYNC_SERVER_STATUS_IO_ERROR));
  TEST_ASSERT_EQUAL_STRING("transport error", async_server_status_to_string(ASYNC_SERVER_STATUS_TRANSPORT_ERROR));
  TEST_ASSERT_EQUAL_STRING("internal error", async_server_status_to_string(ASYNC_SERVER_STATUS_INTERNAL_ERROR));
  TEST_ASSERT_EQUAL_STRING("unknown error", async_server_status_to_string(999));
}

/* test_transport_to_string - REMOVED: transport types no longer exposed in public API */

/* Server creation tests */
void test_create_null_callback(void) {
  async_server_t *s = async_server_create(NULL, NULL);
  TEST_ASSERT_NULL(s);
}

void test_create_tcp(void) {
  server = async_server_create(test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);
}

void test_create_udp(void) {
  server = async_server_create(test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);
}

void test_destroy_null(void) {
  /* Should not crash */
  async_server_destroy(NULL);
}

/* State query tests */
void test_get_state_null(void) {
  async_server_state_t state = async_server_get_state(NULL);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATE_STOPPED, state);
}

void test_get_state_stopped(void) {
  server = async_server_create(test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);

  async_server_state_t state = async_server_get_state(server);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATE_STOPPED, state);
}

void test_is_listening_null(void) {
  int listening = async_server_is_listening(NULL);
  TEST_ASSERT_EQUAL(0, listening);
}

void test_is_listening_stopped(void) {
  server = async_server_create(test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);

  int listening = async_server_is_listening(server);
  TEST_ASSERT_EQUAL(0, listening);
}

void test_get_connection_count_null(void) {
  size_t count = async_server_get_connection_count(NULL);
  TEST_ASSERT_EQUAL(0, count);
}

void test_get_connection_count_initial(void) {
  server = async_server_create(test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);

  size_t count = async_server_get_connection_count(server);
  TEST_ASSERT_EQUAL(0, count);
}

/* Operation with NULL server tests */
void test_listen_null_server(void) {
  async_server_status_t status = async_server_listen(NULL, "tcp://0.0.0.0:8080", 0);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATUS_INVALID_PARAM, status);
}

void test_send_null_server(void) {
  const char *data = "test";
  async_server_status_t status = async_server_send(NULL, NULL, data, 4);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATUS_INVALID_PARAM, status);
}




void test_sendv_null_server(void) {
  async_server_iovec_t iov[1];
  iov[0].data = "test";
  iov[0].len = 4;

  async_server_status_t status = async_server_sendv(NULL, NULL, iov, 1);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATUS_INVALID_PARAM, status);
}



void test_broadcast_null_server(void) {
  const char *data = "test";
  async_server_status_t status = async_server_broadcast(NULL, data, 4);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATUS_INVALID_PARAM, status);
}



void test_close_connection_null_server(void) {
  /* Should not crash */
  async_server_close_connection(NULL, NULL);
}


void test_stop_null(void) {
  /* Should not crash */
  async_server_stop(NULL);
}

/* Configuration tests */
void test_set_max_connections_null(void) {
  /* Should not crash */
  async_server_set_max_connections(NULL, 100);
}

void test_set_max_connections(void) {
  server = async_server_create(test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);

  /* Should not crash, no return value to test */
  async_server_set_max_connections(server, 100);
}

void test_set_idle_timeout_null(void) {
  /* Should not crash */
  async_server_set_idle_timeout(NULL, 30000);
}

void test_set_idle_timeout(void) {
  server = async_server_create(test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);

  /* Should not crash, no return value to test */
  async_server_set_idle_timeout(server, 30000);
}

/* Connection info tests */
void test_get_connection_info_null_connection(void) {
  async_server_connection_info_t info;
  async_server_status_t status = async_server_get_connection_info(NULL, &info);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATUS_INVALID_PARAM, status);
}


void test_connection_user_data_null(void) {
  /* Should not crash */
  async_server_connection_set_user_data(NULL, (void*)0x1234);
  void *data = async_server_connection_get_user_data(NULL);
  TEST_ASSERT_NULL(data);
}

/* Statistics tests */
void test_get_stats_null_server(void) {
  async_server_stats_t stats;
  /* Should not crash */
  async_server_get_stats(NULL, &stats);
}


void test_get_stats_initial(void) {
  server = async_server_create(test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);

  async_server_stats_t stats;
  async_server_get_stats(server, &stats);

  /* Initial stats should be zero */
  TEST_ASSERT_EQUAL(0, stats.bytes_sent);
  TEST_ASSERT_EQUAL(0, stats.bytes_received);
  TEST_ASSERT_EQUAL(0, stats.messages_sent);
  TEST_ASSERT_EQUAL(0, stats.messages_received);
  TEST_ASSERT_EQUAL(0, stats.total_connections);
  TEST_ASSERT_EQUAL(0, stats.active_connections);
  TEST_ASSERT_EQUAL(0, stats.rejected_connections);
  TEST_ASSERT_EQUAL(0, stats.send_errors);
  TEST_ASSERT_EQUAL(0, stats.receive_errors);
  TEST_ASSERT_EQUAL(0, stats.scatter_gather_sends);
  TEST_ASSERT_EQUAL(0, stats.total_iov_buffers_sent);
  TEST_ASSERT_EQUAL(0, stats.broadcasts);
}

void test_reset_stats_null(void) {
  /* Should not crash */
  async_server_reset_stats(NULL);
}

void test_reset_stats(void) {
  server = async_server_create(test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);

  /* Should not crash */
  async_server_reset_stats(server);

  async_server_stats_t stats;
  async_server_get_stats(server, &stats);
  TEST_ASSERT_EQUAL(0, stats.bytes_sent);
}

/* TLS configuration tests */
void test_set_tls_config_null_server(void) {
  async_server_tls_config_t config = {0};
  async_server_status_t status = async_server_set_tls_config(NULL, &config);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATUS_INVALID_PARAM, status);
}


void test_set_tls_config_wrong_transport(void) {
  server = async_server_create(test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);

  async_server_tls_config_t config = {0};
  async_server_status_t status = async_server_set_tls_config(server, &config);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATUS_INVALID_PARAM, status);
}

/* Multicast tests (UDP only) */
void test_join_multicast_null_server(void) {
  async_server_status_t status = async_server_join_multicast_group(NULL, "239.0.0.1", NULL);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATUS_INVALID_PARAM, status);
}


void test_join_multicast_wrong_transport(void) {
  server = async_server_create(test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);

  async_server_status_t status = async_server_join_multicast_group(server, "239.0.0.1", NULL);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATUS_TRANSPORT_ERROR, status);
}

void test_leave_multicast_null_server(void) {
  async_server_status_t status = async_server_leave_multicast_group(NULL, "239.0.0.1", NULL);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATUS_INVALID_PARAM, status);
}


void test_set_multicast_ttl_null_server(void) {
  async_server_status_t status = async_server_set_multicast_ttl(NULL, 32);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATUS_INVALID_PARAM, status);
}


void test_set_multicast_ttl_invalid_value(void) {
  server = async_server_create(test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);

  /* TTL out of range (1-255) */
  async_server_status_t status = async_server_set_multicast_ttl(server, 0);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATUS_INVALID_PARAM, status);

  status = async_server_set_multicast_ttl(server, 256);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATUS_INVALID_PARAM, status);
}

void test_set_multicast_loop_null_server(void) {
  async_server_status_t status = async_server_set_multicast_loop(NULL, 1);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATUS_INVALID_PARAM, status);
}


int main(void) {
  UNITY_BEGIN();

  /* String conversion */
  RUN_TEST(test_status_to_string);
  /* test_transport_to_string - REMOVED */

  /* Server creation/destruction */
  RUN_TEST(test_create_null_callback);
  RUN_TEST(test_create_tcp);
  RUN_TEST(test_create_udp);
  RUN_TEST(test_destroy_null);

  /* State queries */
  RUN_TEST(test_get_state_null);
  RUN_TEST(test_get_state_stopped);
  RUN_TEST(test_is_listening_null);
  RUN_TEST(test_is_listening_stopped);
  RUN_TEST(test_get_connection_count_null);
  RUN_TEST(test_get_connection_count_initial);

  /* Operations with NULL/invalid parameters */
  RUN_TEST(test_listen_null_server);
  RUN_TEST(test_send_null_server);
  RUN_TEST(test_sendv_null_server);
  RUN_TEST(test_broadcast_null_server);
  RUN_TEST(test_close_connection_null_server);
  RUN_TEST(test_stop_null);

  /* Configuration */
  RUN_TEST(test_set_max_connections_null);
  RUN_TEST(test_set_max_connections);
  RUN_TEST(test_set_idle_timeout_null);
  RUN_TEST(test_set_idle_timeout);

  /* Connection info */
  RUN_TEST(test_get_connection_info_null_connection);
  RUN_TEST(test_connection_user_data_null);

  /* Statistics */
  RUN_TEST(test_get_stats_null_server);
  RUN_TEST(test_get_stats_initial);
  RUN_TEST(test_reset_stats_null);
  RUN_TEST(test_reset_stats);

  /* TLS configuration */
  RUN_TEST(test_set_tls_config_null_server);
  RUN_TEST(test_set_tls_config_wrong_transport);

  /* Multicast configuration */
  RUN_TEST(test_join_multicast_null_server);
  RUN_TEST(test_join_multicast_wrong_transport);
  RUN_TEST(test_leave_multicast_null_server);
  RUN_TEST(test_set_multicast_ttl_null_server);
  RUN_TEST(test_set_multicast_ttl_invalid_value);
  RUN_TEST(test_set_multicast_loop_null_server);

  return UNITY_END();
}
