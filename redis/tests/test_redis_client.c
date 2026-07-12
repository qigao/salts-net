/**
 * @file test_redis_client.c
 * @brief Unit tests for Redis Client (RESP Protocol)
 */

#include "../redis_client.h"
#include "tinytest.h"
#include "turbo_error.h"
#include <string.h>
#include <stdio.h>

#define TEST_PASS() do { check(1); return; } while (0)
#define TEST_ASSERT_TRUE(expr) check((expr))
#define TEST_ASSERT_NULL(value) check_null((value))
#define TEST_ASSERT_NOT_NULL(value) check_not_null((value))
#define TEST_ASSERT_EQUAL(expected, actual) check((intptr_t)(actual) == (intptr_t)(expected))
#define TEST_ASSERT_EQUAL_STRING(expected, actual) check_str_eq((actual), (expected))
#define TEST_ASSERT_GREATER_THAN(threshold, actual) check((long long)(actual) > (long long)(threshold))
#define TEST_ASSERT_LESS_THAN(limit, actual) check((long long)(actual) < (long long)(limit))
#define REDIS_RUN_TEST(fn, label) it(label) { fn(); }

void setUp(void) {
}

void tearDown(void) {
}

static void on_connect_status(redis_client_t *client, int status, void *user_data) {
    int *captured = (int *)user_data;
    (void)client;
    if (captured) {
        *captured = status;
    }
}

// =============================================================================
// Configuration Tests
// =============================================================================

void test_create_client_default(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(6379, client->config.port);
    TEST_ASSERT_EQUAL_STRING("127.0.0.1", client->config.host);
    redis_client_destroy(client);
}

void test_create_client_with_config(void) {
    redis_config_t config = {
        .host = "redis.example.com",
        .port = 6380,
        .password = "secret123",
        .database = 5,
        .timeout_ms = 10000,
        .command_timeout_ms = 8000,
        .max_pipeline = 50
    };

    redis_client_t *client = redis_client_create_with_config(&config);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL_STRING("redis.example.com", client->config.host);
    TEST_ASSERT_EQUAL(6380, client->config.port);
    TEST_ASSERT_EQUAL_STRING("secret123", client->config.password);
    TEST_ASSERT_EQUAL(5, client->config.database);
    TEST_ASSERT_EQUAL(10000, client->config.timeout_ms);
    TEST_ASSERT_EQUAL(8000, client->config.command_timeout_ms);
    TEST_ASSERT_EQUAL(50, client->config.max_pipeline);
    redis_client_destroy(client);
}

void test_create_client_null_host(void) {
    redis_config_t config = {
        .host = NULL,
        .port = 6379
    };

    redis_client_t *client = redis_client_create_with_config(&config);
    TEST_ASSERT_NOT_NULL(client);
    /* NULL host should be handled gracefully */
    redis_client_destroy(client);
}

void test_destroy_null_client(void) {
    /* Should not crash */
    redis_client_destroy(NULL);
    TEST_PASS();
}

void test_disconnect_null_client(void) {
    /* Should not crash */
    redis_client_disconnect(NULL);
    TEST_PASS();
}

void test_interrupt_without_connection(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(TURBO_EINVAL, redis_client_interrupt(NULL, TURBO_ESHUTDOWN));
    TEST_ASSERT_EQUAL(TURBO_ENOTCONN, redis_client_interrupt(client, TURBO_ESHUTDOWN));
    redis_client_destroy(client);
}

// =============================================================================
// Initial State Tests
// =============================================================================

void test_client_initial_state(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    TEST_ASSERT_NOT_NULL(client);

    TEST_ASSERT_EQUAL(0, client->is_connected);
    TEST_ASSERT_EQUAL(0, client->is_authenticated);
    TEST_ASSERT_EQUAL(0, client->queued_commands);
    TEST_ASSERT_NULL(client->command_queue);
    TEST_ASSERT_NULL(client->command_queue_tail);
    TEST_ASSERT_EQUAL(0, client->is_subscriber);

    redis_client_destroy(client);
}

void test_client_recv_buffer_allocated(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_NOT_NULL(client->recv_buffer);
    TEST_ASSERT_GREATER_THAN(0, client->recv_buffer_size);
    TEST_ASSERT_EQUAL(0, client->recv_buffer_used);
    redis_client_destroy(client);
}

void test_client_connect_without_coro_context_fails(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    int status = 123;
    int result;

    TEST_ASSERT_NOT_NULL(client);
    result = redis_client_connect(client, on_connect_status, &status);

    TEST_ASSERT_EQUAL(-1, result);
    TEST_ASSERT_EQUAL(-1, status);
    TEST_ASSERT_EQUAL(0, client->is_connected);

    redis_client_destroy(client);
}

// =============================================================================
// Reply Structure Tests
// =============================================================================

void test_reply_free_null(void) {
    /* Should not crash */
    redis_reply_free(NULL);
    TEST_PASS();
}

void test_reply_type_values(void) {
    TEST_ASSERT_EQUAL(0, REDIS_REPLY_STRING);
    TEST_ASSERT_EQUAL(1, REDIS_REPLY_ERROR);
    TEST_ASSERT_EQUAL(2, REDIS_REPLY_INTEGER);
    TEST_ASSERT_EQUAL(3, REDIS_REPLY_BULK_STRING);
    TEST_ASSERT_EQUAL(4, REDIS_REPLY_ARRAY);
    TEST_ASSERT_EQUAL(5, REDIS_REPLY_NULL);
}

// =============================================================================
// Stream Structure Tests
// =============================================================================

void test_stream_entry_free_null(void) {
    /* Should not crash */
    redis_stream_entry_free(NULL);
    TEST_PASS();
}

void test_stream_result_free_null(void) {
    /* Should not crash */
    redis_stream_result_free(NULL, 0);
    redis_stream_result_free(NULL, 5);
    TEST_PASS();
}

// =============================================================================
// Command API Tests (without connection)
// =============================================================================

void test_command_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    TEST_ASSERT_NOT_NULL(client);

    /* Commands should fail when not connected */
    int result = redis_set(client, "key", "value", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    result = redis_get(client, "key", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_destroy(client);
}

void test_commandv_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    TEST_ASSERT_NOT_NULL(client);

    const char *argv[] = {"SET", "key", "value"};
    int result = redis_commandv(client, 3, argv, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_destroy(client);
}

void test_commandv_null_client(void) {
    const char *argv[] = {"SET", "key", "value"};
    int result = redis_commandv(NULL, 3, argv, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

// =============================================================================
// Stream API Tests (without connection)
// =============================================================================

void test_xadd_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    TEST_ASSERT_NOT_NULL(client);

    const char *fields[] = {"field1"};
    const char *values[] = {"value1"};

    int result = redis_xadd(client, "stream", 1000,
                            1, fields, values, NULL,
                            NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_destroy(client);
}

void test_xadd_null_params(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);

    /* NULL key */
    int result = redis_xadd(client, NULL, 0, 1, NULL, NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    /* NULL fields */
    result = redis_xadd(client, "stream", 0, 1, NULL, NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    /* Zero field count */
    const char *fields[] = {"f"};
    const char *values[] = {"v"};
    result = redis_xadd(client, "stream", 0, 0, fields, values, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_destroy(client);
}

void test_xread_null_params(void) {
    int result = redis_xread(NULL, 10, 1000, 1, NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_t *client = redis_client_create("127.0.0.1", 6379);

    result = redis_xread(client, 10, 1000, 0, NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_destroy(client);
}

void test_xreadgroup_null_params(void) {
    int result = redis_xreadgroup(NULL, "group", "consumer",
                                   10, 1000, 1, NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_t *client = redis_client_create("127.0.0.1", 6379);

    result = redis_xreadgroup(client, NULL, "consumer",
                               10, 1000, 1, NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    result = redis_xreadgroup(client, "group", NULL,
                               10, 1000, 1, NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_destroy(client);
}

void test_xgroup_create_null_params(void) {
    int result = redis_xgroup_create(NULL, "stream", "group", "0", 1, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_t *client = redis_client_create("127.0.0.1", 6379);

    result = redis_xgroup_create(client, NULL, "group", "0", 1, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    result = redis_xgroup_create(client, "stream", NULL, "0", 1, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_destroy(client);
}

void test_xack_null_params(void) {
    int result = redis_xack(NULL, "stream", "group", 1, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_t *client = redis_client_create("127.0.0.1", 6379);

    result = redis_xack(client, NULL, "group", 1, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    result = redis_xack(client, "stream", NULL, 1, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    result = redis_xack(client, "stream", "group", 0, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_destroy(client);
}

void test_xdel_null_params(void) {
    int result = redis_xdel(NULL, "stream", 1, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_t *client = redis_client_create("127.0.0.1", 6379);

    result = redis_xdel(client, NULL, 1, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    result = redis_xdel(client, "stream", 0, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_destroy(client);
}

// =============================================================================
// Pub/Sub API Tests (without connection)
// =============================================================================

void test_publish_null_params(void) {
    int result = redis_publish(NULL, "channel", "msg", 3, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_t *client = redis_client_create("127.0.0.1", 6379);

    result = redis_publish(client, NULL, "msg", 3, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_destroy(client);
}

void test_subscribe_null_params(void) {
    int result = redis_subscribe(NULL, 1, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_t *client = redis_client_create("127.0.0.1", 6379);

    result = redis_subscribe(client, 0, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_destroy(client);
}

void test_psubscribe_null_params(void) {
    int result = redis_psubscribe(NULL, 1, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_t *client = redis_client_create("127.0.0.1", 6379);

    result = redis_psubscribe(client, 0, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_destroy(client);
}

void test_unsubscribe_null_client(void) {
    int result = redis_unsubscribe(NULL, 0, NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

void test_punsubscribe_null_client(void) {
    int result = redis_punsubscribe(NULL, 0, NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

// =============================================================================
// Convenience Command Tests (without connection)
// =============================================================================

void test_ping_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    int result = redis_ping(client, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_del_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    const char *keys[] = {"key1", "key2"};
    int result = redis_del(client, 2, keys, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_exists_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    int result = redis_exists(client, "key", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_expire_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    int result = redis_expire(client, "key", 3600, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_incr_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    int result = redis_incr(client, "counter", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_lpush_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    const char *values[] = {"a", "b"};
    int result = redis_lpush(client, "list", 2, values, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_rpush_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    const char *values[] = {"a", "b"};
    int result = redis_rpush(client, "list", 2, values, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_lpop_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    int result = redis_lpop(client, "list", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_rpop_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    int result = redis_rpop(client, "list", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_hset_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    int result = redis_hset(client, "hash", "field", "value", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_hget_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    int result = redis_hget(client, "hash", "field", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_sadd_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    const char *members[] = {"m1", "m2"};
    int result = redis_sadd(client, "set", 2, members, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_smembers_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    int result = redis_smembers(client, "set", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_xlen_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    int result = redis_xlen(client, "stream", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_xtrim_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    int result = redis_xtrim(client, "stream", 1000, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

// =============================================================================
// Error Message Tests
// =============================================================================

void test_get_error_null_client(void) {
    const char *err = redis_client_get_error(NULL);
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_EQUAL_STRING("Invalid client", err);
}

void test_get_error_valid_client(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    const char *err = redis_client_get_error(client);
    TEST_ASSERT_NOT_NULL(err);
    redis_client_destroy(client);
}

void test_ext_commands_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    TEST_ASSERT_NOT_NULL(client);

    TEST_ASSERT_EQUAL(-1, redis_mget(client, 1, (const char*[]){"k"}, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_setnx(client, "k", "v", NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_setex(client, "k", 10, "v", NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_incrby(client, "k", 5, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_lrange(client, "k", 0, -1, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_hgetall(client, "k", NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_sismember(client, "k", "m", NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_zcard(client, "k", NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_ttl(client, "k", NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_select(client, 1, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_multi(client, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_eval(client, "return nil", 0, NULL, 0, NULL, NULL, NULL));

    // HyperLogLog
    TEST_ASSERT_EQUAL(-1, redis_pfadd(client, "hll", 1, (const char*[]){"e"}, NULL, NULL));
    // Geo
    TEST_ASSERT_EQUAL(-1, redis_geodist(client, "geo", "m1", "m2", "km", NULL, NULL));
    // Bitmaps
    TEST_ASSERT_EQUAL(-1, redis_getbit(client, "bitmap", 10, NULL, NULL));
    // Client/Connection
    TEST_ASSERT_EQUAL(-1, redis_auth(client, "user", "pass", NULL, NULL));

    redis_client_destroy(client);
}

void test_ext_commands_null_params(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    TEST_ASSERT_NOT_NULL(client);

    TEST_ASSERT_EQUAL(-1, redis_mset(NULL, 1, NULL, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_mset(client, 0, NULL, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_mget(client, 0, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_lmove(client, NULL, NULL, NULL, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_hmset(client, "k", 0, NULL, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_zadd(client, "k", 0, NULL, NULL, NULL, NULL));

    TEST_ASSERT_EQUAL(-1, redis_xrange(client, NULL, NULL, NULL, 0, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_xrevrange(client, NULL, NULL, NULL, 0, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_xpending(client, NULL, NULL, NULL, NULL, 0, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_xclaim(client, NULL, NULL, NULL, 0, 0, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_xautoclaim(client, NULL, NULL, NULL, 0, NULL, 0, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_xgroup_setid(client, NULL, NULL, NULL, NULL, NULL));

    // HyperLogLog
    TEST_ASSERT_EQUAL(-1, redis_pfadd(client, NULL, 0, NULL, NULL, NULL));
    // Geo
    TEST_ASSERT_EQUAL(-1, redis_geoadd(client, "geo", 0, NULL, NULL, NULL, NULL, NULL));
    // Bitmaps
    TEST_ASSERT_EQUAL(-1, redis_bitop(client, NULL, NULL, 0, NULL, NULL, NULL));
    // Client/Connection
    TEST_ASSERT_EQUAL(-1, redis_auth(client, NULL, NULL, NULL, NULL));

    redis_client_destroy(client);
}


suite("redis_client") {
    before_each() {
        setUp();
    }

    after_each() {
        tearDown();
    }

    group("Configuration") {
        REDIS_RUN_TEST(test_create_client_default, "should create client with defaults");
        REDIS_RUN_TEST(test_create_client_with_config, "should create client with custom config");
        REDIS_RUN_TEST(test_create_client_null_host, "should handle null host");
        REDIS_RUN_TEST(test_destroy_null_client, "should destroy null client safely");
        REDIS_RUN_TEST(test_disconnect_null_client, "should disconnect null client safely");
        REDIS_RUN_TEST(test_interrupt_without_connection, "should report absent socket on interrupt");
    }

    group("Initial State") {
        REDIS_RUN_TEST(test_client_initial_state, "should start disconnected with empty compatibility queue state");
        REDIS_RUN_TEST(test_client_recv_buffer_allocated, "should allocate recv buffer");
        REDIS_RUN_TEST(test_client_connect_without_coro_context_fails, "should fail connect without coroutine context");
    }

    group("Reply Structure") {
        REDIS_RUN_TEST(test_reply_free_null, "should free null reply safely");
        REDIS_RUN_TEST(test_reply_type_values, "should expose stable reply type values");
    }

    group("Stream Structure") {
        REDIS_RUN_TEST(test_stream_entry_free_null, "should free null stream entry safely");
        REDIS_RUN_TEST(test_stream_result_free_null, "should free null stream result safely");
    }

    group("Command API") {
        REDIS_RUN_TEST(test_command_not_connected, "should reject commands when not connected");
        REDIS_RUN_TEST(test_commandv_not_connected, "should reject argv commands when not connected");
        REDIS_RUN_TEST(test_commandv_null_client, "should reject argv commands for null client");
    }

    group("Stream API") {
        REDIS_RUN_TEST(test_xadd_not_connected, "should reject xadd when not connected");
        REDIS_RUN_TEST(test_xadd_null_params, "should validate xadd parameters");
        REDIS_RUN_TEST(test_xread_null_params, "should validate xread parameters");
        REDIS_RUN_TEST(test_xreadgroup_null_params, "should validate xreadgroup parameters");
        REDIS_RUN_TEST(test_xgroup_create_null_params, "should validate xgroup create parameters");
        REDIS_RUN_TEST(test_xack_null_params, "should validate xack parameters");
        REDIS_RUN_TEST(test_xdel_null_params, "should validate xdel parameters");
    }

    group("PubSub API") {
        REDIS_RUN_TEST(test_publish_null_params, "should validate publish parameters");
        REDIS_RUN_TEST(test_subscribe_null_params, "should validate subscribe parameters");
        REDIS_RUN_TEST(test_psubscribe_null_params, "should validate psubscribe parameters");
        REDIS_RUN_TEST(test_unsubscribe_null_client, "should reject unsubscribe for null client");
        REDIS_RUN_TEST(test_punsubscribe_null_client, "should reject punsubscribe for null client");
    }

    group("Convenience Commands") {
        REDIS_RUN_TEST(test_ping_not_connected, "should reject ping when not connected");
        REDIS_RUN_TEST(test_del_not_connected, "should reject del when not connected");
        REDIS_RUN_TEST(test_exists_not_connected, "should reject exists when not connected");
        REDIS_RUN_TEST(test_expire_not_connected, "should reject expire when not connected");
        REDIS_RUN_TEST(test_incr_not_connected, "should reject incr when not connected");
        REDIS_RUN_TEST(test_lpush_not_connected, "should reject lpush when not connected");
        REDIS_RUN_TEST(test_rpush_not_connected, "should reject rpush when not connected");
        REDIS_RUN_TEST(test_lpop_not_connected, "should reject lpop when not connected");
        REDIS_RUN_TEST(test_rpop_not_connected, "should reject rpop when not connected");
        REDIS_RUN_TEST(test_hset_not_connected, "should reject hset when not connected");
        REDIS_RUN_TEST(test_hget_not_connected, "should reject hget when not connected");
        REDIS_RUN_TEST(test_sadd_not_connected, "should reject sadd when not connected");
        REDIS_RUN_TEST(test_smembers_not_connected, "should reject smembers when not connected");
        REDIS_RUN_TEST(test_xlen_not_connected, "should reject xlen when not connected");
        REDIS_RUN_TEST(test_xtrim_not_connected, "should reject xtrim when not connected");
    }

    group("Error Messages") {
        REDIS_RUN_TEST(test_get_error_null_client, "should return invalid client for null error query");
        REDIS_RUN_TEST(test_get_error_valid_client, "should return error string for valid client");
    }

    group("Extended Commands") {
        REDIS_RUN_TEST(test_ext_commands_not_connected, "should reject extended commands when not connected");
        REDIS_RUN_TEST(test_ext_commands_null_params, "should validate extended command parameters");
    }
}
