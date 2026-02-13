/**
 * @file fmt.c
 * @brief High-performance type-safe formatting implementation
 */

#include "platform.h"
#include "fmt.h"
#include "sds.h"
#include "stb_sprintf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
 

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

static inline int format_arg_to_buffer(char *dst, char *end, const fmt_arg_t *arg,
                                       const char *modifier, size_t mod_len) {
  char temp[256];
  char mod_buf[64] = {0};
  int written = 0;

  if (mod_len > 0 && mod_len < 60) {
    memcpy(mod_buf, modifier, mod_len);
    mod_buf[mod_len] = '\0';
  }

  switch (arg->type) {
  case FMT_TYPE_CHAR:
    if (mod_buf[0]) {
      char fb[64];
      snprintf(fb, sizeof(fb), (strpbrk(mod_buf, "c") != NULL) ? "%%%s" : "%%%sc", mod_buf);
      written = stbsp_snprintf(temp, sizeof(temp), fb, arg->val.c);
    } else {
      written = stbsp_snprintf(temp, sizeof(temp), P("%c"), arg->val.c);
    }
    break;

  case FMT_TYPE_INT:
    if (mod_buf[0]) {
      char fb[64];
      snprintf(fb, sizeof(fb), (strpbrk(mod_buf, "diouxXc") != NULL) ? "%%%s" : "%%%sd", mod_buf);
      written = stbsp_snprintf(temp, sizeof(temp), fb, arg->val.i);
    } else {
      written = stbsp_snprintf(temp, sizeof(temp), P("%d"), arg->val.i);
    }
    break;

  case FMT_TYPE_UINT:
    if (mod_buf[0]) {
      char fb[64];
      snprintf(fb, sizeof(fb), (strpbrk(mod_buf, "diouxXc") != NULL) ? "%%%s" : "%%%su", mod_buf);
      written = stbsp_snprintf(temp, sizeof(temp), fb, arg->val.u);
    } else {
      written = stbsp_snprintf(temp, sizeof(temp), P("%u"), arg->val.u);
    }
    break;

  case FMT_TYPE_LONG:
    if (mod_buf[0]) {
      char fb[64];
      snprintf(fb, sizeof(fb), (strpbrk(mod_buf, "diouxXc") != NULL) ? "%%%s" : "%%%sld", mod_buf);
      written = stbsp_snprintf(temp, sizeof(temp), fb, arg->val.l);
    } else {
      written = stbsp_snprintf(temp, sizeof(temp), P("%ld"), arg->val.l);
    }
    break;

  case FMT_TYPE_ULONG:
    if (mod_buf[0]) {
      char fb[64];
      snprintf(fb, sizeof(fb), (strpbrk(mod_buf, "ouxXc") != NULL) ? "%%%s" : "%%%slu", mod_buf);
      written = stbsp_snprintf(temp, sizeof(temp), fb, arg->val.ul);
    } else {
      written = stbsp_snprintf(temp, sizeof(temp), P("%lu"), arg->val.ul);
    }
    break;

  case FMT_TYPE_LLONG:
    if (mod_buf[0]) {
      char fb[64];
      snprintf(fb, sizeof(fb), (strpbrk(mod_buf, "diouxXc") != NULL) ? "%%%s" : "%%%slld", mod_buf);
      written = stbsp_snprintf(temp, sizeof(temp), fb, arg->val.ll);
    } else {
      written = stbsp_snprintf(temp, sizeof(temp), P("%lld"), arg->val.ll);
    }
    break;

  case FMT_TYPE_ULLONG:
    if (mod_buf[0]) {
      char fb[64];
      snprintf(fb, sizeof(fb), (strpbrk(mod_buf, "ouxXc") != NULL) ? "%%%s" : "%%%sllu", mod_buf);
      written = stbsp_snprintf(temp, sizeof(temp), fb, arg->val.ull);
    } else {
      written = stbsp_snprintf(temp, sizeof(temp), P("%llu"), arg->val.ull);
    }
    break;

  case FMT_TYPE_DOUBLE:
    if (mod_buf[0]) {
      char fb[64];
      snprintf(fb, sizeof(fb), (strpbrk(mod_buf, "fegEG") != NULL) ? "%%%s" : "%%%sg", mod_buf);
      written = stbsp_snprintf(temp, sizeof(temp), fb, arg->val.f);
    } else {
      written = stbsp_snprintf(temp, sizeof(temp), P("%g"), arg->val.f);
    }
    break;

  case FMT_TYPE_STR: {
    const char *s = arg->val.s ? arg->val.s : "(null)";
    if (mod_buf[0]) {
      char fb[64];
      snprintf(fb, sizeof(fb), (strpbrk(mod_buf, "s") != NULL) ? "%%%s" : "%%%ss", mod_buf);
      char *padded = sdsnewlen(s, strlen(s));
      if (!padded)
        return 0;
      padded = sdsMakeRoomFor(padded, 8);
      if (!padded)
        return 0;
      written = stbsp_snprintf(temp, sizeof(temp), fb, padded);
      sdsfree(padded);
    } else {
      size_t slen = strlen(s);
      if (dst + slen > end)
        slen = (size_t)(end - dst);
      memcpy(dst, s, slen);
      return (int)slen;
    }
    break;
  }

  case FMT_TYPE_PTR:
    written = stbsp_snprintf(temp, sizeof(temp), P("%p"), arg->val.p);
    break;

  case FMT_TYPE_SIZE:
    written = stbsp_snprintf(temp, sizeof(temp), P("%zu"), arg->val.sz);
    break;

  case FMT_TYPE_BOOL:
    written = stbsp_snprintf(temp, sizeof(temp), P("%s"), arg->val.b ? "true" : "false");
    break;

  case FMT_TYPE_STRV: {
    const char *s = arg->val.sv.data ? arg->val.sv.data : "(null)";
    size_t slen = arg->val.sv.data ? arg->val.sv.len : 6;
    if (dst + slen > end)
      slen = (size_t)(end - dst);
    memcpy(dst, s, slen);
    return (int)slen;
  }

  default:
    written =
        stbsp_snprintf(temp, sizeof(temp), P("0x%llx"), (unsigned long long)(uintptr_t)arg->val.p);
    break;
  }

  if (written > 0) {
    size_t copy_len = (size_t)written;
    if (dst + copy_len > end)
      copy_len = (size_t)(end - dst);
    memcpy(dst, temp, copy_len);
    return (int)copy_len;
  }
  return 0;
}

/* ============================================================================
 * Internal re2c Formatting Loop
 * ============================================================================ */

CXX_C_API int fmt_print(char *buf, size_t size, const char *fmt, const fmt_arg_t *args,
                        size_t arg_count) {
  if (!buf || !fmt || size == 0)
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
      if (dst + len > end)
        len = (size_t)(end - dst);
      memcpy(dst, token_view.data, len);
      dst += len;
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
      }
      // If arg_idx out of bounds, we simply skip? Or print raw?
      // The re2c version skipped printing it entirely if idx > count.
      // We'll mimic re2c behavior: if arg missing, nothing output for the specifier.
      break;

    case FMT_TOKEN_INVALID:
      // Copy exact content
      {
        size_t len = token_view.len;
        if (dst + len > end)
          len = (size_t)(end - dst);
        memcpy(dst, token_view.data, len);
        dst += len;
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
