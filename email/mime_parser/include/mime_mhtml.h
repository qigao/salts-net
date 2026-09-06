/**
 * @file mime_mhtml.h
 * @brief RFC 2557 MHTML (MIME HTML) Support
 *
 * MHTML aggregates HTML documents and their linked resources (images, CSS, JS)
 * into a single MIME message using multipart/related.
 *
 * Structure:
 *   multipart/related; type="text/html"
 *     - text/html (root document)
 *     - image/png (Content-Location: image.png)
 *     - text/css (Content-Location: style.css)
 *     - image/jpeg (Content-ID: <photo@example.com>)
 *
 * Used for:
 * - Sending complete web pages via email
 * - Saving web pages as single files (.mhtml, .mht)
 * - Archiving web content
 */

#ifndef MIME_MHTML_H
#define MIME_MHTML_H

#include "platform.h"
#include <stddef.h>
#include "salts_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── MHTML Resource ────────────────────────────────────────────────── */

typedef struct mime_mhtml_resource_s {
  char *content_type;           // e.g., "image/png", "text/css"
  char *content_location;       // URL or relative path
  char *content_id;             // CID for cid: references
  char *content_encoding;       // e.g., "base64", "quoted-printable"

  const char *data;             // Resource data (zero-copy pointer)
  size_t data_len;

  struct mime_mhtml_resource_s *next;
} mime_mhtml_resource_t;

/* ── MHTML Document ────────────────────────────────────────────────── */

typedef struct {
  char *html_content;           // Root HTML document
  size_t html_len;
  char *html_content_type;      // Usually "text/html; charset=utf-8"

  mime_mhtml_resource_t *resources;  // Linked list of resources
  int resource_count;

  char *boundary;               // Multipart boundary
} mime_mhtml_document_t;

/* ── Parsing ───────────────────────────────────────────────────────── */

/**
 * Parse MHTML document from multipart/related message
 * Returns document structure (caller must free with mime_mhtml_document_free)
 */
mime_mhtml_document_t *mime_parse_mhtml(mem_pool_t *pool,
                                                   const char *mhtml_data,
                                                   size_t len);

/**
 * Free MHTML document
 */
void mime_mhtml_document_free(mime_mhtml_document_t *doc);

/* ── Resource lookup ───────────────────────────────────────────────── */

/**
 * Find resource by Content-Location
 * Returns NULL if not found
 */
mime_mhtml_resource_t *mime_mhtml_find_by_location(
    mime_mhtml_document_t *doc,
    const char *location);

/**
 * Find resource by Content-ID
 * Example: find_by_cid(doc, "photo@example.com") or find_by_cid(doc, "cid:photo@example.com")
 * Returns NULL if not found
 */
mime_mhtml_resource_t *mime_mhtml_find_by_cid(
    mime_mhtml_document_t *doc,
    const char *cid);

/* ── Building ──────────────────────────────────────────────────────── */

/**
 * Create new MHTML document builder
 */
mime_mhtml_document_t *mime_mhtml_document_create(mem_pool_t *pool);

/**
 * Set root HTML content
 */
int mime_mhtml_set_html(mime_mhtml_document_t *doc,
                                   const char *html, size_t len,
                                   const char *charset);

/**
 * Add resource to MHTML document
 * data is copied if copy_data is true, otherwise zero-copy pointer is used
 */
int mime_mhtml_add_resource(mime_mhtml_document_t *doc,
                                       const char *content_type,
                                       const char *content_location,
                                       const char *content_id,
                                       const char *data, size_t data_len,
                                       int copy_data);

/**
 * Serialize MHTML document to multipart/related format
 * Returns serialized data (caller must free)
 */
char *mime_mhtml_serialize(mime_mhtml_document_t *doc,
                                      size_t *output_len);

/* ── Helpers ───────────────────────────────────────────────────────── */

/**
 * Check if Content-Type is multipart/related with type=text/html
 */
int mime_is_mhtml(const char *content_type, size_t len);

/**
 * Extract Content-Location from header
 */
char *mime_extract_content_location(mem_pool_t *pool,
                                               const char *headers, size_t len);

/**
 * Extract Content-ID from header
 * Strips angle brackets if present
 */
char *mime_extract_content_id(mem_pool_t *pool,
                                         const char *headers, size_t len);

/**
 * Resolve relative URL in MHTML context
 * Example: resolve("image.png", "http://example.com/page.html") -> "http://example.com/image.png"
 */
char *mime_mhtml_resolve_url(mem_pool_t *pool,
                                        const char *relative_url,
                                        const char *base_url);

#ifdef __cplusplus
}
#endif

#endif /* MIME_MHTML_H */
