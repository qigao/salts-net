/**
 * @file turbo_stream_epoll.c
 * @brief Linux epoll backend scaffold for turbo_stream_t.
 *
 * Stubs fail loudly with TURBO_ENOTSUP until the direct epoll path is
 * implemented. No silent fallback.
 */

#include "turbo_stream_internal.h"
#include <stdlib.h>

static int epoll_init(turbo_stream_t *s) { UNUSED(s); return TURBO_ENOTSUP; }
static int epoll_connect(turbo_stream_t *s, const struct sockaddr *a) { UNUSED(s); UNUSED(a); return TURBO_ENOTSUP; }
static int epoll_connect_pipe(turbo_stream_t *s, const char *n) { UNUSED(s); UNUSED(n); return TURBO_ENOTSUP; }
static int epoll_send(turbo_stream_t *s, const char *d, size_t l) { UNUSED(s); UNUSED(d); UNUSED(l); return TURBO_ENOTSUP; }
static int epoll_flush(turbo_stream_t *s) { UNUSED(s); return TURBO_ENOTSUP; }
static int epoll_recv_start(turbo_stream_t *s) { UNUSED(s); return TURBO_ENOTSUP; }
static void epoll_recv_stop(turbo_stream_t *s) { UNUSED(s); }
static void epoll_close(turbo_stream_t *s) { UNUSED(s); turbo_stream_finalize_close(s); }
static int epoll_get_local(turbo_stream_t *s, struct sockaddr_storage *a) { UNUSED(s); UNUSED(a); return TURBO_ENOTSUP; }
static int epoll_get_peer(turbo_stream_t *s, struct sockaddr_storage *a) { UNUSED(s); UNUSED(a); return TURBO_ENOTSUP; }
static int epoll_bind(turbo_stream_listener_t *l, const struct sockaddr *a) { UNUSED(l); UNUSED(a); return TURBO_ENOTSUP; }
static int epoll_bind_pipe(turbo_stream_listener_t *l, const char *n) { UNUSED(l); UNUSED(n); return TURBO_ENOTSUP; }
static int epoll_listen(turbo_stream_listener_t *l, int b) { UNUSED(l); UNUSED(b); return TURBO_ENOTSUP; }
static void epoll_listener_close(turbo_stream_listener_t *l) { UNUSED(l); }

const turbo_stream_backend_ops_t turbo_stream_epoll_ops = {
  .init         = epoll_init,
  .connect      = epoll_connect,
  .connect_pipe = epoll_connect_pipe,
  .send         = epoll_send,
  .flush        = epoll_flush,
  .recv_start   = epoll_recv_start,
  .recv_stop    = epoll_recv_stop,
  .close        = epoll_close,
  .get_local_addr = epoll_get_local,
  .get_peer_addr  = epoll_get_peer,
  .bind          = epoll_bind,
  .bind_pipe     = epoll_bind_pipe,
  .listen        = epoll_listen,
  .listener_close = epoll_listener_close,
};
