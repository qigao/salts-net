/**
 * @file test_redis_cluster.c
 * @brief Unit tests for Redis Cluster Client
 */

#include "../redis_cluster.h"
#include "unity.h"
#include <string.h>
#include <stdio.h>

void setUp(void) {
}

void tearDown(void) {
}

// =============================================================================
// Hash Slot Tests
// =============================================================================

void test_keyslot_simple(void) {
    /* Known hash slot values */
    uint16_t slot = redis_cluster_keyslot("foo", 3);
    TEST_ASSERT_LESS_THAN(REDIS_CLUSTER_SLOTS, slot);
}

void test_keyslot_consistency(void) {
    /* Same key should always return same slot */
    uint16_t slot1 = redis_cluster_keyslot("mykey", 5);
    uint16_t slot2 = redis_cluster_keyslot("mykey", 5);
    TEST_ASSERT_EQUAL(slot1, slot2);
}

void test_keyslot_hash_tag(void) {
    /* Keys with same hash tag go to same slot */
    uint16_t slot1 = redis_cluster_keyslot("user:{123}:profile", 18);
    uint16_t slot2 = redis_cluster_keyslot("user:{123}:settings", 19);
    uint16_t slot3 = redis_cluster_keyslot("user:{123}:orders", 17);

    TEST_ASSERT_EQUAL(slot1, slot2);
    TEST_ASSERT_EQUAL(slot2, slot3);
}

void test_keyslot_different_tags(void) {
    /* Different hash tags should (likely) go to different slots */
    uint16_t slot1 = redis_cluster_keyslot("user:{100}:data", 15);
    uint16_t slot2 = redis_cluster_keyslot("user:{200}:data", 15);

    /* Not guaranteed different, but very likely */
    /* Just verify they're valid slots */
    TEST_ASSERT_LESS_THAN(REDIS_CLUSTER_SLOTS, slot1);
    TEST_ASSERT_LESS_THAN(REDIS_CLUSTER_SLOTS, slot2);
}

void test_keyslot_empty_tag(void) {
    /* Empty hash tag {} should use full key */
    uint16_t slot1 = redis_cluster_keyslot("foo{}bar", 8);
    uint16_t slot2 = redis_cluster_keyslot("foo{}bar", 8);
    TEST_ASSERT_EQUAL(slot1, slot2);
}

void test_keyslot_no_closing_brace(void) {
    /* No closing brace - use full key */
    uint16_t slot1 = redis_cluster_keyslot("foo{bar", 7);
    uint16_t slot2 = redis_cluster_keyslot("foo{bar", 7);
    TEST_ASSERT_EQUAL(slot1, slot2);
}

void test_keyslot_empty_key(void) {
    uint16_t slot = redis_cluster_keyslot("", 0);
    TEST_ASSERT_LESS_THAN(REDIS_CLUSTER_SLOTS, slot);
}

// =============================================================================
// Configuration Tests
// =============================================================================

void test_cluster_create_null_config(void) {
    redis_cluster_t *cluster = redis_cluster_create(NULL);
    TEST_ASSERT_NULL(cluster);
}

void test_cluster_create_no_seeds(void) {
    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    redis_cluster_t *cluster = redis_cluster_create(&config);
    TEST_ASSERT_NULL(cluster);
}

void test_cluster_create_with_seeds(void) {
    const char *hosts[] = {"127.0.0.1", "127.0.0.2"};
    uint16_t ports[] = {7000, 7001};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 2;

    redis_cluster_t *cluster = redis_cluster_create(&config);
    TEST_ASSERT_NOT_NULL(cluster);

    redis_cluster_destroy(cluster);
}

void test_cluster_create_custom_config(void) {
    const char *hosts[] = {"redis1.example.com"};
    uint16_t ports[] = {6379};

    redis_cluster_config_t config = {
        .seed_hosts = hosts,
        .seed_ports = ports,
        .seed_count = 1,
        .password = "secret123",
        .connections_per_node = 10,
        .connect_timeout_ms = 10000,
        .command_timeout_ms = 8000,
        .topology_refresh_ms = 60000,
        .max_redirections = 10,
        .route_reads_to_replicas = 1
    };

    redis_cluster_t *cluster = redis_cluster_create(&config);
    TEST_ASSERT_NOT_NULL(cluster);

    redis_cluster_destroy(cluster);
}

void test_cluster_destroy_null(void) {
    /* Should not crash */
    redis_cluster_destroy(NULL);
    TEST_PASS();
}

// =============================================================================
// Lifecycle Tests
// =============================================================================

void test_cluster_connect_null(void) {
    int result = redis_cluster_connect(NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

void test_cluster_disconnect_null(void) {
    /* Should not crash */
    redis_cluster_disconnect(NULL);
    TEST_PASS();
}

void test_cluster_refresh_null(void) {
    int result = redis_cluster_refresh(NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

void test_cluster_connect_disconnect(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;

    redis_cluster_t *cluster = redis_cluster_create(&config);
    TEST_ASSERT_NOT_NULL(cluster);

    /* Connect may fail without actual Redis, but shouldn't crash */
    redis_cluster_connect(cluster);
    redis_cluster_disconnect(cluster);
    redis_cluster_disconnect(cluster);  /* Double disconnect */

    redis_cluster_destroy(cluster);
}

// =============================================================================
// Command Tests (without connection)
// =============================================================================

void test_cluster_command_null(void) {
    int result = redis_cluster_command(NULL, NULL, NULL, "PING");
    TEST_ASSERT_EQUAL(-1, result);
}

void test_cluster_command_null_format(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;

    redis_cluster_t *cluster = redis_cluster_create(&config);

    int result = redis_cluster_command(cluster, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_cluster_destroy(cluster);
}

void test_cluster_command_key_null(void) {
    int result = redis_cluster_command_key(NULL, "key", NULL, NULL, "GET key");
    TEST_ASSERT_EQUAL(-1, result);
}

void test_cluster_commandv_null(void) {
    const char *argv[] = {"SET", "key", "value"};
    int result = redis_cluster_commandv(NULL, 3, argv, NULL, -1, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

void test_cluster_commandv_null_argv(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;

    redis_cluster_t *cluster = redis_cluster_create(&config);

    int result = redis_cluster_commandv(cluster, 3, NULL, NULL, -1, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_cluster_destroy(cluster);
}

// =============================================================================
// Convenience Function Tests
// =============================================================================

void test_cluster_set_not_connected(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;

    redis_cluster_t *cluster = redis_cluster_create(&config);

    int result = redis_cluster_set(cluster, "key", "value", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_cluster_destroy(cluster);
}

void test_cluster_get_not_connected(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;

    redis_cluster_t *cluster = redis_cluster_create(&config);

    int result = redis_cluster_get(cluster, "key", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_cluster_destroy(cluster);
}

void test_cluster_del_not_connected(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;

    redis_cluster_t *cluster = redis_cluster_create(&config);

    int result = redis_cluster_del(cluster, "key", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_cluster_destroy(cluster);
}

void test_cluster_hset_not_connected(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;

    redis_cluster_t *cluster = redis_cluster_create(&config);

    int result = redis_cluster_hset(cluster, "hash", "field", "value", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_cluster_destroy(cluster);
}

void test_cluster_xadd_null(void) {
    const char *fields[] = {"f1"};
    const char *values[] = {"v1"};

    int result = redis_cluster_xadd(NULL, "stream", 1000, 1, fields, values, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

void test_cluster_xread_null(void) {
    int result = redis_cluster_xread(NULL, "stream", 10, 1000, "0", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

// =============================================================================
// Multi-Key Tests
// =============================================================================

void test_cluster_mdelete_null(void) {
    const char *keys[] = {"key1", "key2"};
    int result = redis_cluster_mdelete(NULL, 2, keys, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

void test_cluster_mdelete_no_keys(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;

    redis_cluster_t *cluster = redis_cluster_create(&config);

    int result = redis_cluster_mdelete(cluster, 0, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_cluster_destroy(cluster);
}

void test_cluster_mget_null(void) {
    const char *keys[] = {"key1", "key2"};
    int result = redis_cluster_mget(NULL, 2, keys, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

void test_cluster_mget_hash_tags(void) {
    /* Keys with same hash tag should be in same slot */
    const char *keys[] = {"{user:1}:name", "{user:1}:email", "{user:1}:age"};

    uint16_t slot0 = redis_cluster_keyslot(keys[0], strlen(keys[0]));
    uint16_t slot1 = redis_cluster_keyslot(keys[1], strlen(keys[1]));
    uint16_t slot2 = redis_cluster_keyslot(keys[2], strlen(keys[2]));

    TEST_ASSERT_EQUAL(slot0, slot1);
    TEST_ASSERT_EQUAL(slot1, slot2);
}

// =============================================================================
// Statistics Tests
// =============================================================================

void test_cluster_get_stats_null(void) {
    redis_cluster_stats_t stats = {0};

    /* Should not crash */
    redis_cluster_get_stats(NULL, &stats);
    redis_cluster_get_stats(NULL, NULL);

    TEST_PASS();
}

void test_cluster_get_stats(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;

    redis_cluster_t *cluster = redis_cluster_create(&config);

    redis_cluster_stats_t stats;
    redis_cluster_get_stats(cluster, &stats);

    TEST_ASSERT_EQUAL(0, stats.commands_sent);
    TEST_ASSERT_EQUAL(0, stats.commands_failed);
    TEST_ASSERT_EQUAL(0, stats.redirections);

    redis_cluster_destroy(cluster);
}

void test_cluster_reset_stats_null(void) {
    /* Should not crash */
    redis_cluster_reset_stats(NULL);
    TEST_PASS();
}

void test_cluster_reset_stats(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;

    redis_cluster_t *cluster = redis_cluster_create(&config);

    redis_cluster_reset_stats(cluster);

    redis_cluster_stats_t stats;
    redis_cluster_get_stats(cluster, &stats);
    TEST_ASSERT_EQUAL(0, stats.commands_sent);

    redis_cluster_destroy(cluster);
}

// =============================================================================
// Health Tests
// =============================================================================

void test_cluster_is_healthy_null(void) {
    int healthy = redis_cluster_is_healthy(NULL);
    TEST_ASSERT_EQUAL(0, healthy);
}

void test_cluster_is_healthy_not_connected(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;

    redis_cluster_t *cluster = redis_cluster_create(&config);

    int healthy = redis_cluster_is_healthy(cluster);
    TEST_ASSERT_EQUAL(0, healthy);

    redis_cluster_destroy(cluster);
}

void test_cluster_node_count_null(void) {
    size_t masters = 99, replicas = 99;
    redis_cluster_node_count(NULL, &masters, &replicas);
    TEST_ASSERT_EQUAL(0, masters);
    TEST_ASSERT_EQUAL(0, replicas);
}

void test_cluster_node_count(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;

    redis_cluster_t *cluster = redis_cluster_create(&config);

    size_t masters, replicas;
    redis_cluster_node_count(cluster, &masters, &replicas);

    /* Before connect, no nodes discovered */
    TEST_ASSERT_EQUAL(0, masters);
    TEST_ASSERT_EQUAL(0, replicas);

    redis_cluster_destroy(cluster);
}

void test_cluster_get_node_null(void) {
    const redis_cluster_node_t *node = redis_cluster_get_node(NULL, 0);
    TEST_ASSERT_NULL(node);
}

void test_cluster_get_node_invalid_slot(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;

    redis_cluster_t *cluster = redis_cluster_create(&config);

    const redis_cluster_node_t *node = redis_cluster_get_node(cluster, REDIS_CLUSTER_SLOTS + 1);
    TEST_ASSERT_NULL(node);

    redis_cluster_destroy(cluster);
}

// =============================================================================
// Main
// =============================================================================

int main(void) {
    UNITY_BEGIN();

    /* Hash slot tests */
    RUN_TEST(test_keyslot_simple);
    RUN_TEST(test_keyslot_consistency);
    RUN_TEST(test_keyslot_hash_tag);
    RUN_TEST(test_keyslot_different_tags);
    RUN_TEST(test_keyslot_empty_tag);
    RUN_TEST(test_keyslot_no_closing_brace);
    RUN_TEST(test_keyslot_empty_key);

    /* Configuration tests */
    RUN_TEST(test_cluster_create_null_config);
    RUN_TEST(test_cluster_create_no_seeds);
    RUN_TEST(test_cluster_create_with_seeds);
    RUN_TEST(test_cluster_create_custom_config);
    RUN_TEST(test_cluster_destroy_null);

    /* Lifecycle tests */
    RUN_TEST(test_cluster_connect_null);
    RUN_TEST(test_cluster_disconnect_null);
    RUN_TEST(test_cluster_refresh_null);
    RUN_TEST(test_cluster_connect_disconnect);

    /* Command tests */
    RUN_TEST(test_cluster_command_null);
    RUN_TEST(test_cluster_command_null_format);
    RUN_TEST(test_cluster_command_key_null);
    RUN_TEST(test_cluster_commandv_null);
    RUN_TEST(test_cluster_commandv_null_argv);

    /* Convenience function tests */
    RUN_TEST(test_cluster_set_not_connected);
    RUN_TEST(test_cluster_get_not_connected);
    RUN_TEST(test_cluster_del_not_connected);
    RUN_TEST(test_cluster_hset_not_connected);
    RUN_TEST(test_cluster_xadd_null);
    RUN_TEST(test_cluster_xread_null);

    /* Multi-key tests */
    RUN_TEST(test_cluster_mdelete_null);
    RUN_TEST(test_cluster_mdelete_no_keys);
    RUN_TEST(test_cluster_mget_null);
    RUN_TEST(test_cluster_mget_hash_tags);

    /* Statistics tests */
    RUN_TEST(test_cluster_get_stats_null);
    RUN_TEST(test_cluster_get_stats);
    RUN_TEST(test_cluster_reset_stats_null);
    RUN_TEST(test_cluster_reset_stats);

    /* Health tests */
    RUN_TEST(test_cluster_is_healthy_null);
    RUN_TEST(test_cluster_is_healthy_not_connected);
    RUN_TEST(test_cluster_node_count_null);
    RUN_TEST(test_cluster_node_count);
    RUN_TEST(test_cluster_get_node_null);
    RUN_TEST(test_cluster_get_node_invalid_slot);

    return UNITY_END();
}
