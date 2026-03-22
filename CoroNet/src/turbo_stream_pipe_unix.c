/**
 * @file turbo_stream_pipe_unix.c
 * @brief Unix domain socket backend for turbo_stream_t.
 *
 * Pattern: Simple delegation to the active platform (epoll/kqueue).
 * No complex state here, just address translation to AF_UNIX.
 */

#ifndef _WIN32

#include "turbo_stream_internal.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#ifdef __linux__
#define NATIVE_OPS turbo_stream_epoll_ops
#else
#define NATIVE_OPS turbo_stream_kqueue_ops
#endif

/* ── Client ─────────────────────────────────────────────── */

static int pu_init(turbo_stream_t *s) { return NATIVE_OPS.init(s); }
static int pu_connect(turbo_stream_t *s, const struct sockaddr *a) { return NATIVE_OPS.connect(s, a); }

static int pu_connect_pipe(turbo_stream_t *s, const char *name) {
    if (!s || !name) return TURBO_EINVAL;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, name, sizeof(addr.sun_path)-1);
    return pu_connect(s, (struct sockaddr *)&addr);
}

static int pu_send(turbo_stream_t *s, const char *d, size_t l) { return NATIVE_OPS.send(s, d, l); }
static int pu_flush(turbo_stream_t *s) { return NATIVE_OPS.flush(s); }
static int pu_recv_start(turbo_stream_t *s) { return NATIVE_OPS.recv_start(s); }
static void pu_recv_stop(turbo_stream_t *s) { NATIVE_OPS.recv_stop(s); }
static void pu_close(turbo_stream_t *s) { NATIVE_OPS.close(s); }
static int pu_get_local(turbo_stream_t *s, struct sockaddr_storage *a) { return NATIVE_OPS.get_local_addr(s, a); }
static int pu_get_peer(turbo_stream_t *s, struct sockaddr_storage *a) { return NATIVE_OPS.get_peer_addr(s, a); }

/* ── Listener ───────────────────────────────────────────── */

static int pu_bind(turbo_stream_listener_t *l, const struct sockaddr *a) { return NATIVE_OPS.bind(l, a); }

static int pu_bind_pipe(turbo_stream_listener_t *l, const char *name) {
    if (!l || !name) return TURBO_EINVAL;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, name, sizeof(addr.sun_path)-1);
    unlink(name);
    return pu_bind(l, (struct sockaddr *)&addr);
}

static int pu_listen(turbo_stream_listener_t *l, int b) { return NATIVE_OPS.listen(l, b); }
static void pu_listener_close(turbo_stream_listener_t *l) { NATIVE_OPS.listener_close(l); }

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

#endif
