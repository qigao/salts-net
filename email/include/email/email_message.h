/**
 * @file email_message.h
 * @brief Email message construction and parsing
 *
 * Features:
 * - RFC 2822 compliant message building
 * - MIME multipart support (text/html alternatives, attachments)
 * - RFC 2047 encoded-word for non-ASCII headers
 * - RFC 2231 parameter encoding for filenames
 * - S/MIME encryption/signing support
 */

#ifndef EMAIL_MESSAGE_H
#define EMAIL_MESSAGE_H

#include "platform.h"
#include "cmeta_buffer.h"
#include "tstr.h"
#include "mime_parser.h"
#include "mime_rfc2822.h"
#include "mime_smime.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Message Priority ──────────────────────────────────────────────── */

typedef enum {
  EMAIL_PRIORITY_NORMAL = 0,
  EMAIL_PRIORITY_HIGH = 1,
  EMAIL_PRIORITY_LOW = 2
} email_priority_t;

/* ── Attachment ────────────────────────────────────────────────────── */

typedef struct email_attachment_s {
  char *filename;           // Display filename
  char *content_type;       // MIME type (e.g., "image/png")
  const char *data;         // Attachment data (zero-copy)
  size_t data_len;
  int inline_attachment;    // 1 for inline (Content-ID), 0 for attachment
  char *content_id;         // For inline images (e.g., "logo@example.com")
  struct email_attachment_s *next;
} email_attachment_t;

/* ── Email Message ─────────────────────────────────────────────────── */

typedef struct {
  // Headers
  mime_address_t *from;
  mime_address_t *to;
  mime_address_t *cc;
  mime_address_t *bcc;
  mime_address_t *reply_to;
  char *subject;
  char *message_id;
  char *in_reply_to;
  char *references;
  email_priority_t priority;

  // Body
  char *text_body;          // Plain text version
  char *html_body;          // HTML version
  email_attachment_t *attachments;
  int attachment_count;

  // S/MIME
  mime_smime_ctx_t *smime_ctx;  // Optional: for signing/encryption
  int sign_message;
  int encrypt_message;

  // Internal
  mem_pool_t *pool;
} email_message_t;

/* ── Message Creation ──────────────────────────────────────────────── */

/**
 * Create new email message
 */
email_message_t *email_message_create(mem_pool_t *pool);

/**
 * Free email message
 */
void email_message_free(email_message_t *msg);

/* ── Address Management ────────────────────────────────────────────── */

/**
 * Set From address
 * Example: email_message_set_from(msg, "John Doe", "john@example.com")
 */
int email_message_set_from(email_message_t *msg,
                                      const char *name,
                                      const char *email);

/**
 * Add To recipient
 */
int email_message_add_to(email_message_t *msg,
                                    const char *name,
                                    const char *email);

/**
 * Add CC recipient
 */
int email_message_add_cc(email_message_t *msg,
                                    const char *name,
                                    const char *email);

/**
 * Add BCC recipient
 */
int email_message_add_bcc(email_message_t *msg,
                                     const char *name,
                                     const char *email);

/**
 * Set Reply-To address
 */
int email_message_set_reply_to(email_message_t *msg,
                                          const char *name,
                                          const char *email);

/* ── Content ───────────────────────────────────────────────────────── */

/**
 * Set subject (automatically encodes non-ASCII)
 */
int email_message_set_subject(email_message_t *msg,
                                         const char *subject);

/**
 * Set plain text body
 */
int email_message_set_text_body(email_message_t *msg,
                                           const char *text);

/**
 * Set HTML body
 */
int email_message_set_html_body(email_message_t *msg,
                                           const char *html);

/**
 * Set priority
 */
void email_message_set_priority(email_message_t *msg,
                                           email_priority_t priority);

/* ── Attachments ───────────────────────────────────────────────────── */

/**
 * Add file attachment
 * data is zero-copy (caller must keep alive until message is sent)
 */
int email_message_add_attachment(email_message_t *msg,
                                            const char *filename,
                                            const char *content_type,
                                            const char *data,
                                            size_t data_len);

/**
 * Add inline attachment (for HTML <img src="cid:...">)
 */
int email_message_add_inline_attachment(email_message_t *msg,
                                                   const char *content_id,
                                                   const char *filename,
                                                   const char *content_type,
                                                   const char *data,
                                                   size_t data_len);

/* ── S/MIME ────────────────────────────────────────────────────────── */

/**
 * Enable message signing
 */
int email_message_enable_signing(email_message_t *msg,
                                            const char *cert_path,
                                            const char *key_path,
                                            const char *key_password);

/**
 * Enable message encryption
 */
int email_message_enable_encryption(email_message_t *msg,
                                               const char *recipient_cert_path);

/* ── Serialization ─────────────────────────────────────────────────── */

/**
 * Build RFC 2822 message with MIME parts
 * Returns serialized message (caller must free with tstr_free)
 */
tstr email_message_to_string(email_message_t *msg);

/* ── Parsing ───────────────────────────────────────────────────────── */

/**
 * Parse RFC 2822 message from string
 */
email_message_t *email_message_parse(mem_pool_t *pool,
                                                const char *raw_message,
                                                size_t len);

#ifdef __cplusplus
}
#endif

#endif /* EMAIL_MESSAGE_H */
