#include "lexer.h"
#include <string.h>
#include <stdlib.h>

// Safe substring copy - no malloc needed!
void copy_substring(const char* src, int start, int len, char* dest, int dest_size) {
    if (!src || !dest || len < 0 || dest_size <= 0) return;
    
    int copy_len = len < (dest_size - 1) ? len : (dest_size - 1);
    memcpy(dest, src + start, copy_len);
    dest[copy_len] = '\0';
}

// Wrapper for re2c parse function - simplified interface
int parse_url(const char* url_string, url_t* result) {
    if (!url_string || !result) return 0;
    
    // Clear the result structure - no malloc cleanup needed!
    memset(result, 0, sizeof(url_t));
    
    // Call the re2c generated parser (from lexer.re)
    // We'll adapt the re2c code to use our new structure
    return parse(url_string, result);
}
