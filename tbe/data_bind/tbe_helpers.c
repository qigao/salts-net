#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Helper used by JIT-compiled MIR code to extract a variable-length string.
 * This implementation assumes a 2-byte little-endian length prefix.
 */
char* tbe_read_varstring_copy(const uint8_t* buf, size_t offset) {
    uint16_t len;
    // We assume little-endian for now as per mir_codec.c's hardcoded logic
    memcpy(&len, buf + offset, 2);
    
    char* s = (char*)malloc(len + 1);
    if (!s) return NULL;
    
    memcpy(s, buf + offset + 2, len);
    s[len] = '\0';
    return s;
}

/**
 * @brief Older variant if needed
 */
char* tbe_read_varstring(const uint8_t* buf, size_t offset) {
    return tbe_read_varstring_copy(buf, offset);
}
