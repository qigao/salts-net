#ifndef TURBO_URI_PARSER_H
#define TURBO_URI_PARSER_H

#include <stdint.h>

// Host types
typedef enum {
  URI_HOST_UNKNOWN = 0,
  URI_HOST_REGNAME,
  URI_HOST_IPV6ADDR,
  URI_HOST_IPV4ADDR,
  URI_HOST_IPVFUTURE
} uri_host_type_t;

// Simplified URL structure - all stack allocated, no malloc!
typedef struct uri_s {
  char scheme[32];
  char userinfo[256];
  char host[256];
  char path[1024];
  char query[1024];
  char fragment[256];
  int port; // Use int to preserve original value (even if invalid)
  uint8_t host_type;
  uint8_t valid;
} uri_t;

/**
 * Parse a URL string into a uri_t structure.
 *
 * @param url_string The URL string to parse (null-terminated)
 * @param result     Pointer to uri_t structure to fill
 * @return 1 on success, 0 on failure
 */
int uri_parse(const char *url_string, uri_t *result);

/**
 * Helper function for copying substrings safely.
 *
 * @param src       Source string
 * @param start     Start offset in source
 * @param len       Length to copy
 * @param dest      Destination buffer
 * @param dest_size Size of destination buffer
 */
void uri_copy_substring(const char *src, int start, int len, char *dest, int dest_size);

#endif // TURBO_URI_PARSER_H
