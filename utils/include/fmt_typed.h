/**
 * @file fmt_typed.h
 * @brief Type-safe formatting using C11 _Generic
 * 
 * This module provides compile-time type detection for format arguments,
 * allowing {} placeholders to work without explicit type specifiers.
 * 
 * Usage:
 *   TLOG_INFO("Count: {}, Name: {}", 42, "test");
 *   // Automatically detects: int, const char*
 */

#ifndef FMT_TYPED_H
#define FMT_TYPED_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "stb_sprintf.h"
#include "../parser/fmt_lexer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Type Tags
 * ============================================================================ */

typedef enum {
    FMT_TYPE_NONE = 0,
    FMT_TYPE_CHAR,
    FMT_TYPE_INT,
    FMT_TYPE_UINT,
    FMT_TYPE_LONG,
    FMT_TYPE_ULONG,
    FMT_TYPE_LLONG,
    FMT_TYPE_ULLONG,
    FMT_TYPE_DOUBLE,
    FMT_TYPE_STR,
    FMT_TYPE_PTR,
    FMT_TYPE_SIZE,
    FMT_TYPE_BOOL
} fmt_type_t;

/* ============================================================================
 * Tagged Argument Structure
 * ============================================================================ */

typedef struct {
    fmt_type_t type;
    union {
        char c;
        int i;
        unsigned int u;
        long l;
        unsigned long ul;
        long long ll;
        unsigned long long ull;
        double f;
        const char* s;
        const void* p;
        size_t sz;
        int b;  /* bool stored as int */
    } val;
} fmt_arg_t;

/* ============================================================================
 * _Generic Type Wrapper Macros (C11)
 * ============================================================================ */

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L

/* Helper functions to create typed args - avoids _Generic type conversion issues */
static inline fmt_arg_t fmt_arg_char(char x) { return (fmt_arg_t){FMT_TYPE_CHAR, {.c = x}}; }
static inline fmt_arg_t fmt_arg_int(int x) { return (fmt_arg_t){FMT_TYPE_INT, {.i = x}}; }
static inline fmt_arg_t fmt_arg_uint(unsigned int x) { return (fmt_arg_t){FMT_TYPE_UINT, {.u = x}}; }
static inline fmt_arg_t fmt_arg_long(long x) { return (fmt_arg_t){FMT_TYPE_LONG, {.l = x}}; }
static inline fmt_arg_t fmt_arg_ulong(unsigned long x) { return (fmt_arg_t){FMT_TYPE_ULONG, {.ul = x}}; }
static inline fmt_arg_t fmt_arg_llong(long long x) { return (fmt_arg_t){FMT_TYPE_LLONG, {.ll = x}}; }
static inline fmt_arg_t fmt_arg_ullong(unsigned long long x) { return (fmt_arg_t){FMT_TYPE_ULLONG, {.ull = x}}; }
static inline fmt_arg_t fmt_arg_double(double x) { return (fmt_arg_t){FMT_TYPE_DOUBLE, {.f = x}}; }
static inline fmt_arg_t fmt_arg_str(const char* x) { return (fmt_arg_t){FMT_TYPE_STR, {.s = x}}; }
static inline fmt_arg_t fmt_arg_ptr(const void* x) { return (fmt_arg_t){FMT_TYPE_PTR, {.p = x}}; }

/* Wrap a single argument with type information
 * Note: No default case - we explicitly list all supported types.
 * If a type is not listed, you'll get a compile error (which is better than UB).
 */
#define FMT_ARG(x) _Generic((x), \
    char:               fmt_arg_char((char)(x)), \
    signed char:        fmt_arg_char((char)(x)), \
    unsigned char:      fmt_arg_char((char)(x)), \
    short:              fmt_arg_int((int)(x)), \
    unsigned short:     fmt_arg_uint((unsigned)(x)), \
    int:                fmt_arg_int(x), \
    unsigned int:       fmt_arg_uint(x), \
    long:               fmt_arg_long(x), \
    unsigned long:      fmt_arg_ulong(x), \
    long long:          fmt_arg_llong(x), \
    unsigned long long: fmt_arg_ullong(x), \
    float:              fmt_arg_double((double)(x)), \
    double:             fmt_arg_double(x), \
    char*:              fmt_arg_str((const char*)(x)), \
    const char*:        fmt_arg_str(x), \
    void*:              fmt_arg_ptr(x), \
    const void*:        fmt_arg_ptr(x) \
)

#else
/* Fallback for pre-C11: no type detection, treat as int */
static inline fmt_arg_t fmt_arg_int_fallback(int x) { return (fmt_arg_t){FMT_TYPE_INT, {.i = x}}; }
#define FMT_ARG(x) fmt_arg_int_fallback((int)(x))
#endif



/* ============================================================================
 * Argument Count Macros (for variadic)
 * ============================================================================ */

/* Count arguments (up to 16) */
#define FMT_NARGS_IMPL(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16,N,...) N
#define FMT_NARGS(...) FMT_NARGS_IMPL(__VA_ARGS__,16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0)

/* Expand each argument with FMT_ARG */
#define FMT_WRAP_1(a)  FMT_ARG(a)
#define FMT_WRAP_2(a,b)  FMT_ARG(a), FMT_ARG(b)
#define FMT_WRAP_3(a,b,c)  FMT_ARG(a), FMT_ARG(b), FMT_ARG(c)
#define FMT_WRAP_4(a,b,c,d)  FMT_ARG(a), FMT_ARG(b), FMT_ARG(c), FMT_ARG(d)
#define FMT_WRAP_5(a,b,c,d,e)  FMT_ARG(a), FMT_ARG(b), FMT_ARG(c), FMT_ARG(d), FMT_ARG(e)
#define FMT_WRAP_6(a,b,c,d,e,f)  FMT_ARG(a), FMT_ARG(b), FMT_ARG(c), FMT_ARG(d), FMT_ARG(e), FMT_ARG(f)
#define FMT_WRAP_7(a,b,c,d,e,f,g)  FMT_WRAP_6(a,b,c,d,e,f), FMT_ARG(g)
#define FMT_WRAP_8(a,b,c,d,e,f,g,h)  FMT_WRAP_7(a,b,c,d,e,f,g), FMT_ARG(h)

/* Dispatch to correct wrapper based on count */
#define FMT_WRAP_N_IMPL(N, ...) FMT_WRAP_##N(__VA_ARGS__)
#define FMT_WRAP_N(N, ...) FMT_WRAP_N_IMPL(N, __VA_ARGS__)

/* Main wrapper: creates array of typed args */
#define FMT_ARGS(...) (fmt_arg_t[]){ FMT_WRAP_N(FMT_NARGS(__VA_ARGS__), __VA_ARGS__) }

/* ============================================================================
 * Type-Safe Formatting Function
 * ============================================================================ */

/**
 * @brief Format a string using typed arguments
 * 
 * @param buf      Output buffer
 * @param size     Buffer size
 * @param fmt      Format string with {} placeholders
 * @param args     Array of typed arguments
 * @param arg_count Number of arguments
 * @return Number of characters written
 */
static inline int fmt_typed_print(char* buf, size_t size, const char* fmt, 
                                   const fmt_arg_t* args, size_t arg_count) {
    if (!buf || !fmt || size == 0) return 0;

    char* dst = buf;
    char* end = buf + size - 1;
    const char* cursor = fmt;
    size_t arg_idx = 0;

    while (dst < end) {
        const char* token_start;
        size_t token_len;
        fmt_token_t token = fmt_scan(&cursor, &token_start, &token_len);

        switch (token) {
            case FMT_TOKEN_END:
                *dst = '\0';
                return (int)(dst - buf);

            case FMT_TOKEN_TEXT: {
                size_t copy_len = token_len;
                if (dst + copy_len > end) copy_len = (size_t)(end - dst);
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

            case FMT_TOKEN_PLACEHOLDER:
            case FMT_TOKEN_SPECIFIER: {
                /* Get next argument */
                if (arg_idx >= arg_count) {
                    /* No more arguments - print placeholder as-is */
                    if (dst < end) *dst++ = '{';
                    if (dst < end) *dst++ = '}';
                    break;
                }

                const fmt_arg_t* arg = &args[arg_idx++];
                char temp[256];
                int written = 0;

                /* For SPECIFIER, extract modifier (e.g., {:08x} -> "08x") */
                char modifier[64] = "";
                if (token == FMT_TOKEN_SPECIFIER && token_len > 0 && token_len < 60) {
                    memcpy(modifier, token_start, token_len);
                    modifier[token_len] = '\0';
                }

                /* Format based on type */
                switch (arg->type) {
                    case FMT_TYPE_CHAR: {
                        if (modifier[0]) {
                            char fmt_buf[64];
                            stbsp_snprintf(fmt_buf, sizeof(fmt_buf), "%%%sc", modifier);
                            written = stbsp_snprintf(temp, sizeof(temp), fmt_buf, arg->val.c);
                        } else {
                            written = stbsp_snprintf(temp, sizeof(temp), "%c", arg->val.c);
                        }
                        break;
                    }
                    case FMT_TYPE_INT: {
                        if (modifier[0]) {
                            char fmt_buf[64];
                            stbsp_snprintf(fmt_buf, sizeof(fmt_buf), "%%%sd", modifier);
                            written = stbsp_snprintf(temp, sizeof(temp), fmt_buf, arg->val.i);
                        } else {
                            written = stbsp_snprintf(temp, sizeof(temp), "%d", arg->val.i);
                        }
                        break;
                    }
                    case FMT_TYPE_UINT: {
                        if (modifier[0]) {
                            char fmt_buf[64];
                            /* Check if modifier ends with x/X for hex */
                            size_t mlen = strlen(modifier);
                            char last = mlen > 0 ? modifier[mlen-1] : 'd';
                            if (last != 'x' && last != 'X' && last != 'o' && last != 'u') {
                                stbsp_snprintf(fmt_buf, sizeof(fmt_buf), "%%%su", modifier);
                            } else {
                                stbsp_snprintf(fmt_buf, sizeof(fmt_buf), "%%%s", modifier);
                            }
                            written = stbsp_snprintf(temp, sizeof(temp), fmt_buf, arg->val.u);
                        } else {
                            written = stbsp_snprintf(temp, sizeof(temp), "%u", arg->val.u);
                        }
                        break;
                    }
                    case FMT_TYPE_LONG: {
                        if (modifier[0]) {
                            char fmt_buf[64];
                            stbsp_snprintf(fmt_buf, sizeof(fmt_buf), "%%%sld", modifier);
                            written = stbsp_snprintf(temp, sizeof(temp), fmt_buf, arg->val.l);
                        } else {
                            written = stbsp_snprintf(temp, sizeof(temp), "%ld", arg->val.l);
                        }
                        break;
                    }
                    case FMT_TYPE_ULONG: {
                        written = stbsp_snprintf(temp, sizeof(temp), "%lu", arg->val.ul);
                        break;
                    }
                    case FMT_TYPE_LLONG: {
                        written = stbsp_snprintf(temp, sizeof(temp), "%lld", arg->val.ll);
                        break;
                    }
                    case FMT_TYPE_ULLONG: {
                        written = stbsp_snprintf(temp, sizeof(temp), "%llu", arg->val.ull);
                        break;
                    }
                    case FMT_TYPE_DOUBLE: {
                        if (modifier[0]) {
                            char fmt_buf[64];
                            size_t mlen = strlen(modifier);
                            char last = mlen > 0 ? modifier[mlen-1] : 'f';
                            if (last != 'f' && last != 'e' && last != 'E' && last != 'g' && last != 'G') {
                                stbsp_snprintf(fmt_buf, sizeof(fmt_buf), "%%%sf", modifier);
                            } else {
                                stbsp_snprintf(fmt_buf, sizeof(fmt_buf), "%%%s", modifier);
                            }
                            written = stbsp_snprintf(temp, sizeof(temp), fmt_buf, arg->val.f);
                        } else {
                            written = stbsp_snprintf(temp, sizeof(temp), "%g", arg->val.f);
                        }
                        break;
                    }
                    case FMT_TYPE_STR: {
                        const char* s = arg->val.s ? arg->val.s : "(null)";
                        if (modifier[0]) {
                            char fmt_buf[64];
                            stbsp_snprintf(fmt_buf, sizeof(fmt_buf), "%%%ss", modifier);
                            written = stbsp_snprintf(temp, sizeof(temp), fmt_buf, s);
                        } else {
                            written = stbsp_snprintf(temp, sizeof(temp), "%s", s);
                        }
                        break;
                    }
                    case FMT_TYPE_PTR: {
                        written = stbsp_snprintf(temp, sizeof(temp), "%p", arg->val.p);
                        break;
                    }
                    case FMT_TYPE_SIZE: {
                        written = stbsp_snprintf(temp, sizeof(temp), "%zu", arg->val.sz);
                        break;
                    }
                    case FMT_TYPE_BOOL: {
                        written = stbsp_snprintf(temp, sizeof(temp), "%s", 
                                                  arg->val.b ? "true" : "false");
                        break;
                    }
                    default:
                        /* Unknown type - print as hex */
                        written = stbsp_snprintf(temp, sizeof(temp), "0x%llx", 
                                                  (unsigned long long)(uintptr_t)arg->val.p);
                        break;
                }

                if (written > 0) {
                    size_t copy_len = (size_t)written;
                    if (dst + copy_len > end) copy_len = (size_t)(end - dst);
                    memcpy(dst, temp, copy_len);
                    dst += copy_len;
                }
                break;
            }

            case FMT_TOKEN_INVALID:
            default:
                /* Copy invalid token as-is */
                if (token_len > 0) {
                    size_t copy_len = token_len;
                    if (dst + copy_len > end) copy_len = (size_t)(end - dst);
                    memcpy(dst, token_start, copy_len);
                    dst += copy_len;
                }
                break;
        }
    }

    *dst = '\0';
    return (int)(dst - buf);
}

#ifdef __cplusplus
}
#endif

#endif /* FMT_TYPED_H */
