/**
 * @file turbo_datagram_kqueue.c
 * @brief BSD/macOS kqueue backend scaffold for turbo_datagram_t.
 */

#include "turbo_datagram_internal.h"
#include <stdlib.h>

static int dk_init(turbo_datagram_t *d, const char *h, unsigned short p) { UNUSED(d); UNUSED(h); UNUSED(p); return TURBO_ENOTSUP; }
static int dk_connect(turbo_datagram_t *d, const char *h, unsigned short p) { UNUSED(d); UNUSED(h); UNUSED(p); return TURBO_ENOTSUP; }
static int dk_send_buffer(turbo_datagram_t *d, const struct sockaddr *a, mem_buffer_t *b, size_t l) { UNUSED(d); UNUSED(a); UNUSED(b); UNUSED(l); return TURBO_ENOTSUP; }
static int dk_recv_start(turbo_datagram_t *d) { UNUSED(d); return TURBO_ENOTSUP; }
static void dk_recv_stop(turbo_datagram_t *d) { UNUSED(d); }
static void dk_close(turbo_datagram_t *d) { turbo_datagram_finalize_close(d); }
static int dk_get_local(turbo_datagram_t *d, struct sockaddr_storage *a) { UNUSED(d); UNUSED(a); return TURBO_ENOTSUP; }
static int dk_join(turbo_datagram_t *d, const char *g, const char *i) { UNUSED(d); UNUSED(g); UNUSED(i); return TURBO_ENOTSUP; }
static int dk_leave(turbo_datagram_t *d, const char *g, const char *i) { UNUSED(d); UNUSED(g); UNUSED(i); return TURBO_ENOTSUP; }
static int dk_mloop(turbo_datagram_t *d, int o) { UNUSED(d); UNUSED(o); return TURBO_ENOTSUP; }
static int dk_mttl(turbo_datagram_t *d, int t) { UNUSED(d); UNUSED(t); return TURBO_ENOTSUP; }
static int dk_bcast(turbo_datagram_t *d, int o) { UNUSED(d); UNUSED(o); return TURBO_ENOTSUP; }

const turbo_datagram_backend_ops_t turbo_datagram_kqueue_ops = {
  .init            = dk_init,
  .connect         = dk_connect,
  .send_buffer     = dk_send_buffer,
  .recv_start      = dk_recv_start,
  .recv_stop       = dk_recv_stop,
  .close           = dk_close,
  .get_local_addr  = dk_get_local,
  .join_multicast  = dk_join,
  .leave_multicast = dk_leave,
  .set_multicast_loop = dk_mloop,
  .set_multicast_ttl  = dk_mttl,
  .set_broadcast   = dk_bcast,
};
