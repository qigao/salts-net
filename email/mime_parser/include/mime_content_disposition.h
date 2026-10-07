/**
 * @file mime_content_disposition.h
 * @brief RFC 2183 Content-Disposition header parser
 *
 * Parses Content-Disposition headers for:
 * - HTTP file uploads: Content-Disposition: form-data; name="field"; filename="file.jpg"
 * - HTTP downloads: Content-Disposition: attachment; filename="report.pdf"
 * - Email attachments: Content-Disposition: inline; filename="image.png"
 */

#ifndef MIME_CONTENT_DISPOSITION_H
#define MIME_CONTENT_DISPOSITION_H

#include "platform.h"
#include <stddef.h>
#include "cmeta_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Disposition types ─────────────────────────────────────────────── */

typedef enum {
  MIME_DISPOSITION_INLINE,       // Display inline
  MIME_DISPOSITION_ATTACHMENT,   // Download as attachment
  MIME_DISPOSITION_FORM_DATA,    // Multipart form field
  MIME_DISPOSITION_UNKNOWN
} mime_disposition_type_t;

/* ── Parsed result ─────────────────────────────────────────────────── */

typedef struct {
  mime_disposition_type_t type;

  // Parameters (zero-copy pointers into original string)
  const char *filename;          // filename parameter
  size_t filename_len;

  const char *name;              // name parameter (for form-data)
  size_t name_len;

  const char *creation_date;     // creation-date parameter (optional)
  size_t creation_date_len;

  const char *modification_date; // modification-date parameter (optional)
  size_t modification_date_len;

  size_t size;                   // size parameter (0 if not present)
} mime_content_disposition_t;

/* ── API ───────────────────────────────────────────────────────────── */

/**
 * Parse Content-Disposition header value
 *
 * Examples:
 *   "attachment; filename=\"document.pdf\""
 *   "form-data; name=\"file\"; filename=\"photo.jpg\""
 *   "inline; filename=image.png; size=12345"
 *
 * @param value Header value string
 * @param len Length of value
 * @param result Output structure (zero-copy pointers)
 * @return 0 on success, -1 on error
 */
int mime_parse_content_disposition(const char *value, size_t len,
                                              mime_content_disposition_t *result);

/**
 * Get disposition type name
 */
const char *mime_disposition_type_name(mime_disposition_type_t type);

/**
 * Copy filename to pool-allocated string (handles RFC 2231 encoding if needed)
 * Returns NULL-terminated string or NULL on error
 */
char *mime_disposition_get_filename(mem_pool_t *pool,
                                               const mime_content_disposition_t *disp);

/**
 * Copy name to pool-allocated string
 * Returns NULL-terminated string or NULL on error
 */
char *mime_disposition_get_name(mem_pool_t *pool,
                                          const mime_content_disposition_t *disp);

#ifdef __cplusplus
}
#endif

#endif /* MIME_CONTENT_DISPOSITION_H */
