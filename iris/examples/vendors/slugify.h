#ifndef SLUGIFY_H
#define SLUGIFY_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/* Error codes */
#define SLUGIFY_SUCCESS 0
#define SLUGIFY_ERROR_BUFFER 1
#define SLUGIFY_ERROR_INVALID 2
#define SLUGIFY_ERROR_EMPTY 3
#define SLUGIFY_ERROR_MEMORY 4

/* Unicode validation limits */
#define UNICODE_MAX_CODEPOINT 0x10FFFF
#define UNICODE_SURROGATE_HIGH_START 0xD800
#define UNICODE_SURROGATE_LOW_END 0xDFFF

typedef struct {
  char separator;     /* Default: '-' */
  size_t max_length;  /* Max output length, 0 = no limit */
  bool preserve_case; /* 1 to preserve case, 0 to convert to lowercase (default) */
} slugify_options_t;

/* Transliteration table entry */
typedef struct {
  uint32_t unicode;
  const char *ascii;
} transliteration_entry_t;

/**
 * slugify - converts a UTF-8 string into a slug.
 *
 * Parameters:
 *   input    - NUL-terminated UTF-8 string to slugify
 *   options  - slugify options (can be NULL for defaults)
 *
 * Returns:
 *   Allocates and returns a slug string.
 *   Caller must free the returned string.
 *
 * Example usage:
 *   slugify_options_t opts = {
 *       .separator = '-',
 *       .max_length = 0,
 *       .preserve_case = 1,  // true
 *   };
 *   char *slug = slugify("Hello World!", &opts);
 */

char *slugify(const char *input, const slugify_options_t *options);

#endif /* SLUGIFY_H */
