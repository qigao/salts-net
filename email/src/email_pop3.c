#include "email/email_pop3.h"
#include "CoroNet.h"
#include <fmt.h>
#include "turbo_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

struct pop3_client_s {
  coro_context_t *ctx;
  pop3_config_t config;
  coro_socket_t *socket;
  turbo_mutex_t socket_mutex;
  char error_msg[512];
  char read_buffer[8192];
  char line_buffer[8192];
  size_t read_buffer_len;
};

static void pop3_socket_publish(pop3_client_t *client, coro_socket_t *socket) {
  turbo_mutex_lock(&client->socket_mutex);
  client->socket = socket;
  turbo_mutex_unlock(&client->socket_mutex);
}

static coro_socket_t *pop3_socket_take(pop3_client_t *client) {
  coro_socket_t *socket;
  turbo_mutex_lock(&client->socket_mutex);
  socket = client->socket;
  client->socket = NULL;
  turbo_mutex_unlock(&client->socket_mutex);
  return socket;
}

/* ── Helpers ───────────────────────────────────────────────────────── */

static void pop3_reset_read_state(pop3_client_t *client) {
  if (!client) return;
  client->read_buffer_len = 0;
  client->read_buffer[0] = '\0';
  client->line_buffer[0] = '\0';
}

static int pop3_read_line(pop3_client_t *client, char **line) {
  if (!client || !client->socket) return -1;

  while (1) {
    size_t i;
    for (i = 0; i < client->read_buffer_len; i++) {
      if (client->read_buffer[i] == '\n') {
        size_t line_len = i + 1;
        if (line_len >= sizeof(client->line_buffer)) {
          fmt(client->error_msg, sizeof(client->error_msg), "POP3 response line too large");
          return -1;
        }

        memcpy(client->line_buffer, client->read_buffer, line_len);
        client->line_buffer[line_len] = '\0';

        client->read_buffer_len -= line_len;
        memmove(client->read_buffer, client->read_buffer + line_len,
                client->read_buffer_len);
        client->read_buffer[client->read_buffer_len] = '\0';

        if (line) {
          *line = client->line_buffer;
        }
        return 0;
      }
    }

    {
      char *data = NULL;
      size_t len = 0;
      int result = coro_socket_recv(client->socket, &data, &len);
      size_t free_space;

      if (result != 0 || !data || len == 0) {
        if (data) {
          coro_socket_free_recv(data);
        }
        fmt(client->error_msg, sizeof(client->error_msg), "Failed to read POP3 response");
        return -1;
      }

      free_space = sizeof(client->read_buffer) - 1 - client->read_buffer_len;
      if (len > free_space) {
        coro_socket_free_recv(data);
        fmt(client->error_msg, sizeof(client->error_msg), "POP3 response buffer overflow");
        return -1;
      }

      memcpy(client->read_buffer + client->read_buffer_len, data, len);
      client->read_buffer_len += len;
      client->read_buffer[client->read_buffer_len] = '\0';
      coro_socket_free_recv(data);
    }
  }
}

static int pop3_read_response(pop3_client_t *client) {
  char *line = NULL;
  size_t line_len;

  if (pop3_read_line(client, &line) != 0) {
    return -1;
  }

  line_len = strlen(line);
  while (line_len > 0 &&
         (line[line_len - 1] == '\r' || line[line_len - 1] == '\n')) {
    line[--line_len] = '\0';
  }

  // POP3 responses start with +OK or -ERR
  if (line_len < 3) {
    fmt(client->error_msg, sizeof(client->error_msg), "Invalid POP3 response");
    return -1;
  }

  if (strncmp(line, "+OK", 3) == 0) {
    return 0; // Success
  } else if (strncmp(line, "-ERR", 4) == 0) {
    fmt(client->error_msg, sizeof(client->error_msg), "POP3 error: {}", line + 5);
    return -1;
  }

  fmt(client->error_msg, sizeof(client->error_msg), "Unknown POP3 response: {}", line);
  return -1;
}

static int pop3_send_command(pop3_client_t *client, const char *cmd) {
  if (!client || !client->socket || !cmd) return -1;

  char buffer[1024];
  int len = fmt(buffer, sizeof(buffer), "{}\r\n", cmd);

  int rc = coro_socket_send(client->socket, buffer, len);
  if (rc != 0) {
    fmt(client->error_msg, sizeof(client->error_msg), "Failed to send POP3 command: {}", cmd);
    return -1;
  }

  return pop3_read_response(client);
}

static char *pop3_read_multiline(pop3_client_t *client, size_t *out_len) {
  if (!client || !client->socket) return NULL;

  // Read until we see ".\r\n" on a line by itself
  size_t capacity = 8192;
  size_t total = 0;
  char *result = malloc(capacity);
  if (!result) return NULL;

  while (1) {
    char *line = NULL;
    size_t line_len;
    const char *payload = NULL;
    size_t payload_len;

    if (pop3_read_line(client, &line) != 0) {
      free(result);
      return NULL;
    }

    line_len = strlen(line);
    while (line_len > 0 &&
           (line[line_len - 1] == '\r' || line[line_len - 1] == '\n')) {
      line_len--;
    }

    if (line_len == 1 && line[0] == '.') {
      break;
    }

    payload = line;
    payload_len = line_len;
    if (payload_len >= 2 && payload[0] == '.' && payload[1] == '.') {
      payload++;
      payload_len--;
    }

    if (total + payload_len + 2 >= capacity) {
      while (total + payload_len + 2 >= capacity) {
        capacity *= 2;
      }
      {
        char *new_result = realloc(result, capacity);
        if (!new_result) {
          free(result);
          return NULL;
        }
        result = new_result;
      }
    }

    memcpy(result + total, payload, payload_len);
    total += payload_len;
    result[total++] = '\r';
    result[total++] = '\n';
  }

  result[total] = '\0';
  if (out_len) *out_len = total;
  return result;
}

/* ── Client Creation ───────────────────────────────────────────────── */

pop3_client_t *pop3_client_create(coro_context_t *ctx,
                                   const pop3_config_t *config) {
  if (!ctx || !config) return NULL;

  pop3_client_t *client = calloc(1, sizeof(pop3_client_t));
  if (!client) return NULL;

  client->ctx = ctx;
  turbo_mutex_init(&client->socket_mutex);
  client->config = *config;

  if (config->host) client->config.host = strdup(config->host);
  if (config->username) client->config.username = strdup(config->username);
  if (config->password) client->config.password = strdup(config->password);

  if (client->config.timeout_ms == 0) {
    client->config.timeout_ms = 30000;
  }

  return client;
}

void pop3_client_free(pop3_client_t *client) {
  if (!client) return;

  {
    coro_socket_t *socket = pop3_socket_take(client);
    if (socket) coro_socket_destroy(socket);
  }

  free(client->config.host);
  free(client->config.username);
  free(client->config.password);
  turbo_mutex_destroy(&client->socket_mutex);
  free(client);
}

/* ── Connection ────────────────────────────────────────────────────── */

int pop3_connect(pop3_client_t *client) {
  int socket_type;

  if (!client) return -1;

  // Create socket
  socket_type = client->config.use_tls ? CORO_SOCKET_TLS : CORO_SOCKET_TCP_V4;
  {
    coro_socket_t *socket = coro_socket_create(client->ctx, (coro_socket_type_t)socket_type);
    if (socket) pop3_socket_publish(client, socket);
  }
  if (!client->socket) {
    fmt(client->error_msg, sizeof(client->error_msg), "Failed to create socket");
    return -1;
  }

  if (coro_socket_connect(client->socket, client->config.host,
                          client->config.port) != 0) {
    fmt(client->error_msg, sizeof(client->error_msg), "Failed to connect to {}:{}", client->config.host,
        client->config.port);
    coro_socket_t *socket = pop3_socket_take(client);
    if (socket) coro_socket_destroy(socket);
    return -1;
  }

  // Read greeting
  if (pop3_read_response(client) != 0) {
    pop3_disconnect(client);
    return -1;
  }

  // STLS if needed
  if (client->config.use_stls) {
    if (pop3_send_command(client, "STLS") != 0) {
      pop3_disconnect(client);
      return -1;
    }

    if (coro_socket_upgrade_tls(client->socket, client->config.host) != 0) {
      fmt(client->error_msg, sizeof(client->error_msg), "Failed to upgrade POP3 connection to TLS");
      pop3_disconnect(client);
      return -1;
    }
    pop3_reset_read_state(client);
  }

  if (client->config.username && client->config.password) {
    // USER
    char user_cmd[512];
    fmt(user_cmd, sizeof(user_cmd), "USER {}", client->config.username);
    if (pop3_send_command(client, user_cmd) != 0) {
      pop3_disconnect(client);
      return -1;
    }

    // PASS
    char pass_cmd[512];
    fmt(pass_cmd, sizeof(pass_cmd), "PASS {}", client->config.password);
    if (pop3_send_command(client, pass_cmd) != 0) {
      pop3_disconnect(client);
      return -1;
    }
  }

  return 0;
}

void pop3_disconnect(pop3_client_t *client) {
  if (!client || !client->socket) return;

  // Send QUIT
  pop3_send_command(client, "QUIT");

  {
    coro_socket_t *socket = pop3_socket_take(client);
    if (socket) coro_socket_destroy(socket);
  }
}

int pop3_interrupt(pop3_client_t *client, int status) {
  int rc;
  if (!client) return TURBO_EINVAL;
  turbo_mutex_lock(&client->socket_mutex);
  rc = client->socket ? coro_socket_interrupt_wait(client->socket, status) : TURBO_ENOTCONN;
  turbo_mutex_unlock(&client->socket_mutex);
  return rc;
}

/* ── Mailbox Operations ────────────────────────────────────────────── */

int pop3_stat(pop3_client_t *client, int *total_size) {
  if (!client) return -1;

  if (pop3_send_command(client, "STAT") != 0) return -1;

  // Parse response: "+OK count size"
  int count = 0, size = 0;
  if (sscanf(client->line_buffer, "+OK %d %d", &count, &size) != 2) {
    fmt(client->error_msg, sizeof(client->error_msg), "Failed to parse STAT response");
    return -1;
  }

  if (total_size) *total_size = size;
  return count;
}

pop3_message_info_t *pop3_list(pop3_client_t *client, int *count) {
  if (!client) return NULL;

  if (pop3_send_command(client, "LIST") != 0) return NULL;

  // Read multiline response
  size_t data_len;
  char *data = pop3_read_multiline(client, &data_len);
  if (!data) return NULL;

  // Count lines
  int line_count = 0;
  for (size_t i = 0; i < data_len; i++) {
    if (data[i] == '\n') line_count++;
  }

  pop3_message_info_t *list = calloc(line_count, sizeof(pop3_message_info_t));
  if (!list) {
    free(data);
    return NULL;
  }

  // Parse lines: "msg_num size"
  int idx = 0;
  char *line = strtok(data, "\r\n");
  while (line && idx < line_count) {
    int msg_num, size;
    if (sscanf(line, "%d %d", &msg_num, &size) == 2) {
      list[idx].msg_num = msg_num;
      list[idx].size = size;
      list[idx].uidl = NULL;
      idx++;
    }
    line = strtok(NULL, "\r\n");
  }

  free(data);
  if (count) *count = idx;
  return list;
}

char **pop3_uidl(pop3_client_t *client, int *count) {
  if (!client) return NULL;

  if (pop3_send_command(client, "UIDL") != 0) return NULL;

  size_t data_len;
  char *data = pop3_read_multiline(client, &data_len);
  if (!data) return NULL;

  // Count lines
  int line_count = 0;
  for (size_t i = 0; i < data_len; i++) {
    if (data[i] == '\n') line_count++;
  }

  char **uidls = calloc(line_count, sizeof(char *));
  if (!uidls) {
    free(data);
    return NULL;
  }

  // Parse lines: "msg_num uidl"
  int idx = 0;
  char *line = strtok(data, "\r\n");
  while (line && idx < line_count) {
    int msg_num;
    char uidl[256];
    if (sscanf(line, "%d %255s", &msg_num, uidl) == 2) {
      uidls[idx++] = strdup(uidl);
    }
    line = strtok(NULL, "\r\n");
  }

  free(data);
  if (count) *count = idx;
  return uidls;
}

/* ── Message Operations ────────────────────────────────────────────── */

int pop3_retrieve_raw(pop3_client_t *client, int msg_num,
                      char **data, size_t *len) {
  char retr_cmd[64];
  char *raw_msg;
  size_t msg_len = 0;
  if (!client || !data || msg_num <= 0) return TURBO_EINVAL;
  *data = NULL;
  if (len) *len = 0;
  fmt(retr_cmd, sizeof(retr_cmd), "RETR {}", msg_num);
  if (pop3_send_command(client, retr_cmd) != 0) return TURBO_EIO;
  raw_msg = pop3_read_multiline(client, &msg_len);
  if (!raw_msg) return TURBO_EIO;
  *data = raw_msg;
  if (len) *len = msg_len;
  return TURBO_OK;
}

email_message_t *pop3_retrieve_message(pop3_client_t *client, int msg_num) {
  if (!client) return NULL;

  size_t msg_len = 0;
  char *raw_msg = NULL;
  if (pop3_retrieve_raw(client, msg_num, &raw_msg, &msg_len) != TURBO_OK) return NULL;

  // Parse message
  mem_pool_t pool;
  mem_init(&pool, msg_len + 4096);

  email_message_t *msg = email_message_parse(&pool, raw_msg, msg_len);

  free(raw_msg);

  if (!msg) {
    mem_destroy(&pool);
    return NULL;
  }

  return msg;
}

email_message_t *pop3_retrieve_headers(pop3_client_t *client, int msg_num) {
  if (!client) return NULL;

  char top_cmd[64];
  fmt(top_cmd, sizeof(top_cmd), "TOP {} 0", msg_num);

  if (pop3_send_command(client, top_cmd) != 0) return NULL;

  size_t msg_len;
  char *raw_headers = pop3_read_multiline(client, &msg_len);
  if (!raw_headers) return NULL;

  mem_pool_t pool;
  mem_init(&pool, msg_len + 1024);

  email_message_t *msg = email_message_parse(&pool, raw_headers, msg_len);

  free(raw_headers);

  if (!msg) {
    mem_destroy(&pool);
    return NULL;
  }

  return msg;
}

int pop3_delete_message(pop3_client_t *client, int msg_num) {
  if (!client) return -1;

  char dele_cmd[64];
  fmt(dele_cmd, sizeof(dele_cmd), "DELE {}", msg_num);

  return pop3_send_command(client, dele_cmd);
}

int pop3_reset(pop3_client_t *client) {
  if (!client) return -1;
  return pop3_send_command(client, "RSET");
}

/* ── Error Handling ────────────────────────────────────────────────── */

const char *pop3_get_error(pop3_client_t *client) {
  return client ? client->error_msg : "Invalid client";
}
