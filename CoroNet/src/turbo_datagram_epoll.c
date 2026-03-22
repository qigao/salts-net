/**
 * @file turbo_datagram_epoll.c
 * @brief Linux epoll backend scaffold for turbo_datagram_t.
 */

#include "turbo_datagram_internal.h"
#include <stdlib.h>

static int de_init(turbo_datagram_t *d, const char *h, unsigned short p) { UNUSED(d); UNUSED(h); UNUSED(p); return TURBO_ENOTSUP; }
static int de_connect(turbo_datagram_t *d, const char *h, unsigned short p) { UNUSED(d); UNUSED(h); UNUSED(p); return TURBO_ENOTSUP; }
static int de_send_buffer(turbo_datagram_t *d, const struct sockaddr *a, mem_buffer_t *b, size_t l) { UNUSED(d); UNUSED(a); UNUSED(b); UNUSED(l); return TURBO_ENOTSUP; }
static int de_recv_start(turbo_datagram_t *d) { UNUSED(d); return TURBO_ENOTSUP; }
static void de_recv_stop(turbo_datagram_t *d) { UNUSED(d); }
static void de_close(turbo_datagram_t *d) { turbo_datagram_finalize_close(d); }
static int de_get_local(turbo_datagram_t *d, struct sockaddr_storage *a) { UNUSED(d); UNUSED(a); return TURBO_ENOTSUP; }
static int de_join(turbo_datagram_t *d, const char *g, const char *i) { UNUSED(d); UNUSED(g); UNUSED(i); return TURBO_ENOTSUP; }
static int de_leave(turbo_datagram_t *d, const char *g, const char *i) { UNUSED(d); UNUSED(g); UNUSED(i); return TURBO_ENOTSUP; }
static int de_mloop(turbo_datagram_t *d, int o) { UNUSED(d); UNUSED(o); return TURBO_ENOTSUP; }
static int de_mttl(turbo_datagram_t *d, int t) { UNUSED(d); UNUSED(t); return TURBO_ENOTSUP; }
static int de_bcast(turbo_datagram_t *d, int o) { UNUSED(d); UNUSED(o); return TURBO_ENOTSUP; }

const turbo_datagram_backend_ops_t turbo_datagram_epoll_ops = {
  .init            = de_init,
  .connect         = de_connect,
  .send_buffer     = de_send_buffer,
  .recv_start      = de_recv_start,
  .recv_stop       = de_recv_stop,
  .close           = de_close,
  .get_local_addr  = de_get_local,
  .join_multicast  = de_join,
  .leave_multicast = de_leave,
  .set_multicast_loop = de_mloop,
  .set_multicast_ttl  = de_mttl,
  .set_broadcast   = de_bcast,
};
