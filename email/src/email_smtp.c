#include "email/email_smtp.h"
#include "CoroNet.h"
#include "base64_utils.h"
#include <fmt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── SMTP Client Structure ─────────────────────────────────────────── */

struct smtp_client_s {
  coro_context_t *ctx;
  smtp_config_t config;
  coro_socket_t *socket;
  char error_msg[512];
  int last_code;
  char read_buffer[4096];
  char response_buffer[4096];
  size_t read_buffer_len;
};

/* ── Helpers ───────────────────────────────────────────────────────── */

static void smtp_reset_read_state(smtp_client_t *client) {
  if (!client) return;
  client->read_buffer_len = 0;
  client->read_buffer[0] = '\0';
  client->response_buffer[0] = '\0';
}

static int smtp_read_line(smtp_client_t *client, char **line) {
  if (!client || !client->socket) return -1;

  while (1) {
    for (size_t i = 0; i < client->read_buffer_len; i++) {
      if (client->read_buffer[i] == '\n') {
        size_t line_len = i + 1;
        if (line_len >= sizeof(client->read_buffer)) {
          line_len = sizeof(client->read_buffer) - 1;
        }

        memcpy(client->response_buffer, client->read_buffer, line_len);
        client->response_buffer[line_len] = '\0';

        client->read_buffer_len -= line_len;
        memmove(client->read_buffer, client->read_buffer + line_len,
                client->read_buffer_len);
        client->read_buffer[client->read_buffer_len] = '\0';

        if (line) {
          *line = client->response_buffer;
        }
        return 0;
      }
    }

    char *data = NULL;
    size_t len = 0;
    int result = coro_socket_recv(client->socket, &data, &len);
    if (result != 0 || !data || len == 0) {
      fmt(client->error_msg, sizeof(client->error_msg), "Failed to read SMTP response");
      if (data) {
        coro_socket_free_recv(data);
      }
      return -1;
    }

    size_t space = sizeof(client->read_buffer) - 1 - client->read_buffer_len;
    if (len > space) {
      coro_socket_free_recv(data);
      fmt(client->error_msg, sizeof(client->error_msg), "SMTP response too large");
      return -1;
    }

    memcpy(client->read_buffer + client->read_buffer_len, data, len);
    client->read_buffer_len += len;
    client->read_buffer[client->read_buffer_len] = '\0';
    coro_socket_free_recv(data);
  }
}

static int smtp_read_response(smtp_client_t *client, int *code) {
  if (!client || !client->socket) return -1;

  char *line = NULL;
  if (smtp_read_line(client, &line) != 0) {
    return -1;
  }

  // Parse response code (first 3 digits)
  if (!isdigit((unsigned char)line[0]) ||
      !isdigit((unsigned char)line[1]) ||
      !isdigit((unsigned char)line[2])) {
    fmt(client->error_msg, sizeof(client->error_msg), "Invalid SMTP response: {}", line);
    return -1;
  }

  int response_code = (line[0] - '0') * 100 +
                      (line[1] - '0') * 10 +
                      (line[2] - '0');

  if (code) *code = response_code;
  client->last_code = response_code;

  while (line[3] != '\0' && line[3] == '-') {
    if (smtp_read_line(client, &line) != 0) {
      return -1;
    }

    if (!isdigit((unsigned char)line[0]) ||
        !isdigit((unsigned char)line[1]) ||
        !isdigit((unsigned char)line[2])) {
      fmt(client->error_msg, sizeof(client->error_msg), "Invalid SMTP response: {}", line);
      return -1;
    }

    client->last_code = (line[0] - '0') * 100 +
                        (line[1] - '0') * 10 +
                        (line[2] - '0');
    if (client->last_code != response_code) {
      fmt(client->error_msg, sizeof(client->error_msg), "Mismatched SMTP multi-line response: {}", line);
      return -1;
    }
  }

  fmt(client->response_buffer, sizeof(client->response_buffer), "{}", line);
  return response_code;
}

static int smtp_send_command(smtp_client_t *client, const char *cmd) {
  if (!client || !client->socket || !cmd) return -1;

  char buffer[1024];
  int len = fmt(buffer, sizeof(buffer), "{}\r\n", cmd);

  int rc = coro_socket_send(client->socket, buffer, len);
  if (rc != 0) {
    fmt(client->error_msg, sizeof(client->error_msg), "Failed to send SMTP command ({}): {}", rc, cmd);
    return -1;
  }

  return 0;
}

static int smtp_expect_code(smtp_client_t *client, int expected) {
  int code = smtp_read_response(client, NULL);
  if (code != expected) {
    fmt(client->error_msg, sizeof(client->error_msg), "Expected {}, got {}: {}", expected, code, client->read_buffer);
    return -1;
  }
  return 0;
}

/* ── Client Creation ───────────────────────────────────────────────── */

smtp_client_t *smtp_client_create(coro_context_t *ctx,
                                   const smtp_config_t *config) {
  if (!ctx || !config) return NULL;

  smtp_client_t *client = calloc(1, sizeof(smtp_client_t));
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

void smtp_client_free(smtp_client_t *client) {
  if (!client) return;

  if (client->socket) {
    coro_socket_destroy(client->socket);
  }

  free(client->config.host);
  free(client->config.username);
  free(client->config.password);
  free(client);
}

/* ── Connection ────────────────────────────────────────────────────── */

int smtp_connect(smtp_client_t *client) {
  int socket_type;

  if (!client) return -1;

  // Create socket
  socket_type = client->config.use_tls ? CORO_SOCKET_TLS : CORO_SOCKET_TCP_V4;
  client->socket = coro_socket_create(client->ctx, (coro_socket_type_t)socket_type);
  if (!client->socket) {
    fmt(client->error_msg, sizeof(client->error_msg), "Failed to create socket");
    return -1;
  }

  if (coro_socket_connect(client->socket, client->config.host,
                          client->config.port) != 0) {
    fmt(client->error_msg, sizeof(client->error_msg), "Failed to connect to {}:{}", client->config.host,
        client->config.port);
    coro_socket_destroy(client->socket);
    client->socket = NULL;
    return -1;
  }

  // Read 220 greeting
  if (smtp_expect_code(client, 220) != 0) {
    smtp_disconnect(client);
    return -1;
  }

  // Plain local SMTP does not need extension negotiation.
  int needs_extended_smtp =
      client->config.use_starttls ||
      (client->config.username != NULL && client->config.password != NULL);
  char hello_cmd[256];
  fmt(hello_cmd, sizeof(hello_cmd), "{} {}", needs_extended_smtp ? "EHLO" : "HELO", client->config.host);
  if (smtp_send_command(client, hello_cmd) != 0) {
    smtp_disconnect(client);
    return -1;
  }

  if (smtp_expect_code(client, 250) != 0) {
    smtp_disconnect(client);
    return -1;
  }

  // STARTTLS if needed
  if (client->config.use_starttls) {
    if (smtp_send_command(client, "STARTTLS") != 0) {
      smtp_disconnect(client);
      return -1;
    }

    if (smtp_expect_code(client, 220) != 0) {
      smtp_disconnect(client);
      return -1;
    }

    if (coro_socket_upgrade_tls(client->socket, client->config.host) != 0) {
      fmt(client->error_msg, sizeof(client->error_msg), "Failed to upgrade SMTP connection to TLS");
      smtp_disconnect(client);
      return -1;
    }
    smtp_reset_read_state(client);

    // Re-send EHLO after STARTTLS
    if (smtp_send_command(client, hello_cmd) != 0) {
      smtp_disconnect(client);
      return -1;
    }

    if (smtp_expect_code(client, 250) != 0) {
      smtp_disconnect(client);
      return -1;
    }
  }

  // AUTH if credentials provided
  if (client->config.username && client->config.password) {
    if (client->config.auth_method == SMTP_AUTH_PLAIN) {
      // AUTH PLAIN: base64("\0username\0password")
      char auth_str[512];
      size_t username_len = strlen(client->config.username);
      size_t password_len = strlen(client->config.password);

      // Build: \0username\0password
      size_t auth_len = 1 + username_len + 1 + password_len;
      if (auth_len >= sizeof(auth_str)) {
        smtp_disconnect(client);
        return -1;
      }

      auth_str[0] = '\0';
      memcpy(auth_str + 1, client->config.username, username_len);
      auth_str[1 + username_len] = '\0';
      memcpy(auth_str + 1 + username_len + 1, client->config.password, password_len);

      char *auth_b64 = NULL;
      if (tn_base64_encode((uint8_t *)auth_str, auth_len, &auth_b64) != 0) {
        smtp_disconnect(client);
        return -1;
      }

      char auth_cmd[1024];
      fmt(auth_cmd, sizeof(auth_cmd), "AUTH PLAIN {}", auth_b64);
      free(auth_b64);

      if (smtp_send_command(client, auth_cmd) != 0) {
        smtp_disconnect(client);
        return -1;
      }

      if (smtp_expect_code(client, 235) != 0) {
        smtp_disconnect(client);
        return -1;
      }
    } else if (client->config.auth_method == SMTP_AUTH_LOGIN) {
      // AUTH LOGIN
      if (smtp_send_command(client, "AUTH LOGIN") != 0) {
        smtp_disconnect(client);
        return -1;
      }

      if (smtp_expect_code(client, 334) != 0) {
        smtp_disconnect(client);
        return -1;
      }

      // Send username (base64)
      char *user_b64 = NULL;
      if (tn_base64_encode((uint8_t *)client->config.username,
                           strlen(client->config.username), &user_b64) != 0) {
        smtp_disconnect(client);
        return -1;
      }

      if (smtp_send_command(client, user_b64) != 0) {
        free(user_b64);
        smtp_disconnect(client);
        return -1;
      }
      free(user_b64);

      if (smtp_expect_code(client, 334) != 0) {
        smtp_disconnect(client);
        return -1;
      }

      // Send password (base64)
      char *pass_b64 = NULL;
      if (tn_base64_encode((uint8_t *)client->config.password,
                           strlen(client->config.password), &pass_b64) != 0) {
        smtp_disconnect(client);
        return -1;
      }

      if (smtp_send_command(client, pass_b64) != 0) {
        free(pass_b64);
        smtp_disconnect(client);
        return -1;
      }
      free(pass_b64);

      if (smtp_expect_code(client, 235) != 0) {
        smtp_disconnect(client);
        return -1;
      }
    }
  }

  return 0;
}

void smtp_disconnect(smtp_client_t *client) {
  if (!client || !client->socket) return;

  char saved_error[sizeof(client->error_msg)];
  saved_error[0] = '\0';
  if (client->error_msg[0] != '\0') {
    memcpy(saved_error, client->error_msg, sizeof(saved_error));
    saved_error[sizeof(saved_error) - 1] = '\0';
  }

  // Send QUIT command
  smtp_send_command(client, "QUIT");
  smtp_expect_code(client, 221);

  if (saved_error[0] != '\0') {
    memcpy(client->error_msg, saved_error, sizeof(client->error_msg));
    client->error_msg[sizeof(client->error_msg) - 1] = '\0';
  }

  coro_socket_destroy(client->socket);
  client->socket = NULL;
}

/* ── Send Email ────────────────────────────────────────────────────── */

int smtp_send_raw(smtp_client_t *client,
                  const char *from_email,
                  const char **to_emails,
                  int to_count,
                  const char *raw_message,
                  size_t message_len) {
  if (!client || !client->socket || !from_email || !to_emails || !raw_message) {
    return -1;
  }

  // MAIL FROM
  char mail_from[512];
  fmt(mail_from, sizeof(mail_from), "MAIL FROM:<{}>", from_email);
  if (smtp_send_command(client, mail_from) != 0) return -1;
  if (smtp_expect_code(client, 250) != 0) return -1;

  // RCPT TO (for each recipient)
  for (int i = 0; i < to_count; i++) {
    char rcpt_to[512];
    fmt(rcpt_to, sizeof(rcpt_to), "RCPT TO:<{}>", to_emails[i]);
    if (smtp_send_command(client, rcpt_to) != 0) return -1;
    if (smtp_expect_code(client, 250) != 0) return -1;
  }

  // DATA
  if (smtp_send_command(client, "DATA") != 0) return -1;
  if (smtp_expect_code(client, 354) != 0) return -1;

  // Send message body
  if (coro_socket_send(client->socket, raw_message, message_len) != 0) {
    fmt(client->error_msg, sizeof(client->error_msg), "Failed to send message body");
    return -1;
  }

  // Send ".\r\n" to end DATA
  if (smtp_send_command(client, ".") != 0) return -1;
  if (smtp_expect_code(client, 250) != 0) return -1;

  return 0;
}

int smtp_send_message(smtp_client_t *client, email_message_t *msg) {
  if (!client || !msg) return -1;

  // Build recipient list
  int to_count = 0;
  mime_address_t *addr = msg->to;
  while (addr) {
    to_count++;
    addr = addr->next;
  }

  addr = msg->cc;
  while (addr) {
    to_count++;
    addr = addr->next;
  }

  addr = msg->bcc;
  while (addr) {
    to_count++;
    addr = addr->next;
  }

  if (to_count == 0) {
    fmt(client->error_msg, sizeof(client->error_msg), "No recipients specified");
    return -1;
  }

  const char **to_emails = malloc(to_count * sizeof(char *));
  if (!to_emails) return -1;

  int idx = 0;
  addr = msg->to;
  while (addr) {
    to_emails[idx++] = addr->email;
    addr = addr->next;
  }

  addr = msg->cc;
  while (addr) {
    to_emails[idx++] = addr->email;
    addr = addr->next;
  }

  addr = msg->bcc;
  while (addr) {
    to_emails[idx++] = addr->email;
    addr = addr->next;
  }

  // Serialize message
  tstr_t raw_msg = email_message_to_string(msg);
  if (!raw_msg) {
    free(to_emails);
    return -1;
  }

  // Send
  int result = smtp_send_raw(client, msg->from->email, to_emails, to_count,
                             raw_msg, tstr_len(raw_msg));

  tstr_free(raw_msg);
  free(to_emails);

  return result;
}

/* ── Error Handling ────────────────────────────────────────────────── */

const char *smtp_get_error(smtp_client_t *client) {
  return client ? client->error_msg : "Invalid client";
}

int smtp_get_last_code(smtp_client_t *client) {
  return client ? client->last_code : 0;
}
