#include "email/email_imap.h"
#include "email_cnet_transport.h"
#include <ctype.h>
#include <fmt.h>
#include <salts/error_codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { IMAP_MAX_LITERAL_BYTES = 64 * 1024 * 1024, IMAP_MAX_RESULT_COUNT = 4096 };

struct imap_client_s {
  imap_config_t config;
  email_cnet_transport_t transport;
  char error_msg[512];
  int tag_counter;
  char tag_buffer[16];
  char read_buffer[8192];
  char line_buffer[8192];
  size_t read_buffer_len;
};

/* ── Helpers ───────────────────────────────────────────────────────── */

static void imap_reset_read_state(imap_client_t *client) {
  if (!client) return;
  client->read_buffer_len = 0;
  client->read_buffer[0] = '\0';
  client->line_buffer[0] = '\0';
}

static int imap_parse_literal_size(imap_client_t *client, size_t *out_size) {
  const char *cursor;
  size_t value = 0u;
  if (!client || !out_size) return -1;
  cursor = strrchr(client->line_buffer, '{');
  if (!cursor || cursor[1] < '0' || cursor[1] > '9') {
    fmt_text(client->error_msg, sizeof(client->error_msg), "Invalid FETCH response: no literal");
    return -1;
  }
  cursor++;
  while (*cursor >= '0' && *cursor <= '9') {
    const size_t digit = (size_t)(*cursor - '0');
    if (value > (IMAP_MAX_LITERAL_BYTES - digit) / 10u) {
      fmt_text(client->error_msg, sizeof(client->error_msg), "IMAP literal exceeds size limit");
      return -1;
    }
    value = value * 10u + digit;
    cursor++;
  }
  if (*cursor != '}' || value == 0u || value > IMAP_MAX_LITERAL_BYTES) {
    fmt_text(client->error_msg, sizeof(client->error_msg), "Invalid IMAP literal size");
    return -1;
  }
  *out_size = value;
  return 0;
}

static char *imap_generate_tag(imap_client_t *client) {
  fmt(client->tag_buffer, sizeof(client->tag_buffer), "A{:04d}", ++client->tag_counter);
  return client->tag_buffer;
}

static int imap_read_line(imap_client_t *client) {
  if (!client || !email_cnet_transport_is_connected(&client->transport)) return -1;

  while (1) {
    size_t i;
    for (i = 0; i < client->read_buffer_len; i++) {
      if (client->read_buffer[i] == '\n') {
        size_t line_len = i + 1;
        if (line_len >= sizeof(client->line_buffer)) {
          fmt_text(client->error_msg, sizeof(client->error_msg), "IMAP response line too large");
          return -1;
        }

        memcpy(client->line_buffer, client->read_buffer, line_len);
        client->line_buffer[line_len] = '\0';

        client->read_buffer_len -= line_len;
        memmove(client->read_buffer, client->read_buffer + line_len, client->read_buffer_len);
        client->read_buffer[client->read_buffer_len] = '\0';

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
        fmt_text(client->error_msg, sizeof(client->error_msg), "Failed to read IMAP response");
        return -1;
      }

      client->read_buffer_len += len;
      client->read_buffer[client->read_buffer_len] = '\0';
    }
  }
}

static int imap_read_exact(imap_client_t *client, char *out, size_t len) {
  size_t total = 0;
  if (!client || !email_cnet_transport_is_connected(&client->transport) || !out) return -1;

  while (total < len) {
    if (client->read_buffer_len > 0) {
      size_t take = client->read_buffer_len;
      if (take > len - total) {
        take = len - total;
      }
      memcpy(out + total, client->read_buffer, take);
      total += take;
      client->read_buffer_len -= take;
      memmove(client->read_buffer, client->read_buffer + take, client->read_buffer_len);
      client->read_buffer[client->read_buffer_len] = '\0';
      continue;
    }

    {
      size_t chunk_len = 0;
      int result =
          email_cnet_transport_receive(&client->transport, out + total, len - total, &chunk_len);
      if (result != SALTS_OK || chunk_len == 0) {
        fmt_text(client->error_msg, sizeof(client->error_msg), "Failed to read IMAP literal");
        return -1;
      }
      total += chunk_len;
    }
  }

  return 0;
}

static int imap_read_response(imap_client_t *client, const char *expected_tag) {
  if (imap_read_line(client) != 0) return -1;

  // Parse response: "TAG OK/NO/BAD ..." or "* UNTAGGED ..."
  if (strlen(client->line_buffer) < 3) {
    fmt_text(client->error_msg, sizeof(client->error_msg), "Invalid IMAP response");
    return -1;
  }

  // Check for tagged response
  if (expected_tag && strncmp(client->line_buffer, expected_tag, strlen(expected_tag)) == 0) {
    // Tagged response: "A001 OK ..."
    const char *status = client->line_buffer + strlen(expected_tag) + 1;
    if (strncmp(status, "OK", 2) == 0) {
      return 0; // Success
    } else if (strncmp(status, "NO", 2) == 0) {
      fmt(client->error_msg, sizeof(client->error_msg), "IMAP NO: {}", status + 3);
      return -1;
    } else if (strncmp(status, "BAD", 3) == 0) {
      fmt(client->error_msg, sizeof(client->error_msg), "IMAP BAD: {}", status + 4);
      return -1;
    }
  }

  return 0;
}

static int imap_send_command(imap_client_t *client, const char *tag, const char *cmd) {
  if (!client || !email_cnet_transport_is_connected(&client->transport) || !tag || !cmd) return -1;

  char buffer[2048];
  int len = fmt(buffer, sizeof(buffer), "{} {}\r\n", tag, cmd);

  int rc = email_cnet_transport_send(&client->transport, buffer, (size_t)len);
  if (rc != SALTS_OK) {
    fmt(client->error_msg, sizeof(client->error_msg), "Failed to send IMAP command: {}", cmd);
    return -1;
  }

  return 0;
}

static int imap_command(imap_client_t *client, const char *cmd) {
  char *tag = imap_generate_tag(client);

  if (imap_send_command(client, tag, cmd) != 0) {
    return -1;
  }

  // Read responses until we get the tagged response
  while (1) {
    if (imap_read_response(client, tag) != 0) {
      return -1;
    }

    // Check if this is the tagged response
    if (strncmp(client->line_buffer, tag, strlen(tag)) == 0) {
      break;
    }
  }

  return 0;
}

/* ── Client Creation ───────────────────────────────────────────────── */

imap_client_t *imap_client_create(const imap_config_t *config) {
  if (!config || !config->host || config->host[0] == '\0' || config->port <= 0 ||
      config->port > 65535 || config->timeout_ms < 0 || (config->use_tls && config->use_starttls)) {
    return NULL;
  }

  imap_client_t *client = calloc(1, sizeof(imap_client_t));
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

void imap_client_free(imap_client_t *client) {
  if (!client) return;

  if (email_cnet_transport_destroy(&client->transport) != SALTS_OK) return;

  free(client->config.host);
  free(client->config.username);
  free(client->config.password);
  free(client);
}

/* ── Connection ────────────────────────────────────────────────────── */

int imap_connect(imap_client_t *client) {
  if (!client) return -1;

  imap_reset_read_state(client);
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

  // Read greeting: "* OK ..."
  if (imap_read_response(client, NULL) != 0) {
    imap_disconnect(client);
    return -1;
  }

  // STARTTLS if needed
  if (client->config.use_starttls) {
    if (imap_command(client, "STARTTLS") != 0) {
      imap_disconnect(client);
      return -1;
    }

    if (email_cnet_transport_start_tls(&client->transport, client->config.host) != SALTS_OK) {
      fmt(client->error_msg, sizeof(client->error_msg),
          "Failed to upgrade IMAP connection to TLS at {} (CNet status {})",
          email_cnet_transport_stage(&client->transport),
          email_cnet_transport_status(&client->transport));
      imap_disconnect(client);
      return -1;
    }
    imap_reset_read_state(client);
  }

  if (client->config.username && client->config.password) {
    // LOGIN
    char login_cmd[1024];
    fmt(login_cmd, sizeof(login_cmd), "LOGIN {} {}", client->config.username,
        client->config.password);

    if (imap_command(client, login_cmd) != 0) {
      imap_disconnect(client);
      return -1;
    }
  }

  return 0;
}

void imap_disconnect(imap_client_t *client) {
  if (!client || !email_cnet_transport_is_connected(&client->transport)) {
    if (client) (void)email_cnet_transport_close(&client->transport);
    return;
  }

  // Send LOGOUT
  imap_command(client, "LOGOUT");

  (void)email_cnet_transport_close(&client->transport);
}

/* ── Mailbox Operations ────────────────────────────────────────────── */

char **imap_list_mailboxes(imap_client_t *client, const char *reference, const char *pattern,
                           int *count) {
  if (!client) return NULL;
  if (count) *count = 0;

  char list_cmd[512];
  fmt(list_cmd, sizeof(list_cmd), "LIST \"{}\" \"{}\"", reference ? reference : "",
      pattern ? pattern : "*");

  char *tag = imap_generate_tag(client);
  if (imap_send_command(client, tag, list_cmd) != 0) {
    return NULL;
  }

  // Collect LIST responses
  char **mailboxes = NULL;
  int capacity = 16;
  int n = 0;
  mailboxes = malloc(capacity * sizeof(char *));
  if (!mailboxes) return NULL;

  while (1) {
    if (imap_read_line(client) != 0) {
      for (int i = 0; i < n; i++)
        free(mailboxes[i]);
      free(mailboxes);
      return NULL;
    }

    // Check for tagged response (end)
    if (strncmp(client->line_buffer, tag, strlen(tag)) == 0) {
      break;
    }

    // Parse untagged LIST response: * LIST (...) "delimiter" "name"
    if (strncmp(client->line_buffer, "* LIST", 6) == 0) {
      // Find last quoted string (mailbox name)
      char *last_quote = strrchr(client->line_buffer, '"');
      if (last_quote && last_quote > client->line_buffer) {
        char *start_quote = last_quote - 1;
        while (start_quote > client->line_buffer && *start_quote != '"') {
          start_quote--;
        }
        if (*start_quote == '"') {
          size_t name_len = last_quote - start_quote - 1;
          char *name = malloc(name_len + 1u);
          if (!name) {
            for (int i = 0; i < n; i++)
              free(mailboxes[i]);
            free(mailboxes);
            return NULL;
          }
          memcpy(name, start_quote + 1, name_len);
          name[name_len] = '\0';

          if (n >= capacity) {
            char **new_mailboxes;
            if (capacity >= IMAP_MAX_RESULT_COUNT) {
              free(name);
              for (int i = 0; i < n; i++)
                free(mailboxes[i]);
              free(mailboxes);
              fmt_text(client->error_msg, sizeof(client->error_msg), "Too many IMAP mailboxes");
              return NULL;
            }
            capacity *= 2;
            new_mailboxes = realloc(mailboxes, (size_t)capacity * sizeof(char *));
            if (!new_mailboxes) {
              free(name);
              for (int i = 0; i < n; i++)
                free(mailboxes[i]);
              free(mailboxes);
              return NULL;
            }
            mailboxes = new_mailboxes;
          }
          mailboxes[n++] = name;
        }
      }
    }
  }

  if (count) *count = n;
  return mailboxes;
}

imap_mailbox_t *imap_select_mailbox(imap_client_t *client, const char *mailbox) {
  if (!client || !mailbox) return NULL;

  char select_cmd[512];
  fmt(select_cmd, sizeof(select_cmd), "SELECT \"{}\"", mailbox);

  char *tag = imap_generate_tag(client);
  if (imap_send_command(client, tag, select_cmd) != 0) {
    return NULL;
  }

  imap_mailbox_t *info = calloc(1, sizeof(imap_mailbox_t));
  if (!info) return NULL;
  info->name = strdup(mailbox);
  if (!info->name) {
    free(info);
    return NULL;
  }

  // Parse SELECT responses
  while (1) {
    if (imap_read_line(client) != 0) {
      free(info->name);
      free(info);
      return NULL;
    }

    // Check for tagged response (end)
    if (strncmp(client->line_buffer, tag, strlen(tag)) == 0) {
      break;
    }

    // Parse untagged responses
    if (client->line_buffer[0] == '*') {
      // * 123 EXISTS
      if (strstr(client->line_buffer, "EXISTS")) {
        sscanf(client->line_buffer, "* %d EXISTS", &info->exists);
      }
      // * 5 RECENT
      else if (strstr(client->line_buffer, "RECENT")) {
        sscanf(client->line_buffer, "* %d RECENT", &info->recent);
      }
      // * OK [UNSEEN 12]
      else if (strstr(client->line_buffer, "UNSEEN")) {
        char *unseen_str = strstr(client->line_buffer, "UNSEEN");
        sscanf(unseen_str, "UNSEEN %d", &info->unseen);
      }
      // * OK [UIDNEXT 4392]
      else if (strstr(client->line_buffer, "UIDNEXT")) {
        char *uidnext_str = strstr(client->line_buffer, "UIDNEXT");
        sscanf(uidnext_str, "UIDNEXT %d", &info->uidnext);
      }
      // * OK [UIDVALIDITY 3857529045]
      else if (strstr(client->line_buffer, "UIDVALIDITY")) {
        char *uidval_str = strstr(client->line_buffer, "UIDVALIDITY");
        sscanf(uidval_str, "UIDVALIDITY %d", &info->uidvalidity);
      }
    }
  }

  return info;
}

int imap_create_mailbox(imap_client_t *client, const char *mailbox) {
  if (!client || !mailbox) return -1;

  char create_cmd[512];
  fmt(create_cmd, sizeof(create_cmd), "CREATE \"{}\"", mailbox);

  return imap_command(client, create_cmd);
}

int imap_delete_mailbox(imap_client_t *client, const char *mailbox) {
  if (!client || !mailbox) return -1;

  char delete_cmd[512];
  fmt(delete_cmd, sizeof(delete_cmd), "DELETE \"{}\"", mailbox);

  return imap_command(client, delete_cmd);
}

/* ── Message Operations ────────────────────────────────────────────── */

int *imap_search(imap_client_t *client, const char *criteria, int *count) {
  if (!client || !criteria) return NULL;
  if (count) *count = 0;

  char search_cmd[1024];
  fmt(search_cmd, sizeof(search_cmd), "SEARCH {}", criteria);

  char *tag = imap_generate_tag(client);
  if (imap_send_command(client, tag, search_cmd) != 0) {
    return NULL;
  }

  int *results = NULL;
  int capacity = 64;
  int n = 0;
  results = malloc(capacity * sizeof(int));
  if (!results) return NULL;

  // Read responses
  while (1) {
    if (imap_read_line(client) != 0) {
      free(results);
      return NULL;
    }

    // Check for tagged response (end)
    if (strncmp(client->line_buffer, tag, strlen(tag)) == 0) {
      break;
    }

    // Parse SEARCH response: * SEARCH 1 2 3 4 5
    if (strncmp(client->line_buffer, "* SEARCH", 8) == 0) {
      char *ptr = client->line_buffer + 8;
      while (*ptr) {
        while (*ptr == ' ')
          ptr++;
        if (isdigit(*ptr)) {
          int num = atoi(ptr);
          if (n >= capacity) {
            int *new_results;
            if (capacity >= IMAP_MAX_RESULT_COUNT) {
              free(results);
              fmt_text(client->error_msg, sizeof(client->error_msg),
                       "Too many IMAP search results");
              return NULL;
            }
            capacity *= 2;
            new_results = realloc(results, (size_t)capacity * sizeof(int));
            if (!new_results) {
              free(results);
              return NULL;
            }
            results = new_results;
          }
          results[n++] = num;
          while (isdigit(*ptr))
            ptr++;
        } else {
          break;
        }
      }
    }
  }

  if (count) *count = n;
  return results;
}

email_message_t *imap_fetch_message(imap_client_t *client, int seq_num) {
  if (!client) return NULL;

  char fetch_cmd[256];
  fmt(fetch_cmd, sizeof(fetch_cmd), "FETCH {} BODY[]", seq_num);

  char *tag = imap_generate_tag(client);
  if (imap_send_command(client, tag, fetch_cmd) != 0) {
    return NULL;
  }

  // Read FETCH response: * 1 FETCH (BODY[] {size}
  if (imap_read_line(client) != 0) {
    return NULL;
  }

  // Parse literal size: BODY[] {1234}
  size_t literal_size = 0u;
  if (imap_parse_literal_size(client, &literal_size) != 0) return NULL;

  // Read literal data
  char *message_data = malloc(literal_size + 1u);
  size_t total_read = 0u;
  if (!message_data) return NULL;

  while (total_read < literal_size) {
    size_t to_read = literal_size - total_read;
    if (imap_read_exact(client, message_data + total_read, to_read) != 0) {
      free(message_data);
      return NULL;
    }
    total_read = literal_size;
  }
  message_data[literal_size] = '\0';

  // Parse message using mime_parser
  mem_pool_t pool;
  mem_init(&pool, literal_size + 4096);
  email_message_t *msg = email_message_parse(&pool, message_data, literal_size);
  free(message_data);

  if (!msg) {
    mem_destroy(&pool);
    fmt_text(client->error_msg, sizeof(client->error_msg), "Failed to parse message");
    return NULL;
  }

  // Read closing parenthesis and tagged response
  while (1) {
    if (imap_read_line(client) != 0) {
      email_message_free(msg);
      mem_destroy(&pool);
      return NULL;
    }
    if (strncmp(client->line_buffer, tag, strlen(tag)) == 0) {
      break;
    }
  }

  return msg;
}

email_message_t *imap_fetch_message_uid(imap_client_t *client, int uid) {
  if (!client) return NULL;

  char fetch_cmd[256];
  fmt(fetch_cmd, sizeof(fetch_cmd), "UID FETCH {} BODY[]", uid);

  char *tag = imap_generate_tag(client);
  if (imap_send_command(client, tag, fetch_cmd) != 0) {
    return NULL;
  }

  // Read FETCH response
  if (imap_read_line(client) != 0) {
    return NULL;
  }

  // Parse literal size
  size_t literal_size = 0u;
  if (imap_parse_literal_size(client, &literal_size) != 0) return NULL;

  // Read literal data
  char *message_data = malloc(literal_size + 1u);
  size_t total_read = 0u;
  if (!message_data) return NULL;

  while (total_read < literal_size) {
    size_t to_read = literal_size - total_read;
    if (imap_read_exact(client, message_data + total_read, to_read) != 0) {
      free(message_data);
      return NULL;
    }
    total_read = literal_size;
  }
  message_data[literal_size] = '\0';

  // Parse message
  mem_pool_t pool;
  mem_init(&pool, literal_size + 4096);
  email_message_t *msg = email_message_parse(&pool, message_data, literal_size);
  free(message_data);

  if (!msg) {
    mem_destroy(&pool);
    fmt_text(client->error_msg, sizeof(client->error_msg), "Failed to parse message");
    return NULL;
  }

  // Read closing and tagged response
  while (1) {
    if (imap_read_line(client) != 0) {
      email_message_free(msg);
      mem_destroy(&pool);
      return NULL;
    }
    if (strncmp(client->line_buffer, tag, strlen(tag)) == 0) {
      break;
    }
  }

  return msg;
}

imap_message_info_t *imap_fetch_info(imap_client_t *client, int seq_num) {
  if (!client) return NULL;

  char fetch_cmd[256];
  fmt(fetch_cmd, sizeof(fetch_cmd), "FETCH {} (UID FLAGS RFC822.SIZE ENVELOPE)", seq_num);

  char *tag = imap_generate_tag(client);
  if (imap_send_command(client, tag, fetch_cmd) != 0) {
    return NULL;
  }

  imap_message_info_t *info = calloc(1, sizeof(imap_message_info_t));
  info->seq_num = seq_num;

  // Read FETCH response
  while (1) {
    if (imap_read_line(client) != 0) {
      free(info->flags);
      free(info->envelope);
      free(info);
      return NULL;
    }

    // Check for tagged response (end)
    if (strncmp(client->line_buffer, tag, strlen(tag)) == 0) {
      break;
    }

    // Parse FETCH response: * 1 FETCH (FLAGS (\Seen) RFC822.SIZE 1234 ...)
    if (strstr(client->line_buffer, "FETCH")) {
      // Extract FLAGS
      char *flags_start = strstr(client->line_buffer, "FLAGS (");
      if (flags_start) {
        flags_start += 7;
        char *flags_end = strchr(flags_start, ')');
        if (flags_end) {
          size_t flags_len = flags_end - flags_start;
          info->flags = malloc(flags_len + 1);
          memcpy(info->flags, flags_start, flags_len);
          info->flags[flags_len] = '\0';
        }
      }

      // Extract RFC822.SIZE
      char *size_str = strstr(client->line_buffer, "RFC822.SIZE");
      if (size_str) {
        sscanf(size_str, "RFC822.SIZE %d", &info->size);
      }

      // Extract UID if present
      char *uid_str = strstr(client->line_buffer, "UID");
      if (uid_str) {
        sscanf(uid_str, "UID %d", &info->uid);
      }
    }
  }

  return info;
}

int imap_set_flags(imap_client_t *client, int seq_num, const char *flags) {
  if (!client || !flags) return -1;

  char store_cmd[512];
  fmt(store_cmd, sizeof(store_cmd), "STORE {} FLAGS ({})", seq_num, flags);

  return imap_command(client, store_cmd);
}

int imap_delete_message(imap_client_t *client, int seq_num) {
  return imap_set_flags(client, seq_num, "\\Deleted");
}

int imap_expunge(imap_client_t *client) {
  if (!client) return -1;
  return imap_command(client, "EXPUNGE");
}

/* ── Error Handling ────────────────────────────────────────────────── */

const char *imap_get_error(imap_client_t *client) {
  return client ? client->error_msg : "Invalid client";
}
