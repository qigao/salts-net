#include "email/email_imap.h"
#include "CoroNet.h"
#include <fmt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

struct imap_client_s {
  coro_context_t *ctx;
  imap_config_t config;
  coro_socket_t *socket;
  char error_msg[512];
  int tag_counter;
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

static char *imap_generate_tag(imap_client_t *client) {
  static char tag[16];
  fmt(tag, sizeof(tag), "A{:04d}", ++client->tag_counter);
  return tag;
}

static int imap_read_line(imap_client_t *client) {
  if (!client || !client->socket) return -1;

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
        memmove(client->read_buffer, client->read_buffer + line_len,
                client->read_buffer_len);
        client->read_buffer[client->read_buffer_len] = '\0';

        return 0;
      }
    }

    {
      char *data = NULL;
      size_t len = 0;
      size_t free_space;
      int result = coro_socket_recv(client->socket, &data, &len);
      if (result != 0 || !data || len == 0) {
        if (data) {
          coro_socket_free_recv(data);
        }
        fmt_text(client->error_msg, sizeof(client->error_msg), "Failed to read IMAP response");
        return -1;
      }

      free_space = sizeof(client->read_buffer) - 1 - client->read_buffer_len;
      if (len > free_space) {
        coro_socket_free_recv(data);
        fmt_text(client->error_msg, sizeof(client->error_msg), "IMAP response buffer overflow");
        return -1;
      }

      memcpy(client->read_buffer + client->read_buffer_len, data, len);
      client->read_buffer_len += len;
      client->read_buffer[client->read_buffer_len] = '\0';
      coro_socket_free_recv(data);
    }
  }
}

static int imap_read_exact(imap_client_t *client, char *out, size_t len) {
  size_t total = 0;
  if (!client || !client->socket || !out) return -1;

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
      char *data = NULL;
      size_t chunk_len = 0;
      int result = coro_socket_recv(client->socket, &data, &chunk_len);
      if (result != 0 || !data || chunk_len == 0) {
        if (data) {
          coro_socket_free_recv(data);
        }
        fmt_text(client->error_msg, sizeof(client->error_msg), "Failed to read IMAP literal");
        return -1;
      }

      if (chunk_len > len - total) {
        size_t remaining = len - total;
        size_t extra = chunk_len - remaining;
        memcpy(out + total, data, remaining);
        total += remaining;
        if (extra > sizeof(client->read_buffer) - 1) {
          coro_socket_free_recv(data);
          fmt_text(client->error_msg, sizeof(client->error_msg), "IMAP literal overflow");
          return -1;
        }
        memcpy(client->read_buffer, data + remaining, extra);
        client->read_buffer_len = extra;
        client->read_buffer[client->read_buffer_len] = '\0';
      } else {
        memcpy(out + total, data, chunk_len);
        total += chunk_len;
      }
      coro_socket_free_recv(data);
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
  if (!client || !client->socket || !tag || !cmd) return -1;

  char buffer[2048];
  int len = fmt(buffer, sizeof(buffer), "{} {}\r\n", tag, cmd);

  int rc = coro_socket_send(client->socket, buffer, len);
  if (rc != 0) {
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

imap_client_t *imap_client_create(coro_context_t *ctx,
                                   const imap_config_t *config) {
  if (!ctx || !config) return NULL;

  imap_client_t *client = calloc(1, sizeof(imap_client_t));
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

void imap_client_free(imap_client_t *client) {
  if (!client) return;

  if (client->socket) coro_socket_destroy(client->socket);

  free(client->config.host);
  free(client->config.username);
  free(client->config.password);
  free(client);
}

/* ── Connection ────────────────────────────────────────────────────── */

int imap_connect(imap_client_t *client) {
  int socket_type;

  if (!client) return -1;

  // Create socket
  socket_type = client->config.use_tls ? CORO_SOCKET_TLS : CORO_SOCKET_TCP_V4;
  client->socket = coro_socket_create(client->ctx, (coro_socket_type_t)socket_type);
  if (!client->socket) {
    fmt_text(client->error_msg, sizeof(client->error_msg), "Failed to create socket");
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

    if (coro_socket_upgrade_tls(client->socket, client->config.host) != 0) {
      fmt_text(client->error_msg, sizeof(client->error_msg), "Failed to upgrade IMAP connection to TLS");
      imap_disconnect(client);
      return -1;
    }
    imap_reset_read_state(client);
  }

  if (client->config.username && client->config.password) {
    // LOGIN
    char login_cmd[1024];
    fmt(login_cmd, sizeof(login_cmd), "LOGIN {} {}", client->config.username, client->config.password);

    if (imap_command(client, login_cmd) != 0) {
      imap_disconnect(client);
      return -1;
    }
  }

  return 0;
}

void imap_disconnect(imap_client_t *client) {
  if (!client || !client->socket) return;

  // Send LOGOUT
  imap_command(client, "LOGOUT");

  coro_socket_destroy(client->socket);
  client->socket = NULL;
}

/* ── Mailbox Operations ────────────────────────────────────────────── */

char **imap_list_mailboxes(imap_client_t *client,
                            const char *reference,
                            const char *pattern,
                            int *count) {
  if (!client) return NULL;

  char list_cmd[512];
  fmt(list_cmd, sizeof(list_cmd), "LIST \"{}\" \"{}\"", reference ? reference : "", pattern ? pattern : "*");

  char *tag = imap_generate_tag(client);
  if (imap_send_command(client, tag, list_cmd) != 0) {
    return NULL;
  }

  // Collect LIST responses
  char **mailboxes = NULL;
  int capacity = 16;
  int n = 0;
  mailboxes = malloc(capacity * sizeof(char *));

  while (1) {
    if (imap_read_line(client) != 0) {
      for (int i = 0; i < n; i++) free(mailboxes[i]);
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
          char *name = malloc(name_len + 1);
          memcpy(name, start_quote + 1, name_len);
          name[name_len] = '\0';

          if (n >= capacity) {
            capacity *= 2;
            mailboxes = realloc(mailboxes, capacity * sizeof(char *));
          }
          mailboxes[n++] = name;
        }
      }
    }
  }

  if (count) *count = n;
  return mailboxes;
}

imap_mailbox_t *imap_select_mailbox(imap_client_t *client,
                                     const char *mailbox) {
  if (!client || !mailbox) return NULL;

  char select_cmd[512];
  fmt(select_cmd, sizeof(select_cmd), "SELECT \"{}\"", mailbox);

  char *tag = imap_generate_tag(client);
  if (imap_send_command(client, tag, select_cmd) != 0) {
    return NULL;
  }

  imap_mailbox_t *info = calloc(1, sizeof(imap_mailbox_t));
  info->name = strdup(mailbox);

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
        while (*ptr == ' ') ptr++;
        if (isdigit(*ptr)) {
          int num = atoi(ptr);
          if (n >= capacity) {
            capacity *= 2;
            results = realloc(results, capacity * sizeof(int));
          }
          results[n++] = num;
          while (isdigit(*ptr)) ptr++;
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
  char *literal_start = strstr(client->line_buffer, "{");
  if (!literal_start) {
    fmt_text(client->error_msg, sizeof(client->error_msg), "Invalid FETCH response: no literal");
    return NULL;
  }

  int literal_size = atoi(literal_start + 1);
  if (literal_size <= 0) {
    fmt(client->error_msg, sizeof(client->error_msg), "Invalid literal size: {}", literal_size);
    return NULL;
  }

  // Read literal data
  char *message_data = malloc(literal_size + 1);
  int total_read = 0;

  while (total_read < literal_size) {
    size_t to_read = (size_t)(literal_size - total_read);
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
  char *literal_start = strstr(client->line_buffer, "{");
  if (!literal_start) {
    fmt_text(client->error_msg, sizeof(client->error_msg), "Invalid FETCH response: no literal");
    return NULL;
  }

  int literal_size = atoi(literal_start + 1);
  if (literal_size <= 0) {
    fmt(client->error_msg, sizeof(client->error_msg), "Invalid literal size: {}", literal_size);
    return NULL;
  }

  // Read literal data
  char *message_data = malloc(literal_size + 1);
  int total_read = 0;

  while (total_read < literal_size) {
    size_t to_read = (size_t)(literal_size - total_read);
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
