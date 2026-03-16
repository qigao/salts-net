#include "email/email_imap.h"
#include "CoroNet.h"
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
};

/* ── Helpers ───────────────────────────────────────────────────────── */

static char *imap_generate_tag(imap_client_t *client) {
  static char tag[16];
  snprintf(tag, sizeof(tag), "A%04d", ++client->tag_counter);
  return tag;
}

static int imap_read_line(imap_client_t *client) {
  if (!client || !client->socket) return -1;

  char *data = NULL;
  size_t len = 0;

  int result = coro_socket_recv(client->socket, &data, &len);
  if (result != 0 || !data) {
    snprintf(client->error_msg, sizeof(client->error_msg),
             "Failed to read IMAP response");
    return -1;
  }

  // Copy to read_buffer
  size_t copy_len = len < sizeof(client->read_buffer) - 1 ? len : sizeof(client->read_buffer) - 1;
  memcpy(client->read_buffer, data, copy_len);
  client->read_buffer[copy_len] = '\0';

  coro_socket_free_recv(data);
  return 0;
}

static int imap_read_response(imap_client_t *client, const char *expected_tag) {
  if (imap_read_line(client) != 0) return -1;

  // Parse response: "TAG OK/NO/BAD ..." or "* UNTAGGED ..."
  if (strlen(client->read_buffer) < 3) {
    snprintf(client->error_msg, sizeof(client->error_msg),
             "Invalid IMAP response");
    return -1;
  }

  // Check for tagged response
  if (expected_tag && strncmp(client->read_buffer, expected_tag, strlen(expected_tag)) == 0) {
    // Tagged response: "A001 OK ..."
    const char *status = client->read_buffer + strlen(expected_tag) + 1;
    if (strncmp(status, "OK", 2) == 0) {
      return 0; // Success
    } else if (strncmp(status, "NO", 2) == 0) {
      snprintf(client->error_msg, sizeof(client->error_msg),
               "IMAP NO: %s", status + 3);
      return -1;
    } else if (strncmp(status, "BAD", 3) == 0) {
      snprintf(client->error_msg, sizeof(client->error_msg),
               "IMAP BAD: %s", status + 4);
      return -1;
    }
  }

  return 0;
}

static int imap_send_command(imap_client_t *client, const char *tag, const char *cmd) {
  if (!client || !client->socket || !tag || !cmd) return -1;

  char buffer[2048];
  int len = snprintf(buffer, sizeof(buffer), "%s %s\r\n", tag, cmd);

  int sent = coro_socket_send(client->socket, buffer, len);
  if (sent != len) {
    snprintf(client->error_msg, sizeof(client->error_msg),
             "Failed to send IMAP command: %s", cmd);
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
    if (strncmp(client->read_buffer, tag, strlen(tag)) == 0) {
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
  if (!client) return -1;

  // Create socket
  client->socket = coro_socket_create_tcpv4(client->ctx);
  if (!client->socket) {
    snprintf(client->error_msg, sizeof(client->error_msg),
             "Failed to create socket");
    return -1;
  }

  // Build connection URL - use tcp:// for all connections
  char url[512];
  snprintf(url, sizeof(url), "tcp://%s:%d",
           client->config.host, client->config.port);

  // Connect
  if (coro_socket_connect(client->socket, url) != 0) {
    snprintf(client->error_msg, sizeof(client->error_msg),
             "Failed to connect to %s", url);
    coro_socket_destroy(client->socket);
    client->socket = NULL;
    return -1;
  }

  // TODO: If use_tls is set, upgrade connection to TLS
  // coro_socket_upgrade_tls(client->socket);

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

    // TODO: Upgrade to TLS
    // coro_socket_upgrade_tls(client->socket);
  }

  // LOGIN
  char login_cmd[1024];
  snprintf(login_cmd, sizeof(login_cmd), "LOGIN %s %s",
           client->config.username, client->config.password);

  if (imap_command(client, login_cmd) != 0) {
    imap_disconnect(client);
    return -1;
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
  snprintf(list_cmd, sizeof(list_cmd), "LIST \"%s\" \"%s\"",
           reference ? reference : "", pattern ? pattern : "*");

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
    if (strncmp(client->read_buffer, tag, strlen(tag)) == 0) {
      break;
    }

    // Parse untagged LIST response: * LIST (...) "delimiter" "name"
    if (strncmp(client->read_buffer, "* LIST", 6) == 0) {
      // Find last quoted string (mailbox name)
      char *last_quote = strrchr(client->read_buffer, '"');
      if (last_quote && last_quote > client->read_buffer) {
        char *start_quote = last_quote - 1;
        while (start_quote > client->read_buffer && *start_quote != '"') {
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
  snprintf(select_cmd, sizeof(select_cmd), "SELECT \"%s\"", mailbox);

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
    if (strncmp(client->read_buffer, tag, strlen(tag)) == 0) {
      break;
    }

    // Parse untagged responses
    if (client->read_buffer[0] == '*') {
      // * 123 EXISTS
      if (strstr(client->read_buffer, "EXISTS")) {
        sscanf(client->read_buffer, "* %d EXISTS", &info->exists);
      }
      // * 5 RECENT
      else if (strstr(client->read_buffer, "RECENT")) {
        sscanf(client->read_buffer, "* %d RECENT", &info->recent);
      }
      // * OK [UNSEEN 12]
      else if (strstr(client->read_buffer, "UNSEEN")) {
        char *unseen_str = strstr(client->read_buffer, "UNSEEN");
        sscanf(unseen_str, "UNSEEN %d", &info->unseen);
      }
      // * OK [UIDNEXT 4392]
      else if (strstr(client->read_buffer, "UIDNEXT")) {
        char *uidnext_str = strstr(client->read_buffer, "UIDNEXT");
        sscanf(uidnext_str, "UIDNEXT %d", &info->uidnext);
      }
      // * OK [UIDVALIDITY 3857529045]
      else if (strstr(client->read_buffer, "UIDVALIDITY")) {
        char *uidval_str = strstr(client->read_buffer, "UIDVALIDITY");
        sscanf(uidval_str, "UIDVALIDITY %d", &info->uidvalidity);
      }
    }
  }

  return info;
}

int imap_create_mailbox(imap_client_t *client, const char *mailbox) {
  if (!client || !mailbox) return -1;

  char create_cmd[512];
  snprintf(create_cmd, sizeof(create_cmd), "CREATE \"%s\"", mailbox);

  return imap_command(client, create_cmd);
}

int imap_delete_mailbox(imap_client_t *client, const char *mailbox) {
  if (!client || !mailbox) return -1;

  char delete_cmd[512];
  snprintf(delete_cmd, sizeof(delete_cmd), "DELETE \"%s\"", mailbox);

  return imap_command(client, delete_cmd);
}

/* ── Message Operations ────────────────────────────────────────────── */

int *imap_search(imap_client_t *client, const char *criteria, int *count) {
  if (!client || !criteria) return NULL;

  char search_cmd[1024];
  snprintf(search_cmd, sizeof(search_cmd), "SEARCH %s", criteria);

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
    if (strncmp(client->read_buffer, tag, strlen(tag)) == 0) {
      break;
    }

    // Parse SEARCH response: * SEARCH 1 2 3 4 5
    if (strncmp(client->read_buffer, "* SEARCH", 8) == 0) {
      char *ptr = client->read_buffer + 8;
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
  snprintf(fetch_cmd, sizeof(fetch_cmd), "FETCH %d BODY[]", seq_num);

  char *tag = imap_generate_tag(client);
  if (imap_send_command(client, tag, fetch_cmd) != 0) {
    return NULL;
  }

  // Read FETCH response: * 1 FETCH (BODY[] {size}
  if (imap_read_line(client) != 0) {
    return NULL;
  }

  // Parse literal size: BODY[] {1234}
  char *literal_start = strstr(client->read_buffer, "{");
  if (!literal_start) {
    snprintf(client->error_msg, sizeof(client->error_msg),
             "Invalid FETCH response: no literal");
    return NULL;
  }

  int literal_size = atoi(literal_start + 1);
  if (literal_size <= 0) {
    snprintf(client->error_msg, sizeof(client->error_msg),
             "Invalid literal size: %d", literal_size);
    return NULL;
  }

  // Read literal data
  char *message_data = malloc(literal_size + 1);
  int total_read = 0;

  while (total_read < literal_size) {
    char *data = NULL;
    size_t len = 0;
    if (coro_socket_recv(client->socket, &data, &len) != 0 || !data) {
      free(message_data);
      snprintf(client->error_msg, sizeof(client->error_msg),
               "Failed to read message body");
      return NULL;
    }

    int to_copy = (total_read + len > literal_size) ? (literal_size - total_read) : len;
    memcpy(message_data + total_read, data, to_copy);
    total_read += to_copy;
    coro_socket_free_recv(data);
  }
  message_data[literal_size] = '\0';

  // Parse message using mime_parser
  mem_pool_t pool;
  mem_init(&pool, literal_size + 4096);
  email_message_t *msg = email_message_parse(&pool, message_data, literal_size);
  free(message_data);

  if (!msg) {
    mem_destroy(&pool);
    snprintf(client->error_msg, sizeof(client->error_msg),
             "Failed to parse message");
    return NULL;
  }

  // Read closing parenthesis and tagged response
  while (1) {
    if (imap_read_line(client) != 0) {
      email_message_free(msg);
      mem_destroy(&pool);
      return NULL;
    }
    if (strncmp(client->read_buffer, tag, strlen(tag)) == 0) {
      break;
    }
  }

  return msg;
}

email_message_t *imap_fetch_message_uid(imap_client_t *client, int uid) {
  if (!client) return NULL;

  char fetch_cmd[256];
  snprintf(fetch_cmd, sizeof(fetch_cmd), "UID FETCH %d BODY[]", uid);

  char *tag = imap_generate_tag(client);
  if (imap_send_command(client, tag, fetch_cmd) != 0) {
    return NULL;
  }

  // Read FETCH response
  if (imap_read_line(client) != 0) {
    return NULL;
  }

  // Parse literal size
  char *literal_start = strstr(client->read_buffer, "{");
  if (!literal_start) {
    snprintf(client->error_msg, sizeof(client->error_msg),
             "Invalid FETCH response: no literal");
    return NULL;
  }

  int literal_size = atoi(literal_start + 1);
  if (literal_size <= 0) {
    snprintf(client->error_msg, sizeof(client->error_msg),
             "Invalid literal size: %d", literal_size);
    return NULL;
  }

  // Read literal data
  char *message_data = malloc(literal_size + 1);
  int total_read = 0;

  while (total_read < literal_size) {
    char *data = NULL;
    size_t len = 0;
    if (coro_socket_recv(client->socket, &data, &len) != 0 || !data) {
      free(message_data);
      snprintf(client->error_msg, sizeof(client->error_msg),
               "Failed to read message body");
      return NULL;
    }

    int to_copy = (total_read + len > literal_size) ? (literal_size - total_read) : len;
    memcpy(message_data + total_read, data, to_copy);
    total_read += to_copy;
    coro_socket_free_recv(data);
  }
  message_data[literal_size] = '\0';

  // Parse message
  mem_pool_t pool;
  mem_init(&pool, literal_size + 4096);
  email_message_t *msg = email_message_parse(&pool, message_data, literal_size);
  free(message_data);

  if (!msg) {
    mem_destroy(&pool);
    snprintf(client->error_msg, sizeof(client->error_msg),
             "Failed to parse message");
    return NULL;
  }

  // Read closing and tagged response
  while (1) {
    if (imap_read_line(client) != 0) {
      email_message_free(msg);
      mem_destroy(&pool);
      return NULL;
    }
    if (strncmp(client->read_buffer, tag, strlen(tag)) == 0) {
      break;
    }
  }

  return msg;
}

imap_message_info_t *imap_fetch_info(imap_client_t *client, int seq_num) {
  if (!client) return NULL;

  char fetch_cmd[256];
  snprintf(fetch_cmd, sizeof(fetch_cmd), "FETCH %d (FLAGS RFC822.SIZE ENVELOPE)", seq_num);

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
    if (strncmp(client->read_buffer, tag, strlen(tag)) == 0) {
      break;
    }

    // Parse FETCH response: * 1 FETCH (FLAGS (\Seen) RFC822.SIZE 1234 ...)
    if (strstr(client->read_buffer, "FETCH")) {
      // Extract FLAGS
      char *flags_start = strstr(client->read_buffer, "FLAGS (");
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
      char *size_str = strstr(client->read_buffer, "RFC822.SIZE");
      if (size_str) {
        sscanf(size_str, "RFC822.SIZE %d", &info->size);
      }

      // Extract UID if present
      char *uid_str = strstr(client->read_buffer, "UID");
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
  snprintf(store_cmd, sizeof(store_cmd), "STORE %d FLAGS (%s)", seq_num, flags);

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
