/**
 * @file fmt.c
 * @brief High-performance type-safe formatting implementation
 */

#include "platform.h"
#include "fmt.h"
#include "sds.h"
#include "stb_sprintf.h"
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Suppress warnings for stb_sprintf optimization and dynamic format strings */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#pragma GCC diagnostic ignored "-Wformat-contains-nul"
#endif

/*
 * stb_sprintf optimization reads 4-8 bytes at a time for performance.
 * Short format strings can cause AddressSanitizer overreads.
 */
#define P(s) (s "\0\0\0\0\0\0\0\0")

/* ============================================================================
 * Formatting Helpers
 * ============================================================================ */

enum {
  FMT_TEMP_CAP = 256,
  FMT_FORMAT_CAP = 64,
  FMT_MODIFIER_MAX = 59
};

static inline size_t fmt_available(const char *dst, const char *end) {
  return (dst < end) ? (size_t)(end - dst) : 0;
}

static inline int fmt_copy_to_buffer(char *dst, char *end, const char *src, size_t len) {
  size_t avail = fmt_available(dst, end);
  if (len > avail)
    len = avail;
  if (len > 0)
    memcpy(dst, src, len);
  return (int)len;
}

#define FMT_CALL_STB(buf, cap, format, ap)  stbsp_vsnprintf((buf), (int)(cap), (format), (ap))
#define FMT_CALL_LIBC(buf, cap, format, ap) vsnprintf((buf), (cap), (format), (ap))

#define FMT_DEFINE_VWRITE(name, call_backend)                                                        \
  static int fmt_vwrite_##name(char *dst, char *end, const char *format, va_list ap) {              \
    char temp[FMT_TEMP_CAP];                                                                         \
    size_t avail = fmt_available(dst, end);                                                          \
    va_list ap_copy;                                                                                 \
    va_copy(ap_copy, ap);                                                                            \
                                                                                                     \
    int written = call_backend(temp, sizeof(temp), format, ap);                                      \
    if (written <= 0) {                                                                              \
      va_end(ap_copy);                                                                               \
      return 0;                                                                                      \
    }                                                                                                \
                                                                                                     \
    size_t requested = (size_t)written;                                                              \
    size_t copy_len = requested;                                                                     \
    if (copy_len > avail)                                                                            \
      copy_len = avail;                                                                              \
                                                                                                     \
    if (requested < sizeof(temp)) {                                                                  \
      va_end(ap_copy);                                                                               \
      return fmt_copy_to_buffer(dst, end, temp, copy_len);                                           \
    }                                                                                                \
                                                                                                     \
    if (copy_len == 0) {                                                                             \
      va_end(ap_copy);                                                                               \
      return 0;                                                                                      \
    }                                                                                                \
                                                                                                     \
    if (copy_len > (size_t)INT_MAX - 1)                                                              \
      copy_len = (size_t)INT_MAX - 1;                                                                \
    char *dynamic = (char *)malloc(copy_len + 1);                                                    \
    if (!dynamic) {                                                                                  \
      va_end(ap_copy);                                                                               \
      return 0;                                                                                      \
    }                                                                                                \
                                                                                                     \
    int second = call_backend(dynamic, copy_len + 1, format, ap_copy);                               \
    va_end(ap_copy);                                                                                 \
    if (second <= 0) {                                                                               \
      free(dynamic);                                                                                 \
      return 0;                                                                                      \
    }                                                                                                \
                                                                                                     \
    int copied = fmt_copy_to_buffer(dst, end, dynamic, copy_len);                                    \
    free(dynamic);                                                                                   \
    return copied;                                                                                   \
  }

#define FMT_DEFINE_WRITE(name)                                                                       \
  static int fmt_write_##name(char *dst, char *end, const char *format, ...) {                       \
    va_list ap;                                                                                      \
    va_start(ap, format);                                                                            \
    int copied = fmt_vwrite_##name(dst, end, format, ap);                                            \
    va_end(ap);                                                                                      \
    return copied;                                                                                   \
  }

FMT_DEFINE_VWRITE(stb, FMT_CALL_STB)
FMT_DEFINE_WRITE(stb)
FMT_DEFINE_VWRITE(libc, FMT_CALL_LIBC)
FMT_DEFINE_WRITE(libc)

#undef FMT_DEFINE_WRITE
#undef FMT_DEFINE_VWRITE
#undef FMT_CALL_LIBC
#undef FMT_CALL_STB

static inline void fmt_build_format(char *dst, size_t dst_size, const char *modifier,
                                    const char *default_suffix, const char *conversion_chars) {
  if (strpbrk(modifier, conversion_chars) != NULL) {
    snprintf(dst, dst_size, "%%%s", modifier);
  } else {
    snprintf(dst, dst_size, "%%%s%s", modifier, default_suffix);
  }
}

static inline int format_arg_to_buffer(char *dst, char *end, const fmt_arg_t *arg,
                                       const char *modifier, size_t mod_len) {
  char mod_buf[FMT_FORMAT_CAP] = {0};

  if (mod_len > 0 && mod_len <= FMT_MODIFIER_MAX) {
    memcpy(mod_buf, modifier, mod_len);
    mod_buf[mod_len] = '\0';
  }

  switch (arg->type) {
  case FMT_TYPE_CHAR:
    if (mod_buf[0]) {
      char fb[FMT_FORMAT_CAP] = {0};
      fmt_build_format(fb, sizeof(fb), mod_buf, "c", "c");
      return fmt_write_stb(dst, end, fb, arg->val.c);
    }
    return fmt_write_stb(dst, end, P("%c"), arg->val.c);

  case FMT_TYPE_INT:
    if (mod_buf[0]) {
      char fb[FMT_FORMAT_CAP] = {0};
      fmt_build_format(fb, sizeof(fb), mod_buf, "d", "diouxXc");
      return fmt_write_stb(dst, end, fb, arg->val.i);
    }
    return fmt_write_stb(dst, end, P("%d"), arg->val.i);

  case FMT_TYPE_UINT:
    if (mod_buf[0]) {
      char fb[FMT_FORMAT_CAP] = {0};
      fmt_build_format(fb, sizeof(fb), mod_buf, "u", "diouxXc");
      return fmt_write_stb(dst, end, fb, arg->val.u);
    }
    return fmt_write_stb(dst, end, P("%u"), arg->val.u);

  case FMT_TYPE_LONG:
    if (mod_buf[0]) {
      char fb[FMT_FORMAT_CAP] = {0};
      fmt_build_format(fb, sizeof(fb), mod_buf, "ld", "diouxXc");
      return fmt_write_stb(dst, end, fb, arg->val.l);
    }
    return fmt_write_stb(dst, end, P("%ld"), arg->val.l);

  case FMT_TYPE_ULONG:
    if (mod_buf[0]) {
      char fb[FMT_FORMAT_CAP] = {0};
      fmt_build_format(fb, sizeof(fb), mod_buf, "lu", "ouxXc");
      return fmt_write_stb(dst, end, fb, arg->val.ul);
    }
    return fmt_write_stb(dst, end, P("%lu"), arg->val.ul);

  case FMT_TYPE_LLONG:
    if (mod_buf[0]) {
      char fb[FMT_FORMAT_CAP] = {0};
      fmt_build_format(fb, sizeof(fb), mod_buf, "lld", "diouxXc");
      return fmt_write_stb(dst, end, fb, arg->val.ll);
    }
    return fmt_write_stb(dst, end, P("%lld"), arg->val.ll);

  case FMT_TYPE_ULLONG:
    if (mod_buf[0]) {
      char fb[FMT_FORMAT_CAP] = {0};
      fmt_build_format(fb, sizeof(fb), mod_buf, "llu", "ouxXc");
      return fmt_write_stb(dst, end, fb, arg->val.ull);
    }
    return fmt_write_stb(dst, end, P("%llu"), arg->val.ull);

  case FMT_TYPE_DOUBLE:
    if (mod_buf[0]) {
      char fb[FMT_FORMAT_CAP] = {0};
      fmt_build_format(fb, sizeof(fb), mod_buf, "g", "fegEG");
      return fmt_write_stb(dst, end, fb, arg->val.f);
    }
    return fmt_write_stb(dst, end, P("%.17g"), arg->val.f);

  case FMT_TYPE_STR: {
    const char *s = arg->val.s ? arg->val.s : "(null)";
    if (mod_buf[0]) {
      char fb[FMT_FORMAT_CAP] = {0};
      fmt_build_format(fb, sizeof(fb), mod_buf, "s", "s");
      return fmt_write_libc(dst, end, fb, s);
    }
    return fmt_copy_to_buffer(dst, end, s, strlen(s));
  }

  case FMT_TYPE_PTR:
    return fmt_write_stb(dst, end, P("%p"), arg->val.p);

  case FMT_TYPE_SIZE:
    return fmt_write_stb(dst, end, P("%zu"), arg->val.sz);

  case FMT_TYPE_BOOL: {
    const char *bstr = arg->val.b ? "true" : "false";
    size_t blen = arg->val.b ? 4 : 5;
    return fmt_copy_to_buffer(dst, end, bstr, blen);
  }

  case FMT_TYPE_STRV: {
    const char *s = arg->val.sv.data ? arg->val.sv.data : "(null)";
    size_t slen = arg->val.sv.data ? arg->val.sv.len : 6;
    return fmt_copy_to_buffer(dst, end, s, slen);
  }

  case FMT_TYPE_TIME: {
    time_t sec = (time_t)arg->val.tv.tv_sec;
    struct tm tm_buf;
#ifdef _WIN32
    if (localtime_s(&tm_buf, &sec) != 0) {
      return fmt_copy_to_buffer(dst, end, "(invalid time)", 14);
    }
#else
    if (!localtime_r(&sec, &tm_buf)) {
      return fmt_copy_to_buffer(dst, end, "(invalid time)", 14);
    }
#endif
    char temp[FMT_TEMP_CAP];
    const char *time_fmt = (mod_buf[0]) ? mod_buf : "%Y-%m-%d %H:%M:%S";
    int written = (int)strftime(temp, sizeof(temp), time_fmt, &tm_buf);
    if (written > 0 && !mod_buf[0] && arg->val.tv.tv_usec > 0) {
      int ms = arg->val.tv.tv_usec / 1000;
      written += snprintf(temp + written, sizeof(temp) - (size_t)written, ".%03d", ms);
    }
    if (written <= 0)
      return 0;
    size_t copy_len = (size_t)written;
    if (copy_len >= sizeof(temp))
      copy_len = sizeof(temp) - 1;
    return fmt_copy_to_buffer(dst, end, temp, copy_len);
  }

  default:
    return fmt_write_stb(dst, end, P("0x%llx"), (unsigned long long)(uintptr_t)arg->val.p);
  }
}

/* ============================================================================
 * Internal re2c Formatting Loop
 * ============================================================================ */

CXX_C_API int fmt_print(char *buf, size_t size, const char *fmt, const fmt_arg_t *args,
                        size_t arg_count) {
  if (!buf || size == 0)
    return 0;
  buf[0] = '\0';
  if (!fmt || (!args && arg_count > 0))
    return 0;

  const char *cursor = fmt;
  char *dst = buf;
  char *end = buf + size - 1; /* Room for null terminator */
  size_t arg_idx = 0;
  tstr_v token_view = tstr_v_from_buf(NULL, 0);

  while (dst < end) {
    fmt_token_t token = fmt_scan_v(&cursor, &token_view);

    switch (token) {
    case FMT_TOKEN_END:
      goto done;

    case FMT_TOKEN_TEXT: {
      size_t len = token_view.len;
      int copied = fmt_copy_to_buffer(dst, end, token_view.data, len);
      dst += copied;
      break;
    }

    case FMT_TOKEN_LBRACE_ESC:
      if (dst < end)
        *dst++ = '{';
      break;

    case FMT_TOKEN_RBRACE_ESC:
      if (dst < end)
        *dst++ = '}';
      break;

    case FMT_TOKEN_PLACEHOLDER:
      if (arg_idx < arg_count) {
        dst += format_arg_to_buffer(dst, end, &args[arg_idx++], NULL, 0);
      } else {
        if (dst < end)
          *dst++ = '{';
        if (dst < end)
          *dst++ = '}';
      }
      break;

    case FMT_TOKEN_SPECIFIER:
      if (arg_idx < arg_count) {
        // token_start points to internal content, length is token_len
        dst += format_arg_to_buffer(dst, end, &args[arg_idx++], token_view.data, token_view.len);
      } else {
        dst += fmt_copy_to_buffer(dst, end, "{:", 2);
        dst += fmt_copy_to_buffer(dst, end, token_view.data, token_view.len);
        dst += fmt_copy_to_buffer(dst, end, "}", 1);
      }
      break;

    case FMT_TOKEN_INVALID:
      // Copy exact content
      {
        size_t len = token_view.len;
        int copied = fmt_copy_to_buffer(dst, end, token_view.data, len);
        dst += copied;
      }
      break;
    }
  }

done:
  if (size > 0) {
    if (dst >= end) {
      buf[size - 1] = '\0';
    } else {
      *dst = '\0';
    }
  }
  return (int)(dst - buf);
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
