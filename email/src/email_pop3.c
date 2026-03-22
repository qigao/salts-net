#include "email/email_pop3.h"
#include "CoroNet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

struct pop3_client_s {
  coro_context_t *ctx;
  pop3_config_t config;
  coro_socket_t *socket;
  char error_msg[512];
  char read_buffer[8192];
};

/* ── Helpers ───────────────────────────────────────────────────────── */

static int pop3_read_response(pop3_client_t *client) {
  if (!client || !client->socket) return -1;

  char *data = NULL;
  size_t len = 0;

  int result = coro_socket_recv(client->socket, &data, &len);
  if (result != 0 || !data) {
    snprintf(client->error_msg, sizeof(client->error_msg),
             "Failed to read POP3 response");
    return -1;
  }

  // Copy to read_buffer
  size_t copy_len = len < sizeof(client->read_buffer) - 1 ? len : sizeof(client->read_buffer) - 1;
  memcpy(client->read_buffer, data, copy_len);
  client->read_buffer[copy_len] = '\0';

  coro_socket_free_recv(data);

  // POP3 responses start with +OK or -ERR
  if (copy_len < 3) {
    snprintf(client->error_msg, sizeof(client->error_msg),
             "Invalid POP3 response");
    return -1;
  }

  if (strncmp(client->read_buffer, "+OK", 3) == 0) {
    return 0; // Success
  } else if (strncmp(client->read_buffer, "-ERR", 4) == 0) {
    snprintf(client->error_msg, sizeof(client->error_msg),
             "POP3 error: %s", client->read_buffer + 5);
    return -1;
  }

  snprintf(client->error_msg, sizeof(client->error_msg),
           "Unknown POP3 response: %s", client->read_buffer);
  return -1;
}

static int pop3_send_command(pop3_client_t *client, const char *cmd) {
  if (!client || !client->socket || !cmd) return -1;

  char buffer[1024];
  int len = snprintf(buffer, sizeof(buffer), "%s\r\n", cmd);

  int sent = coro_socket_send(client->socket, buffer, len);
  if (sent != len) {
    snprintf(client->error_msg, sizeof(client->error_msg),
             "Failed to send POP3 command: %s", cmd);
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
    char *data = NULL;
    size_t len = 0;

    int ret = coro_socket_recv(client->socket, &data, &len);
    if (ret != 0 || !data) break;

    // Expand buffer if needed
    if (total + len >= capacity) {
      capacity *= 2;
      char *new_result = realloc(result, capacity);
      if (!new_result) {
        free(result);
        coro_socket_free_recv(data);
        return NULL;
      }
      result = new_result;
    }

    memcpy(result + total, data, len);
    total += len;

    coro_socket_free_recv(data);

    // Check for terminator ".\r\n"
    if (total >= 3 &&
        result[total - 3] == '.' &&
        result[total - 2] == '\r' &&
        result[total - 1] == '\n') {
      total -= 3; // Remove terminator
      break;
    }
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

  if (client->socket) coro_socket_destroy(client->socket);

  free(client->config.host);
  free(client->config.username);
  free(client->config.password);
  free(client);
}

/* ── Connection ────────────────────────────────────────────────────── */

int pop3_connect(pop3_client_t *client) {
  if (!client) return -1;

  // Create socket
  client->socket = coro_socket_create_tcpv4(client->ctx);
  if (!client->socket) {
    snprintf(client->error_msg, sizeof(client->error_msg),
             "Failed to create socket");
    return -1;
  }

  if (coro_socket_connect(client->socket, client->config.host,
                          client->config.port) != 0) {
    snprintf(client->error_msg, sizeof(client->error_msg),
             "Failed to connect to %s:%d", client->config.host,
             client->config.port);
    coro_socket_destroy(client->socket);
    client->socket = NULL;
    return -1;
  }

  // TODO: If use_tls is set, upgrade connection to TLS
  // coro_socket_upgrade_tls(client->socket);

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

    // TODO: Upgrade to TLS
    // coro_socket_upgrade_tls(client->socket);
  }

  // USER
  char user_cmd[512];
  snprintf(user_cmd, sizeof(user_cmd), "USER %s", client->config.username);
  if (pop3_send_command(client, user_cmd) != 0) {
    pop3_disconnect(client);
    return -1;
  }

  // PASS
  char pass_cmd[512];
  snprintf(pass_cmd, sizeof(pass_cmd), "PASS %s", client->config.password);
  if (pop3_send_command(client, pass_cmd) != 0) {
    pop3_disconnect(client);
    return -1;
  }

  return 0;
}

void pop3_disconnect(pop3_client_t *client) {
  if (!client || !client->socket) return;

  // Send QUIT
  pop3_send_command(client, "QUIT");

  coro_socket_destroy(client->socket);
  client->socket = NULL;
}

/* ── Mailbox Operations ────────────────────────────────────────────── */

int pop3_stat(pop3_client_t *client, int *total_size) {
  if (!client) return -1;

  if (pop3_send_command(client, "STAT") != 0) return -1;

  // Parse response: "+OK count size"
  int count = 0, size = 0;
  if (sscanf(client->read_buffer, "+OK %d %d", &count, &size) != 2) {
    snprintf(client->error_msg, sizeof(client->error_msg),
             "Failed to parse STAT response");
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

email_message_t *pop3_retrieve_message(pop3_client_t *client, int msg_num) {
  if (!client) return NULL;

  char retr_cmd[64];
  snprintf(retr_cmd, sizeof(retr_cmd), "RETR %d", msg_num);

  if (pop3_send_command(client, retr_cmd) != 0) return NULL;

  size_t msg_len;
  char *raw_msg = pop3_read_multiline(client, &msg_len);
  if (!raw_msg) return NULL;

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
  snprintf(top_cmd, sizeof(top_cmd), "TOP %d 0", msg_num);

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
  snprintf(dele_cmd, sizeof(dele_cmd), "DELE %d", msg_num);

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
