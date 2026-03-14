/**
 * @file feeds_ctx.h
 * @brief Feeds plugin context — unified CSV/JSON/XML via turbo_parser.
 */
#ifndef FEEDS_CTX_H
#define FEEDS_CTX_H

#include "turbo_buffer.h"
#include "exprtk.h"
#include "turbo_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FEEDS_MAX_HANDLES 16

typedef struct feeds_ctx_s {
  turbo_csv_stream_processor_t *csv_stream_handles[FEEDS_MAX_HANDLES];
  turbo_csv_doc_t *csv_doc_handles[FEEDS_MAX_HANDLES];
  char error_msg[256];
} feeds_ctx_t;

typedef struct {
  feeds_ctx_t *ctx;
  exprtk_env_t *env;
  mem_pool_t *scratch;
} feeds_ud_t;

#define FEEDS_ZERO ((exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = 0.0})

#define FEEDS_ERROR(ud, msg)                                                                    \
  do {                                                                                          \
    if ((ud) && (ud)->ctx) {                                                                    \
      strncpy((ud)->ctx->error_msg, (msg), sizeof((ud)->ctx->error_msg) - 1);                  \
      (ud)->ctx->error_msg[sizeof((ud)->ctx->error_msg) - 1] = '\0';                           \
    }                                                                                           \
    if ((ud) && (ud)->env) {                                                                    \
      (ud)->env->aborted = 1;                                                                   \
    }                                                                                           \
  } while (0)

static inline char *feeds_arena_cstr(mem_pool_t *a, tstr_v sv) {
  char *buf = (char *)mem_alloc(a, sv.len + 1);
  if (buf) {
    memcpy(buf, sv.data, sv.len);
    buf[sv.len] = '\0';
  }
  return buf;
}

void *feeds_ctx_create(void);
void feeds_ctx_destroy(void *ctx);
void feeds_load(void *ctx, void *env, void *scratch);

#endif /* FEEDS_CTX_H */
