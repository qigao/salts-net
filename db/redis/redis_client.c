#include "redis_client.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stb_sprintf.h>

/* Windows doesn't have strndup */
#ifdef _WIN32
static char* strndup(const char *s, size_t n) {
    size_t len = strnlen(s, n);
    char *result = malloc(len + 1);
    if (result) {
        memcpy(result, s, len);
        result[len] = '\0';
    }
    return result;
}
#endif

/* Command structure */
struct redis_command_s {
  char *command_str;
  size_t command_len;
  redis_command_cb_t callback;
  void *user_data;
  redis_command_t *next;
};

/* Default configuration */
static redis_config_t default_config = {.host = "127.0.0.1",
                                        .port = 6379,
                                        .password = NULL,
                                        .database = 0,
                                        .timeout_ms = 5000,
                                        .command_timeout_ms = 5000,
                                        .max_pipeline = 100};

/* Forward declarations */
static void on_client_event(async_client_t *client, const async_client_event_t *event,
                            void *user_data);
static int parse_resp_reply(redis_client_t *client, redis_reply_t **reply);
static void process_reply(redis_client_t *client, redis_reply_t *reply);
static char *build_resp_command(int argc, const char **argv, const size_t *argvlen,
                                size_t *out_len);

redis_client_t *redis_client_create(const char *host, uint16_t port) {
  redis_config_t config = default_config;
  config.host = host;
  config.port = port;
  return redis_client_create_with_config(&config);
}

redis_client_t *redis_client_create_with_config(const redis_config_t *config) {
  redis_client_t *client = calloc(1, sizeof(redis_client_t));
  if (!client)
    return NULL;

  /* Copy configuration */
  client->config = *config;
  if (config->host) {
    client->config.host = strdup(config->host);
  }
  if (config->password) {
    client->config.password = strdup(config->password);
  }

  /* Allocate receive buffer */
  client->recv_buffer_size = 16384; /* 16KB initial buffer */
  client->recv_buffer = malloc(client->recv_buffer_size);
  if (!client->recv_buffer) {
    free(client);
    return NULL;
  }

  return client;
}

int redis_client_connect(redis_client_t *client, redis_connect_cb_t callback, void *user_data) {
  if (!client)
    return -1;

  client->connect_cb = callback;
  client->connect_user_data = user_data;

  /* Create async client */
  client->client = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, on_client_event, client);
  if (!client->client) {
    return -1;
  }

  /* Connect */
  async_client_status_t status =
      async_client_connect(client->client, client->config.host, client->config.port);
  return status == ASYNC_CLIENT_STATUS_OK ? 0 : -1;
}

static void on_client_event(async_client_t *ac, const async_client_event_t *event,
                            void *user_data) {
  redis_client_t *client = (redis_client_t *)user_data;

  switch (event->type) {
  case ASYNC_CLIENT_EVENT_CONNECTED:
    client->is_connected = 1;

    /* Authenticate if password provided */
    if (client->config.password) {
      redis_command(client, NULL, NULL, "AUTH %s", client->config.password);
    }

    /* Select database if not default */
    if (client->config.database != 0) {
      redis_command(client, NULL, NULL, "SELECT %d", client->config.database);
    }

    if (client->connect_cb) {
      client->connect_cb(client, 0, client->connect_user_data);
    }
    break;

  case ASYNC_CLIENT_EVENT_DATA: {
    const uint8_t *data = (const uint8_t *)event->data;
    size_t len = event->length;

    /* Append to receive buffer */
    if (client->recv_buffer_used + len > client->recv_buffer_size) {
      /* Grow buffer */
      size_t new_size = client->recv_buffer_size * 2;
      while (new_size < client->recv_buffer_used + len) {
        new_size *= 2;
      }
      char *new_buffer = realloc(client->recv_buffer, new_size);
      if (!new_buffer)
        return;
      client->recv_buffer = new_buffer;
      client->recv_buffer_size = new_size;
    }

    memcpy(client->recv_buffer + client->recv_buffer_used, data, len);
    client->recv_buffer_used += len;

    /* Try to parse replies */
    while (client->recv_buffer_used > 0) {
      redis_reply_t *reply = NULL;
      int parsed = parse_resp_reply(client, &reply);

      if (parsed < 0) {
        /* Parse error */
        break;
      } else if (parsed == 0) {
        /* Need more data */
        break;
      } else {
        /* Successfully parsed */
        process_reply(client, reply);

        /* Remove parsed data from buffer */
        memmove(client->recv_buffer, client->recv_buffer + parsed,
                client->recv_buffer_used - parsed);
        client->recv_buffer_used -= parsed;
      }
    }
    break;
  }

  case ASYNC_CLIENT_EVENT_CLOSED:
    client->is_connected = 0;
    break;

  case ASYNC_CLIENT_EVENT_ERROR:
    if (client->connect_cb && !client->is_connected) {
      client->connect_cb(client, -1, client->connect_user_data);
    }
    break;
  }
}

/* RESP parser */
static int parse_resp_reply(redis_client_t *client, redis_reply_t **reply) {
  if (client->recv_buffer_used < 1)
    return 0;

  char *buf = client->recv_buffer;
  size_t len = client->recv_buffer_used;
  char type = buf[0];

  *reply = calloc(1, sizeof(redis_reply_t));
  if (!*reply)
    return -1;

  char *end = memchr(buf + 1, '\n', len - 1);
  if (!end)
    return 0; /* Need more data */

  size_t line_len = end - buf - 1;
  if (line_len > 0 && buf[line_len] == '\r') {
    line_len--;
  }

  switch (type) {
  case '+': /* Simple string */
    (*reply)->type = REDIS_REPLY_STRING;
    (*reply)->str = strndup(buf + 1, line_len);
    (*reply)->len = line_len;
    return (end - buf) + 1;

  case '-': /* Error */
    (*reply)->type = REDIS_REPLY_ERROR;
    (*reply)->str = strndup(buf + 1, line_len);
    (*reply)->len = line_len;
    return (end - buf) + 1;

  case ':': /* Integer */
    (*reply)->type = REDIS_REPLY_INTEGER;
    (*reply)->integer = strtoll(buf + 1, NULL, 10);
    return (end - buf) + 1;

  case '$': { /* Bulk string */
    int64_t bulk_len = strtoll(buf + 1, NULL, 10);
    if (bulk_len == -1) {
      (*reply)->type = REDIS_REPLY_NULL;
      return (end - buf) + 1;
    }

    size_t total_needed = (end - buf) + 1 + bulk_len + 2;
    if (len < total_needed)
      return 0; /* Need more data */

    (*reply)->type = REDIS_REPLY_BULK_STRING;
    (*reply)->str = malloc(bulk_len + 1);
    memcpy((*reply)->str, end + 1, bulk_len);
    (*reply)->str[bulk_len] = '\0';
    (*reply)->len = bulk_len;
    return total_needed;
  }

  case '*': { /* Array */
    int64_t array_len = strtoll(buf + 1, NULL, 10);
    if (array_len == -1) {
      (*reply)->type = REDIS_REPLY_NULL;
      return (end - buf) + 1;
    }

    (*reply)->type = REDIS_REPLY_ARRAY;
    (*reply)->element_count = array_len;
    (*reply)->elements = calloc(array_len, sizeof(redis_reply_t *));

    size_t consumed = (end - buf) + 1;

    /* Parse array elements recursively */
    for (int i = 0; i < array_len; i++) {
      /* Temporarily adjust buffer */
      client->recv_buffer += consumed;
      client->recv_buffer_used -= consumed;

      int elem_consumed = parse_resp_reply(client, &(*reply)->elements[i]);

      /* Restore buffer */
      client->recv_buffer -= consumed;
      client->recv_buffer_used += consumed;

      if (elem_consumed <= 0) {
        /* Failed to parse element */
        redis_reply_free(*reply);
        *reply = NULL;
        return elem_consumed;
      }

      consumed += elem_consumed;
    }

    return consumed;
  }

  default:
    free(*reply);
    *reply = NULL;
    return -1; /* Unknown type */
  }
}

static void process_reply(redis_client_t *client, redis_reply_t *reply) {
  if (!client->command_queue) {
    redis_reply_free(reply);
    return;
  }

  /* Dequeue command */
  redis_command_t *cmd = client->command_queue;
  client->command_queue = cmd->next;
  if (!client->command_queue) {
    client->command_queue_tail = NULL;
  }
  client->queued_commands--;

  /* Call callback */
  if (cmd->callback) {
    cmd->callback(client, reply, cmd->user_data);
  }

  /* Free command */
  free(cmd->command_str);
  free(cmd);

  /* Free reply */
  redis_reply_free(reply);
}

void redis_reply_free(redis_reply_t *reply) {
  if (!reply)
    return;

  if (reply->str) {
    free(reply->str);
  }

  if (reply->elements) {
    for (size_t i = 0; i < reply->element_count; i++) {
      redis_reply_free(reply->elements[i]);
    }
    free(reply->elements);
  }

  free(reply);
}

/* Build RESP command */
static char *build_resp_command(int argc, const char **argv, const size_t *argvlen,
                                size_t *out_len) {
  /* Calculate total size */
  size_t total = 0;
  total += 1 + 20 + 2; /* *<count>\r\n */

  for (int i = 0; i < argc; i++) {
    size_t len = argvlen ? argvlen[i] : strlen(argv[i]);
    total += 1 + 20 + 2; /* $<len>\r\n */
    total += len + 2;    /* <data>\r\n */
  }

  char *cmd = malloc(total);
  if (!cmd)
    return NULL;

  char *p = cmd;
  p += sprintf(p, "*%d\r\n", argc);

  for (int i = 0; i < argc; i++) {
    size_t len = argvlen ? argvlen[i] : strlen(argv[i]);
    p += sprintf(p, "$%zu\r\n", len);
    memcpy(p, argv[i], len);
    p += len;
    *p++ = '\r';
    *p++ = '\n';
  }

  *out_len = p - cmd;
  return cmd;
}

int redis_commandv(redis_client_t *client, int argc, const char **argv, const size_t *argvlen,
                   redis_command_cb_t callback, void *user_data) {
  if (!client || !client->is_connected)
    return -1;

  /* Build RESP command */
  size_t cmd_len;
  char *cmd_str = build_resp_command(argc, argv, argvlen, &cmd_len);
  if (!cmd_str)
    return -1;

  /* Create command */
  redis_command_t *cmd = calloc(1, sizeof(redis_command_t));
  if (!cmd) {
    free(cmd_str);
    return -1;
  }

  cmd->command_str = cmd_str;
  cmd->command_len = cmd_len;
  cmd->callback = callback;
  cmd->user_data = user_data;

  /* Queue command */
  if (client->command_queue_tail) {
    client->command_queue_tail->next = cmd;
  } else {
    client->command_queue = cmd;
  }
  client->command_queue_tail = cmd;
  client->queued_commands++;

  /* Send command */
  async_client_send(client->client, cmd_str, cmd_len);

  return 0;
}

int redis_command(redis_client_t *client, redis_command_cb_t callback, void *user_data,
                  const char *format, ...) {
  va_list ap;
  char *cmd_str;
  int argc = 0;
  const char *argv[32];
  char arg_buf[32][256];

  /* Parse format string */
  va_start(ap, format);
  const char *p = format;

  while (*p && argc < 32) {
    while (*p == ' ')
      p++;
    if (!*p)
      break;

    if (*p == '%') {
      p++;
      if (*p == 's') {
        argv[argc++] = va_arg(ap, const char *);
      } else if (*p == 'd') {
        int val = va_arg(ap, int);
        stbsp_snprintf(arg_buf[argc], sizeof(arg_buf[argc]), "%d", val);
        argv[argc++] = arg_buf[argc - 1];
      }
      p++;
    } else {
      const char *start = p;
      while (*p && *p != ' ' && *p != '%')
        p++;
      size_t len = p - start;
      if (len < sizeof(arg_buf[argc])) {
        memcpy(arg_buf[argc], start, len);
        arg_buf[argc][len] = '\0';
        argv[argc++] = arg_buf[argc - 1];
      }
    }
  }

  va_end(ap);

  return redis_commandv(client, argc, argv, NULL, callback, user_data);
}

void redis_client_disconnect(redis_client_t *client) {
  if (client && client->client) {
    async_client_close(client->client);
  }
}

void redis_client_destroy(redis_client_t *client) {
  if (!client)
    return;

  redis_client_disconnect(client);

  if (client->client) {
    async_client_destroy(client->client);
  }

  /* Free command queue */
  redis_command_t *cmd = client->command_queue;
  while (cmd) {
    redis_command_t *next = cmd->next;
    free(cmd->command_str);
    free(cmd);
    cmd = next;
  }

  free(client->recv_buffer);
  free((char *)client->config.host);
  free((char *)client->config.password);
  free(client);
}

const char *redis_client_get_error(redis_client_t *client) {
  return client ? "Error" : "Invalid client";
}

/* Convenience functions */

int redis_set(redis_client_t *client, const char *key, const char *value,
              redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"SET", key, value};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_get(redis_client_t *client, const char *key, redis_command_cb_t callback,
              void *user_data) {
  const char *argv[] = {"GET", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_del(redis_client_t *client, int key_count, const char **keys, redis_command_cb_t callback,
              void *user_data) {
  const char **argv = malloc((key_count + 1) * sizeof(char *));
  argv[0] = "DEL";
  for (int i = 0; i < key_count; i++) {
    argv[i + 1] = keys[i];
  }
  int result = redis_commandv(client, key_count + 1, argv, NULL, callback, user_data);
  free(argv);
  return result;
}

int redis_exists(redis_client_t *client, const char *key, redis_command_cb_t callback,
                 void *user_data) {
  const char *argv[] = {"EXISTS", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_expire(redis_client_t *client, const char *key, int seconds, redis_command_cb_t callback,
                 void *user_data) {
  char seconds_str[32];
  stbsp_snprintf(seconds_str, sizeof(seconds_str), "%d", seconds);
  const char *argv[] = {"EXPIRE", key, seconds_str};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_incr(redis_client_t *client, const char *key, redis_command_cb_t callback,
               void *user_data) {
  const char *argv[] = {"INCR", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_ping(redis_client_t *client, redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"PING"};
  return redis_commandv(client, 1, argv, NULL, callback, user_data);
}

int redis_lpush(redis_client_t *client, const char *key, int value_count, const char **values,
                redis_command_cb_t callback, void *user_data) {
  const char **argv = malloc((value_count + 2) * sizeof(char *));
  argv[0] = "LPUSH";
  argv[1] = key;
  for (int i = 0; i < value_count; i++) {
    argv[i + 2] = values[i];
  }
  int result = redis_commandv(client, value_count + 2, argv, NULL, callback, user_data);
  free(argv);
  return result;
}

int redis_rpush(redis_client_t *client, const char *key, int value_count, const char **values,
                redis_command_cb_t callback, void *user_data) {
  const char **argv = malloc((value_count + 2) * sizeof(char *));
  argv[0] = "RPUSH";
  argv[1] = key;
  for (int i = 0; i < value_count; i++) {
    argv[i + 2] = values[i];
  }
  int result = redis_commandv(client, value_count + 2, argv, NULL, callback, user_data);
  free(argv);
  return result;
}

int redis_lpop(redis_client_t *client, const char *key, redis_command_cb_t callback,
               void *user_data) {
  const char *argv[] = {"LPOP", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_rpop(redis_client_t *client, const char *key, redis_command_cb_t callback,
               void *user_data) {
  const char *argv[] = {"RPOP", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_hset(redis_client_t *client, const char *key, const char *field, const char *value,
               redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"HSET", key, field, value};
  return redis_commandv(client, 4, argv, NULL, callback, user_data);
}

int redis_hget(redis_client_t *client, const char *key, const char *field,
               redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"HGET", key, field};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_sadd(redis_client_t *client, const char *key, int member_count, const char **members,
               redis_command_cb_t callback, void *user_data) {
  const char **argv = malloc((member_count + 2) * sizeof(char *));
  argv[0] = "SADD";
  argv[1] = key;
  for (int i = 0; i < member_count; i++) {
    argv[i + 2] = members[i];
  }
  int result = redis_commandv(client, member_count + 2, argv, NULL, callback, user_data);
  free(argv);
  return result;
}

int redis_smembers(redis_client_t *client, const char *key, redis_command_cb_t callback,
                   void *user_data) {
  const char *argv[] = {"SMEMBERS", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

/* =============================================================================
 * Redis Streams Implementation
 * =============================================================================
 */

int redis_xadd(redis_client_t *client, const char *key, size_t maxlen,
               size_t field_count, const char **fields,
               const char **values, const size_t *value_lens,
               redis_command_cb_t callback, void *user_data) {
  if (!client || !key || !fields || !values || field_count == 0)
    return -1;

  /* Calculate argc: XADD key [MAXLEN ~ count] * field value [...] */
  size_t argc = 3 + field_count * 2;  /* XADD key * + pairs */
  if (maxlen > 0) argc += 3;          /* MAXLEN ~ count */

  const char **argv = malloc(argc * sizeof(char *));
  size_t *argvlen = malloc(argc * sizeof(size_t));
  if (!argv || !argvlen) {
    free(argv);
    free(argvlen);
    return -1;
  }

  char maxlen_str[32];
  size_t idx = 0;

  argv[idx] = "XADD";
  argvlen[idx++] = 4;

  argv[idx] = key;
  argvlen[idx++] = strlen(key);

  if (maxlen > 0) {
    argv[idx] = "MAXLEN";
    argvlen[idx++] = 6;
    argv[idx] = "~";
    argvlen[idx++] = 1;
    stbsp_snprintf(maxlen_str, sizeof(maxlen_str), "%zu", maxlen);
    argv[idx] = maxlen_str;
    argvlen[idx++] = strlen(maxlen_str);
  }

  argv[idx] = "*";
  argvlen[idx++] = 1;

  for (size_t i = 0; i < field_count; i++) {
    argv[idx] = fields[i];
    argvlen[idx++] = strlen(fields[i]);
    argv[idx] = values[i];
    argvlen[idx++] = value_lens ? value_lens[i] : strlen(values[i]);
  }

  int result = redis_commandv(client, (int)idx, argv, argvlen, callback, user_data);

  free(argv);
  free(argvlen);
  return result;
}

/* Stream callback context */
typedef struct {
  redis_stream_cb_t callback;
  void *user_data;
} redis_stream_cb_ctx_t;

/* Parse stream reply into result structure */
static void on_stream_reply(redis_client_t *client, redis_reply_t *reply, void *user_data) {
  redis_stream_cb_ctx_t *ctx = (redis_stream_cb_ctx_t *)user_data;

  if (!ctx || !ctx->callback) {
    free(ctx);
    return;
  }

  if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->element_count == 0) {
    ctx->callback(client, NULL, 0, ctx->user_data);
    free(ctx);
    return;
  }

  /* Parse: [[stream_name, [[id, [field, value, ...]], ...]], ...] */
  size_t result_count = reply->element_count;
  redis_stream_result_t *results = calloc(result_count, sizeof(redis_stream_result_t));

  for (size_t i = 0; i < result_count; i++) {
    redis_reply_t *stream_reply = reply->elements[i];
    if (!stream_reply || stream_reply->type != REDIS_REPLY_ARRAY ||
        stream_reply->element_count < 2)
      continue;

    /* Stream name */
    if (stream_reply->elements[0]->type == REDIS_REPLY_BULK_STRING) {
      results[i].stream_name = strdup(stream_reply->elements[0]->str);
    }

    /* Entries */
    redis_reply_t *entries_reply = stream_reply->elements[1];
    if (entries_reply->type != REDIS_REPLY_ARRAY)
      continue;

    results[i].entry_count = entries_reply->element_count;
    results[i].entries = calloc(results[i].entry_count, sizeof(redis_stream_entry_t));

    for (size_t j = 0; j < entries_reply->element_count; j++) {
      redis_reply_t *entry = entries_reply->elements[j];
      if (!entry || entry->type != REDIS_REPLY_ARRAY || entry->element_count < 2)
        continue;

      /* Entry ID */
      if (entry->elements[0]->type == REDIS_REPLY_BULK_STRING) {
        results[i].entries[j].id = strdup(entry->elements[0]->str);
      }

      /* Fields */
      redis_reply_t *fields = entry->elements[1];
      if (fields->type == REDIS_REPLY_ARRAY && fields->element_count >= 2) {
        size_t fc = fields->element_count / 2;
        results[i].entries[j].field_count = fc;
        results[i].entries[j].fields = calloc(fc, sizeof(char *));
        results[i].entries[j].values = calloc(fc, sizeof(char *));
        results[i].entries[j].value_lens = calloc(fc, sizeof(size_t));

        for (size_t k = 0; k < fc; k++) {
          redis_reply_t *f = fields->elements[k * 2];
          redis_reply_t *v = fields->elements[k * 2 + 1];

          if (f->type == REDIS_REPLY_BULK_STRING) {
            results[i].entries[j].fields[k] = strdup(f->str);
          }
          if (v->type == REDIS_REPLY_BULK_STRING) {
            results[i].entries[j].values[k] = malloc(v->len);
            memcpy(results[i].entries[j].values[k], v->str, v->len);
            results[i].entries[j].value_lens[k] = v->len;
          }
        }
      }
    }
  }

  ctx->callback(client, results, result_count, ctx->user_data);
  redis_stream_result_free(results, result_count);
  free(ctx);
}

int redis_xread(redis_client_t *client, size_t count, int block_ms,
                size_t stream_count, const char **keys, const char **ids,
                redis_stream_cb_t callback, void *user_data) {
  if (!client || !keys || !ids || stream_count == 0)
    return -1;

  /* XREAD [COUNT count] [BLOCK ms] STREAMS key [key ...] id [id ...] */
  size_t argc = 1 + 1 + stream_count * 2;  /* XREAD STREAMS keys ids */
  if (count > 0) argc += 2;                 /* COUNT count */
  if (block_ms >= 0) argc += 2;             /* BLOCK ms */

  const char **argv = malloc(argc * sizeof(char *));
  if (!argv) return -1;

  char count_str[32], block_str[32];
  size_t idx = 0;

  argv[idx++] = "XREAD";

  if (count > 0) {
    argv[idx++] = "COUNT";
    stbsp_snprintf(count_str, sizeof(count_str), "%zu", count);
    argv[idx++] = count_str;
  }

  if (block_ms >= 0) {
    argv[idx++] = "BLOCK";
    stbsp_snprintf(block_str, sizeof(block_str), "%d", block_ms);
    argv[idx++] = block_str;
  }

  argv[idx++] = "STREAMS";

  for (size_t i = 0; i < stream_count; i++) {
    argv[idx++] = keys[i];
  }
  for (size_t i = 0; i < stream_count; i++) {
    argv[idx++] = ids[i];
  }

  redis_stream_cb_ctx_t *ctx = malloc(sizeof(redis_stream_cb_ctx_t));
  if (!ctx) {
    free(argv);
    return -1;
  }
  ctx->callback = callback;
  ctx->user_data = user_data;

  int result = redis_commandv(client, (int)idx, argv, NULL,
                               (redis_command_cb_t)on_stream_reply, ctx);
  free(argv);
  return result;
}

int redis_xgroup_create(redis_client_t *client, const char *key,
                        const char *group, const char *id, int mkstream,
                        redis_command_cb_t callback, void *user_data) {
  if (!client || !key || !group || !id)
    return -1;

  if (mkstream) {
    const char *argv[] = {"XGROUP", "CREATE", key, group, id, "MKSTREAM"};
    return redis_commandv(client, 6, argv, NULL, callback, user_data);
  } else {
    const char *argv[] = {"XGROUP", "CREATE", key, group, id};
    return redis_commandv(client, 5, argv, NULL, callback, user_data);
  }
}

int redis_xreadgroup(redis_client_t *client, const char *group, const char *consumer,
                     size_t count, int block_ms,
                     size_t stream_count, const char **keys, const char **ids,
                     redis_stream_cb_t callback, void *user_data) {
  if (!client || !group || !consumer || !keys || !ids || stream_count == 0)
    return -1;

  /* XREADGROUP GROUP group consumer [COUNT count] [BLOCK ms] STREAMS key [key ...] id [...] */
  size_t argc = 5 + 1 + stream_count * 2;  /* XREADGROUP GROUP g c STREAMS keys ids */
  if (count > 0) argc += 2;
  if (block_ms >= 0) argc += 2;

  const char **argv = malloc(argc * sizeof(char *));
  if (!argv) return -1;

  char count_str[32], block_str[32];
  size_t idx = 0;

  argv[idx++] = "XREADGROUP";
  argv[idx++] = "GROUP";
  argv[idx++] = group;
  argv[idx++] = consumer;

  if (count > 0) {
    argv[idx++] = "COUNT";
    stbsp_snprintf(count_str, sizeof(count_str), "%zu", count);
    argv[idx++] = count_str;
  }

  if (block_ms >= 0) {
    argv[idx++] = "BLOCK";
    stbsp_snprintf(block_str, sizeof(block_str), "%d", block_ms);
    argv[idx++] = block_str;
  }

  argv[idx++] = "STREAMS";

  for (size_t i = 0; i < stream_count; i++) {
    argv[idx++] = keys[i];
  }
  for (size_t i = 0; i < stream_count; i++) {
    argv[idx++] = ids[i];
  }

  redis_stream_cb_ctx_t *ctx = malloc(sizeof(redis_stream_cb_ctx_t));
  if (!ctx) {
    free(argv);
    return -1;
  }
  ctx->callback = callback;
  ctx->user_data = user_data;

  int result = redis_commandv(client, (int)idx, argv, NULL,
                               (redis_command_cb_t)on_stream_reply, ctx);
  free(argv);
  return result;
}

int redis_xack(redis_client_t *client, const char *key, const char *group,
               size_t id_count, const char **ids,
               redis_command_cb_t callback, void *user_data) {
  if (!client || !key || !group || !ids || id_count == 0)
    return -1;

  size_t argc = 3 + id_count;
  const char **argv = malloc(argc * sizeof(char *));
  if (!argv) return -1;

  argv[0] = "XACK";
  argv[1] = key;
  argv[2] = group;
  for (size_t i = 0; i < id_count; i++) {
    argv[3 + i] = ids[i];
  }

  int result = redis_commandv(client, (int)argc, argv, NULL, callback, user_data);
  free(argv);
  return result;
}

int redis_xdel(redis_client_t *client, const char *key,
               size_t id_count, const char **ids,
               redis_command_cb_t callback, void *user_data) {
  if (!client || !key || !ids || id_count == 0)
    return -1;

  size_t argc = 2 + id_count;
  const char **argv = malloc(argc * sizeof(char *));
  if (!argv) return -1;

  argv[0] = "XDEL";
  argv[1] = key;
  for (size_t i = 0; i < id_count; i++) {
    argv[2 + i] = ids[i];
  }

  int result = redis_commandv(client, (int)argc, argv, NULL, callback, user_data);
  free(argv);
  return result;
}

int redis_xlen(redis_client_t *client, const char *key,
               redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"XLEN", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_xtrim(redis_client_t *client, const char *key, size_t maxlen,
                redis_command_cb_t callback, void *user_data) {
  char maxlen_str[32];
  stbsp_snprintf(maxlen_str, sizeof(maxlen_str), "%zu", maxlen);
  const char *argv[] = {"XTRIM", key, "MAXLEN", "~", maxlen_str};
  return redis_commandv(client, 5, argv, NULL, callback, user_data);
}

void redis_stream_entry_free(redis_stream_entry_t *entry) {
  if (!entry) return;

  free(entry->id);
  if (entry->fields) {
    for (size_t i = 0; i < entry->field_count; i++) {
      free(entry->fields[i]);
      free(entry->values[i]);
    }
    free(entry->fields);
    free(entry->values);
    free(entry->value_lens);
  }
}

void redis_stream_result_free(redis_stream_result_t *results, size_t count) {
  if (!results) return;

  for (size_t i = 0; i < count; i++) {
    free(results[i].stream_name);
    for (size_t j = 0; j < results[i].entry_count; j++) {
      redis_stream_entry_free(&results[i].entries[j]);
    }
    free(results[i].entries);
  }
  free(results);
}

/* =============================================================================
 * Redis Pub/Sub Implementation
 * =============================================================================
 */

/* Subscription linked list node */
struct redis_subscription_s {
  char *channel;
  int is_pattern;
  redis_pubsub_cb_t callback;
  void *user_data;
  redis_subscription_t *next;
};

int redis_publish(redis_client_t *client, const char *channel,
                  const void *message, size_t len,
                  redis_command_cb_t callback, void *user_data) {
  if (!client || !channel)
    return -1;

  const char *argv[] = {"PUBLISH", channel, (const char *)message};
  size_t argvlen[] = {7, strlen(channel), len};

  return redis_commandv(client, 3, argv, argvlen, callback, user_data);
}

int redis_subscribe(redis_client_t *client, size_t channel_count,
                    const char **channels, redis_pubsub_cb_t on_message,
                    void *user_data) {
  if (!client || !channels || channel_count == 0)
    return -1;

  client->is_subscriber = 1;
  client->pubsub_cb = on_message;
  client->pubsub_user_data = user_data;

  size_t argc = 1 + channel_count;
  const char **argv = malloc(argc * sizeof(char *));
  if (!argv) return -1;

  argv[0] = "SUBSCRIBE";
  for (size_t i = 0; i < channel_count; i++) {
    argv[i + 1] = channels[i];
  }

  int result = redis_commandv(client, (int)argc, argv, NULL, NULL, NULL);
  free(argv);
  return result;
}

int redis_psubscribe(redis_client_t *client, size_t pattern_count,
                     const char **patterns, redis_pubsub_cb_t on_message,
                     void *user_data) {
  if (!client || !patterns || pattern_count == 0)
    return -1;

  client->is_subscriber = 1;
  client->pubsub_cb = on_message;
  client->pubsub_user_data = user_data;

  size_t argc = 1 + pattern_count;
  const char **argv = malloc(argc * sizeof(char *));
  if (!argv) return -1;

  argv[0] = "PSUBSCRIBE";
  for (size_t i = 0; i < pattern_count; i++) {
    argv[i + 1] = patterns[i];
  }

  int result = redis_commandv(client, (int)argc, argv, NULL, NULL, NULL);
  free(argv);
  return result;
}

int redis_unsubscribe(redis_client_t *client, size_t channel_count,
                      const char **channels) {
  if (!client)
    return -1;

  if (channel_count == 0 || !channels) {
    const char *argv[] = {"UNSUBSCRIBE"};
    return redis_commandv(client, 1, argv, NULL, NULL, NULL);
  }

  size_t argc = 1 + channel_count;
  const char **argv = malloc(argc * sizeof(char *));
  if (!argv) return -1;

  argv[0] = "UNSUBSCRIBE";
  for (size_t i = 0; i < channel_count; i++) {
    argv[i + 1] = channels[i];
  }

  int result = redis_commandv(client, (int)argc, argv, NULL, NULL, NULL);
  free(argv);
  return result;
}

int redis_punsubscribe(redis_client_t *client, size_t pattern_count,
                       const char **patterns) {
  if (!client)
    return -1;

  if (pattern_count == 0 || !patterns) {
    const char *argv[] = {"PUNSUBSCRIBE"};
    return redis_commandv(client, 1, argv, NULL, NULL, NULL);
  }

  size_t argc = 1 + pattern_count;
  const char **argv = malloc(argc * sizeof(char *));
  if (!argv) return -1;

  argv[0] = "PUNSUBSCRIBE";
  for (size_t i = 0; i < pattern_count; i++) {
    argv[i + 1] = patterns[i];
  }

  int result = redis_commandv(client, (int)argc, argv, NULL, NULL, NULL);
  free(argv);
  return result;
}
