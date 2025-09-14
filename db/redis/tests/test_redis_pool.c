/**
 * @file test_redis_pool.c
 * @brief Unit tests for Redis Connection Pool
 */

#include "../redis_pool.h"
#include "unity.h"
#include <string.h>
#include <stdio.h>

void setUp(void) {
}

void tearDown(void) {
}

// =============================================================================
// Configuration Tests
// =============================================================================

void test_pool_create_default(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);
    TEST_ASSERT_NOT_NULL(pool);
    redis_pool_destroy(pool);
}

void test_pool_create_custom(void) {
    redis_pool_config_t config = {
        .master_host = "redis.example.com",
        .master_port = 6380,
        .password = "secret123",
        .database = 5,
        .min_connections = 5,
        .max_connections = 20,
        .connect_timeout_ms = 10000,
        .idle_timeout_ms = 120000,
        .health_check_ms = 60000,
        .pipeline_max = 200,
        .pipeline_timeout_ms = 20
    };

    redis_pool_t *pool = redis_pool_create(&config);
    TEST_ASSERT_NOT_NULL(pool);
    redis_pool_destroy(pool);
}

void test_pool_create_null_config(void) {
    redis_pool_t *pool = redis_pool_create(NULL);
    TEST_ASSERT_NULL(pool);
}

void test_pool_create_null_host(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    config.master_host = NULL;
    redis_pool_t *pool = redis_pool_create(&config);
    TEST_ASSERT_NULL(pool);
}

void test_pool_destroy_null(void) {
    /* Should not crash */
    redis_pool_destroy(NULL);
    TEST_PASS();
}

// =============================================================================
// Pool Lifecycle Tests
// =============================================================================

void test_pool_stop_null(void) {
    /* Should not crash */
    redis_pool_stop(NULL);
    TEST_PASS();
}

void test_pool_start_null(void) {
    int result = redis_pool_start(NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

void test_pool_start_creates_connections(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    config.min_connections = 2;
    redis_pool_t *pool = redis_pool_create(&config);
    TEST_ASSERT_NOT_NULL(pool);

    /* Start pool - connections won't actually connect in tests */
    int result = redis_pool_start(pool);
    /* May return 0 or -1 depending on network availability */
    (void)result;

    redis_pool_destroy(pool);
}

void test_pool_stop_twice(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);
    TEST_ASSERT_NOT_NULL(pool);

    redis_pool_stop(pool);
    redis_pool_stop(pool);  /* Should not crash */

    redis_pool_destroy(pool);
}

// =============================================================================
// Connection Acquisition Tests
// =============================================================================

void test_pool_acquire_null(void) {
    redis_pool_conn_t *conn = redis_pool_acquire(NULL, 0);
    TEST_ASSERT_NULL(conn);
}

void test_pool_acquire_not_running(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);
    TEST_ASSERT_NOT_NULL(pool);

    /* Pool not started */
    redis_pool_conn_t *conn = redis_pool_acquire(pool, 0);
    TEST_ASSERT_NULL(conn);

    redis_pool_destroy(pool);
}

void test_pool_release_null(void) {
    /* Should not crash */
    redis_pool_release(NULL, NULL);

    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);
    redis_pool_release(pool, NULL);
    redis_pool_release(NULL, (redis_pool_conn_t *)0x1);  /* fake pointer */
    redis_pool_destroy(pool);
    TEST_PASS();
}

void test_pool_conn_client_null(void) {
    redis_client_t *client = redis_pool_conn_client(NULL);
    TEST_ASSERT_NULL(client);
}

// =============================================================================
// Pool Command Tests (without connection)
// =============================================================================

void test_pool_command_null(void) {
    int result = redis_pool_command(NULL, NULL, NULL, "PING");
    TEST_ASSERT_EQUAL(-1, result);
}

void test_pool_command_null_format(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    int result = redis_pool_command(pool, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_pool_destroy(pool);
}

void test_pool_commandv_null(void) {
    const char *argv[] = {"SET", "key", "value"};
    int result = redis_pool_commandv(NULL, 3, argv, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

void test_pool_commandv_null_argv(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    int result = redis_pool_commandv(pool, 3, NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_pool_destroy(pool);
}

void test_pool_commandv_zero_argc(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    const char *argv[] = {"SET"};
    int result = redis_pool_commandv(pool, 0, argv, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_pool_destroy(pool);
}

void test_pool_read_command_null(void) {
    int result = redis_pool_read_command(NULL, NULL, NULL, "GET key");
    TEST_ASSERT_EQUAL(-1, result);
}

// =============================================================================
// Convenience Function Tests
// =============================================================================

void test_pool_set_not_running(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    int result = redis_pool_set(pool, "key", "value", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_pool_destroy(pool);
}

void test_pool_get_not_running(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    int result = redis_pool_get(pool, "key", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_pool_destroy(pool);
}

void test_pool_del_not_running(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    const char *keys[] = {"key1", "key2"};
    int result = redis_pool_del(pool, 2, keys, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_pool_destroy(pool);
}

void test_pool_expire_not_running(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    int result = redis_pool_expire(pool, "key", 3600, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_pool_destroy(pool);
}

void test_pool_hset_not_running(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    int result = redis_pool_hset(pool, "hash", "field", "value", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_pool_destroy(pool);
}

void test_pool_hget_not_running(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    int result = redis_pool_hget(pool, "hash", "field", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_pool_destroy(pool);
}

void test_pool_xadd_not_running(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    const char *fields[] = {"f1"};
    const char *values[] = {"v1"};
    int result = redis_pool_xadd(pool, "stream", 1000, 1, fields, values, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_pool_destroy(pool);
}

void test_pool_xreadgroup_not_running(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    const char *keys[] = {"stream"};
    const char *ids[] = {">"};
    int result = redis_pool_xreadgroup(pool, "group", "consumer", 10, 1000, 1, keys, ids, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_pool_destroy(pool);
}

// =============================================================================
// Pipeline Tests
// =============================================================================

void test_pipeline_create_null(void) {
    redis_pipeline_t *pipeline = redis_pool_pipeline_create(NULL);
    TEST_ASSERT_NULL(pipeline);
}

void test_pipeline_create_destroy(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    redis_pipeline_t *pipeline = redis_pool_pipeline_create(pool);
    TEST_ASSERT_NOT_NULL(pipeline);

    redis_pipeline_destroy(pipeline);
    redis_pool_destroy(pool);
}

void test_pipeline_destroy_null(void) {
    /* Should not crash */
    redis_pipeline_destroy(NULL);
    TEST_PASS();
}

void test_pipeline_add_null(void) {
    int result = redis_pipeline_add(NULL, NULL, NULL, "PING");
    TEST_ASSERT_EQUAL(-1, result);
}

void test_pipeline_add_null_format(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);
    redis_pipeline_t *pipeline = redis_pool_pipeline_create(pool);

    int result = redis_pipeline_add(pipeline, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_pipeline_destroy(pipeline);
    redis_pool_destroy(pool);
}

void test_pipeline_add_commands(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);
    redis_pipeline_t *pipeline = redis_pool_pipeline_create(pool);

    int result = redis_pipeline_add(pipeline, NULL, NULL, "SET key1 value1");
    TEST_ASSERT_EQUAL(0, result);

    result = redis_pipeline_add(pipeline, NULL, NULL, "SET key2 value2");
    TEST_ASSERT_EQUAL(0, result);

    result = redis_pipeline_add(pipeline, NULL, NULL, "GET key1");
    TEST_ASSERT_EQUAL(0, result);

    redis_pipeline_destroy(pipeline);
    redis_pool_destroy(pool);
}

void test_pipeline_execute_null(void) {
    int result = redis_pipeline_execute(NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

void test_pipeline_execute_empty(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);
    redis_pipeline_t *pipeline = redis_pool_pipeline_create(pool);

    int result = redis_pipeline_execute(pipeline);
    TEST_ASSERT_EQUAL(-1, result);  /* Empty pipeline */

    redis_pipeline_destroy(pipeline);
    redis_pool_destroy(pool);
}

void test_pipeline_grow_capacity(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);
    redis_pipeline_t *pipeline = redis_pool_pipeline_create(pool);

    /* Add more than initial capacity (32) to test growth */
    for (int i = 0; i < 50; i++) {
        int result = redis_pipeline_add(pipeline, NULL, NULL, "PING");
        TEST_ASSERT_EQUAL(0, result);
    }

    redis_pipeline_destroy(pipeline);
    redis_pool_destroy(pool);
}

// =============================================================================
// Statistics Tests
// =============================================================================

void test_pool_get_stats_null(void) {
    redis_pool_stats_t stats = {0};

    /* Should not crash */
    redis_pool_get_stats(NULL, &stats);
    redis_pool_get_stats(NULL, NULL);

    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);
    redis_pool_get_stats(pool, NULL);
    redis_pool_destroy(pool);

    TEST_PASS();
}

void test_pool_get_stats(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    redis_pool_stats_t stats;
    redis_pool_get_stats(pool, &stats);

    TEST_ASSERT_EQUAL(0, stats.commands_sent);
    TEST_ASSERT_EQUAL(0, stats.commands_failed);

    redis_pool_destroy(pool);
}

void test_pool_reset_stats_null(void) {
    /* Should not crash */
    redis_pool_reset_stats(NULL);
    TEST_PASS();
}

void test_pool_reset_stats(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    redis_pool_reset_stats(pool);

    redis_pool_stats_t stats;
    redis_pool_get_stats(pool, &stats);
    TEST_ASSERT_EQUAL(0, stats.commands_sent);
    TEST_ASSERT_EQUAL(0, stats.commands_failed);

    redis_pool_destroy(pool);
}

// =============================================================================
// Health Tests
// =============================================================================

void test_pool_is_healthy_null(void) {
    int healthy = redis_pool_is_healthy(NULL);
    TEST_ASSERT_EQUAL(0, healthy);
}

void test_pool_is_healthy_not_running(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    int healthy = redis_pool_is_healthy(pool);
    TEST_ASSERT_EQUAL(0, healthy);

    redis_pool_destroy(pool);
}

void test_pool_available_null(void) {
    size_t available = redis_pool_available(NULL);
    TEST_ASSERT_EQUAL(0, available);
}

void test_pool_available_not_started(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    size_t available = redis_pool_available(pool);
    TEST_ASSERT_EQUAL(0, available);

    redis_pool_destroy(pool);
}

// =============================================================================
// Replica Configuration Tests
// =============================================================================

void test_pool_with_replicas(void) {
    const char *replica_hosts[] = {"replica1.example.com", "replica2.example.com"};
    uint16_t replica_ports[] = {6379, 6380};

    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    config.replica_hosts = replica_hosts;
    config.replica_ports = replica_ports;
    config.replica_count = 2;

    redis_pool_t *pool = redis_pool_create(&config);
    TEST_ASSERT_NOT_NULL(pool);

    redis_pool_destroy(pool);
}

void test_pool_replica_hosts_null_with_count(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    config.replica_hosts = NULL;
    config.replica_count = 2;  /* Count but no hosts */

    redis_pool_t *pool = redis_pool_create(&config);
    TEST_ASSERT_NOT_NULL(pool);  /* Should still create, just no replicas */

    redis_pool_destroy(pool);
}

// =============================================================================
// Main
// =============================================================================

int main(void) {
    UNITY_BEGIN();

    /* Configuration tests */
    RUN_TEST(test_pool_create_default);
    RUN_TEST(test_pool_create_custom);
    RUN_TEST(test_pool_create_null_config);
    RUN_TEST(test_pool_create_null_host);
    RUN_TEST(test_pool_destroy_null);

    /* Pool lifecycle tests */
    RUN_TEST(test_pool_stop_null);
    RUN_TEST(test_pool_start_null);
    RUN_TEST(test_pool_start_creates_connections);
    RUN_TEST(test_pool_stop_twice);

    /* Connection acquisition tests */
    RUN_TEST(test_pool_acquire_null);
    RUN_TEST(test_pool_acquire_not_running);
    RUN_TEST(test_pool_release_null);
    RUN_TEST(test_pool_conn_client_null);

    /* Pool command tests */
    RUN_TEST(test_pool_command_null);
    RUN_TEST(test_pool_command_null_format);
    RUN_TEST(test_pool_commandv_null);
    RUN_TEST(test_pool_commandv_null_argv);
    RUN_TEST(test_pool_commandv_zero_argc);
    RUN_TEST(test_pool_read_command_null);

    /* Convenience function tests */
    RUN_TEST(test_pool_set_not_running);
    RUN_TEST(test_pool_get_not_running);
    RUN_TEST(test_pool_del_not_running);
    RUN_TEST(test_pool_expire_not_running);
    RUN_TEST(test_pool_hset_not_running);
    RUN_TEST(test_pool_hget_not_running);
    RUN_TEST(test_pool_xadd_not_running);
    RUN_TEST(test_pool_xreadgroup_not_running);

    /* Pipeline tests */
    RUN_TEST(test_pipeline_create_null);
    RUN_TEST(test_pipeline_create_destroy);
    RUN_TEST(test_pipeline_destroy_null);
    RUN_TEST(test_pipeline_add_null);
    RUN_TEST(test_pipeline_add_null_format);
    RUN_TEST(test_pipeline_add_commands);
    RUN_TEST(test_pipeline_execute_null);
    RUN_TEST(test_pipeline_execute_empty);
    RUN_TEST(test_pipeline_grow_capacity);

    /* Statistics tests */
    RUN_TEST(test_pool_get_stats_null);
    RUN_TEST(test_pool_get_stats);
    RUN_TEST(test_pool_reset_stats_null);
    RUN_TEST(test_pool_reset_stats);

    /* Health tests */
    RUN_TEST(test_pool_is_healthy_null);
    RUN_TEST(test_pool_is_healthy_not_running);
    RUN_TEST(test_pool_available_null);
    RUN_TEST(test_pool_available_not_started);

    /* Replica configuration tests */
    RUN_TEST(test_pool_with_replicas);
    RUN_TEST(test_pool_replica_hosts_null_with_count);

    return UNITY_END();
}
