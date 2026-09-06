/**
 * @file email_imap.h
 * @brief Synchronous IMAP4rev1 client using Salts CNet
 *
 * Features:
 * - IMAP4rev1 protocol support
 * - IMAPS (TLS) support
 * - STARTTLS upgrade
 * - Mailbox operations (LIST, SELECT, CREATE, DELETE)
 * - Message fetch with MIME parsing
 * - Search capabilities
 * - Caller-thread synchronous I/O over a bounded CNet transport
 */

#ifndef EMAIL_IMAP_H
#define EMAIL_IMAP_H

#include "platform.h"
#include "email_message.h"
#include "salts_str.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── IMAP Configuration ────────────────────────────────────────────── */

typedef struct {
  char *host;       // IMAP server hostname
  int port;         // Port (143, 993)
  int use_tls;      // 1 for IMAPS (993), 0 for plain
  int use_starttls; // 1 to upgrade with STARTTLS
  char *username;
  char *password;
  int timeout_ms; // Connection timeout (default 30000)
} imap_config_t;

/* ── Mailbox Info ──────────────────────────────────────────────────── */

typedef struct {
  char *name;      // Mailbox name
  char *delimiter; // Hierarchy delimiter
  int exists;      // Number of messages
  int recent;      // Number of recent messages
  int unseen;      // Number of unseen messages
  int uidnext;     // Next UID
  int uidvalidity; // UID validity
} imap_mailbox_t;

/* ── Message Info ──────────────────────────────────────────────────── */

typedef struct {
  int seq_num;    // Sequence number
  int uid;        // Unique ID
  int size;       // Message size in bytes
  char *flags;    // Flags (e.g., "\\Seen \\Answered")
  char *envelope; // Envelope data
} imap_message_info_t;

/* ── IMAP Client ───────────────────────────────────────────────────── */

typedef struct imap_client_s imap_client_t;

/**
 * Create a synchronous IMAP client with copied configuration strings.
 *
 * @param config Required host, port, TLS mode, credentials, and timeout policy.
 * @return Owned client, or NULL for invalid configuration or allocation/runtime failure.
 */
imap_client_t *imap_client_create(const imap_config_t *config);

/**
 * Stop the owned CNet transport and free the IMAP client.
 */
void imap_client_free(imap_client_t *client);

/* ── Connection ────────────────────────────────────────────────────── */

/**
 * Connect and login to IMAP server
 */
int imap_connect(imap_client_t *client);

/**
 * Logout and disconnect
 */
void imap_disconnect(imap_client_t *client);

/* ── Mailbox Operations ────────────────────────────────────────────── */

/**
 * List mailboxes
 * Returns array of mailbox names (caller must free)
 */
char **imap_list_mailboxes(imap_client_t *client, const char *reference, const char *pattern,
                           int *count);

/**
 * Select mailbox
 */
imap_mailbox_t *imap_select_mailbox(imap_client_t *client, const char *mailbox);

/**
 * Create mailbox
 */
int imap_create_mailbox(imap_client_t *client, const char *mailbox);

/**
 * Delete mailbox
 */
int imap_delete_mailbox(imap_client_t *client, const char *mailbox);

/* ── Message Operations ────────────────────────────────────────────── */

/**
 * Search messages
 * Returns array of sequence numbers (caller must free)
 */
int *imap_search(imap_client_t *client, const char *criteria, int *count);

/**
 * Fetch message by sequence number
 */
email_message_t *imap_fetch_message(imap_client_t *client, int seq_num);

/**
 * Fetch message by UID
 */
email_message_t *imap_fetch_message_uid(imap_client_t *client, int uid);

/**
 * Fetch message info (without body)
 */
imap_message_info_t *imap_fetch_info(imap_client_t *client, int seq_num);

/**
 * Set message flags
 */
int imap_set_flags(imap_client_t *client, int seq_num, const char *flags);

/**
 * Delete message (set \\Deleted flag)
 */
int imap_delete_message(imap_client_t *client, int seq_num);

/**
 * Expunge deleted messages
 */
int imap_expunge(imap_client_t *client);

/* ── Error Handling ────────────────────────────────────────────────── */

/**
 * Get last error message
 */
const char *imap_get_error(imap_client_t *client);

#ifdef __cplusplus
}
#endif

#endif /* EMAIL_IMAP_H */
