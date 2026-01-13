/**
 * @file fmt_va.h
 * @brief Helper for using va_list with fmt_printd-style {} placeholders
 * 
 * This provides a bridge between variadic arguments (va_list) and the fmt library's
 * {} placeholder syntax. It allows logger functions to accept modern format strings
 * while still using C's variadic argument mechanism.
 * 
 * Usage:
 *   char buffer[256];
 *   fmt_va_print(buffer, sizeof(buffer), "Hello {s}, value: {d}", args);
 * 
 * Supported format specifiers:
 *   {s} or {:s} - String (const char*)
 *   {d} or {:d} - Integer (int)
 *   {u} or {:u} - Unsigned integer (unsigned int)
 *   {x} or {:x} - Hex integer (unsigned int)
 *   {f} or {:f} - Float/double (promoted to double in va_list)
 *   {p} or {:p} - Pointer (void*)
 */

#ifndef FMT_VA_H
#define FMT_VA_H

#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include "stb_sprintf.h"
#include "fmt_va_lexer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Format a string using fmt-style {} placeholders with va_list arguments.
 * 
 * Note: Since va_list doesn't carry type information, you must use typed placeholders:
 * - {s} for strings
 * - {d} for integers
 * - {u} for unsigned integers
 * - {x} for hex
 * - {f} for floats/doubles
 * - {p} for pointers
 * 
 * @param buf Output buffer
 * @param size Size of output buffer
 * @param fmt Format string with {} placeholders
 * @param args va_list of arguments
 * @return Number of characters written (excluding null terminator)
 */
static inline int fmt_va_vprint(char *buf, size_t size, const char *fmt, va_list args) {
    if (!buf || !fmt || size == 0) {
        return 0;
    }

    char *dst = buf;
    char *end = buf + size - 1;
    const char *cursor = fmt;
    
    // Padded format strings for stbsp_sprintf safety
    static const char fmt_str[] = "%s\0\0\0\0\0\0\0\0";
    static const char fmt_int[] = "%d\0\0\0\0\0\0\0\0";
    static const char fmt_uint[] = "%u\0\0\0\0\0\0\0\0";
    static const char fmt_hex[] = "%x\0\0\0\0\0\0\0\0";
    static const char fmt_float[] = "%f\0\0\0\0\0\0\0\0";
    static const char fmt_ptr[] = "%p\0\0\0\0\0\0\0\0";
    
    while (dst < end) {
        const char *token_start;
        size_t token_len;
        fmt_token_t token = fmt_va_scan(&cursor, &token_start, &token_len);
        
        switch (token) {
            case FMT_TOKEN_END:
                *dst = '\0';
                return (int)(dst - buf);
                
            case FMT_TOKEN_TEXT: {
                // Copy literal text
                int copy_len = (int)token_len;
                if (dst + copy_len > end) {
                    copy_len = (int)(end - dst);
                }
                memcpy(dst, token_start, copy_len);
                dst += copy_len;
                break;
            }
            
            case FMT_TOKEN_ESCAPED_BRACE:
                // Copy single '{'
                if (dst < end) {
                    *dst++ = '{';
                }
                break;
                
            case FMT_TOKEN_STRING: {
                const char *str = va_arg(args, const char*);
                char temp[256];
                int written = stbsp_snprintf(temp, sizeof(temp), fmt_str, str ? str : "(null)");
                if (written > 0) {
                    int copy_len = written;
                    if (dst + copy_len > end) {
                        copy_len = (int)(end - dst);
                    }
                    memcpy(dst, temp, copy_len);
                    dst += copy_len;
                }
                break;
            }
            
            case FMT_TOKEN_INT: {
                int val = va_arg(args, int);
                char temp[64];
                int written = stbsp_snprintf(temp, sizeof(temp), fmt_int, val);
                if (written > 0) {
                    int copy_len = written;
                    if (dst + copy_len > end) {
                        copy_len = (int)(end - dst);
                    }
                    memcpy(dst, temp, copy_len);
                    dst += copy_len;
                }
                break;
            }
            
            case FMT_TOKEN_UINT: {
                unsigned int val = va_arg(args, unsigned int);
                char temp[64];
                int written = stbsp_snprintf(temp, sizeof(temp), fmt_uint, val);
                if (written > 0) {
                    int copy_len = written;
                    if (dst + copy_len > end) {
                        copy_len = (int)(end - dst);
                    }
                    memcpy(dst, temp, copy_len);
                    dst += copy_len;
                }
                break;
            }
            
            case FMT_TOKEN_HEX: {
                unsigned int val = va_arg(args, unsigned int);
                char temp[64];
                int written = stbsp_snprintf(temp, sizeof(temp), fmt_hex, val);
                if (written > 0) {
                    int copy_len = written;
                    if (dst + copy_len > end) {
                        copy_len = (int)(end - dst);
                    }
                    memcpy(dst, temp, copy_len);
                    dst += copy_len;
                }
                break;
            }
            
            case FMT_TOKEN_FLOAT: {
                double val = va_arg(args, double);
                char temp[64];
                int written = stbsp_snprintf(temp, sizeof(temp), fmt_float, val);
                if (written > 0) {
                    int copy_len = written;
                    if (dst + copy_len > end) {
                        copy_len = (int)(end - dst);
                    }
                    memcpy(dst, temp, copy_len);
                    dst += copy_len;
                }
                break;
            }
            
            case FMT_TOKEN_POINTER: {
                void *val = va_arg(args, void*);
                char temp[64];
                int written = stbsp_snprintf(temp, sizeof(temp), fmt_ptr, val);
                if (written > 0) {
                    int copy_len = written;
                    if (dst + copy_len > end) {
                        copy_len = (int)(end - dst);
                    }
                    memcpy(dst, temp, copy_len);
                    dst += copy_len;
                }
                break;
            }
            
            case FMT_TOKEN_EMPTY:
            case FMT_TOKEN_UNKNOWN:
                // Copy placeholder as-is (can't determine type from va_list)
                if (token_len > 0 && dst + token_len < end) {
                    memcpy(dst, token_start, token_len);
                    dst += token_len;
                }
                break;
        }
    }
    
    *dst = '\0';
    return (int)(dst - buf);
}

/**
 * Format a string using fmt-style {} placeholders with variadic arguments.
 * 
 * @param buf Output buffer
 * @param size Size of output buffer
 * @param fmt Format string with typed {} placeholders
 * @param ... Variable arguments matching the placeholders
 * @return Number of characters written (excluding null terminator)
 */
static inline int fmt_va_print(char *buf, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int result = fmt_va_vprint(buf, size, fmt, args);
    va_end(args);
    return result;
}

#ifdef __cplusplus
}
#endif

#endif // FMT_VA_H
