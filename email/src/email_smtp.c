#include "email/email_smtp.h"
#include "base64_utils.h"
#include "email_cnet_transport.h"
#include <ctype.h>
#include <fmt.h>
#include <salts/error_codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── SMTP Client Structure ─────────────────────────────────────────── */

struct smtp_client_s {
  smtp_config_t config;
  email_cnet_transport_t transport;
  char error_msg[512];
  int last_code;
  char read_buffer[4096];     /* raw bytes received but not yet line-split */
  char response_buffer[4096]; /* last complete SMTP response line (set by smtp_read_response) */
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
  if (!client || !email_cnet_transport_is_connected(&client->transport)) return -1;

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
        memmove(client->read_buffer, client->read_buffer + line_len, client->read_buffer_len);
        client->read_buffer[client->read_buffer_len] = '\0';

        if (line) {
          *line = client->response_buffer;
        }
        return 0;
      }
    }

    size_t len = 0;
    size_t space = sizeof(client->read_buffer) - 1 - client->read_buffer_len;
    int result = email_cnet_transport_receive(
        &client->transport, client->read_buffer + client->read_buffer_len, space, &len);
    if (result != SALTS_OK || len == 0) {
      fmt(client->error_msg, sizeof(client->error_msg),
          "Failed to read SMTP response at {} (CNet status {})",
          email_cnet_transport_stage(&client->transport), result);
      return -1;
    }

    client->read_buffer_len += len;
    client->read_buffer[client->read_buffer_len] = '\0';
  }
}

static int smtp_read_response(smtp_client_t *client, int *code) {
  if (!client || !email_cnet_transport_is_connected(&client->transport)) return -1;

  char *line = NULL;
  if (smtp_read_line(client, &line) != 0) {
    return -1;
  }

  // Parse response code (first 3 digits)
  if (!isdigit((unsigned char)line[0]) || !isdigit((unsigned char)line[1]) ||
      !isdigit((unsigned char)line[2])) {
    fmt(client->error_msg, sizeof(client->error_msg), "Invalid SMTP response: {}", line);
    return -1;
  }

  int response_code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');

  if (code) *code = response_code;
  client->last_code = response_code;

  while (line[3] != '\0' && line[3] == '-') {
    if (smtp_read_line(client, &line) != 0) {
      return -1;
    }

    if (!isdigit((unsigned char)line[0]) || !isdigit((unsigned char)line[1]) ||
        !isdigit((unsigned char)line[2])) {
      fmt(client->error_msg, sizeof(client->error_msg), "Invalid SMTP response: {}", line);
      return -1;
    }

    client->last_code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
    if (client->last_code != response_code) {
      fmt(client->error_msg, sizeof(client->error_msg), "Mismatched SMTP multi-line response: {}",
          line);
      return -1;
    }
  }

  fmt(client->response_buffer, sizeof(client->response_buffer), "{}", line);
  return response_code;
}

static int smtp_send_command(smtp_client_t *client, const char *cmd) {
  if (!client || !email_cnet_transport_is_connected(&client->transport) || !cmd) return -1;

  tstr wire_cmd = tstr_format("{}\r\n", cmd);
  if (!wire_cmd) {
    fmt_text(client->error_msg, sizeof(client->error_msg), "Failed to allocate SMTP command");
    return -1;
  }

  int rc = email_cnet_transport_send(&client->transport, wire_cmd, tstr_len(wire_cmd));
  if (rc != SALTS_OK) {
    fmt(client->error_msg, sizeof(client->error_msg), "Failed to send SMTP command ({}): {}", rc,
        cmd);
    tstr_free(wire_cmd);
    return -1;
  }

  tstr_free(wire_cmd);
  return 0;
}

static int smtp_send_hello(smtp_client_t *client, int extended) {
  const char *ehlo_domain =
      client->config.client_hostname ? client->config.client_hostname : "[127.0.0.1]";
  tstr hello_cmd = tstr_format("{} {}", extended ? "EHLO" : "HELO", ehlo_domain);
  if (!hello_cmd) {
    fmt_text(client->error_msg, sizeof(client->error_msg), "Failed to allocate SMTP hello command");
    return -1;
  }

  int rc = smtp_send_command(client, hello_cmd);
  tstr_free(hello_cmd);
  return rc;
}

static int smtp_expect_code(smtp_client_t *client, int expected) {
  int code = smtp_read_response(client, NULL);
  if (code != expected) {
    fmt(client->error_msg, sizeof(client->error_msg), "Expected {}, got {}: {}", expected, code,
        client->response_buffer);
    return -1;
  }
  return 0;
}

static int smtp_send_dotted_body(smtp_client_t *client, const char *data, size_t len) {
  if (!client || !email_cnet_transport_is_connected(&client->transport) || !data || len == 0)
    return 0;

  size_t last_pos = 0;
  size_t i = 0;

  // If the very first character is a dot, escape it
  if (data[0] == '.') {
    int rc = email_cnet_transport_send(&client->transport, ".", 1);
    if (rc != SALTS_OK) {
      fmt_text(client->error_msg, sizeof(client->error_msg), "Failed to send dot escape");
      return -1;
    }
  }

  while (i < len) {
    // Search for "\r\n."
    if (i + 2 < len && data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '.') {
      // Send up to and including "\r\n"
      size_t chunk_len = i + 2 - last_pos;
      if (chunk_len > 0) {
        int rc = email_cnet_transport_send(&client->transport, data + last_pos, chunk_len);
        if (rc != SALTS_OK) {
          fmt_text(client->error_msg, sizeof(client->error_msg), "Failed to send chunk");
          return -1;
        }
      }
      // Send the extra escape dot
      int rc = email_cnet_transport_send(&client->transport, ".", 1);
      if (rc != SALTS_OK) {
        fmt_text(client->error_msg, sizeof(client->error_msg), "Failed to send dot escape");
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
    int rc = email_cnet_transport_send(&client->transport, data + last_pos, len - last_pos);
    if (rc != SALTS_OK) {
      fmt_text(client->error_msg, sizeof(client->error_msg), "Failed to send remainder");
      return -1;
    }
  }

  return 0;
}

/* ── Client Creation ───────────────────────────────────────────────── */

smtp_client_t *smtp_client_create(const smtp_config_t *config) {
  if (!config || !config->host || config->host[0] == '\0' || config->port <= 0 ||
      config->port > 65535 || config->timeout_ms < 0 || (config->use_tls && config->use_starttls)) {
    return NULL;
  }

  smtp_client_t *client = calloc(1, sizeof(smtp_client_t));
  if (!client) return NULL;

  client->config = *config;

  if (config->host) client->config.host = strdup(config->host);
  if (config->username) client->config.username = strdup(config->username);
  if (config->password) client->config.password = strdup(config->password);
  if (config->client_hostname) client->config.client_hostname = strdup(config->client_hostname);
  if (!client->config.host || (config->username && !client->config.username) ||
      (config->password && !client->config.password) ||
      (config->client_hostname && !client->config.client_hostname)) {
    free(client->config.host);
    free(client->config.username);
    free(client->config.password);
    free(client->config.client_hostname);
    free(client);
    return NULL;
  }

  if (email_cnet_transport_init(&client->transport, client->config.timeout_ms) != SALTS_OK) {
    free(client->config.host);
    free(client->config.username);
    free(client->config.password);
    free(client->config.client_hostname);
    free(client);
    return NULL;
  }
  client->config.timeout_ms = (int)client->transport.timeout_ms;

  return client;
}

void smtp_client_free(smtp_client_t *client) {
  if (!client) return;

  if (email_cnet_transport_destroy(&client->transport) != SALTS_OK) return;

  free(client->config.host);
  free(client->config.username);
  free(client->config.password);
  free(client->config.client_hostname);
  free(client);
}

/* ── Connection ────────────────────────────────────────────────────── */

int smtp_connect(smtp_client_t *client) {
  if (!client) return -1;

  smtp_reset_read_state(client);
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

  // Read 220 greeting
  if (smtp_expect_code(client, 220) != 0) {
    smtp_disconnect(client);
    return -1;
  }

  // Plain local SMTP does not need extension negotiation.
  int needs_extended_smtp = client->config.use_starttls ||
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

    if (email_cnet_transport_start_tls(&client->transport, client->config.host) != SALTS_OK) {
      fmt(client->error_msg, sizeof(client->error_msg),
          "Failed to upgrade SMTP connection to TLS at {} (CNet status {})",
          email_cnet_transport_stage(&client->transport),
          email_cnet_transport_status(&client->transport));
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

      tstr auth_cmd = tstr_format("AUTH PLAIN {}", auth_b64);
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
      if (tn_base64_encode((uint8_t *)client->config.username, strlen(client->config.username),
                           &user_b64) != 0) {
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
      if (tn_base64_encode((uint8_t *)client->config.password, strlen(client->config.password),
                           &pass_b64) != 0) {
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
      fmt_text(client->error_msg, sizeof(client->error_msg),
               "CRAM-MD5 authentication is not implemented");
      smtp_disconnect(client);
      return -1;
    }
  }

  return 0;
}

void smtp_disconnect(smtp_client_t *client) {
  if (!client || !email_cnet_transport_is_connected(&client->transport)) {
    if (client) (void)email_cnet_transport_close(&client->transport);
    return;
  }

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

  (void)email_cnet_transport_close(&client->transport);
}

int smtp_interrupt(smtp_client_t *client, int status) {
  return email_cnet_transport_interrupt(client != NULL ? &client->transport : NULL, status);
}

/* ── Send Email ────────────────────────────────────────────────────── */

int smtp_send_raw(smtp_client_t *client, const char *from_email, const char **to_emails,
                  int to_count, const char *raw_message, size_t message_len) {
  if (!client || !email_cnet_transport_is_connected(&client->transport) || !from_email ||
      !to_emails || !raw_message) {
    return -1;
  }

  // MAIL FROM
  tstr mail_from = tstr_format("MAIL FROM:<{}>", from_email);
  if (!mail_from) return -1;
  if (smtp_send_command(client, mail_from) != 0) {
    tstr_free(mail_from);
    return -1;
  }
  tstr_free(mail_from);
  if (smtp_expect_code(client, 250) != 0) return -1;

  // RCPT TO (for each recipient, accepting 250/251/252)
  for (int i = 0; i < to_count; i++) {
    tstr rcpt_to = tstr_format("RCPT TO:<{}>", to_emails[i]);
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
    fmt_text(client->error_msg, sizeof(client->error_msg), "No recipients specified");
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
  tstr raw_msg = email_message_to_string(msg);
  if (!raw_msg) {
    free(to_emails);
    return -1;
  }

  // Send
  int result =
      smtp_send_raw(client, msg->from->email, to_emails, to_count, raw_msg, tstr_len(raw_msg));

  tstr_free(raw_msg);
  free(to_emails);

  return result;
}

/* ── Error Handling ────────────────────────────────────────────────── */

const char *smtp_get_error(smtp_client_t *client) {
  return client ? client->error_msg : "Invalid client";
}

int smtp_get_last_code(smtp_client_t *client) { return client ? client->last_code : 0; }
