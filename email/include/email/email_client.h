/**
 * @file email_client.h
 * @brief Unified email client API
 *
 * High-level API combining SMTP, IMAP, and POP3 clients
 */

#ifndef EMAIL_CLIENT_H
#define EMAIL_CLIENT_H

#include "platform.h"
#include "email_imap.h"
#include "email_message.h"
#include "email_pop3.h"
#include "email_smtp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Unified Email Client ──────────────────────────────────────────── */

typedef struct {
  smtp_client_t *smtp;
  imap_client_t *imap;
  pop3_client_t *pop3;
} email_client_t;

/**
 * Create email client with SMTP configuration
 */
email_client_t *email_client_create_smtp(const smtp_config_t *config);

/**
 * Create email client with IMAP configuration
 */
email_client_t *email_client_create_imap(const imap_config_t *config);

/**
 * Create email client with POP3 configuration
 */
email_client_t *email_client_create_pop3(const pop3_config_t *config);

/**
 * Create full email client (SMTP + IMAP)
 */
email_client_t *email_client_create_full(const smtp_config_t *smtp_config,
                                         const imap_config_t *imap_config);

/**
 * Free email client
 */
void email_client_free(email_client_t *client);

/* ── Quick Send API ────────────────────────────────────────────────── */

/**
 * Send simple text email
 */
int email_send_simple(email_client_t *client, const char *from_name, const char *from_email,
                      const char *to_name, const char *to_email, const char *subject,
                      const char *body);

/**
 * Send HTML email with text alternative
 */
int email_send_html(email_client_t *client, const char *from_name, const char *from_email,
                    const char *to_name, const char *to_email, const char *subject,
                    const char *text_body, const char *html_body);

#ifdef __cplusplus
}
#endif

#endif /* EMAIL_CLIENT_H */
