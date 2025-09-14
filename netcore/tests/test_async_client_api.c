#include <stdlib.h>
#include <string.h>

#include "turbo_async_client.h"
#include "unity.h"

/* Test fixtures */
static async_client_t *client = NULL;
static int event_count = 0;

void setUp(void) {
  event_count = 0;
  client = NULL;
}

void tearDown(void) {
  if (client) {
    async_client_destroy(client);
    client = NULL;
  }
}

/* Simple event callback for tests */
static void test_event_cb(async_client_t *c, const async_client_event_t *event, void *user_data) {
  (void)c;
  (void)event;
  (void)user_data;
  event_count++;
}

/* String conversion tests */
void test_status_to_string(void) {
  TEST_ASSERT_EQUAL_STRING("ok", async_client_status_to_string(ASYNC_CLIENT_STATUS_OK));
  TEST_ASSERT_EQUAL_STRING("invalid parameter", async_client_status_to_string(ASYNC_CLIENT_STATUS_INVALID_PARAM));
  TEST_ASSERT_EQUAL_STRING("allocation failure", async_client_status_to_string(ASYNC_CLIENT_STATUS_ALLOC_FAILED));
  TEST_ASSERT_EQUAL_STRING("client not ready", async_client_status_to_string(ASYNC_CLIENT_STATUS_NOT_READY));
  TEST_ASSERT_EQUAL_STRING("client shutting down", async_client_status_to_string(ASYNC_CLIENT_STATUS_SHUTTING_DOWN));
  TEST_ASSERT_EQUAL_STRING("I/O error", async_client_status_to_string(ASYNC_CLIENT_STATUS_IO_ERROR));
  TEST_ASSERT_EQUAL_STRING("transport error", async_client_status_to_string(ASYNC_CLIENT_STATUS_TRANSPORT_ERROR));
  TEST_ASSERT_EQUAL_STRING("internal error", async_client_status_to_string(ASYNC_CLIENT_STATUS_INTERNAL_ERROR));
  TEST_ASSERT_EQUAL_STRING("unknown error", async_client_status_to_string(999));
}

void test_transport_to_string(void) {
  TEST_ASSERT_EQUAL_STRING("tcp", async_client_transport_to_string(ASYNC_CLIENT_TRANSPORT_TCP));
  TEST_ASSERT_EQUAL_STRING("udp", async_client_transport_to_string(ASYNC_CLIENT_TRANSPORT_UDP));
  TEST_ASSERT_EQUAL_STRING("kcp", async_client_transport_to_string(ASYNC_CLIENT_TRANSPORT_KCP));
  TEST_ASSERT_EQUAL_STRING("tls", async_client_transport_to_string(ASYNC_CLIENT_TRANSPORT_TLS));
  TEST_ASSERT_EQUAL_STRING("pipe", async_client_transport_to_string(ASYNC_CLIENT_TRANSPORT_PIPE));
  TEST_ASSERT_EQUAL_STRING("unknown", async_client_transport_to_string(999));
}

/* Client creation tests */
void test_create_null_callback(void) {
  async_client_t *c = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, NULL, NULL);
  TEST_ASSERT_NULL(c);
}

void test_create_tcp(void) {
  client = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(client);
}

void test_create_udp(void) {
  client = async_client_create(ASYNC_CLIENT_TRANSPORT_UDP, test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(client);
}

void test_destroy_null(void) {
  /* Should not crash */
  async_client_destroy(NULL);
}

/* State query tests */
void test_get_state_null(void) {
  async_client_state_t state = async_client_get_state(NULL);
  TEST_ASSERT_EQUAL(ASYNC_CLIENT_STATE_DISCONNECTED, state);
}

void test_get_state_disconnected(void) {
  client = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(client);

  async_client_state_t state = async_client_get_state(client);
  TEST_ASSERT_EQUAL(ASYNC_CLIENT_STATE_DISCONNECTED, state);
}

void test_is_connected_null(void) {
  int connected = async_client_is_connected(NULL);
  TEST_ASSERT_EQUAL(0, connected);
}

void test_is_connected_disconnected(void) {
  client = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(client);

  int connected = async_client_is_connected(client);
  TEST_ASSERT_EQUAL(0, connected);
}

/* Operation with NULL client tests */
void test_connect_null_client(void) {
  async_client_status_t status = async_client_connect(NULL, "localhost", 8080);
  TEST_ASSERT_EQUAL(ASYNC_CLIENT_STATUS_INVALID_PARAM, status);
}


void test_send_null_client(void) {
  const char *data = "test";
  async_client_status_t status = async_client_send(NULL, data, 4);
  TEST_ASSERT_EQUAL(ASYNC_CLIENT_STATUS_INVALID_PARAM, status);
}



void test_sendv_null_client(void) {
  async_client_iovec_t iov[1];
  iov[0].data = "test";
  iov[0].len = 4;

  async_client_status_t status = async_client_sendv(NULL, iov, 1);
  TEST_ASSERT_EQUAL(ASYNC_CLIENT_STATUS_INVALID_PARAM, status);
}



void test_close_null(void) {
  /* Should not crash */
  async_client_close(NULL);
}

/* Timeout configuration tests */
void test_set_connect_timeout_null(void) {
  /* Should not crash */
  async_client_set_connect_timeout(NULL, 5000);
}

void test_set_connect_timeout(void) {
  client = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(client);

  /* Should not crash, no return value to test */
  async_client_set_connect_timeout(client, 5000);
}

void test_set_operation_timeout_null(void) {
  /* Should not crash */
  async_client_set_operation_timeout(NULL, 5000);
}

void test_set_operation_timeout(void) {
  client = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(client);

  /* Should not crash, no return value to test */
  async_client_set_operation_timeout(client, 5000);
}

/* Statistics tests */
void test_get_stats_null_client(void) {
  async_client_stats_t stats;
  /* Should not crash */
  async_client_get_stats(NULL, &stats);
}


void test_get_stats_initial(void) {
  client = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(client);

  async_client_stats_t stats;
  async_client_get_stats(client, &stats);

  /* Initial stats should be zero */
  TEST_ASSERT_EQUAL(0, stats.bytes_sent);
  TEST_ASSERT_EQUAL(0, stats.bytes_received);
  TEST_ASSERT_EQUAL(0, stats.messages_sent);
  TEST_ASSERT_EQUAL(0, stats.messages_received);
  TEST_ASSERT_EQUAL(0, stats.connection_attempts);
  TEST_ASSERT_EQUAL(0, stats.connection_failures);
  TEST_ASSERT_EQUAL(0, stats.send_errors);
  TEST_ASSERT_EQUAL(0, stats.receive_errors);
  TEST_ASSERT_EQUAL(0, stats.scatter_gather_sends);
  TEST_ASSERT_EQUAL(0, stats.total_iov_buffers_sent);
}

void test_reset_stats_null(void) {
  /* Should not crash */
  async_client_reset_stats(NULL);
}

void test_reset_stats(void) {
  client = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(client);

  /* Should not crash, no way to verify without connecting */
  async_client_reset_stats(client);

  async_client_stats_t stats;
  async_client_get_stats(client, &stats);
  TEST_ASSERT_EQUAL(0, stats.bytes_sent);
}

/* Multicast configuration tests (UDP only) */
void test_set_multicast_ttl_null(void) {
  async_client_status_t status = async_client_set_multicast_ttl(NULL, 32);
  TEST_ASSERT_EQUAL(ASYNC_CLIENT_STATUS_INVALID_PARAM, status);
}

void test_set_multicast_ttl_wrong_transport(void) {
  client = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(client);

  async_client_status_t status = async_client_set_multicast_ttl(client, 32);
  TEST_ASSERT_EQUAL(ASYNC_CLIENT_STATUS_TRANSPORT_ERROR, status);
}

void test_set_multicast_ttl_invalid_value(void) {
  client = async_client_create(ASYNC_CLIENT_TRANSPORT_UDP, test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(client);

  /* TTL out of range (1-255) */
  async_client_status_t status = async_client_set_multicast_ttl(client, 0);
  TEST_ASSERT_EQUAL(ASYNC_CLIENT_STATUS_INVALID_PARAM, status);

  status = async_client_set_multicast_ttl(client, 256);
  TEST_ASSERT_EQUAL(ASYNC_CLIENT_STATUS_INVALID_PARAM, status);
}

void test_set_multicast_loop_null(void) {
  async_client_status_t status = async_client_set_multicast_loop(NULL, 1);
  TEST_ASSERT_EQUAL(ASYNC_CLIENT_STATUS_INVALID_PARAM, status);
}

void test_set_multicast_loop_wrong_transport(void) {
  client = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(client);

  async_client_status_t status = async_client_set_multicast_loop(client, 1);
  TEST_ASSERT_EQUAL(ASYNC_CLIENT_STATUS_TRANSPORT_ERROR, status);
}

int main(void) {
  UNITY_BEGIN();

  /* String conversion */
  RUN_TEST(test_status_to_string);
  RUN_TEST(test_transport_to_string);

  /* Client creation/destruction */
  RUN_TEST(test_create_null_callback);
  RUN_TEST(test_create_tcp);
  RUN_TEST(test_create_udp);
  RUN_TEST(test_destroy_null);

  /* State queries */
  RUN_TEST(test_get_state_null);
  RUN_TEST(test_get_state_disconnected);
  RUN_TEST(test_is_connected_null);
  RUN_TEST(test_is_connected_disconnected);

  /* Operations with NULL/invalid parameters */
  RUN_TEST(test_connect_null_client);
  RUN_TEST(test_send_null_client);
  RUN_TEST(test_sendv_null_client);
  RUN_TEST(test_close_null);

  /* Timeout configuration */
  RUN_TEST(test_set_connect_timeout_null);
  RUN_TEST(test_set_connect_timeout);
  RUN_TEST(test_set_operation_timeout_null);
  RUN_TEST(test_set_operation_timeout);

  /* Statistics */
  RUN_TEST(test_get_stats_null_client);
  RUN_TEST(test_get_stats_initial);
  RUN_TEST(test_reset_stats_null);
  RUN_TEST(test_reset_stats);

  /* Multicast configuration */
  RUN_TEST(test_set_multicast_ttl_null);
  RUN_TEST(test_set_multicast_ttl_wrong_transport);
  RUN_TEST(test_set_multicast_ttl_invalid_value);
  RUN_TEST(test_set_multicast_loop_null);
  RUN_TEST(test_set_multicast_loop_wrong_transport);

  return UNITY_END();
}
