/**
 * @file turbo_coro_context.c
 * @brief Implementation of the opaque event-loop context.
 *
 */

#include "turbo_coro_context.h"
#include <uv.h>
#include <stdlib.h>
#include "turbo_thread.h"
#include "turbo_coro_internal.h"
/* Compile-time guarantee: turbo error codes == libuv error codes.
   MSVC C11 mode uses _Static_assert; C23/C++ use static_assert. */
#ifndef __cplusplus
#if defined(_MSC_VER) && !defined(_Static_assert)
#define _Static_assert static_assert
#endif
#endif

_Static_assert(TURBO_EOF == UV_EOF, "TURBO_EOF mismatch");
_Static_assert(TURBO_ENOMEM == UV_ENOMEM, "TURBO_ENOMEM mismatch");
_Static_assert(TURBO_EINVAL == UV_EINVAL, "TURBO_EINVAL mismatch");
_Static_assert(TURBO_ETIMEDOUT == UV_ETIMEDOUT, "TURBO_ETIMEDOUT mismatch");
_Static_assert(TURBO_ECONNREFUSED == UV_ECONNREFUSED, "TURBO_ECONNREFUSED mismatch");
_Static_assert(TURBO_EPROTONOSUPPORT == UV_EPROTONOSUPPORT, "TURBO_EPROTONOSUPPORT mismatch");
_Static_assert(TURBO_EALREADY == UV_EALREADY, "TURBO_EALREADY mismatch");



turbo_coro_context_t *turbo_coro_context_create(void *loop) {
  turbo_coro_context_t *ctx = calloc(1, sizeof(*ctx));
  if (!ctx)
    return NULL;

  if (loop) {
    ctx->loop = (uv_loop_t *)loop;
    ctx->owns_loop = 0;
  } else {
    ctx->loop = malloc(sizeof(uv_loop_t));
    if (!ctx->loop) {
      free(ctx);
      return NULL;
    }
    uv_loop_init(ctx->loop);
    ctx->owns_loop = 1;
  }
  
  /* Coroutine contexts are strictly single-threaded/cooperative per loop.
     Disable global synchronization overhead to improve performance. */
  turbo_sync_set_single_threaded(1);
  
  return ctx;
}

int turbo_coro_context_run(turbo_coro_context_t *ctx, turbo_run_mode_t mode) {
  if (!ctx || !ctx->loop)
    return 0;
  return uv_run(ctx->loop, (uv_run_mode)mode);
}

void turbo_coro_context_stop(turbo_coro_context_t* ctx) {
    if (ctx) uv_stop(ctx->loop);
}

int turbo_coro_context_alive(turbo_coro_context_t* ctx) {
    if (!ctx) return 0;
    return uv_loop_alive(ctx->loop);
}

uint64_t turbo_coro_context_now(turbo_coro_context_t* ctx) {
    if (!ctx) return 0;
    return uv_now(ctx->loop);
}

void turbo_coro_context_destroy(turbo_coro_context_t* ctx) {
    if (!ctx) return;
    if (ctx->post_initialized) {
        uv_close((uv_handle_t*)&ctx->post_async, NULL);
        /* Drain any remaining nodes */
        turbo_coro_post_node_t *node = ctx->post_head;
        while (node) {
            turbo_coro_post_node_t *next = node->next;
            free(node);
            node = next;
        }
        turbo_mutex_destroy(&ctx->post_mutex);
    }
    if (ctx->owns_loop && ctx->loop) {
        uv_loop_close(ctx->loop);
        free(ctx->loop);
    }
    free(ctx);
}

const char* turbo_strerror(int err) {
    return uv_strerror(err);
}

/* ── Post queue: thread-safe callback posting to event loop ── */

static void post_async_cb(uv_async_t *handle) {
    turbo_coro_context_t *ctx = (turbo_coro_context_t *)handle->data;
    turbo_coro_post_node_t *head;

    turbo_mutex_lock(&ctx->post_mutex);
    head = ctx->post_head;
    ctx->post_head = NULL;
    ctx->post_tail = NULL;
    turbo_mutex_unlock(&ctx->post_mutex);

    while (head) {
        turbo_coro_post_node_t *node = head;
        head = head->next;
        node->fn(node->arg);
        free(node);
    }
}

static int ensure_post_queue(turbo_coro_context_t *ctx) {
    if (ctx->post_initialized) return 0;
    turbo_mutex_init(&ctx->post_mutex);
    int r = uv_async_init(ctx->loop, &ctx->post_async, post_async_cb);
    if (r != 0) {
        turbo_mutex_destroy(&ctx->post_mutex);
        return r;
    }
    ctx->post_async.data = ctx;
    ctx->post_initialized = 1;
    return 0;
}

int turbo_coro_post(turbo_coro_context_t *ctx, turbo_coro_post_fn fn, void *arg) {
    if (!ctx || !fn) return TURBO_EINVAL;

    int r = ensure_post_queue(ctx);
    if (r != 0) return r;

    turbo_coro_post_node_t *node = malloc(sizeof(turbo_coro_post_node_t));
    if (!node) return TURBO_ENOMEM;
    node->fn = fn;
    node->arg = arg;
    node->next = NULL;

    turbo_mutex_lock(&ctx->post_mutex);
    if (ctx->post_tail) {
        ctx->post_tail->next = node;
    } else {
        ctx->post_head = node;
    }
    ctx->post_tail = node;
    turbo_mutex_unlock(&ctx->post_mutex);

    return uv_async_send(&ctx->post_async);
}


