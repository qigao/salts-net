#include "email/email_pop3.h"
#include "email_cnet_transport.h"
#include <ctype.h>
#include <fmt.h>
#include <salts/error_codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { POP3_MAX_MULTILINE_BYTES = 64 * 1024 * 1024 };

struct pop3_client_s {
  pop3_config_t config;
  email_cnet_transport_t transport;
  char error_msg[512];
  char read_buffer[8192];
  char line_buffer[8192];
  size_t read_buffer_len;
};

/* ── Helpers ───────────────────────────────────────────────────────── */

static void pop3_reset_read_state(pop3_client_t *client) {
  if (!client) return;
  client->read_buffer_len = 0;
  client->read_buffer[0] = '\0';
  client->line_buffer[0] = '\0';
}

static int pop3_read_line(pop3_client_t *client, char **line) {
  if (!client || !email_cnet_transport_is_connected(&client->transport)) return -1;

  while (1) {
    size_t i;
    for (i = 0; i < client->read_buffer_len; i++) {
      if (client->read_buffer[i] == '\n') {
        size_t line_len = i + 1;
        if (line_len >= sizeof(client->line_buffer)) {
          fmt_text(client->error_msg, sizeof(client->error_msg), "POP3 response line too large");
          return -1;
        }

        memcpy(client->line_buffer, client->read_buffer, line_len);
        client->line_buffer[line_len] = '\0';

        client->read_buffer_len -= line_len;
        memmove(client->read_buffer, client->read_buffer + line_len, client->read_buffer_len);
        client->read_buffer[client->read_buffer_len] = '\0';

        if (line) {
          *line = client->line_buffer;
        }
        return 0;
      }
    }

    {
      size_t len = 0;
      size_t free_space;
      int result;

      free_space = sizeof(client->read_buffer) - 1 - client->read_buffer_len;
      result = email_cnet_transport_receive(
          &client->transport, client->read_buffer + client->read_buffer_len, free_space, &len);
      if (result != SALTS_OK || len == 0) {
        fmt_text(client->error_msg, sizeof(client->error_msg), "Failed to read POP3 response");
        return -1;
      }

      client->read_buffer_len += len;
      client->read_buffer[client->read_buffer_len] = '\0';
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
  while (line_len > 0 && (line[line_len - 1] == '\r' || line[line_len - 1] == '\n')) {
    line[--line_len] = '\0';
  }

  // POP3 responses start with +OK or -ERR
  if (line_len < 3) {
    fmt_text(client->error_msg, sizeof(client->error_msg), "Invalid POP3 response");
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
  if (!client || !email_cnet_transport_is_connected(&client->transport) || !cmd) return -1;

  char buffer[1024];
  int len = fmt(buffer, sizeof(buffer), "{}\r\n", cmd);

  int rc = email_cnet_transport_send(&client->transport, buffer, (size_t)len);
  if (rc != SALTS_OK) {
    fmt(client->error_msg, sizeof(client->error_msg), "Failed to send POP3 command: {}", cmd);
    return -1;
  }

  return pop3_read_response(client);
}

static char *pop3_read_multiline(pop3_client_t *client, size_t *out_len) {
  if (!client || !email_cnet_transport_is_connected(&client->transport)) return NULL;

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
    while (line_len > 0 && (line[line_len - 1] == '\r' || line[line_len - 1] == '\n')) {
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

    if (total > POP3_MAX_MULTILINE_BYTES - 3u ||
        payload_len > POP3_MAX_MULTILINE_BYTES - total - 3u) {
      fmt_text(client->error_msg, sizeof(client->error_msg), "POP3 multiline response too large");
      free(result);
      return NULL;
    }
    if (total + payload_len + 3u > capacity) {
      const size_t needed = total + payload_len + 3u;
      while (capacity < needed) {
        capacity =
            capacity > POP3_MAX_MULTILINE_BYTES / 2u ? POP3_MAX_MULTILINE_BYTES : capacity * 2u;
        if (capacity < needed && capacity == POP3_MAX_MULTILINE_BYTES) {
          fmt_text(client->error_msg, sizeof(client->error_msg),
                   "POP3 multiline response too large");
          free(result);
          return NULL;
        }
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

pop3_client_t *pop3_client_create(const pop3_config_t *config) {
  if (!config || !config->host || config->host[0] == '\0' || config->port <= 0 ||
      config->port > 65535 || config->timeout_ms < 0 || (config->use_tls && config->use_stls)) {
    return NULL;
  }

  pop3_client_t *client = calloc(1, sizeof(pop3_client_t));
  if (!client) return NULL;

  client->config = *config;

  if (config->host) client->config.host = strdup(config->host);
  if (config->username) client->config.username = strdup(config->username);
  if (config->password) client->config.password = strdup(config->password);
  if (!client->config.host || (config->username && !client->config.username) ||
      (config->password && !client->config.password)) {
    free(client->config.host);
    free(client->config.username);
    free(client->config.password);
    free(client);
    return NULL;
  }

  if (email_cnet_transport_init(&client->transport, client->config.timeout_ms) != SALTS_OK) {
    free(client->config.host);
    free(client->config.username);
    free(client->config.password);
    free(client);
    return NULL;
  }
  client->config.timeout_ms = (int)client->transport.timeout_ms;

  return client;
}

void pop3_client_free(pop3_client_t *client) {
  if (!client) return;

  if (email_cnet_transport_destroy(&client->transport) != SALTS_OK) return;

  free(client->config.host);
  free(client->config.username);
  free(client->config.password);
  free(client);
}

/* ── Connection ────────────────────────────────────────────────────── */

int pop3_connect(pop3_client_t *client) {
  if (!client) return -1;

  pop3_reset_read_state(client);
  if (email_cnet_transport_connect(&client->transport, client->config.host,
                                   (uint16_t)client->config.port,
                                   client->config.use_tls) != SALTS_OK) {
    fmt(client->error_msg, sizeof(client->error_msg),
        "Failed to connect to {}:{} at {} (CNet status {})", client->config.host,
        client->config.port, email_cnet_transport_stage(&client->transport),
        email_cnet_transport_status(&client->transport));
    (void)email_cnet_transport_close(&client->transport);
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

    if (email_cnet_transport_start_tls(&client->transport, client->config.host) != SALTS_OK) {
      fmt(client->error_msg, sizeof(client->error_msg),
          "Failed to upgrade POP3 connection to TLS at {} (CNet status {})",
          email_cnet_transport_stage(&client->transport),
          email_cnet_transport_status(&client->transport));
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
  if (!client || !email_cnet_transport_is_connected(&client->transport)) {
    if (client) (void)email_cnet_transport_close(&client->transport);
    return;
  }

  // Send QUIT
  pop3_send_command(client, "QUIT");

  (void)email_cnet_transport_close(&client->transport);
}

int pop3_interrupt(pop3_client_t *client, int status) {
  return email_cnet_transport_interrupt(client != NULL ? &client->transport : NULL, status);
}

/* ── Mailbox Operations ────────────────────────────────────────────── */

int pop3_stat(pop3_client_t *client, int *total_size) {
  if (!client) return -1;

  if (pop3_send_command(client, "STAT") != 0) return -1;

  // Parse response: "+OK count size"
  int count = 0, size = 0;
  if (sscanf(client->line_buffer, "+OK %d %d", &count, &size) != 2) {
    fmt_text(client->error_msg, sizeof(client->error_msg), "Failed to parse STAT response");
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

int pop3_retrieve_raw(pop3_client_t *client, int msg_num, char **data, size_t *len) {
  char retr_cmd[64];
  char *raw_msg;
  size_t msg_len = 0;
  if (!client || !data || msg_num <= 0) return SALTS_EINVAL;
  *data = NULL;
  if (len) *len = 0;
  fmt(retr_cmd, sizeof(retr_cmd), "RETR {}", msg_num);
  if (pop3_send_command(client, retr_cmd) != 0) return SALTS_EIO;
  raw_msg = pop3_read_multiline(client, &msg_len);
  if (!raw_msg) return SALTS_EIO;
  *data = raw_msg;
  if (len) *len = msg_len;
  return SALTS_OK;
}

email_message_t *pop3_retrieve_message(pop3_client_t *client, int msg_num) {
  if (!client) return NULL;

  size_t msg_len = 0;
  char *raw_msg = NULL;
  if (pop3_retrieve_raw(client, msg_num, &raw_msg, &msg_len) != SALTS_OK) return NULL;

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
