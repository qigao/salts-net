/**
 * @file mime_rfc2822.h
 * @brief RFC 2822 Email Address Parsing
 *
 * Parses email addresses from headers like From, To, Cc, Bcc.
 *
 * Supported formats:
 *   user@example.com
 *   User Name <user@example.com>
 *   "User Name" <user@example.com>
 *   =?UTF-8?B?...?= <user@example.com>
 *   user@example.com (Comment)
 *   user@example.com, another@example.com
 *
 * Handles RFC 2047 encoded display names automatically.
 */

#ifndef MIME_RFC2822_H
#define MIME_RFC2822_H

#include "platform.h"
#include <stddef.h>
#include "turbo_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Email Address Structure ───────────────────────────────────────── */

typedef struct mime_address_s {
  char *display_name;       // Display name (decoded from RFC 2047 if needed)
  char *email;              // Email address (user@domain)
  char *local_part;         // Local part (user)
  char *domain;             // Domain part (example.com)
  struct mime_address_s *next;  // Next address in list
} mime_address_t;

/* ── API ───────────────────────────────────────────────────────────── */

/**
 * Parse email address list from header value
 * Handles multiple addresses separated by commas
 * Returns linked list of addresses (caller must free with mime_address_list_free)
 */
mime_address_t *mime_parse_address_list(mem_pool_t *pool,
                                                   const char *header_value,
                                                   size_t len);

/**
 * Parse single email address
 * Returns single address (caller must free with mime_address_free)
 */
mime_address_t *mime_parse_address(mem_pool_t *pool,
                                              const char *address_str,
                                              size_t len);

/**
 * Free single address
 */
void mime_address_free(mime_address_t *addr);

/**
 * Free address list
 */
void mime_address_list_free(mime_address_t *list);

/**
 * Count addresses in list
 */
int mime_address_list_count(const mime_address_t *list);

/**
 * Get address at index (0-based)
 * Returns NULL if index out of bounds
 */
mime_address_t *mime_address_list_get(mime_address_t *list, int index);

/* ── Validation ────────────────────────────────────────────────────── */

/**
 * Validate email address format
 * Returns 1 if valid, 0 if invalid
 */
int mime_is_valid_email(const char *email, size_t len);

/**
 * Extract email from angle brackets
 * Example: "User <user@example.com>" -> "user@example.com"
 * Returns pool-allocated string or NULL
 */
char *mime_extract_email(mem_pool_t *pool,
                                    const char *address_str,
                                    size_t len);

/**
 * Extract display name from address
 * Example: "User Name <user@example.com>" -> "User Name"
 * Handles RFC 2047 encoded names
 * Returns pool-allocated string or NULL
 */
char *mime_extract_display_name(mem_pool_t *pool,
                                           const char *address_str,
                                           size_t len);

#ifdef __cplusplus
}
#endif

#endif /* MIME_RFC2822_H */
