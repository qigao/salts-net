/**
 * @file email_pop3.h
 * @brief Synchronous POP3 client using Salts CNet
 *
 * Features:
 * - POP3 protocol support
 * - POP3S (TLS) support
 * - STLS upgrade
 * - Message retrieval with MIME parsing
 * - Caller-thread synchronous I/O over a bounded CNet transport
 */

#ifndef EMAIL_POP3_H
#define EMAIL_POP3_H

#include "platform.h"
#include "email_message.h"
#include "salts_str.h"
#include <salts/error_codes.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── POP3 Configuration ────────────────────────────────────────────── */

typedef struct {
  char *host;   // POP3 server hostname
  int port;     // Port (110, 995)
  int use_tls;  // 1 for POP3S (995), 0 for plain
  int use_stls; // 1 to upgrade with STLS
  char *username;
  char *password;
  int timeout_ms; // Connection timeout (default 30000)
} pop3_config_t;

/* ── Message Info ──────────────────────────────────────────────────── */

typedef struct {
  int msg_num; // Message number (1-based)
  int size;    // Message size in bytes
  char *uidl;  // Unique ID (if available)
} pop3_message_info_t;

/* ── POP3 Client ───────────────────────────────────────────────────── */

typedef struct pop3_client_s pop3_client_t;

/**
 * Create a synchronous POP3 client with copied configuration strings.
 *
 * @param config Required host, port, TLS mode, credentials, and timeout policy.
 * @return Owned client, or NULL for invalid configuration or allocation/runtime failure.
 */
pop3_client_t *pop3_client_create(const pop3_config_t *config);

/**
 * Stop the owned CNet transport and free the POP3 client.
 * No interrupting thread may overlap this call.
 */
void pop3_client_free(pop3_client_t *client);

/* ── Connection ────────────────────────────────────────────────────── */

/**
 * Connect and login to POP3 server
 */
int pop3_connect(pop3_client_t *client);

/**
 * Quit and disconnect
 */
void pop3_disconnect(pop3_client_t *client);

/**
 * Interrupt the client's current CNet wait from another thread.
 *
 * The connection remains owned by the progress thread and is closed by the
 * normal disconnect/free path. The interrupting thread must stop before
 * client destruction. Returns SALTS_ENOTCONN when no connection is active.
 *
 * @param client Client whose current wait is to be interrupted.
 * @param status Non-zero Salts status observed by the progress thread.
 * @return SALTS_OK, SALTS_EINVAL, SALTS_ENOTCONN, SALTS_EALREADY, or a wake error.
 */
int pop3_interrupt(pop3_client_t *client, int status);

/* ── Mailbox Operations ────────────────────────────────────────────── */

/**
 * Get mailbox statistics
 * Returns number of messages, sets total_size if not NULL
 */
int pop3_stat(pop3_client_t *client, int *total_size);

/**
 * List all messages
 * Returns array of message info (caller must free)
 */
pop3_message_info_t *pop3_list(pop3_client_t *client, int *count);

/**
 * Get unique IDs for all messages
 * Returns array of UIDLs (caller must free)
 */
char **pop3_uidl(pop3_client_t *client, int *count);

/* ── Message Operations ────────────────────────────────────────────── */

/**
 * Retrieve the raw RFC message by number.
 *
 * On success, `*data` is NUL-terminated, `*len` excludes that terminator,
 * and the caller owns the buffer and must release it with free().
 */
int pop3_retrieve_raw(pop3_client_t *client, int msg_num, char **data, size_t *len);

/**
 * Retrieve message by number
 */
email_message_t *pop3_retrieve_message(pop3_client_t *client, int msg_num);

/**
 * Retrieve message headers only
 */
email_message_t *pop3_retrieve_headers(pop3_client_t *client, int msg_num);

/**
 * Delete message
 */
int pop3_delete_message(pop3_client_t *client, int msg_num);

/**
 * Reset deleted messages (unmark for deletion)
 */
int pop3_reset(pop3_client_t *client);

/* ── Error Handling ────────────────────────────────────────────────── */

/**
 * Get last error message
 */
const char *pop3_get_error(pop3_client_t *client);

#ifdef __cplusplus
}
#endif

#endif /* EMAIL_POP3_H */
