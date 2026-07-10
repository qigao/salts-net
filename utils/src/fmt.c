/**
 * @file fmt.c
 * @brief High-performance type-safe formatting implementation
 */

#include "platform.h"
#include "fmt.h"
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Suppress warnings for dynamic printf-compatible format strings. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#pragma GCC diagnostic ignored "-Wformat-contains-nul"
#endif

/* ============================================================================
 * Formatting Helpers
 * ============================================================================ */

enum {
  FMT_TEMP_CAP = 256,
  FMT_FORMAT_CAP = 64,
  FMT_MODIFIER_MAX = 59
};

typedef struct {
  char text[FMT_FORMAT_CAP];
  size_t len;
} fmt_spec_t;

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

FMT_DEFINE_VWRITE(libc, FMT_CALL_LIBC)
FMT_DEFINE_WRITE(libc)

#undef FMT_DEFINE_WRITE
#undef FMT_DEFINE_VWRITE
#undef FMT_CALL_LIBC

static inline fmt_spec_t fmt_spec_from_token(const char *modifier, size_t mod_len) {
  fmt_spec_t spec;
  spec.text[0] = '\0';
  spec.len = 0;

  if (modifier && mod_len > 0 && mod_len <= FMT_MODIFIER_MAX) {
    memcpy(spec.text, modifier, mod_len);
    spec.text[mod_len] = '\0';
    spec.len = mod_len;
  }

  return spec;
}

static inline int fmt_spec_empty(const fmt_spec_t *spec) {
  return !spec || spec->len == 0;
}

static inline int fmt_spec_has_conversion(const fmt_spec_t *spec, const char *conversion_chars) {
  return !fmt_spec_empty(spec) && strpbrk(spec->text, conversion_chars) != NULL;
}

static inline int fmt_spec_printf(char *dst, size_t dst_size, const fmt_spec_t *spec,
                                  const char *default_suffix, const char *conversion_chars) {
  size_t pos = 0;
  size_t suffix_len = fmt_spec_has_conversion(spec, conversion_chars) ? 0 : strlen(default_suffix);
  size_t needed = 1 + (spec ? spec->len : 0) + suffix_len;

  if (!dst || dst_size == 0 || needed >= dst_size)
    return 0;

  dst[pos++] = '%';
  if (spec && spec->len > 0) {
    memcpy(dst + pos, spec->text, spec->len);
    pos += spec->len;
  }
  if (suffix_len > 0) {
    memcpy(dst + pos, default_suffix, suffix_len);
    pos += suffix_len;
  }
  dst[pos] = '\0';
  return 1;
}

static inline int format_arg_to_buffer(char *dst, char *end, const fmt_arg_t *arg,
                                       const char *modifier, size_t mod_len) {
  fmt_spec_t spec = fmt_spec_from_token(modifier, mod_len);

  switch (arg->type) {
  case FMT_TYPE_CHAR:
    if (!fmt_spec_empty(&spec)) {
      char fb[FMT_FORMAT_CAP] = {0};
      if (!fmt_spec_printf(fb, sizeof(fb), &spec, "c", "c"))
        return 0;
      return fmt_write_libc(dst, end, fb, arg->val.c);
    }
    return fmt_write_libc(dst, end, "%c", arg->val.c);

  case FMT_TYPE_INT:
    if (!fmt_spec_empty(&spec)) {
      char fb[FMT_FORMAT_CAP] = {0};
      if (!fmt_spec_printf(fb, sizeof(fb), &spec, "d", "diouxXc"))
        return 0;
      return fmt_write_libc(dst, end, fb, arg->val.i);
    }
    return fmt_write_libc(dst, end, "%d", arg->val.i);

  case FMT_TYPE_UINT:
    if (!fmt_spec_empty(&spec)) {
      char fb[FMT_FORMAT_CAP] = {0};
      if (!fmt_spec_printf(fb, sizeof(fb), &spec, "u", "diouxXc"))
        return 0;
      return fmt_write_libc(dst, end, fb, arg->val.u);
    }
    return fmt_write_libc(dst, end, "%u", arg->val.u);

  case FMT_TYPE_LONG:
    if (!fmt_spec_empty(&spec)) {
      char fb[FMT_FORMAT_CAP] = {0};
      if (!fmt_spec_printf(fb, sizeof(fb), &spec, "ld", "diouxXc"))
        return 0;
      return fmt_write_libc(dst, end, fb, arg->val.l);
    }
    return fmt_write_libc(dst, end, "%ld", arg->val.l);

  case FMT_TYPE_ULONG:
    if (!fmt_spec_empty(&spec)) {
      char fb[FMT_FORMAT_CAP] = {0};
      if (!fmt_spec_printf(fb, sizeof(fb), &spec, "lu", "ouxXc"))
        return 0;
      return fmt_write_libc(dst, end, fb, arg->val.ul);
    }
    return fmt_write_libc(dst, end, "%lu", arg->val.ul);

  case FMT_TYPE_LLONG:
    if (!fmt_spec_empty(&spec)) {
      char fb[FMT_FORMAT_CAP] = {0};
      if (!fmt_spec_printf(fb, sizeof(fb), &spec, "lld", "diouxXc"))
        return 0;
      return fmt_write_libc(dst, end, fb, arg->val.ll);
    }
    return fmt_write_libc(dst, end, "%lld", arg->val.ll);

  case FMT_TYPE_ULLONG:
    if (!fmt_spec_empty(&spec)) {
      char fb[FMT_FORMAT_CAP] = {0};
      if (!fmt_spec_printf(fb, sizeof(fb), &spec, "llu", "ouxXc"))
        return 0;
      return fmt_write_libc(dst, end, fb, arg->val.ull);
    }
    return fmt_write_libc(dst, end, "%llu", arg->val.ull);

  case FMT_TYPE_DOUBLE:
    if (!fmt_spec_empty(&spec)) {
      char fb[FMT_FORMAT_CAP] = {0};
      if (!fmt_spec_printf(fb, sizeof(fb), &spec, "g", "fegEG"))
        return 0;
      return fmt_write_libc(dst, end, fb, arg->val.f);
    }
    return fmt_write_libc(dst, end, "%.17g", arg->val.f);

  case FMT_TYPE_STR: {
    const char *s = arg->val.s ? arg->val.s : "(null)";
    if (!fmt_spec_empty(&spec)) {
      char fb[FMT_FORMAT_CAP] = {0};
      if (!fmt_spec_printf(fb, sizeof(fb), &spec, "s", "s"))
        return 0;
      return fmt_write_libc(dst, end, fb, s);
    }
    return fmt_copy_to_buffer(dst, end, s, strlen(s));
  }

  case FMT_TYPE_PTR:
    return fmt_write_libc(dst, end, "%p", arg->val.p);

  case FMT_TYPE_SIZE:
    return fmt_write_libc(dst, end, "%zu", arg->val.sz);

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
    const char *time_fmt = !fmt_spec_empty(&spec) ? spec.text : "%Y-%m-%d %H:%M:%S";
    int written = (int)strftime(temp, sizeof(temp), time_fmt, &tm_buf);
    if (written > 0 && fmt_spec_empty(&spec) && arg->val.tv.tv_usec > 0) {
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
    return fmt_write_libc(dst, end, "0x%llx", (unsigned long long)(uintptr_t)arg->val.p);
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

CXX_C_API tstr_t fmt_print_tstr(tstr_t s, const char *fmt, const fmt_arg_t *args,
                                size_t arg_count) {
  enum { FMT_TSTR_STACK_CAP = 256 };
  char stack[FMT_TSTR_STACK_CAP];
  char *buf = stack;
  size_t cap = FMT_TSTR_STACK_CAP;

  if (!s)
    s = tstr_new();
  if (!fmt || (!args && arg_count > 0))
    return s;

  for (;;) {
    int written = fmt_print(buf, cap, fmt, args, arg_count);
    if (written < 0)
      break;

    if ((size_t)written < cap - 1) {
      if (written > 0)
        s = tstr_cat_len(s, buf, (size_t)written);
      break;
    }

    if (buf != stack)
      free(buf);
    if (cap > (SIZE_MAX / 2))
      return s;

    cap *= 2;
    buf = (char *)malloc(cap);
    if (!buf)
      return s;
  }

  if (buf != stack)
    free(buf);
  return s;
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
