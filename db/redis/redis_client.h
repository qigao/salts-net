#ifndef REDIS_CLIENT_H
#define REDIS_CLIENT_H

#include "turbo_async_client.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct redis_client_s redis_client_t;
typedef struct redis_command_s redis_command_t;
typedef struct redis_reply_s redis_reply_t;
typedef struct redis_subscription_s redis_subscription_t;

/* Pub/Sub message callback (forward declaration) */
typedef void (*redis_pubsub_cb_t)(redis_client_t *client,
                                  const char *channel,
                                  const void *message, size_t len,
                                  void *user_data);

/* RESP reply types */
typedef enum {
    REDIS_REPLY_STRING,      /* Simple string: +OK\r\n */
    REDIS_REPLY_ERROR,       /* Error: -ERR message\r\n */
    REDIS_REPLY_INTEGER,     /* Integer: :1000\r\n */
    REDIS_REPLY_BULK_STRING, /* Bulk string: $6\r\nfoobar\r\n */
    REDIS_REPLY_ARRAY,       /* Array: *2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n */
    REDIS_REPLY_NULL         /* Null: $-1\r\n */
} redis_reply_type_t;

/* Redis reply structure */
struct redis_reply_s {
    redis_reply_type_t type;
    int64_t integer;           /* For REDIS_REPLY_INTEGER */
    char *str;                 /* For STRING, ERROR, BULK_STRING */
    size_t len;                /* Length of str */
    redis_reply_t **elements;  /* For REDIS_REPLY_ARRAY */
    size_t element_count;      /* Number of array elements */
};

/* Command callback */
typedef void (*redis_command_cb_t)(redis_client_t *client, redis_reply_t *reply, void *user_data);

/* Connection callback */
typedef void (*redis_connect_cb_t)(redis_client_t *client, int status, void *user_data);

/* Redis client configuration */
typedef struct {
    const char *host;
    uint16_t port;
    const char *password;      /* Optional auth password */
    int database;              /* Database number (0-15) */
    uint32_t timeout_ms;       /* Connection timeout */
    uint32_t command_timeout_ms; /* Command timeout */
    size_t max_pipeline;       /* Max pipelined commands */
} redis_config_t;

/* Redis client structure */
struct redis_client_s {
    async_client_t *client;
    redis_config_t config;

    /* Connection state */
    int is_connected;
    int is_authenticated;
    int selected_db;

    /* Command queue */
    redis_command_t *command_queue;
    redis_command_t *command_queue_tail;
    size_t queued_commands;

    /* Current parsing state */
    char *recv_buffer;
    size_t recv_buffer_size;
    size_t recv_buffer_used;

    /* Callbacks */
    redis_connect_cb_t connect_cb;
    void *connect_user_data;

    /* Pub/Sub state */
    int is_subscriber;              /* In subscriber mode */
    redis_subscription_t *subscriptions;  /* Active subscriptions */
    redis_pubsub_cb_t pubsub_cb;    /* Global pubsub callback */
    void *pubsub_user_data;

    /* User data */
    void *user_data;
};

/* API Functions */

/**
 * Create Redis client with default configuration
 * @param host Redis server host
 * @param port Redis server port
 * @return Redis client instance or NULL on error
 */
redis_client_t* redis_client_create(const char *host, uint16_t port);

/**
 * Create Redis client with custom configuration
 * @param config Redis configuration
 * @return Redis client instance or NULL on error
 */
redis_client_t* redis_client_create_with_config(const redis_config_t *config);

/**
 * Connect to Redis server
 * @param client Redis client
 * @param callback Connection callback
 * @param user_data User data for callback
 * @return 0 on success, negative on error
 */
int redis_client_connect(redis_client_t *client, redis_connect_cb_t callback, void *user_data);

/**
 * Execute Redis command
 * @param client Redis client
 * @param callback Command callback
 * @param user_data User data for callback
 * @param format Command format string (e.g., "SET %s %s")
 * @param ... Command arguments
 * @return 0 on success, negative on error
 */
int redis_command(redis_client_t *client, redis_command_cb_t callback, 
                  void *user_data, const char *format, ...);

/**
 * Execute Redis command with argv
 * @param client Redis client
 * @param argc Number of arguments
 * @param argv Array of argument strings
 * @param argvlen Array of argument lengths
 * @param callback Command callback
 * @param user_data User data for callback
 * @return 0 on success, negative on error
 */
int redis_commandv(redis_client_t *client, int argc, const char **argv, 
                   const size_t *argvlen, redis_command_cb_t callback, void *user_data);

/**
 * Disconnect from Redis server
 * @param client Redis client
 */
void redis_client_disconnect(redis_client_t *client);

/**
 * Destroy Redis client
 * @param client Redis client
 */
void redis_client_destroy(redis_client_t *client);

/**
 * Free Redis reply
 * @param reply Redis reply to free
 */
void redis_reply_free(redis_reply_t *reply);

/**
 * Get error message from client
 * @param client Redis client
 * @return Error message or NULL
 */
const char* redis_client_get_error(redis_client_t *client);

/* Convenience functions for common commands */

/**
 * SET key value
 */
int redis_set(redis_client_t *client, const char *key, const char *value,
              redis_command_cb_t callback, void *user_data);

/**
 * GET key
 */
int redis_get(redis_client_t *client, const char *key,
              redis_command_cb_t callback, void *user_data);

/**
 * DEL key [key ...]
 */
int redis_del(redis_client_t *client, int key_count, const char **keys,
              redis_command_cb_t callback, void *user_data);

/**
 * EXISTS key
 */
int redis_exists(redis_client_t *client, const char *key,
                 redis_command_cb_t callback, void *user_data);

/**
 * EXPIRE key seconds
 */
int redis_expire(redis_client_t *client, const char *key, int seconds,
                 redis_command_cb_t callback, void *user_data);

/**
 * INCR key
 */
int redis_incr(redis_client_t *client, const char *key,
               redis_command_cb_t callback, void *user_data);

/**
 * LPUSH key value [value ...]
 */
int redis_lpush(redis_client_t *client, const char *key, int value_count, const char **values,
                redis_command_cb_t callback, void *user_data);

/**
 * RPUSH key value [value ...]
 */
int redis_rpush(redis_client_t *client, const char *key, int value_count, const char **values,
                redis_command_cb_t callback, void *user_data);

/**
 * LPOP key
 */
int redis_lpop(redis_client_t *client, const char *key,
               redis_command_cb_t callback, void *user_data);

/**
 * RPOP key
 */
int redis_rpop(redis_client_t *client, const char *key,
               redis_command_cb_t callback, void *user_data);

/**
 * HSET key field value
 */
int redis_hset(redis_client_t *client, const char *key, const char *field, const char *value,
               redis_command_cb_t callback, void *user_data);

/**
 * HGET key field
 */
int redis_hget(redis_client_t *client, const char *key, const char *field,
               redis_command_cb_t callback, void *user_data);

/**
 * SADD key member [member ...]
 */
int redis_sadd(redis_client_t *client, const char *key, int member_count, const char **members,
               redis_command_cb_t callback, void *user_data);

/**
 * SMEMBERS key
 */
int redis_smembers(redis_client_t *client, const char *key,
                   redis_command_cb_t callback, void *user_data);

/**
 * PING
 */
int redis_ping(redis_client_t *client, redis_command_cb_t callback, void *user_data);

/* =============================================================================
 * Redis Streams API (for reliable message queuing)
 * =============================================================================
 */

/**
 * Stream entry structure
 */
typedef struct {
    char *id;                  /* Entry ID (e.g., "1234567890123-0") */
    char **fields;             /* Field names */
    char **values;             /* Field values */
    size_t *value_lens;        /* Value lengths (for binary data) */
    size_t field_count;        /* Number of fields */
} redis_stream_entry_t;

/**
 * Stream read result
 */
typedef struct {
    char *stream_name;         /* Stream key name */
    redis_stream_entry_t *entries;
    size_t entry_count;
} redis_stream_result_t;

/**
 * Stream message callback
 */
typedef void (*redis_stream_cb_t)(redis_client_t *client,
                                   redis_stream_result_t *results,
                                   size_t result_count,
                                   void *user_data);

/**
 * XADD key [MAXLEN ~ count] * field value [field value ...]
 * Add entry to stream
 *
 * @param client Redis client
 * @param key Stream key
 * @param maxlen Max stream length (0 = unlimited)
 * @param field_count Number of field-value pairs
 * @param fields Array of field names
 * @param values Array of values
 * @param value_lens Array of value lengths (NULL for null-terminated strings)
 * @param callback Callback receives entry ID
 * @param user_data User data
 * @return 0 on success
 */
int redis_xadd(redis_client_t *client, const char *key, size_t maxlen,
               size_t field_count, const char **fields,
               const char **values, const size_t *value_lens,
               redis_command_cb_t callback, void *user_data);

/**
 * XREAD [COUNT count] [BLOCK ms] STREAMS key [key ...] id [id ...]
 * Read from streams
 *
 * @param client Redis client
 * @param count Max entries to read (0 = all available)
 * @param block_ms Block timeout in ms (0 = no block, -1 = forever)
 * @param stream_count Number of streams
 * @param keys Stream keys
 * @param ids Entry IDs ("$" for new, "0" for all)
 * @param callback Stream callback
 * @param user_data User data
 * @return 0 on success
 */
int redis_xread(redis_client_t *client, size_t count, int block_ms,
                size_t stream_count, const char **keys, const char **ids,
                redis_stream_cb_t callback, void *user_data);

/**
 * XGROUP CREATE key group id [MKSTREAM]
 * Create consumer group
 *
 * @param client Redis client
 * @param key Stream key
 * @param group Group name
 * @param id Start ID ("$" for new messages, "0" for all)
 * @param mkstream Create stream if not exists
 * @param callback Command callback
 * @param user_data User data
 * @return 0 on success
 */
int redis_xgroup_create(redis_client_t *client, const char *key,
                        const char *group, const char *id, int mkstream,
                        redis_command_cb_t callback, void *user_data);

/**
 * XREADGROUP GROUP group consumer [COUNT count] [BLOCK ms] STREAMS key [key ...] id [id ...]
 * Read from stream as consumer group member
 *
 * @param client Redis client
 * @param group Group name
 * @param consumer Consumer name
 * @param count Max entries (0 = all)
 * @param block_ms Block timeout (0 = no block)
 * @param stream_count Number of streams
 * @param keys Stream keys
 * @param ids Entry IDs (">" for new messages)
 * @param callback Stream callback
 * @param user_data User data
 * @return 0 on success
 */
int redis_xreadgroup(redis_client_t *client, const char *group, const char *consumer,
                     size_t count, int block_ms,
                     size_t stream_count, const char **keys, const char **ids,
                     redis_stream_cb_t callback, void *user_data);

/**
 * XACK key group id [id ...]
 * Acknowledge processed messages
 *
 * @param client Redis client
 * @param key Stream key
 * @param group Group name
 * @param id_count Number of IDs
 * @param ids Entry IDs to acknowledge
 * @param callback Callback receives count of acknowledged
 * @param user_data User data
 * @return 0 on success
 */
int redis_xack(redis_client_t *client, const char *key, const char *group,
               size_t id_count, const char **ids,
               redis_command_cb_t callback, void *user_data);

/**
 * XDEL key id [id ...]
 * Delete entries from stream
 */
int redis_xdel(redis_client_t *client, const char *key,
               size_t id_count, const char **ids,
               redis_command_cb_t callback, void *user_data);

/**
 * XLEN key
 * Get stream length
 */
int redis_xlen(redis_client_t *client, const char *key,
               redis_command_cb_t callback, void *user_data);

/**
 * XTRIM key MAXLEN [~] count
 * Trim stream to max length
 */
int redis_xtrim(redis_client_t *client, const char *key, size_t maxlen,
                redis_command_cb_t callback, void *user_data);

/**
 * Free stream entry
 */
void redis_stream_entry_free(redis_stream_entry_t *entry);

/**
 * Free stream result
 */
void redis_stream_result_free(redis_stream_result_t *result, size_t count);

/* =============================================================================
 * Redis Pub/Sub API (for simple real-time broadcast)
 * =============================================================================
 */

/**
 * PUBLISH channel message
 * Publish message to channel
 *
 * @param client Redis client
 * @param channel Channel name
 * @param message Message data
 * @param len Message length
 * @param callback Callback receives subscriber count
 * @param user_data User data
 * @return 0 on success
 */
int redis_publish(redis_client_t *client, const char *channel,
                  const void *message, size_t len,
                  redis_command_cb_t callback, void *user_data);

/**
 * SUBSCRIBE channel [channel ...]
 * Subscribe to channels
 *
 * @param client Redis client
 * @param channel_count Number of channels
 * @param channels Channel names
 * @param on_message Message callback
 * @param user_data User data
 * @return 0 on success
 */
int redis_subscribe(redis_client_t *client, size_t channel_count,
                    const char **channels, redis_pubsub_cb_t on_message,
                    void *user_data);

/**
 * PSUBSCRIBE pattern [pattern ...]
 * Subscribe to channel patterns
 */
int redis_psubscribe(redis_client_t *client, size_t pattern_count,
                     const char **patterns, redis_pubsub_cb_t on_message,
                     void *user_data);

/**
 * UNSUBSCRIBE [channel ...]
 * Unsubscribe from channels
 */
int redis_unsubscribe(redis_client_t *client, size_t channel_count,
                      const char **channels);

/**
 * PUNSUBSCRIBE [pattern ...]
 * Unsubscribe from patterns
 */
int redis_punsubscribe(redis_client_t *client, size_t pattern_count,
                       const char **patterns);

#ifdef __cplusplus
}
#endif

#endif /* REDIS_CLIENT_H */
