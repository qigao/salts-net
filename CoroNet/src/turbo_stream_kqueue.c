/**
 * @file turbo_stream_kqueue.c
 * @brief BSD/macOS kqueue backend scaffold for turbo_stream_t.
 *
 * Stubs fail loudly with TURBO_ENOTSUP until the direct kqueue path is
 * implemented. No silent fallback.
 */

#include "turbo_stream_internal.h"
#include <stdlib.h>

static int kq_init(turbo_stream_t *s) { UNUSED(s); return TURBO_ENOTSUP; }
static int kq_connect(turbo_stream_t *s, const struct sockaddr *a) { UNUSED(s); UNUSED(a); return TURBO_ENOTSUP; }
static int kq_connect_pipe(turbo_stream_t *s, const char *n) { UNUSED(s); UNUSED(n); return TURBO_ENOTSUP; }
static int kq_send(turbo_stream_t *s, const char *d, size_t l) { UNUSED(s); UNUSED(d); UNUSED(l); return TURBO_ENOTSUP; }
static int kq_flush(turbo_stream_t *s) { UNUSED(s); return TURBO_ENOTSUP; }
static int kq_recv_start(turbo_stream_t *s) { UNUSED(s); return TURBO_ENOTSUP; }
static void kq_recv_stop(turbo_stream_t *s) { UNUSED(s); }
static void kq_close(turbo_stream_t *s) { UNUSED(s); turbo_stream_finalize_close(s); }
static int kq_get_local(turbo_stream_t *s, struct sockaddr_storage *a) { UNUSED(s); UNUSED(a); return TURBO_ENOTSUP; }
static int kq_get_peer(turbo_stream_t *s, struct sockaddr_storage *a) { UNUSED(s); UNUSED(a); return TURBO_ENOTSUP; }
static int kq_bind(turbo_stream_listener_t *l, const struct sockaddr *a) { UNUSED(l); UNUSED(a); return TURBO_ENOTSUP; }
static int kq_bind_pipe(turbo_stream_listener_t *l, const char *n) { UNUSED(l); UNUSED(n); return TURBO_ENOTSUP; }
static int kq_listen(turbo_stream_listener_t *l, int b) { UNUSED(l); UNUSED(b); return TURBO_ENOTSUP; }
static void kq_listener_close(turbo_stream_listener_t *l) { UNUSED(l); }

const turbo_stream_backend_ops_t turbo_stream_kqueue_ops = {
  .init         = kq_init,
  .connect      = kq_connect,
  .connect_pipe = kq_connect_pipe,
  .send         = kq_send,
  .flush        = kq_flush,
  .recv_start   = kq_recv_start,
  .recv_stop    = kq_recv_stop,
  .close        = kq_close,
  .get_local_addr = kq_get_local,
  .get_peer_addr  = kq_get_peer,
  .bind          = kq_bind,
  .bind_pipe     = kq_bind_pipe,
  .listen        = kq_listen,
  .listener_close = kq_listener_close,
};
