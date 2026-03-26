/**
 * @file redis_cluster.c
 * @brief Redis Cluster Client Implementation
 */

#include "redis_cluster.h"
#include "redis_pool.h"
#include "turbo_str.h"
#include <fmt.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#ifdef _WIN32
#define strtok_r strtok_s
#endif

/* =============================================================================
 * CRC16 Implementation (XMODEM)
 * =============================================================================
 */

static const uint16_t crc16_table[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
    0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52b5, 0x4294, 0x72f7, 0x62d6,
    0x9339, 0x8318, 0xb37b, 0xa35a, 0xd3bd, 0xc39c, 0xf3ff, 0xe3de,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64e6, 0x74c7, 0x44a4, 0x5485,
    0xa56a, 0xb54b, 0x8528, 0x9509, 0xe5ee, 0xf5cf, 0xc5ac, 0xd58d,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76d7, 0x66f6, 0x5695, 0x46b4,
    0xb75b, 0xa77a, 0x9719, 0x8738, 0xf7df, 0xe7fe, 0xd79d, 0xc7bc,
    0x48c4, 0x58e5, 0x6886, 0x78a7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xc9cc, 0xd9ed, 0xe98e, 0xf9af, 0x8948, 0x9969, 0xa90a, 0xb92b,
    0x5af5, 0x4ad4, 0x7ab7, 0x6a96, 0x1a71, 0x0a50, 0x3a33, 0x2a12,
    0xdbfd, 0xcbdc, 0xfbbf, 0xeb9e, 0x9b79, 0x8b58, 0xbb3b, 0xab1a,
    0x6ca6, 0x7c87, 0x4ce4, 0x5cc5, 0x2c22, 0x3c03, 0x0c60, 0x1c41,
    0xedae, 0xfd8f, 0xcdec, 0xddcd, 0xad2a, 0xbd0b, 0x8d68, 0x9d49,
    0x7e97, 0x6eb6, 0x5ed5, 0x4ef4, 0x3e13, 0x2e32, 0x1e51, 0x0e70,
    0xff9f, 0xefbe, 0xdfdd, 0xcffc, 0xbf1b, 0xaf3a, 0x9f59, 0x8f78,
    0x9188, 0x81a9, 0xb1ca, 0xa1eb, 0xd10c, 0xc12d, 0xf14e, 0xe16f,
    0x1080, 0x00a1, 0x30c2, 0x20e3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83b9, 0x9398, 0xa3fb, 0xb3da, 0xc33d, 0xd31c, 0xe37f, 0xf35e,
    0x02b1, 0x1290, 0x22f3, 0x32d2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xb5ea, 0xa5cb, 0x95a8, 0x8589, 0xf56e, 0xe54f, 0xd52c, 0xc50d,
    0x34e2, 0x24c3, 0x14a0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xa7db, 0xb7fa, 0x8799, 0x97b8, 0xe75f, 0xf77e, 0xc71d, 0xd73c,
    0x26d3, 0x36f2, 0x0691, 0x16b0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xd94c, 0xc96d, 0xf90e, 0xe92f, 0x99c8, 0x89e9, 0xb98a, 0xa9ab,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18c0, 0x08e1, 0x3882, 0x28a3,
    0xcb7d, 0xdb5c, 0xeb3f, 0xfb1e, 0x8bf9, 0x9bd8, 0xabbb, 0xbb9a,
    0x4a75, 0x5a54, 0x6a37, 0x7a16, 0x0af1, 0x1ad0, 0x2ab3, 0x3a92,
    0xfd2e, 0xed0f, 0xdd6c, 0xcd4d, 0xbdaa, 0xad8b, 0x9de8, 0x8dc9,
    0x7c26, 0x6c07, 0x5c64, 0x4c45, 0x3ca2, 0x2c83, 0x1ce0, 0x0cc1,
    0xef1f, 0xff3e, 0xcf5d, 0xdf7c, 0xaf9b, 0xbfba, 0x8fd9, 0x9ff8,
    0x6e17, 0x7e36, 0x4e55, 0x5e74, 0x2e93, 0x3eb2, 0x0ed1, 0x1ef0
};

static uint16_t crc16(const char *buf, size_t len) {
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc = (crc << 8) ^ crc16_table[((crc >> 8) ^ (uint8_t)buf[i]) & 0xff];
    }
    return crc;
}

/* =============================================================================
 * Internal Structures
 * =============================================================================
 */

typedef struct cluster_node_s {
    redis_cluster_node_t info;
    redis_pool_t *pool;
    struct cluster_node_s *next;
} cluster_node_t;

struct redis_cluster_s {
    redis_cluster_config_t config;

    /* Node management */
    cluster_node_t *nodes;
    size_t node_count;

    /* Slot mapping: slot -> node */
    cluster_node_t *slots[REDIS_CLUSTER_SLOTS];

    /* State */
    int connected;

    /* Statistics */
    redis_cluster_stats_t stats;
};

/* =============================================================================
 * Helper Functions
 * =============================================================================
 */

uint16_t redis_cluster_keyslot(const char *key, size_t len) {
    size_t s, e;

    /* Find hash tag: {tag} */
    for (s = 0; s < len; s++) {
        if (key[s] == '{') break;
    }

    if (s < len) {
        for (e = s + 1; e < len; e++) {
            if (key[e] == '}') break;
        }
        /* Use content between {} if non-empty */
        if (e < len && e > s + 1) {
            return crc16(key + s + 1, e - s - 1) & 0x3FFF;
        }
    }

    return crc16(key, len) & 0x3FFF;
}

static cluster_node_t *find_node(redis_cluster_t *cluster,
                                  const char *host, uint16_t port) {
    cluster_node_t *node = cluster->nodes;
    while (node) {
        if (node->info.port == port &&
            strcmp(node->info.host, host) == 0) {
            return node;
        }
        node = node->next;
    }
    return NULL;
}

static cluster_node_t *create_node(redis_cluster_t *cluster,
                                    const char *host, uint16_t port,
                                    const char *node_id, int is_master) {
    cluster_node_t *node = calloc(1, sizeof(cluster_node_t));
    if (!node) return NULL;

    node->info.host = tstr_dup(host);
    node->info.port = port;
    node->info.node_id = node_id ? tstr_dup(node_id) : NULL;
    node->info.is_master = is_master;
    node->info.slot_start = -1;
    node->info.slot_end = -1;

    /* Create connection pool for this node */
    redis_pool_config_t pool_config = {
        .master_host = host,
        .master_port = port,
        .password = cluster->config.password,
        .database = 0,
        .min_connections = 1,
        .max_connections = cluster->config.connections_per_node,
        .connect_timeout_ms = cluster->config.connect_timeout_ms,
        .idle_timeout_ms = 60000,
        .health_check_ms = 30000
    };

    node->pool = redis_pool_create(&pool_config);
    if (!node->pool) {
        tstr_free((tstr_t)node->info.host);
        tstr_free((tstr_t)node->info.node_id);
        free(node);
        return NULL;
    }

    return node;
}

static void destroy_node(cluster_node_t *node) {
    if (!node) return;
    if (node->pool) {
        redis_pool_destroy(node->pool);
    }
    tstr_free((tstr_t)node->info.host);
    tstr_free((tstr_t)node->info.node_id);
    free(node);
}

static void clear_nodes(redis_cluster_t *cluster) {
    cluster_node_t *node = cluster->nodes;
    while (node) {
        cluster_node_t *next = node->next;
        destroy_node(node);
        node = next;
    }
    cluster->nodes = NULL;
    cluster->node_count = 0;

    memset(cluster->slots, 0, sizeof(cluster->slots));
}

/* =============================================================================
 * Topology Discovery
 * =============================================================================
 */

typedef struct {
    redis_cluster_t *cluster;
    int success;
    int pending;
} topology_ctx_t;

static void parse_cluster_slots(redis_cluster_t *cluster, redis_reply_t *reply) {
    if (!reply || reply->type != REDIS_REPLY_ARRAY) return;

    /* CLUSTER SLOTS returns:
     * [[start, end, [master_ip, master_port, node_id], [replica...]], ...] */

    for (size_t i = 0; i < reply->element_count; i++) {
        redis_reply_t *slot_range = reply->elements[i];
        if (!slot_range || slot_range->type != REDIS_REPLY_ARRAY ||
            slot_range->element_count < 3) {
            continue;
        }

        /* Get slot range */
        int start = (int)slot_range->elements[0]->integer;
        int end = (int)slot_range->elements[1]->integer;

        /* Get master info */
        redis_reply_t *master_info = slot_range->elements[2];
        if (!master_info || master_info->type != REDIS_REPLY_ARRAY ||
            master_info->element_count < 2) {
            continue;
        }

        const char *host = master_info->elements[0]->str;
        int port = (int)master_info->elements[1]->integer;
        const char *node_id = (master_info->element_count > 2) ?
                               master_info->elements[2]->str : NULL;

        /* Find or create node */
        cluster_node_t *node = find_node(cluster, host, (uint16_t)port);
        if (!node) {
            node = create_node(cluster, host, (uint16_t)port, node_id, 1);
            if (node) {
                node->next = cluster->nodes;
                cluster->nodes = node;
                cluster->node_count++;
            }
        }

        if (node) {
            node->info.slot_start = start;
            node->info.slot_end = end;
            node->info.is_master = 1;

            /* Map slots to this node */
            for (int s = start; s <= end && s < REDIS_CLUSTER_SLOTS; s++) {
                cluster->slots[s] = node;
            }
        }

        /* Process replicas (elements 3+) */
        for (size_t r = 3; r < slot_range->element_count; r++) {
            redis_reply_t *replica_info = slot_range->elements[r];
            if (!replica_info || replica_info->type != REDIS_REPLY_ARRAY ||
                replica_info->element_count < 2) {
                continue;
            }

            const char *r_host = replica_info->elements[0]->str;
            int r_port = (int)replica_info->elements[1]->integer;
            const char *r_node_id = (replica_info->element_count > 2) ?
                                     replica_info->elements[2]->str : NULL;

            cluster_node_t *replica = find_node(cluster, r_host, (uint16_t)r_port);
            if (!replica) {
                replica = create_node(cluster, r_host, (uint16_t)r_port, r_node_id, 0);
                if (replica) {
                    replica->next = cluster->nodes;
                    cluster->nodes = replica;
                    cluster->node_count++;
                }
            }
        }
    }
}

static void on_cluster_slots_reply(redis_client_t *client, redis_reply_t *reply,
                                    void *user_data) {
    topology_ctx_t *ctx = (topology_ctx_t *)user_data;
    (void)client;

    if (reply && reply->type == REDIS_REPLY_ARRAY) {
        parse_cluster_slots(ctx->cluster, reply);
        ctx->success = 1;
    }

    ctx->pending--;
}

static int discover_topology(redis_cluster_t *cluster) {
    if (cluster->config.seed_count == 0) return -1;

    /* Try each seed node until one responds */
    for (size_t i = 0; i < cluster->config.seed_count; i++) {
        redis_client_t *client = redis_client_create(
            cluster->config.seed_hosts[i],
            cluster->config.seed_ports[i]
        );

        if (!client) continue;

        /* TODO: Implement synchronous connect and command for topology discovery */
        /* For now, create nodes from seeds */
        cluster_node_t *node = create_node(
            cluster,
            cluster->config.seed_hosts[i],
            cluster->config.seed_ports[i],
            NULL,
            1
        );

        if (node) {
            node->next = cluster->nodes;
            cluster->nodes = node;
            cluster->node_count++;

            /* Assign all slots to first seed initially */
            if (i == 0) {
                node->info.slot_start = 0;
                node->info.slot_end = REDIS_CLUSTER_SLOTS - 1;
                for (int s = 0; s < REDIS_CLUSTER_SLOTS; s++) {
                    cluster->slots[s] = node;
                }
            }
        }

        redis_client_destroy(client);
    }

    cluster->stats.topology_refreshes++;
    return cluster->node_count > 0 ? 0 : -1;
}

/* =============================================================================
 * Cluster Lifecycle
 * =============================================================================
 */

redis_cluster_t *redis_cluster_create(const redis_cluster_config_t *config) {
    if (!config || config->seed_count == 0 || !config->seed_hosts) {
        return NULL;
    }

    redis_cluster_t *cluster = calloc(1, sizeof(redis_cluster_t));
    if (!cluster) return NULL;

    /* Copy configuration */
    cluster->config = *config;

    /* Copy seed hosts */
    cluster->config.seed_hosts = malloc(config->seed_count * sizeof(char *));
    cluster->config.seed_ports = malloc(config->seed_count * sizeof(uint16_t));

    if (!cluster->config.seed_hosts || !cluster->config.seed_ports) {
        free(cluster->config.seed_hosts);
        free(cluster->config.seed_ports);
        free(cluster);
        return NULL;
    }

    for (size_t i = 0; i < config->seed_count; i++) {
        cluster->config.seed_hosts[i] = tstr_dup(config->seed_hosts[i]);
        cluster->config.seed_ports[i] = config->seed_ports[i];
    }

    if (config->password) {
        cluster->config.password = tstr_dup(config->password);
    }

    /* Set defaults */
    if (cluster->config.connections_per_node == 0) {
        cluster->config.connections_per_node = 5;
    }
    if (cluster->config.connect_timeout_ms == 0) {
        cluster->config.connect_timeout_ms = 5000;
    }
    if (cluster->config.command_timeout_ms == 0) {
        cluster->config.command_timeout_ms = 5000;
    }
    if (cluster->config.topology_refresh_ms == 0) {
        cluster->config.topology_refresh_ms = 30000;
    }
    if (cluster->config.max_redirections == 0) {
        cluster->config.max_redirections = 5;
    }

    return cluster;
}

int redis_cluster_connect(redis_cluster_t *cluster) {
    if (!cluster) return -1;

    /* Discover topology */
    if (discover_topology(cluster) != 0) {
        return -1;
    }

    /* Start connection pools for all nodes */
    cluster_node_t *node = cluster->nodes;
    int connected = 0;

    while (node) {
        if (redis_pool_start(node->pool) == 0) {
            connected++;
        }
        node = node->next;
    }

    cluster->connected = (connected > 0);
    cluster->stats.master_count = 0;
    cluster->stats.replica_count = 0;

    node = cluster->nodes;
    while (node) {
        if (node->info.is_master) {
            cluster->stats.master_count++;
        } else {
            cluster->stats.replica_count++;
        }
        node = node->next;
    }

    return cluster->connected ? 0 : -1;
}

void redis_cluster_disconnect(redis_cluster_t *cluster) {
    if (!cluster) return;

    cluster_node_t *node = cluster->nodes;
    while (node) {
        if (node->pool) {
            redis_pool_stop(node->pool);
        }
        node = node->next;
    }

    cluster->connected = 0;
}

void redis_cluster_destroy(redis_cluster_t *cluster) {
    if (!cluster) return;

    redis_cluster_disconnect(cluster);
    clear_nodes(cluster);

    /* Free configuration */
    for (size_t i = 0; i < cluster->config.seed_count; i++) {
        tstr_free((tstr_t)cluster->config.seed_hosts[i]);
    }
    free(cluster->config.seed_hosts);
    free(cluster->config.seed_ports);
    tstr_free((tstr_t)cluster->config.password);

    free(cluster);
}

int redis_cluster_refresh(redis_cluster_t *cluster) {
    if (!cluster) return -1;

    /* In production, this would send CLUSTER SLOTS to a connected node
     * and update the topology. For now, just increment counter. */
    cluster->stats.topology_refreshes++;
    return 0;
}

/* =============================================================================
 * Command Routing
 * =============================================================================
 */

const redis_cluster_node_t *redis_cluster_get_node(redis_cluster_t *cluster,
                                                    uint16_t slot) {
    if (!cluster || slot >= REDIS_CLUSTER_SLOTS) return NULL;
    cluster_node_t *node = cluster->slots[slot];
    return node ? &node->info : NULL;
}

static cluster_node_t *get_node_for_key(redis_cluster_t *cluster,
                                         const char *key) {
    if (!cluster || !key) return NULL;

    uint16_t slot = redis_cluster_keyslot(key, strlen(key));
    return cluster->slots[slot];
}

typedef struct {
    redis_cluster_t *cluster;
    redis_command_cb_t user_callback;
    void *user_data;
    char *key;
    int redirections;
} cluster_cmd_ctx_t;

static void on_cluster_command_done(redis_client_t *client, redis_reply_t *reply,
                                     void *user_data) {
    cluster_cmd_ctx_t *ctx = (cluster_cmd_ctx_t *)user_data;
    (void)client;

    /* Check for MOVED/ASK redirections */
    if (reply && reply->type == REDIS_REPLY_ERROR && reply->str) {
        if (strncmp(reply->str, "MOVED ", 6) == 0 ||
            strncmp(reply->str, "ASK ", 4) == 0) {

            ctx->cluster->stats.redirections++;

            if (ctx->redirections < ctx->cluster->config.max_redirections) {
                /* Parse redirection: "MOVED slot host:port" */
                /* TODO: Implement redirection following */
            }
        }
    }

    /* Call user callback */
    if (ctx->user_callback) {
        ctx->user_callback(client, reply, ctx->user_data);
    }

    ctx->cluster->stats.commands_sent++;
    tstr_free((tstr_t)ctx->key);
    free(ctx);
}

int redis_cluster_command_key(redis_cluster_t *cluster, const char *key,
                              redis_command_cb_t callback, void *user_data,
                              const char *format, ...) {
    if (!cluster || !cluster->connected || !key || !format) return -1;

    cluster_node_t *node = get_node_for_key(cluster, key);
    if (!node || !node->pool) {
        cluster->stats.commands_failed++;
        return -1;
    }

    cluster_cmd_ctx_t *ctx = malloc(sizeof(cluster_cmd_ctx_t));
    if (!ctx) return -1;

    ctx->cluster = cluster;
    ctx->user_callback = callback;
    ctx->user_data = user_data;
    ctx->key = tstr_dup(key);
    ctx->redirections = 0;

    va_list ap;
    va_start(ap, format);
    tstr_t cmd_buf = tstr_new();
    if (!cmd_buf) {
        va_end(ap);
        tstr_free((tstr_t)ctx->key);
        free(ctx);
        cluster->stats.commands_failed++;
        return -1;
    }
    cmd_buf = tstr_cat_vfmt(cmd_buf, format, ap);
    va_end(ap);
    if (!cmd_buf) {
        tstr_free((tstr_t)ctx->key);
        free(ctx);
        cluster->stats.commands_failed++;
        return -1;
    }

    int result = redis_pool_command(node->pool, on_cluster_command_done, ctx,
                                     "%s", cmd_buf);
    tstr_free(cmd_buf);
    if (result != 0) {
        tstr_free((tstr_t)ctx->key);
        free(ctx);
        cluster->stats.commands_failed++;
        return -1;
    }

    return 0;
}

int redis_cluster_command(redis_cluster_t *cluster, redis_command_cb_t callback,
                          void *user_data, const char *format, ...) {
    if (!cluster || !format) return -1;

    /* Extract first argument as key */
    va_list ap;
    va_start(ap, format);
    tstr_t cmd_buf = tstr_new();
    if (!cmd_buf) {
        va_end(ap);
        return -1;
    }
    cmd_buf = tstr_cat_vfmt(cmd_buf, format, ap);
    va_end(ap);
    if (!cmd_buf) {
        return -1;
    }

    /* Parse key from command (second word after command name) */
    char *saveptr;
    char *cmd_copy = tstr_dup(cmd_buf);
    char *token = strtok_r(cmd_copy, " ", &saveptr);  /* command */
    char *key = strtok_r(NULL, " ", &saveptr);        /* key */
    (void)token;

    if (!key) {
        tstr_free(cmd_buf);
        tstr_free((tstr_t)cmd_copy);
        return -1;
    }

    char *key_copy = tstr_dup(key);
    tstr_free((tstr_t)cmd_copy);

    int result = redis_cluster_command_key(cluster, key_copy, callback,
                                            user_data, "%s", cmd_buf);
    tstr_free((tstr_t)key_copy);
    tstr_free(cmd_buf);
    return result;
}

int redis_cluster_commandv(redis_cluster_t *cluster, int argc, const char **argv,
                           const size_t *argvlen, int key_index,
                           redis_command_cb_t callback, void *user_data) {
    if (!cluster || !cluster->connected || !argv || argc <= 0) return -1;

    /* Determine key index: default to 1 (first arg after command) */
    int kidx = (key_index >= 0) ? key_index : 1;
    if (kidx >= argc) return -1;

    const char *key = argv[kidx];
    cluster_node_t *node = get_node_for_key(cluster, key);
    if (!node || !node->pool) {
        cluster->stats.commands_failed++;
        return -1;
    }

    cluster_cmd_ctx_t *ctx = malloc(sizeof(cluster_cmd_ctx_t));
    if (!ctx) return -1;

    ctx->cluster = cluster;
    ctx->user_callback = callback;
    ctx->user_data = user_data;
    ctx->key = tstr_dup(key);
    ctx->redirections = 0;

    int result = redis_pool_commandv(node->pool, argc, argv, argvlen,
                                      on_cluster_command_done, ctx);
    if (result != 0) {
        tstr_free((tstr_t)ctx->key);
        free(ctx);
        cluster->stats.commands_failed++;
        return -1;
    }

    return 0;
}

/* =============================================================================
 * Convenience Functions
 * =============================================================================
 */

int redis_cluster_set(redis_cluster_t *cluster, const char *key, const char *value,
                      redis_command_cb_t callback, void *user_data) {
    return redis_cluster_command_key(cluster, key, callback, user_data,
                                      "SET %s %s", key, value);
}

int redis_cluster_get(redis_cluster_t *cluster, const char *key,
                      redis_command_cb_t callback, void *user_data) {
    return redis_cluster_command_key(cluster, key, callback, user_data,
                                      "GET %s", key);
}

int redis_cluster_del(redis_cluster_t *cluster, const char *key,
                      redis_command_cb_t callback, void *user_data) {
    return redis_cluster_command_key(cluster, key, callback, user_data,
                                      "DEL %s", key);
}

int redis_cluster_expire(redis_cluster_t *cluster, const char *key, int seconds,
                         redis_command_cb_t callback, void *user_data) {
    return redis_cluster_command_key(cluster, key, callback, user_data,
                                      "EXPIRE %s %d", key, seconds);
}

int redis_cluster_incr(redis_cluster_t *cluster, const char *key,
                       redis_command_cb_t callback, void *user_data) {
    return redis_cluster_command_key(cluster, key, callback, user_data,
                                      "INCR %s", key);
}

int redis_cluster_hset(redis_cluster_t *cluster, const char *key,
                       const char *field, const char *value,
                       redis_command_cb_t callback, void *user_data) {
    return redis_cluster_command_key(cluster, key, callback, user_data,
                                      "HSET %s %s %s", key, field, value);
}

int redis_cluster_hget(redis_cluster_t *cluster, const char *key,
                       const char *field, redis_command_cb_t callback,
                       void *user_data) {
    return redis_cluster_command_key(cluster, key, callback, user_data,
                                      "HGET %s %s", key, field);
}

int redis_cluster_hdel(redis_cluster_t *cluster, const char *key,
                       const char *field, redis_command_cb_t callback,
                       void *user_data) {
    return redis_cluster_command_key(cluster, key, callback, user_data,
                                      "HDEL %s %s", key, field);
}

int redis_cluster_xadd(redis_cluster_t *cluster, const char *key, size_t maxlen,
                       size_t field_count, const char **fields,
                       const char **values, const size_t *value_lens,
                       redis_command_cb_t callback, void *user_data) {
    if (!cluster || !key || !fields || !values || field_count == 0) return -1;

    cluster_node_t *node = get_node_for_key(cluster, key);
    if (!node || !node->pool) {
        cluster->stats.commands_failed++;
        return -1;
    }

    redis_pool_conn_t *conn = redis_pool_acquire(node->pool, 0);
    if (!conn) return -1;

    redis_client_t *client = redis_pool_conn_client(conn);
    int result = redis_xadd(client, key, maxlen, field_count, fields, values,
                            value_lens, callback, user_data);

    redis_pool_release(node->pool, conn);
    return result;
}

int redis_cluster_xread(redis_cluster_t *cluster, const char *key,
                        size_t count, int block_ms, const char *last_id,
                        redis_stream_cb_t callback, void *user_data) {
    if (!cluster || !key) return -1;

    cluster_node_t *node = get_node_for_key(cluster, key);
    if (!node || !node->pool) {
        cluster->stats.commands_failed++;
        return -1;
    }

    redis_pool_conn_t *conn = redis_pool_acquire(node->pool, 0);
    if (!conn) return -1;

    redis_client_t *client = redis_pool_conn_client(conn);
    const char *keys[] = {key};
    const char *ids[] = {last_id ? last_id : "$"};

    int result = redis_xread(client, count, block_ms, 1, keys, ids,
                              callback, user_data);

    redis_pool_release(node->pool, conn);
    return result;
}

/* =============================================================================
 * Multi-Key Operations
 * =============================================================================
 */

static int verify_same_slot(redis_cluster_t *cluster, int key_count,
                            const char **keys) {
    if (key_count <= 0 || !keys) return -1;

    uint16_t first_slot = redis_cluster_keyslot(keys[0], strlen(keys[0]));

    for (int i = 1; i < key_count; i++) {
        uint16_t slot = redis_cluster_keyslot(keys[i], strlen(keys[i]));
        if (slot != first_slot) {
            return -1;  /* Keys in different slots */
        }
    }

    return first_slot;
}

int redis_cluster_mdelete(redis_cluster_t *cluster, int key_count,
                          const char **keys, redis_command_cb_t callback,
                          void *user_data) {
    if (!cluster || !cluster->connected || key_count <= 0 || !keys) return -1;

    int slot = verify_same_slot(cluster, key_count, keys);
    if (slot < 0) {
        /* Keys span multiple slots - not allowed */
        return -1;
    }

    cluster_node_t *node = cluster->slots[slot];
    if (!node || !node->pool) return -1;

    /* Build DEL command */
    tstr_t cmd_buf = tstr_dup("DEL");
    if (!cmd_buf) return -1;
    for (int i = 0; i < key_count; i++) {
        cmd_buf = tstr_cat_fmt(cmd_buf, " %s", keys[i]);
        if (!cmd_buf) return -1;
    }
    int result = redis_pool_command(node->pool, callback, user_data, "%s", cmd_buf);
    tstr_free(cmd_buf);
    return result;
}

int redis_cluster_mget(redis_cluster_t *cluster, int key_count,
                       const char **keys, redis_command_cb_t callback,
                       void *user_data) {
    if (!cluster || !cluster->connected || key_count <= 0 || !keys) return -1;

    int slot = verify_same_slot(cluster, key_count, keys);
    if (slot < 0) {
        /* Keys span multiple slots - not allowed */
        return -1;
    }

    cluster_node_t *node = cluster->slots[slot];
    if (!node || !node->pool) return -1;

    /* Build MGET command */
    tstr_t cmd_buf = tstr_dup("MGET");
    if (!cmd_buf) return -1;
    for (int i = 0; i < key_count; i++) {
        cmd_buf = tstr_cat_fmt(cmd_buf, " %s", keys[i]);
        if (!cmd_buf) return -1;
    }
    int result = redis_pool_command(node->pool, callback, user_data, "%s", cmd_buf);
    tstr_free(cmd_buf);
    return result;
}

/* =============================================================================
 * Statistics & Health
 * =============================================================================
 */

void redis_cluster_get_stats(redis_cluster_t *cluster, redis_cluster_stats_t *stats) {
    if (!cluster || !stats) return;

    *stats = cluster->stats;

    /* Count total connections across all nodes */
    stats->total_connections = 0;
    cluster_node_t *node = cluster->nodes;
    while (node) {
        if (node->pool) {
            redis_pool_stats_t pool_stats;
            redis_pool_get_stats(node->pool, &pool_stats);
            stats->total_connections += pool_stats.total_connections;
        }
        node = node->next;
    }
}

void redis_cluster_reset_stats(redis_cluster_t *cluster) {
    if (!cluster) return;

    cluster->stats.commands_sent = 0;
    cluster->stats.commands_failed = 0;
    cluster->stats.redirections = 0;
}

int redis_cluster_is_healthy(redis_cluster_t *cluster) {
    if (!cluster || !cluster->connected) return 0;

    /* Check all slots are covered */
    for (int i = 0; i < REDIS_CLUSTER_SLOTS; i++) {
        if (!cluster->slots[i]) return 0;
    }

    return 1;
}

void redis_cluster_node_count(redis_cluster_t *cluster, size_t *masters,
                              size_t *replicas) {
    if (!cluster) {
        if (masters) *masters = 0;
        if (replicas) *replicas = 0;
        return;
    }

    if (masters) *masters = cluster->stats.master_count;
    if (replicas) *replicas = cluster->stats.replica_count;
}
