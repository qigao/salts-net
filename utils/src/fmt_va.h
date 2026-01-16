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
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "stb_sprintf.h"
#include "../parser/fmt_lexer.h"

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
    char *end = buf + size - 1; // Reserve 1 for null terminator
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
        fmt_token_t token = fmt_scan(&cursor, &token_start, &token_len);
        
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
            
            case FMT_TOKEN_LBRACE_ESC:
                if (dst < end) *dst++ = '{';
                break;

            case FMT_TOKEN_RBRACE_ESC:
                if (dst < end) *dst++ = '}';
                break;
                
            case FMT_TOKEN_SPECIFIER: {
                // token_start points to content inside {:, length is token_len
                // We need to guess the type from the specifier string.
                // Simple heuristic: check the last character.
                char type = (token_len > 0) ? token_start[token_len - 1] : 0;
                
                // Construct a printf-style format string for stb_sprintf
                // We map {:[flags]type} to %[flags]type
                // But stb_sprintf might not support all fmt.h modifiers. 
                // For safety in this VA bridge, we primarily look for the type char
                // and use a simple % format, ignoring complex precision for now unless we reconstructing it.
                // However, the USER expectations are probably that modifiers work.
                
                // Reconstructing full format string: "%" + content
                char fmt_buf[64];
                if (token_len < 60) {
                    fmt_buf[0] = '%';
                    memcpy(fmt_buf + 1, token_start, token_len);
                    fmt_buf[token_len + 1] = '\0';
                } else {
                    // Too long, fallback to default
                    type = 0; 
                }

                char temp[256];
                int written = 0;

                switch (type) {
                    case 's': {
                        const char *str = va_arg(args, const char*);
                        // Safety check for common invalid pointers that cause crashes
                        // 0xFFFFFFFFFFFFFFFF (-1) is a common error result cast to pointer
                        if (str == (const char*)(uintptr_t)-1) {
                            str = "(invalid_ptr_-1)";
                        }
                        written = stbsp_snprintf(temp, sizeof(temp), fmt_buf, str ? str : "(null)");
                        break;
                    }
                    case 'd': // int
                    case 'i': {
                        int val = va_arg(args, int);
                        written = stbsp_snprintf(temp, sizeof(temp), fmt_buf, val);
                        break;
                    }
                    case 'u': { // unsigned
                        unsigned int val = va_arg(args, unsigned int);
                        written = stbsp_snprintf(temp, sizeof(temp), fmt_buf, val);
                        break;
                    }
                    case 'x': 
                    case 'X': { // hex
                        unsigned int val = va_arg(args, unsigned int);
                        written = stbsp_snprintf(temp, sizeof(temp), fmt_buf, val);
                        break;
                    }
                    case 'f':
                    case 'g': 
                    case 'e':
                    case 'G':
                    case 'E': { // double/float
                        double val = va_arg(args, double);
                        written = stbsp_snprintf(temp, sizeof(temp), fmt_buf, val);
                        break;
                    }
                    case 'p': { // pointer
                        void *val = va_arg(args, void*);
                        written = stbsp_snprintf(temp, sizeof(temp), fmt_buf, val);
                        break;
                    }
                    default:
                        // Unknown or no type specifier found at end. 
                        // It might be precision-only (e.g. {:.2}) which defaults to float in fmt.h usually?
                        // Or just print the placeholder raw as fallback?
                        // For safety, let's treat it as a string if it looks like just alignment? 
                        // Actually, without type info, we can't safely va_arg. 
                        // So we print the raw placeholder "{:...}"
                        
                        // We need to reconstruct the braces
                        if (dst + token_len + 4 <= end) { // {:...}
                             *dst++ = '{'; *dst++ = ':';
                             memcpy(dst, token_start, token_len);
                             dst += token_len;
                             *dst++ = '}';
                        }
                        written = 0;
                        break;
                }
                
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
            
            case FMT_TOKEN_PLACEHOLDER:
            case FMT_TOKEN_INVALID:
            default:
                // Copy placeholder as-is (can't determine type from va_list)
                 if (dst < end) *dst++ = '{'; 
                 // Note: Logic for invalid might need to be more robust to print original chars
                 // But since lexer consumes the whole invalid block, we might not have it all easily accessable 
                 // if we only have payload. 
                 // Actually fmt_scan returns payload. For PLACEHOLDER payload is empty. 
                 // For INVALID, payload is the content.
                 
                 if (token == FMT_TOKEN_PLACEHOLDER) {
                     if (dst < end) *dst++ = '}';
                 } else {
                     // INVALID: print content then close brace? 
                     // fmt_lexer returns inner content IIRC? 
                     // Wait, fmt_lexer.re for INVALID:
                     // "*token_len = (size_t)(YYCURSOR - start);" -> includes braces!
                     // So we just copy token content.
                     // But wait, scan signature: token_start, token_len. 
                     // For INVALID, it covers the whole match.
                     // For PLACEHOLDER, it covers 0 len?
                     // Let's check generated code behavior or .re file
                     
                     // .re:
                     // "{}" -> token_len = 2; return PLACEHOLDER.
                     // Invalid -> token_len = full match.
                     
                     // So for both, we can just copy token_start[0..token_len]
                     // Wait, for PLACEHOLDER, token_len=2, start points to "{".
                     // For SPECIFIER, start points to AFTER "{:".
                     
                     if (token == FMT_TOKEN_SPECIFIER) {
                         // Already handled above
                     } else {
                        // For PLACEHOLDER, INVALID, TEXT -> copy [token_start, token_start+token_len]
                        // Re-do the copy logic to be generic for pass-through
                        
                        // Wait, my SPECIFIER block handled it separately.
                        // So here we only handle PLACEHOLDER and INVALID and TEXT-fallback?
                        // TEXT is handled.
                        
                        int copy_len = (int)token_len;
                        if (dst + copy_len > end) {
                            copy_len = (int)(end - dst);
                        }
                        memcpy(dst, token_start, copy_len);
                        dst += copy_len;
                     }
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
