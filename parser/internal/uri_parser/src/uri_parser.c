#include "uri_parser.h"
#include <stdlib.h>
#include <string.h>

// Forward declaration of re2c generated function
extern int uri_parse_internal(const char *url_str, uri_t *uri);

// Safe substring copy - no malloc needed!
void uri_copy_substring(const char *src, int start, int len, char *dest, int dest_size) {
    if (!src || !dest || len < 0 || dest_size <= 0)
        return;

    int copy_len = len < (dest_size - 1) ? len : (dest_size - 1);
    memcpy(dest, src + start, copy_len);
    dest[copy_len] = '\0';
}

// Wrapper for re2c parse function - simplified interface
int uri_parse(const char *url_string, uri_t *result) {
    if (!url_string || !result)
        return 0;

    // Clear the result structure - no malloc cleanup needed!
    memset(result, 0, sizeof(uri_t));

    // Call the re2c generated parser
    return uri_parse_internal(url_string, result);
}
