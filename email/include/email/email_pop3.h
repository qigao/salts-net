/**
 * @file email_pop3.h
 * @brief POP3 client using CoroNet
 *
 * Features:
 * - POP3 protocol support
 * - POP3S (TLS) support
 * - STLS upgrade
 * - Message retrieval with MIME parsing
 * - Async coroutine-based I/O
 */

#ifndef EMAIL_POP3_H
#define EMAIL_POP3_H

#include "platform.h"
#include "email_message.h"
#include "CoroNet.h"
#include "turbo_str.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── POP3 Configuration ────────────────────────────────────────────── */

typedef struct {
  char *host;               // POP3 server hostname
  int port;                 // Port (110, 995)
  int use_tls;              // 1 for POP3S (995), 0 for plain
  int use_stls;             // 1 to upgrade with STLS
  char *username;
  char *password;
  int timeout_ms;           // Connection timeout (default 30000)
} pop3_config_t;

/* ── Message Info ──────────────────────────────────────────────────── */

typedef struct {
  int msg_num;              // Message number (1-based)
  int size;                 // Message size in bytes
  char *uidl;               // Unique ID (if available)
} pop3_message_info_t;

/* ── POP3 Client ───────────────────────────────────────────────────── */

typedef struct pop3_client_s pop3_client_t;

/**
 * Create POP3 client
 */
CXX_C_API pop3_client_t *pop3_client_create(coro_context_t *ctx,
                                             const pop3_config_t *config);

/**
 * Free POP3 client
 */
CXX_C_API void pop3_client_free(pop3_client_t *client);

/* ── Connection ────────────────────────────────────────────────────── */

/**
 * Connect and login to POP3 server
 */
CXX_C_API int pop3_connect(pop3_client_t *client);

/**
 * Quit and disconnect
 */
CXX_C_API void pop3_disconnect(pop3_client_t *client);

/**
 * Interrupt the client's current CoroNet socket wait from another thread.
 *
 * The socket remains owned by the POP3 coroutine and is destroyed by the
 * normal disconnect/free path. Returns TURBO_ENOTCONN when no socket is
 * currently published.
 */
CXX_C_API int pop3_interrupt(pop3_client_t *client, int status);

/* ── Mailbox Operations ────────────────────────────────────────────── */

/**
 * Get mailbox statistics
 * Returns number of messages, sets total_size if not NULL
 */
CXX_C_API int pop3_stat(pop3_client_t *client, int *total_size);

/**
 * List all messages
 * Returns array of message info (caller must free)
 */
CXX_C_API pop3_message_info_t *pop3_list(pop3_client_t *client, int *count);

/**
 * Get unique IDs for all messages
 * Returns array of UIDLs (caller must free)
 */
CXX_C_API char **pop3_uidl(pop3_client_t *client, int *count);

/* ── Message Operations ────────────────────────────────────────────── */

/**
 * Retrieve the raw RFC message by number.
 *
 * On success, `*data` is NUL-terminated, `*len` excludes that terminator,
 * and the caller owns the buffer and must release it with free().
 */
CXX_C_API int pop3_retrieve_raw(pop3_client_t *client, int msg_num,
                                char **data, size_t *len);

/**
 * Retrieve message by number
 */
CXX_C_API email_message_t *pop3_retrieve_message(pop3_client_t *client,
                                                  int msg_num);

/**
 * Retrieve message headers only
 */
CXX_C_API email_message_t *pop3_retrieve_headers(pop3_client_t *client,
                                                  int msg_num);

/**
 * Delete message
 */
CXX_C_API int pop3_delete_message(pop3_client_t *client, int msg_num);

/**
 * Reset deleted messages (unmark for deletion)
 */
CXX_C_API int pop3_reset(pop3_client_t *client);

/* ── Error Handling ────────────────────────────────────────────────── */

/**
 * Get last error message
 */
CXX_C_API const char *pop3_get_error(pop3_client_t *client);

#ifdef __cplusplus
}
#endif

#endif /* EMAIL_POP3_H */
