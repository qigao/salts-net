#include <stdlib.h>
#include <string.h>

#include "turbo_tproxy.h"
#include "unity.h"

/* Test fixtures */
static tproxy_server_t *server = NULL;
static int event_count = 0;
static int listening_event = 0;
static int connection_event = 0;
static int error_event = 0;

/* Simple event callback for tests */
static void test_event_cb(tproxy_server_t *srv, const tproxy_event_t *event,
                          void *user_data) {
  (void)srv;
  (void)event;
  (void)user_data;
  event_count++;

  switch (event->type) {
    case TPROXY_EVENT_LISTENING:
      listening_event = 1;
      break;
    case TPROXY_EVENT_CONNECTION:
      connection_event = 1;
      break;
    case TPROXY_EVENT_ERROR:
      error_event = 1;
      break;
    default:
      break;
  }
}

void setUp(void) {
  event_count = 0;
  listening_event = 0;
  connection_event = 0;
  error_event = 0;
  server = NULL;
}

void tearDown(void) {
  if (server) {
    tproxy_server_destroy(server);
    server = NULL;
  }
}

/* String conversion tests */
void test_event_type_to_string(void) {
  TEST_ASSERT_EQUAL_STRING("listening", tproxy_event_type_to_string(TPROXY_EVENT_LISTENING));
  TEST_ASSERT_EQUAL_STRING("connection", tproxy_event_type_to_string(TPROXY_EVENT_CONNECTION));
  TEST_ASSERT_EQUAL_STRING("data", tproxy_event_type_to_string(TPROXY_EVENT_DATA));
  TEST_ASSERT_EQUAL_STRING("disconnect", tproxy_event_type_to_string(TPROXY_EVENT_DISCONNECT));
  TEST_ASSERT_EQUAL_STRING("error", tproxy_event_type_to_string(TPROXY_EVENT_ERROR));
  TEST_ASSERT_EQUAL_STRING("upstream_connected", tproxy_event_type_to_string(TPROXY_EVENT_UPSTREAM_CONNECTED));
  TEST_ASSERT_EQUAL_STRING("closed", tproxy_event_type_to_string(TPROXY_EVENT_CLOSED));
  TEST_ASSERT_EQUAL_STRING("unknown", tproxy_event_type_to_string(999));
}

/* Server creation tests */
void test_create_server_with_null_config(void) {
  server = tproxy_server_create(NULL, test_event_cb, NULL);
  TEST_ASSERT_NULL(server);
}

void test_create_server_with_null_callback(void) {
  tproxy_config_t config = {0};
  config.listen_host = "127.0.0.1";
  config.listen_port = 1080;

  server = tproxy_server_create(&config, NULL, NULL);
  TEST_ASSERT_NULL(server);
}

void test_create_server_default_config(void) {
  tproxy_config_t config = {0};
  config.listen_host = "127.0.0.1";
  config.listen_port = 1080;

  server = tproxy_server_create(&config, test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);
}

void test_create_server_with_upstream(void) {
  tproxy_config_t config = {0};
  config.listen_host = "127.0.0.1";
  config.listen_port = 1080;
  config.upstream_host = "127.0.0.1";
  config.upstream_port = 1081;

  server = tproxy_server_create(&config, test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);
}

void test_create_server_with_socks5(void) {
  tproxy_config_t config = {0};
  config.listen_host = "127.0.0.1";
  config.listen_port = 1080;
  config.enable_socks5 = 1;

  server = tproxy_server_create(&config, test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);
}

void test_create_server_with_udp(void) {
  tproxy_config_t config = {0};
  config.listen_host = "127.0.0.1";
  config.listen_port = 1080;
  config.enable_udp = 1;

  server = tproxy_server_create(&config, test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);
}

void test_destroy_null_server(void) {
  /* Should not crash */
  tproxy_server_destroy(NULL);
}

void test_destroy_server(void) {
  tproxy_config_t config = {0};
  config.listen_host = "127.0.0.1";
  config.listen_port = 1080;

  server = tproxy_server_create(&config, test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);

  tproxy_server_destroy(server);
  server = NULL;
}

/* Start/Stop tests */
void test_start_null_server(void) {
  turbo_client_status_t status = tproxy_server_start(NULL);
  TEST_ASSERT_EQUAL(TURBO_CLIENT_STATUS_INVALID_PARAM, status);
}

void test_stop_null_server(void) {
  /* Should not crash */
  tproxy_server_stop(NULL);
}

void test_stop_server(void) {
  tproxy_config_t config = {0};
  config.listen_host = "127.0.0.1";
  config.listen_port = 1080;

  server = tproxy_server_create(&config, test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);

  /* Don't actually start, just test stop */
  tproxy_server_stop(server);
}

/* Send tests */
void test_send_to_null_server(void) {
  turbo_client_status_t status = tproxy_server_send(NULL, NULL, "test", 4);
  TEST_ASSERT_EQUAL(TURBO_CLIENT_STATUS_INVALID_PARAM, status);
}

void test_send_null_data(void) {
  tproxy_config_t config = {0};
  config.listen_host = "127.0.0.1";
  config.listen_port = 1080;

  server = tproxy_server_create(&config, test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);

  turbo_client_status_t status = tproxy_server_send(server, NULL, NULL, 4);
  TEST_ASSERT_EQUAL(TURBO_CLIENT_STATUS_INVALID_PARAM, status);
}

/* Connection management tests */
void test_connection_close_null(void) {
  /* Should not crash */
  tproxy_connection_close(NULL, NULL);
}

/* Address query tests */
void test_get_original_dest_null_connection(void) {
  tproxy_config_t config = {0};
  config.listen_host = "127.0.0.1";
  config.listen_port = 1080;

  server = tproxy_server_create(&config, test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);

  char host[256];
  int port;
  int result = tproxy_connection_get_original_dest(server, NULL, host, sizeof(host), &port);
  TEST_ASSERT_EQUAL(-1, result);
}

void test_get_client_addr_null_connection(void) {
  tproxy_config_t config = {0};
  config.listen_host = "127.0.0.1";
  config.listen_port = 1080;

  server = tproxy_server_create(&config, test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);

  char host[256];
  int port;
  int result = tproxy_connection_get_client_addr(server, NULL, host, sizeof(host), &port);
  TEST_ASSERT_EQUAL(-1, result);
}

void test_get_original_dest_query_length(void) {
  tproxy_config_t config = {0};
  config.listen_host = "127.0.0.1";
  config.listen_port = 1080;

  server = tproxy_server_create(&config, test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);

  /* Query only length */
  int port;
  int result = tproxy_connection_get_original_dest(server, NULL, NULL, 0, &port);
  TEST_ASSERT_EQUAL(-1, result);
}

/* Statistics tests */
void test_get_stats_null_server(void) {
  int active = -1;
  uint64_t total = 0, recv = 0, sent = 0;

  tproxy_server_get_stats(NULL, &active, &total, &recv, &sent);

  /* Should not crash, values should remain unchanged */
  TEST_ASSERT_EQUAL(-1, active);
  TEST_ASSERT_EQUAL(0, total);
}

void test_get_stats(void) {
  tproxy_config_t config = {0};
  config.listen_host = "127.0.0.1";
  config.listen_port = 1080;

  server = tproxy_server_create(&config, test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);

  int active = -1;
  uint64_t total = 0, recv = 0, sent = 0;

  tproxy_server_get_stats(server, &active, &total, &recv, &sent);

  TEST_ASSERT_EQUAL(0, active);
  TEST_ASSERT_EQUAL(0, total);
  TEST_ASSERT_EQUAL(0, recv);
  TEST_ASSERT_EQUAL(0, sent);
}

/* Configuration tests */
void test_server_with_zero_max_connections(void) {
  tproxy_config_t config = {0};
  config.listen_host = "127.0.0.1";
  config.listen_port = 1080;
  config.max_connections = 0;

  server = tproxy_server_create(&config, test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);
}

void test_server_with_zero_buffer_size(void) {
  tproxy_config_t config = {0};
  config.listen_host = "127.0.0.1";
  config.listen_port = 1080;
  config.buffer_size = 0;

  server = tproxy_server_create(&config, test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);
}

void test_server_with_custom_buffer_size(void) {
  tproxy_config_t config = {0};
  config.listen_host = "127.0.0.1";
  config.listen_port = 1080;
  config.buffer_size = 128 * 1024;

  server = tproxy_server_create(&config, test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);
}

void test_server_with_null_listen_host(void) {
  tproxy_config_t config = {0};
  config.listen_host = NULL;
  config.listen_port = 1080;

  server = tproxy_server_create(&config, test_event_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);
}

int main(void) {
  UNITY_BEGIN();

  /* String conversion */
  RUN_TEST(test_event_type_to_string);

  /* Server creation tests */
  RUN_TEST(test_create_server_with_null_config);
  RUN_TEST(test_create_server_with_null_callback);
  RUN_TEST(test_create_server_default_config);
  RUN_TEST(test_create_server_with_upstream);
  RUN_TEST(test_create_server_with_socks5);
  RUN_TEST(test_create_server_with_udp);
  RUN_TEST(test_destroy_null_server);
  RUN_TEST(test_destroy_server);

  /* Start/Stop tests */
  RUN_TEST(test_start_null_server);
  RUN_TEST(test_stop_null_server);
  RUN_TEST(test_stop_server);

  /* Send tests */
  RUN_TEST(test_send_to_null_server);
  RUN_TEST(test_send_null_data);

  /* Connection management tests */
  RUN_TEST(test_connection_close_null);

  /* Address query tests */
  RUN_TEST(test_get_original_dest_null_connection);
  RUN_TEST(test_get_client_addr_null_connection);
  RUN_TEST(test_get_original_dest_query_length);

  /* Statistics tests */
  RUN_TEST(test_get_stats_null_server);
  RUN_TEST(test_get_stats);

  /* Configuration tests */
  RUN_TEST(test_server_with_zero_max_connections);
  RUN_TEST(test_server_with_zero_buffer_size);
  RUN_TEST(test_server_with_custom_buffer_size);
  RUN_TEST(test_server_with_null_listen_host);

  return UNITY_END();
}
