/**
 * @file email_smtp.h
 * @brief SMTP client using CoroNet
 *
 * Features:
 * - SMTP/SMTPS (TLS) support
 * - STARTTLS upgrade
 * - AUTH PLAIN/LOGIN/CRAM-MD5
 * - Pipelining support
 * - Async coroutine-based I/O
 */

#ifndef EMAIL_SMTP_H
#define EMAIL_SMTP_H

#include "platform.h"
#include "email_message.h"
#include "CoroNet.h"
#include "turbo_str.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── SMTP Configuration ────────────────────────────────────────────── */

typedef enum {
  SMTP_AUTH_NONE = 0,
  SMTP_AUTH_PLAIN,
  SMTP_AUTH_LOGIN,
  SMTP_AUTH_CRAM_MD5
} smtp_auth_method_t;

typedef struct {
  char *host;               // SMTP server hostname
  int port;                 // Port (25, 587, 465)
  int use_tls;              // 1 for SMTPS (465), 0 for plain
  int use_starttls;         // 1 to upgrade with STARTTLS (587)
  smtp_auth_method_t auth_method;
  char *username;
  char *password;
  char *client_hostname;    // Client hostname for EHLO (optional)
  int timeout_ms;           // Connection timeout (default 30000)
} smtp_config_t;

/* ── SMTP Client ───────────────────────────────────────────────────── */

typedef struct smtp_client_s smtp_client_t;

/**
 * Create SMTP client
 */
smtp_client_t *smtp_client_create(coro_context_t *ctx,
                                             const smtp_config_t *config);

/**
 * Free SMTP client
 */
void smtp_client_free(smtp_client_t *client);

/* ── Connection ────────────────────────────────────────────────────── */

/**
 * Connect to SMTP server
 * Returns 0 on success, -1 on failure
 */
int smtp_connect(smtp_client_t *client);

/**
 * Disconnect from SMTP server
 */
void smtp_disconnect(smtp_client_t *client);

/**
 * Interrupt the client's current CoroNet socket wait from another thread.
 *
 * The client remains owned by its caller. The coroutine performing connect,
 * receive, send, TLS upgrade, or disconnect observes `status` and completes
 * its normal cleanup before the caller may free the client.
 */
int smtp_interrupt(smtp_client_t *client, int status);

/* ── Send Email ────────────────────────────────────────────────────── */

/**
 * Send email message
 * Returns 0 on success, -1 on failure
 */
int smtp_send_message(smtp_client_t *client,
                                 email_message_t *msg);

/**
 * Send raw RFC 2822 message
 */
int smtp_send_raw(smtp_client_t *client,
                             const char *from_email,
                             const char **to_emails,
                             int to_count,
                             const char *raw_message,
                             size_t message_len);

/* ── Error Handling ────────────────────────────────────────────────── */

/**
 * Get last error message
 */
const char *smtp_get_error(smtp_client_t *client);

/**
 * Get last SMTP response code
 */
int smtp_get_last_code(smtp_client_t *client);

#ifdef __cplusplus
}
#endif

#endif /* EMAIL_SMTP_H */
