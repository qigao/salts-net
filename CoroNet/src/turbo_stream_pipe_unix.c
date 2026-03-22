/**
 * @file turbo_stream_pipe_unix.c
 * @brief Unix domain socket backend for turbo_stream_t (pipe kind).
 *
 * Stubs for now — AF_UNIX + epoll/kqueue implementation comes when the
 * epoll/kqueue TCP backends are implemented. Same vtable, same dispatch.
 */

#include "turbo_stream_internal.h"
#include <stdlib.h>


static int pu_init(turbo_stream_t *s) { UNUSED(s); return TURBO_ENOTSUP; }
static int pu_connect(turbo_stream_t *s, const struct sockaddr *a) { UNUSED(s); UNUSED(a); return TURBO_ENOTSUP; }
static int pu_connect_pipe(turbo_stream_t *s, const char *n) { UNUSED(s); UNUSED(n); return TURBO_ENOTSUP; }
static int pu_send(turbo_stream_t *s, const char *d, size_t l) { UNUSED(s); UNUSED(d); UNUSED(l); return TURBO_ENOTSUP; }
static int pu_flush(turbo_stream_t *s) { UNUSED(s); return TURBO_ENOTSUP; }
static int pu_recv_start(turbo_stream_t *s) { UNUSED(s); return TURBO_ENOTSUP; }
static void pu_recv_stop(turbo_stream_t *s) { UNUSED(s); }
static void pu_close(turbo_stream_t *s) { UNUSED(s); turbo_stream_finalize_close(s); }
static int pu_get_local(turbo_stream_t *s, struct sockaddr_storage *a) { UNUSED(s); UNUSED(a); return TURBO_ENOTSUP; }
static int pu_get_peer(turbo_stream_t *s, struct sockaddr_storage *a) { UNUSED(s); UNUSED(a); return TURBO_ENOTSUP; }
static int pu_bind(turbo_stream_listener_t *l, const struct sockaddr *a) { UNUSED(l); UNUSED(a); return TURBO_ENOTSUP; }
static int pu_bind_pipe(turbo_stream_listener_t *l, const char *n) { UNUSED(l); UNUSED(n); return TURBO_ENOTSUP; }
static int pu_listen(turbo_stream_listener_t *l, int b) { UNUSED(l); UNUSED(b); return TURBO_ENOTSUP; }
static void pu_listener_close(turbo_stream_listener_t *l) { UNUSED(l); }

const turbo_stream_backend_ops_t turbo_stream_pipe_unix_ops = {
  .init         = pu_init,
  .connect      = pu_connect,
  .connect_pipe = pu_connect_pipe,
  .send         = pu_send,
  .flush        = pu_flush,
  .recv_start   = pu_recv_start,
  .recv_stop    = pu_recv_stop,
  .close        = pu_close,
  .get_local_addr = pu_get_local,
  .get_peer_addr  = pu_get_peer,
  .bind          = pu_bind,
  .bind_pipe     = pu_bind_pipe,
  .listen        = pu_listen,
  .listener_close = pu_listener_close,
};

