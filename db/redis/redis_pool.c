/**
 * @file redis_pool.c
 * @brief Redis Connection Pool Implementation
 */

#include "redis_pool.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#ifdef _WIN32
#define strdup _strdup
#endif

/* =============================================================================
 * Internal Structures
 * =============================================================================
 */

typedef enum {
    CONN_STATE_IDLE = 0,
    CONN_STATE_ACTIVE,
    CONN_STATE_CONNECTING,
    CONN_STATE_UNHEALTHY
} conn_state_t;

struct redis_pool_conn_s {
    redis_client_t *client;
    conn_state_t state;
    int is_replica;
    uint64_t last_used;
    uint64_t created_at;
    redis_pool_conn_t *next;
};

struct redis_pool_s {
    /* Configuration */
    redis_pool_config_t config;

    /* Master connections */
    redis_pool_conn_t *master_idle;
    redis_pool_conn_t *master_active;
    size_t master_count;

    /* Replica connections */
    redis_pool_conn_t *replica_idle;
    redis_pool_conn_t *replica_active;
    size_t replica_count;
    size_t next_replica;  /* Round-robin index */

    /* State */
    int running;

    /* Statistics */
    redis_pool_stats_t stats;
};

struct redis_pipeline_s {
    redis_pool_t *pool;
    redis_pool_conn_t *conn;

    /* Command queue */
    struct {
        char *command;
        size_t command_len;
        redis_command_cb_t callback;
        void *user_data;
    } *commands;
    size_t command_count;
    size_t command_capacity;
};

/* =============================================================================
 * Helper Functions
 * =============================================================================
 */

static uint64_t get_time_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static redis_pool_conn_t *create_connection(const char *host, uint16_t port,
                                             const char *password, int database,
                                             int is_replica) {
    redis_pool_conn_t *conn = calloc(1, sizeof(redis_pool_conn_t));
    if (!conn) return NULL;

    redis_config_t config = {
        .host = host,
        .port = port,
        .password = password,
        .database = database,
        .timeout_ms = 5000,
        .command_timeout_ms = 5000,
        .max_pipeline = 100
    };

    conn->client = redis_client_create_with_config(&config);
    if (!conn->client) {
        free(conn);
        return NULL;
    }

    conn->state = CONN_STATE_CONNECTING;
    conn->is_replica = is_replica;
    conn->created_at = get_time_ms();
    conn->last_used = conn->created_at;

    return conn;
}

static void destroy_connection(redis_pool_conn_t *conn) {
    if (!conn) return;
    if (conn->client) {
        redis_client_destroy(conn->client);
    }
    free(conn);
}

static void on_pool_connected(redis_client_t *client, int status, void *user_data) {
    redis_pool_conn_t *conn = (redis_pool_conn_t *)user_data;
    (void)client;

    if (status == 0) {
        conn->state = CONN_STATE_IDLE;
    } else {
        conn->state = CONN_STATE_UNHEALTHY;
    }
}

/* Add connection to idle list */
static void add_to_idle(redis_pool_conn_t **list, redis_pool_conn_t *conn) {
    conn->next = *list;
    *list = conn;
}

/* Remove connection from list */
static redis_pool_conn_t *remove_from_list(redis_pool_conn_t **list,
                                            redis_pool_conn_t *conn) {
    redis_pool_conn_t **pp = list;
    while (*pp) {
        if (*pp == conn) {
            *pp = conn->next;
            conn->next = NULL;
            return conn;
        }
        pp = &(*pp)->next;
    }
    return NULL;
}

/* Get first idle connection from list */
static redis_pool_conn_t *pop_idle(redis_pool_conn_t **list) {
    if (!*list) return NULL;

    redis_pool_conn_t *conn = *list;
    *list = conn->next;
    conn->next = NULL;
    return conn;
}

/* Count connections in list */
static size_t count_list(redis_pool_conn_t *list) {
    size_t count = 0;
    while (list) {
        count++;
        list = list->next;
    }
    return count;
}

/* =============================================================================
 * Pool Lifecycle
 * =============================================================================
 */

redis_pool_t *redis_pool_create(const redis_pool_config_t *config) {
    if (!config || !config->master_host) return NULL;

    redis_pool_t *pool = calloc(1, sizeof(redis_pool_t));
    if (!pool) return NULL;

    /* Copy configuration */
    pool->config = *config;
    pool->config.master_host = strdup(config->master_host);
    if (config->password) {
        pool->config.password = strdup(config->password);
    }

    /* Copy replica hosts */
    if (config->replica_count > 0 && config->replica_hosts) {
        pool->config.replica_hosts = malloc(config->replica_count * sizeof(char *));
        pool->config.replica_ports = malloc(config->replica_count * sizeof(uint16_t));

        for (size_t i = 0; i < config->replica_count; i++) {
            pool->config.replica_hosts[i] = strdup(config->replica_hosts[i]);
            pool->config.replica_ports[i] = config->replica_ports[i];
        }
    }

    /* Set defaults */
    if (pool->config.min_connections == 0) pool->config.min_connections = 2;
    if (pool->config.max_connections == 0) pool->config.max_connections = 10;
    if (pool->config.connect_timeout_ms == 0) pool->config.connect_timeout_ms = 5000;
    if (pool->config.idle_timeout_ms == 0) pool->config.idle_timeout_ms = 60000;
    if (pool->config.health_check_ms == 0) pool->config.health_check_ms = 30000;
    if (pool->config.pipeline_max == 0) pool->config.pipeline_max = 100;

    return pool;
}

int redis_pool_start(redis_pool_t *pool) {
    if (!pool) return -1;

    pool->running = 1;

    /* Create minimum master connections */
    for (size_t i = 0; i < pool->config.min_connections; i++) {
        redis_pool_conn_t *conn = create_connection(
            pool->config.master_host,
            pool->config.master_port,
            pool->config.password,
            pool->config.database,
            0  /* is_replica = false */
        );

        if (conn) {
            redis_client_connect(conn->client, on_pool_connected, conn);
            add_to_idle(&pool->master_idle, conn);
            pool->master_count++;
            pool->stats.connections_created++;
        }
    }

    /* Create replica connections if configured */
    if (pool->config.replica_count > 0) {
        for (size_t i = 0; i < pool->config.replica_count; i++) {
            redis_pool_conn_t *conn = create_connection(
                pool->config.replica_hosts[i],
                pool->config.replica_ports[i],
                pool->config.password,
                pool->config.database,
                1  /* is_replica = true */
            );

            if (conn) {
                redis_client_connect(conn->client, on_pool_connected, conn);
                add_to_idle(&pool->replica_idle, conn);
                pool->replica_count++;
                pool->stats.connections_created++;
            }
        }
    }

    pool->stats.total_connections = pool->master_count + pool->replica_count;

    return pool->master_count > 0 ? 0 : -1;
}

void redis_pool_stop(redis_pool_t *pool) {
    if (!pool) return;

    pool->running = 0;

    /* Close all master connections */
    redis_pool_conn_t *conn = pool->master_idle;
    while (conn) {
        redis_pool_conn_t *next = conn->next;
        destroy_connection(conn);
        pool->stats.connections_closed++;
        conn = next;
    }
    pool->master_idle = NULL;

    conn = pool->master_active;
    while (conn) {
        redis_pool_conn_t *next = conn->next;
        destroy_connection(conn);
        pool->stats.connections_closed++;
        conn = next;
    }
    pool->master_active = NULL;

    /* Close all replica connections */
    conn = pool->replica_idle;
    while (conn) {
        redis_pool_conn_t *next = conn->next;
        destroy_connection(conn);
        pool->stats.connections_closed++;
        conn = next;
    }
    pool->replica_idle = NULL;

    conn = pool->replica_active;
    while (conn) {
        redis_pool_conn_t *next = conn->next;
        destroy_connection(conn);
        pool->stats.connections_closed++;
        conn = next;
    }
    pool->replica_active = NULL;

    pool->master_count = 0;
    pool->replica_count = 0;
}

void redis_pool_destroy(redis_pool_t *pool) {
    if (!pool) return;

    redis_pool_stop(pool);

    /* Free configuration strings */
    free((char *)pool->config.master_host);
    free((char *)pool->config.password);

    if (pool->config.replica_hosts) {
        for (size_t i = 0; i < pool->config.replica_count; i++) {
            free((char *)pool->config.replica_hosts[i]);
        }
        free(pool->config.replica_hosts);
        free(pool->config.replica_ports);
    }

    free(pool);
}

/* =============================================================================
 * Connection Acquisition
 * =============================================================================
 */

redis_pool_conn_t *redis_pool_acquire(redis_pool_t *pool, int read_only) {
    if (!pool || !pool->running) return NULL;

    redis_pool_conn_t *conn = NULL;

    /* Try replica for read-only requests */
    if (read_only && pool->replica_idle) {
        conn = pop_idle(&pool->replica_idle);
        if (conn) {
            conn->state = CONN_STATE_ACTIVE;
            add_to_idle(&pool->replica_active, conn);
            conn->last_used = get_time_ms();
            pool->stats.active_connections++;
            return conn;
        }
    }

    /* Try master connection */
    conn = pop_idle(&pool->master_idle);
    if (conn) {
        conn->state = CONN_STATE_ACTIVE;
        add_to_idle(&pool->master_active, conn);
        conn->last_used = get_time_ms();
        pool->stats.active_connections++;
        return conn;
    }

    /* Create new connection if under limit */
    if (pool->master_count < pool->config.max_connections) {
        conn = create_connection(
            pool->config.master_host,
            pool->config.master_port,
            pool->config.password,
            pool->config.database,
            0
        );

        if (conn) {
            /* Synchronous connect for simplicity */
            redis_client_connect(conn->client, on_pool_connected, conn);
            conn->state = CONN_STATE_ACTIVE;
            add_to_idle(&pool->master_active, conn);
            pool->master_count++;
            pool->stats.connections_created++;
            pool->stats.total_connections++;
            pool->stats.active_connections++;
            return conn;
        }
    }

    /* Pool exhausted */
    pool->stats.waiting_requests++;
    return NULL;
}

void redis_pool_release(redis_pool_t *pool, redis_pool_conn_t *conn) {
    if (!pool || !conn) return;

    conn->state = CONN_STATE_IDLE;
    conn->last_used = get_time_ms();

    if (conn->is_replica) {
        remove_from_list(&pool->replica_active, conn);
        add_to_idle(&pool->replica_idle, conn);
    } else {
        remove_from_list(&pool->master_active, conn);
        add_to_idle(&pool->master_idle, conn);
    }

    if (pool->stats.active_connections > 0) {
        pool->stats.active_connections--;
    }
    pool->stats.idle_connections = count_list(pool->master_idle) +
                                    count_list(pool->replica_idle);
}

redis_client_t *redis_pool_conn_client(redis_pool_conn_t *conn) {
    return conn ? conn->client : NULL;
}

/* =============================================================================
 * Simple Command API
 * =============================================================================
 */

typedef struct {
    redis_pool_t *pool;
    redis_pool_conn_t *conn;
    redis_command_cb_t user_callback;
    void *user_data;
} pool_cmd_ctx_t;

static void on_pool_command_done(redis_client_t *client, redis_reply_t *reply,
                                  void *user_data) {
    pool_cmd_ctx_t *ctx = (pool_cmd_ctx_t *)user_data;
    (void)client;

    /* Call user callback */
    if (ctx->user_callback) {
        ctx->user_callback(client, reply, ctx->user_data);
    }

    /* Release connection back to pool */
    redis_pool_release(ctx->pool, ctx->conn);

    ctx->pool->stats.commands_sent++;
    free(ctx);
}

int redis_pool_command(redis_pool_t *pool, redis_command_cb_t callback,
                       void *user_data, const char *format, ...) {
    if (!pool || !format) return -1;

    redis_pool_conn_t *conn = redis_pool_acquire(pool, 0);
    if (!conn) return -1;

    pool_cmd_ctx_t *ctx = malloc(sizeof(pool_cmd_ctx_t));
    if (!ctx) {
        redis_pool_release(pool, conn);
        return -1;
    }

    ctx->pool = pool;
    ctx->conn = conn;
    ctx->user_callback = callback;
    ctx->user_data = user_data;

    va_list ap;
    va_start(ap, format);

    /* Parse format and build command */
    char cmd_buf[1024];
    vsnprintf(cmd_buf, sizeof(cmd_buf), format, ap);
    va_end(ap);

    int result = redis_command(conn->client, on_pool_command_done, ctx, "%s", cmd_buf);
    if (result != 0) {
        redis_pool_release(pool, conn);
        free(ctx);
        pool->stats.commands_failed++;
        return -1;
    }

    return 0;
}

int redis_pool_commandv(redis_pool_t *pool, int argc, const char **argv,
                        const size_t *argvlen, redis_command_cb_t callback,
                        void *user_data) {
    if (!pool || !argv || argc <= 0) return -1;

    redis_pool_conn_t *conn = redis_pool_acquire(pool, 0);
    if (!conn) return -1;

    pool_cmd_ctx_t *ctx = malloc(sizeof(pool_cmd_ctx_t));
    if (!ctx) {
        redis_pool_release(pool, conn);
        return -1;
    }

    ctx->pool = pool;
    ctx->conn = conn;
    ctx->user_callback = callback;
    ctx->user_data = user_data;

    int result = redis_commandv(conn->client, argc, argv, argvlen,
                                 on_pool_command_done, ctx);
    if (result != 0) {
        redis_pool_release(pool, conn);
        free(ctx);
        pool->stats.commands_failed++;
        return -1;
    }

    return 0;
}

int redis_pool_read_command(redis_pool_t *pool, redis_command_cb_t callback,
                            void *user_data, const char *format, ...) {
    if (!pool || !format) return -1;

    /* Try to get a replica connection for read operations */
    redis_pool_conn_t *conn = redis_pool_acquire(pool, 1);
    if (!conn) return -1;

    pool_cmd_ctx_t *ctx = malloc(sizeof(pool_cmd_ctx_t));
    if (!ctx) {
        redis_pool_release(pool, conn);
        return -1;
    }

    ctx->pool = pool;
    ctx->conn = conn;
    ctx->user_callback = callback;
    ctx->user_data = user_data;

    va_list ap;
    va_start(ap, format);
    char cmd_buf[1024];
    vsnprintf(cmd_buf, sizeof(cmd_buf), format, ap);
    va_end(ap);

    int result = redis_command(conn->client, on_pool_command_done, ctx, "%s", cmd_buf);
    if (result != 0) {
        redis_pool_release(pool, conn);
        free(ctx);
        pool->stats.commands_failed++;
        return -1;
    }

    return 0;
}

/* =============================================================================
 * Convenience Functions
 * =============================================================================
 */

int redis_pool_set(redis_pool_t *pool, const char *key, const char *value,
                   redis_command_cb_t callback, void *user_data) {
    redis_pool_conn_t *conn = redis_pool_acquire(pool, 0);
    if (!conn) return -1;

    pool_cmd_ctx_t *ctx = malloc(sizeof(pool_cmd_ctx_t));
    if (!ctx) {
        redis_pool_release(pool, conn);
        return -1;
    }

    ctx->pool = pool;
    ctx->conn = conn;
    ctx->user_callback = callback;
    ctx->user_data = user_data;

    int result = redis_set(conn->client, key, value, on_pool_command_done, ctx);
    if (result != 0) {
        redis_pool_release(pool, conn);
        free(ctx);
        return -1;
    }
    return 0;
}

int redis_pool_get(redis_pool_t *pool, const char *key,
                   redis_command_cb_t callback, void *user_data) {
    /* GET is read-only, use replica if available */
    redis_pool_conn_t *conn = redis_pool_acquire(pool, 1);
    if (!conn) return -1;

    pool_cmd_ctx_t *ctx = malloc(sizeof(pool_cmd_ctx_t));
    if (!ctx) {
        redis_pool_release(pool, conn);
        return -1;
    }

    ctx->pool = pool;
    ctx->conn = conn;
    ctx->user_callback = callback;
    ctx->user_data = user_data;

    int result = redis_get(conn->client, key, on_pool_command_done, ctx);
    if (result != 0) {
        redis_pool_release(pool, conn);
        free(ctx);
        return -1;
    }
    return 0;
}

int redis_pool_del(redis_pool_t *pool, int key_count, const char **keys,
                   redis_command_cb_t callback, void *user_data) {
    redis_pool_conn_t *conn = redis_pool_acquire(pool, 0);
    if (!conn) return -1;

    pool_cmd_ctx_t *ctx = malloc(sizeof(pool_cmd_ctx_t));
    if (!ctx) {
        redis_pool_release(pool, conn);
        return -1;
    }

    ctx->pool = pool;
    ctx->conn = conn;
    ctx->user_callback = callback;
    ctx->user_data = user_data;

    int result = redis_del(conn->client, key_count, keys, on_pool_command_done, ctx);
    if (result != 0) {
        redis_pool_release(pool, conn);
        free(ctx);
        return -1;
    }
    return 0;
}

int redis_pool_expire(redis_pool_t *pool, const char *key, int seconds,
                      redis_command_cb_t callback, void *user_data) {
    redis_pool_conn_t *conn = redis_pool_acquire(pool, 0);
    if (!conn) return -1;

    pool_cmd_ctx_t *ctx = malloc(sizeof(pool_cmd_ctx_t));
    if (!ctx) {
        redis_pool_release(pool, conn);
        return -1;
    }

    ctx->pool = pool;
    ctx->conn = conn;
    ctx->user_callback = callback;
    ctx->user_data = user_data;

    int result = redis_expire(conn->client, key, seconds, on_pool_command_done, ctx);
    if (result != 0) {
        redis_pool_release(pool, conn);
        free(ctx);
        return -1;
    }
    return 0;
}

int redis_pool_hset(redis_pool_t *pool, const char *key, const char *field,
                    const char *value, redis_command_cb_t callback, void *user_data) {
    redis_pool_conn_t *conn = redis_pool_acquire(pool, 0);
    if (!conn) return -1;

    pool_cmd_ctx_t *ctx = malloc(sizeof(pool_cmd_ctx_t));
    if (!ctx) {
        redis_pool_release(pool, conn);
        return -1;
    }

    ctx->pool = pool;
    ctx->conn = conn;
    ctx->user_callback = callback;
    ctx->user_data = user_data;

    int result = redis_hset(conn->client, key, field, value, on_pool_command_done, ctx);
    if (result != 0) {
        redis_pool_release(pool, conn);
        free(ctx);
        return -1;
    }
    return 0;
}

int redis_pool_hget(redis_pool_t *pool, const char *key, const char *field,
                    redis_command_cb_t callback, void *user_data) {
    /* HGET is read-only */
    redis_pool_conn_t *conn = redis_pool_acquire(pool, 1);
    if (!conn) return -1;

    pool_cmd_ctx_t *ctx = malloc(sizeof(pool_cmd_ctx_t));
    if (!ctx) {
        redis_pool_release(pool, conn);
        return -1;
    }

    ctx->pool = pool;
    ctx->conn = conn;
    ctx->user_callback = callback;
    ctx->user_data = user_data;

    int result = redis_hget(conn->client, key, field, on_pool_command_done, ctx);
    if (result != 0) {
        redis_pool_release(pool, conn);
        free(ctx);
        return -1;
    }
    return 0;
}

int redis_pool_xadd(redis_pool_t *pool, const char *key, size_t maxlen,
                    size_t field_count, const char **fields,
                    const char **values, const size_t *value_lens,
                    redis_command_cb_t callback, void *user_data) {
    redis_pool_conn_t *conn = redis_pool_acquire(pool, 0);
    if (!conn) return -1;

    pool_cmd_ctx_t *ctx = malloc(sizeof(pool_cmd_ctx_t));
    if (!ctx) {
        redis_pool_release(pool, conn);
        return -1;
    }

    ctx->pool = pool;
    ctx->conn = conn;
    ctx->user_callback = callback;
    ctx->user_data = user_data;

    int result = redis_xadd(conn->client, key, maxlen, field_count,
                            fields, values, value_lens,
                            on_pool_command_done, ctx);
    if (result != 0) {
        redis_pool_release(pool, conn);
        free(ctx);
        return -1;
    }
    return 0;
}

/* Stream callback wrapper */
typedef struct {
    redis_pool_t *pool;
    redis_pool_conn_t *conn;
    redis_stream_cb_t user_callback;
    void *user_data;
} pool_stream_ctx_t;

static void on_pool_stream_done(redis_client_t *client,
                                 redis_stream_result_t *results,
                                 size_t result_count, void *user_data) {
    pool_stream_ctx_t *ctx = (pool_stream_ctx_t *)user_data;
    (void)client;

    if (ctx->user_callback) {
        ctx->user_callback(client, results, result_count, ctx->user_data);
    }

    redis_pool_release(ctx->pool, ctx->conn);
    ctx->pool->stats.commands_sent++;
    free(ctx);
}

int redis_pool_xreadgroup(redis_pool_t *pool, const char *group,
                          const char *consumer, size_t count, int block_ms,
                          size_t stream_count, const char **keys, const char **ids,
                          redis_stream_cb_t callback, void *user_data) {
    redis_pool_conn_t *conn = redis_pool_acquire(pool, 0);
    if (!conn) return -1;

    pool_stream_ctx_t *ctx = malloc(sizeof(pool_stream_ctx_t));
    if (!ctx) {
        redis_pool_release(pool, conn);
        return -1;
    }

    ctx->pool = pool;
    ctx->conn = conn;
    ctx->user_callback = callback;
    ctx->user_data = user_data;

    int result = redis_xreadgroup(conn->client, group, consumer,
                                   count, block_ms, stream_count, keys, ids,
                                   on_pool_stream_done, ctx);
    if (result != 0) {
        redis_pool_release(pool, conn);
        free(ctx);
        return -1;
    }
    return 0;
}

/* =============================================================================
 * Pipeline API
 * =============================================================================
 */

redis_pipeline_t *redis_pool_pipeline_create(redis_pool_t *pool) {
    if (!pool) return NULL;

    redis_pipeline_t *pipeline = calloc(1, sizeof(redis_pipeline_t));
    if (!pipeline) return NULL;

    pipeline->pool = pool;
    pipeline->command_capacity = 32;
    pipeline->commands = calloc(pipeline->command_capacity, sizeof(*pipeline->commands));

    if (!pipeline->commands) {
        free(pipeline);
        return NULL;
    }

    return pipeline;
}

int redis_pipeline_add(redis_pipeline_t *pipeline, redis_command_cb_t callback,
                       void *user_data, const char *format, ...) {
    if (!pipeline || !format) return -1;

    /* Grow array if needed */
    if (pipeline->command_count >= pipeline->command_capacity) {
        size_t new_capacity = pipeline->command_capacity * 2;
        void *new_commands = realloc(pipeline->commands,
                                      new_capacity * sizeof(*pipeline->commands));
        if (!new_commands) return -1;
        pipeline->commands = new_commands;
        pipeline->command_capacity = new_capacity;
    }

    /* Format command */
    va_list ap;
    va_start(ap, format);
    char cmd_buf[1024];
    int len = vsnprintf(cmd_buf, sizeof(cmd_buf), format, ap);
    va_end(ap);

    if (len < 0) return -1;

    /* Store command */
    size_t idx = pipeline->command_count;
    pipeline->commands[idx].command = strdup(cmd_buf);
    pipeline->commands[idx].command_len = len;
    pipeline->commands[idx].callback = callback;
    pipeline->commands[idx].user_data = user_data;
    pipeline->command_count++;

    return 0;
}

int redis_pipeline_execute(redis_pipeline_t *pipeline) {
    if (!pipeline || pipeline->command_count == 0) return -1;

    /* Acquire connection */
    pipeline->conn = redis_pool_acquire(pipeline->pool, 0);
    if (!pipeline->conn) return -1;

    redis_client_t *client = pipeline->conn->client;

    /* Send all commands */
    for (size_t i = 0; i < pipeline->command_count; i++) {
        redis_command(client, pipeline->commands[i].callback,
                      pipeline->commands[i].user_data,
                      "%s", pipeline->commands[i].command);
    }

    pipeline->pool->stats.commands_sent += pipeline->command_count;

    /* Release connection */
    redis_pool_release(pipeline->pool, pipeline->conn);
    pipeline->conn = NULL;

    return 0;
}

void redis_pipeline_destroy(redis_pipeline_t *pipeline) {
    if (!pipeline) return;

    for (size_t i = 0; i < pipeline->command_count; i++) {
        free(pipeline->commands[i].command);
    }
    free(pipeline->commands);
    free(pipeline);
}

/* =============================================================================
 * Statistics & Health
 * =============================================================================
 */

void redis_pool_get_stats(redis_pool_t *pool, redis_pool_stats_t *stats) {
    if (!pool || !stats) return;

    *stats = pool->stats;
    stats->idle_connections = count_list(pool->master_idle) +
                              count_list(pool->replica_idle);
    stats->active_connections = count_list(pool->master_active) +
                                count_list(pool->replica_active);
    stats->total_connections = pool->master_count + pool->replica_count;
}

void redis_pool_reset_stats(redis_pool_t *pool) {
    if (!pool) return;

    pool->stats.commands_sent = 0;
    pool->stats.commands_failed = 0;
    pool->stats.waiting_requests = 0;
    pool->stats.health_checks = 0;
    pool->stats.health_failures = 0;
}

int redis_pool_is_healthy(redis_pool_t *pool) {
    if (!pool || !pool->running) return 0;

    /* At least one master connection available */
    return pool->master_idle != NULL || pool->master_active != NULL;
}

size_t redis_pool_available(redis_pool_t *pool) {
    if (!pool) return 0;
    return count_list(pool->master_idle) + count_list(pool->replica_idle);
}
