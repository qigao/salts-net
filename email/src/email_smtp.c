#include "email/email_smtp.h"
#include "CoroNet.h"
#include "base64_utils.h"
#include <fmt.h>
#include "turbo_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── SMTP Client Structure ─────────────────────────────────────────── */

struct smtp_client_s {
  coro_context_t *ctx;
  smtp_config_t config;
  coro_socket_t *socket;
  turbo_mutex_t socket_mutex;
  char error_msg[512];
  int last_code;
  char read_buffer[4096];      /* raw bytes received but not yet line-split */
  char response_buffer[4096];  /* last complete SMTP response line (set by smtp_read_response) */
  size_t read_buffer_len;
};

static void smtp_socket_publish(smtp_client_t *client, coro_socket_t *socket) {
  turbo_mutex_lock(&client->socket_mutex);
  client->socket = socket;
  turbo_mutex_unlock(&client->socket_mutex);
}

static coro_socket_t *smtp_socket_take(smtp_client_t *client) {
  coro_socket_t *socket;
  turbo_mutex_lock(&client->socket_mutex);
  socket = client->socket;
  client->socket = NULL;
  turbo_mutex_unlock(&client->socket_mutex);
  return socket;
}

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

  tstr_t wire_cmd = tstr_format("{}\r\n", cmd);
  if (!wire_cmd) {
    fmt(client->error_msg, sizeof(client->error_msg), "Failed to allocate SMTP command");
    return -1;
  }

  int rc = coro_socket_send(client->socket, wire_cmd, tstr_len(wire_cmd));
  if (rc != 0) {
    fmt(client->error_msg, sizeof(client->error_msg), "Failed to send SMTP command ({}): {}", rc, cmd);
    tstr_free(wire_cmd);
    return -1;
  }

  tstr_free(wire_cmd);
  return 0;
}

static int smtp_send_hello(smtp_client_t *client, int extended) {
  const char *ehlo_domain = client->config.client_hostname
                            ? client->config.client_hostname
                            : "[127.0.0.1]";
  tstr_t hello_cmd = tstr_format("{} {}", extended ? "EHLO" : "HELO", ehlo_domain);
  if (!hello_cmd) {
    fmt(client->error_msg, sizeof(client->error_msg), "Failed to allocate SMTP hello command");
    return -1;
  }

  int rc = smtp_send_command(client, hello_cmd);
  tstr_free(hello_cmd);
  return rc;
}

static int smtp_expect_code(smtp_client_t *client, int expected) {
  int code = smtp_read_response(client, NULL);
  if (code != expected) {
    fmt(client->error_msg, sizeof(client->error_msg), "Expected {}, got {}: {}", expected, code, client->response_buffer);
    return -1;
  }
  return 0;
}

static int smtp_send_dotted_body(smtp_client_t *client, const char *data, size_t len) {
  if (!client || !client->socket || !data || len == 0) return 0;

  size_t last_pos = 0;
  size_t i = 0;

  // If the very first character is a dot, escape it
  if (data[0] == '.') {
    int rc = coro_socket_send(client->socket, ".", 1);
    if (rc != 0) {
      fmt(client->error_msg, sizeof(client->error_msg), "Failed to send dot escape");
      return -1;
    }
  }

  while (i < len) {
    // Search for "\r\n."
    if (i + 2 < len && data[i] == '\r' && data[i+1] == '\n' && data[i+2] == '.') {
      // Send up to and including "\r\n"
      size_t chunk_len = i + 2 - last_pos;
      if (chunk_len > 0) {
        int rc = coro_socket_send(client->socket, data + last_pos, chunk_len);
        if (rc != 0) {
          fmt(client->error_msg, sizeof(client->error_msg), "Failed to send chunk");
          return -1;
        }
      }
      // Send the extra escape dot
      int rc = coro_socket_send(client->socket, ".", 1);
      if (rc != 0) {
        fmt(client->error_msg, sizeof(client->error_msg), "Failed to send dot escape");
        return -1;
      }
      last_pos = i + 2; // Next chunk starts at the original '.'
      i += 3;
    } else {
      i++;
    }
  }

  // Send remainder
  if (len - last_pos > 0) {
    int rc = coro_socket_send(client->socket, data + last_pos, len - last_pos);
    if (rc != 0) {
      fmt(client->error_msg, sizeof(client->error_msg), "Failed to send remainder");
      return -1;
    }
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
  turbo_mutex_init(&client->socket_mutex);
  client->config = *config;

  if (config->host) client->config.host = strdup(config->host);
  if (config->username) client->config.username = strdup(config->username);
  if (config->password) client->config.password = strdup(config->password);
  if (config->client_hostname) client->config.client_hostname = strdup(config->client_hostname);

  if (client->config.timeout_ms == 0) {
    client->config.timeout_ms = 30000;
  }

  return client;
}

void smtp_client_free(smtp_client_t *client) {
  if (!client) return;

  {
    coro_socket_t *socket = smtp_socket_take(client);
    if (socket) coro_socket_destroy(socket);
  }

  free(client->config.host);
  free(client->config.username);
  free(client->config.password);
  free(client->config.client_hostname);
  turbo_mutex_destroy(&client->socket_mutex);
  free(client);
}

/* ── Connection ────────────────────────────────────────────────────── */

int smtp_connect(smtp_client_t *client) {
  int socket_type;

  if (!client) return -1;

  // Create socket
  socket_type = client->config.use_tls ? CORO_SOCKET_TLS : CORO_SOCKET_TCP_V4;
  {
    coro_socket_t *socket = coro_socket_create(client->ctx, (coro_socket_type_t)socket_type);
    if (socket) smtp_socket_publish(client, socket);
  }
  if (!client->socket) {
    fmt(client->error_msg, sizeof(client->error_msg), "Failed to create socket");
    return -1;
  }

  if (coro_socket_connect(client->socket, client->config.host,
                          client->config.port) != 0) {
    fmt(client->error_msg, sizeof(client->error_msg), "Failed to connect to {}:{}", client->config.host,
        client->config.port);
    coro_socket_t *socket = smtp_socket_take(client);
    if (socket) coro_socket_destroy(socket);
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
  if (smtp_send_hello(client, needs_extended_smtp) != 0) {
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
    if (smtp_send_hello(client, needs_extended_smtp) != 0) {
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

      tstr_t auth_cmd = tstr_format("AUTH PLAIN {}", auth_b64);
      free(auth_b64);
      if (!auth_cmd) {
        smtp_disconnect(client);
        return -1;
      }

      if (smtp_send_command(client, auth_cmd) != 0) {
        tstr_free(auth_cmd);
        smtp_disconnect(client);
        return -1;
      }
      tstr_free(auth_cmd);

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
    } else if (client->config.auth_method == SMTP_AUTH_CRAM_MD5) {
      fmt(client->error_msg, sizeof(client->error_msg), "CRAM-MD5 authentication is not implemented");
      smtp_disconnect(client);
      return -1;
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

  {
    coro_socket_t *socket = smtp_socket_take(client);
    if (socket) coro_socket_destroy(socket);
  }
}

int smtp_interrupt(smtp_client_t *client, int status) {
  int rc;
  if (!client) return TURBO_EINVAL;
  turbo_mutex_lock(&client->socket_mutex);
  rc = client->socket ? coro_socket_interrupt_wait(client->socket, status) : TURBO_ENOTCONN;
  turbo_mutex_unlock(&client->socket_mutex);
  return rc;
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
  tstr_t mail_from = tstr_format("MAIL FROM:<{}>", from_email);
  if (!mail_from) return -1;
  if (smtp_send_command(client, mail_from) != 0) {
    tstr_free(mail_from);
    return -1;
  }
  tstr_free(mail_from);
  if (smtp_expect_code(client, 250) != 0) return -1;

  // RCPT TO (for each recipient, accepting 250/251/252)
  for (int i = 0; i < to_count; i++) {
    tstr_t rcpt_to = tstr_format("RCPT TO:<{}>", to_emails[i]);
    if (!rcpt_to) return -1;
    if (smtp_send_command(client, rcpt_to) != 0) {
      tstr_free(rcpt_to);
      return -1;
    }
    tstr_free(rcpt_to);
    
    int rcpt_code = smtp_read_response(client, NULL);
    if (rcpt_code != 250 && rcpt_code != 251 && rcpt_code != 252) {
      fmt(client->error_msg, sizeof(client->error_msg), 
          "Expected RCPT TO status 250/251/252, got {}: {}", rcpt_code, client->response_buffer);
      return -1;
    }
  }

  // DATA
  if (smtp_send_command(client, "DATA") != 0) return -1;
  if (smtp_expect_code(client, 354) != 0) return -1;

  // Send message body with dot escape (DATA transparency)
  if (smtp_send_dotted_body(client, raw_message, message_len) != 0) {
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
