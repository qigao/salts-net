#include "CoroNet/turbo_kcp.h"
#include "CoroNet/turbo_coro_internal.h"
#include "CoroNet/turbo_coro_context.h"
#include "CoroNet/turbo_datagram.h"
#include "CoroNet/turbo_dns.h"
#include "ikcp.h"
#include "platform.h"
#include "turbo_kcp_fec_internal.h"
#include "turbo_error.h"
#include "turbo_thread.h"
#include "tlog.h"
#include <stdlib.h>
#include <string.h>

struct turbo_kcp_s {
  coro_context_t* ctx;
  ikcpcb* ikcp;
  turbo_datagram_t* udp;
  turbo_timer_t* update_timer;
  
  turbo_connect_cb on_connect;
  turbo_recv_cb on_recv;
  turbo_close_cb on_close;
  void* user_data;
  
  struct sockaddr_storage peer_addr;
  int has_peer;
  int connected;
  int connecting;
  int closing;
  int reuse_port;
  turbo_kcp_fec_config_t fec_config;
  turbo_kcp_fec_state_t* fec;
};

static void kcp_record_error(coro_context_t *ctx, int err) {
  if (ctx) {
    ctx->last_error = err;
  }
}

static int kcp_last_error_or(coro_context_t *ctx, int fallback) {
  return (ctx && ctx->last_error != 0) ? ctx->last_error : fallback;
}

static int sockaddr_storage_equal(const struct sockaddr_storage* a,
                                  const struct sockaddr_storage* b) {
  if (!a || !b || a->ss_family != b->ss_family) {
    return 0;
  }

  if (a->ss_family == AF_INET) {
    const struct sockaddr_in* a4 = (const struct sockaddr_in*)a;
    const struct sockaddr_in* b4 = (const struct sockaddr_in*)b;
    return a4->sin_port == b4->sin_port &&
           memcmp(&a4->sin_addr, &b4->sin_addr, sizeof(a4->sin_addr)) == 0;
  }

  if (a->ss_family == AF_INET6) {
    const struct sockaddr_in6* a6 = (const struct sockaddr_in6*)a;
    const struct sockaddr_in6* b6 = (const struct sockaddr_in6*)b;
    return a6->sin6_port == b6->sin6_port &&
           memcmp(&a6->sin6_addr, &b6->sin6_addr, sizeof(a6->sin6_addr)) == 0;
  }

  return 0;
}

/* ── Internal KCP callbacks ───────────────────────────────── */

static void on_timer_tick(turbo_timer_t* timer);

static int kcp_post_wait(coro_context_t* ctx, coro_post_fn fn, void* arg1, void* arg2) {
  int rc;

  if (!ctx || !fn) {
    return TURBO_EINVAL;
  }

  for (;;) {
    rc = coro_post(ctx, fn, arg1, arg2);
    if (rc == 0) {
      return 0;
    }

    turbo_loop_wake((turbo_loop_t*)coro_context_native_loop(ctx));
    turbo_thread_yield();
  }
}

static void kcp_final_free_task(void* arg1, void* arg2) {
  UNUSED(arg2);
  turbo_kcp_t* k = (turbo_kcp_t*)arg1;
  coro_context_t* ctx;
  if (!k) {
    return;
  }
  ctx = k->ctx;
  if (k->update_timer) {
    turbo_timer_set_data(k->update_timer, NULL);
    turbo_timer_stop(k->update_timer);
    turbo_timer_destroy(k->update_timer);
    k->update_timer = NULL;
  }
  TLOG_DEBUG("KCP final free: {}", (void*)k);
  free(k);
  coro_context_release_external(ctx);
}

static int kcp_low_level_output(const char* buf, int len, ikcpcb* ikcp, void* user) {
  UNUSED(ikcp);
  turbo_kcp_t* k = (turbo_kcp_t*)user;
  if (!k->udp || k->closing) return -1;
  if (k->fec) {
    return turbo_kcp_fec_send_data(k->fec, k->udp,
                                   (const struct sockaddr*)&k->peer_addr,
                                   buf, (size_t)len);
  }
  return turbo_datagram_sendto(k->udp, (const struct sockaddr*)&k->peer_addr, buf, (size_t)len);
}

typedef struct kcp_fec_input_s {
  turbo_kcp_t* k;
  int delivered;
} kcp_fec_input_t;

static int kcp_deliver_input(void* user, const char* data, size_t len) {
  kcp_fec_input_t* input = (kcp_fec_input_t*)user;
  if (!input || !input->k || !input->k->ikcp || !data || len == 0) {
    return TURBO_EINVAL;
  }

  ikcp_input(input->k->ikcp, data, (long)len);
  input->delivered++;
  return 0;
}

static int on_udp_recv(void* handle, const mem_slice_t* slice, void* peer) {
  turbo_kcp_t* k = (turbo_kcp_t*)turbo_datagram_get_user_data((turbo_datagram_t*)handle);
  kcp_fec_input_t fec_input;
  int input_rc;

  if (!k || !k->ikcp || k->closing) return 0;

  if (!slice || !slice->data || slice->length == 0) {
    k->connected = 0;
    if (k->on_recv) {
      k->on_recv(k, NULL, NULL);
    }
    return 0;
  }
  
  /* KCP context is single-peer. Lock the first peer and ignore foreign packets. */
  if (peer) {
    const struct sockaddr_storage* incoming = (const struct sockaddr_storage*)peer;
    if (!k->has_peer) {
      /* New client connecting.  Reset ikcp so stale seq/ack state from the
       * previous session doesn't cause the new client's sn=0 ping to be
       * dropped as a "duplicate".  This is safe here because done=1 on the
       * previous client can only be reached after its "pong" was received,
       * i.e. the old session's ikcp output was already flushed and ACK'd. */
      if (k->ikcp) {
        IUINT32 conv = k->ikcp->conv;
        ikcp_release(k->ikcp);
        k->ikcp = ikcp_create(conv, k);
        if (k->ikcp) {
          k->ikcp->output = kcp_low_level_output;
          ikcp_nodelay(k->ikcp, 1, 10, 2, 1);
          ikcp_wndsize(k->ikcp, 128, 128);
          /* Seed the clock */
          ikcp_update(k->ikcp, (uint32_t)coro_context_now(k->ctx));
        }
      }
      memcpy(&k->peer_addr, incoming, sizeof(*incoming));
      k->has_peer = 1;
    } else if (!sockaddr_storage_equal(&k->peer_addr, incoming)) {
      return 0;
    }
  }

  memset(&fec_input, 0, sizeof(fec_input));
  fec_input.k = k;
  if (k->fec) {
    input_rc = turbo_kcp_fec_receive_frame(k->fec, slice, kcp_deliver_input, &fec_input);
    if (input_rc != 0 && fec_input.delivered == 0) {
      return 0;
    }
    if (fec_input.delivered == 0) {
      return 0;
    }
  } else {
    ikcp_input(k->ikcp, slice->data, (long)slice->length);
    fec_input.delivered = 1;
  }

  /* Flush ACKs (and any pending sends) immediately via ikcp_flush().
   * ikcp_update() would be a no-op here: it guards on ts_flush which is
   * always 10ms in the future after the first flush, so slap < 0 → skip.
   * ikcp_flush() has no such guard and sends unconditionally. */
  ikcp_flush(k->ikcp);

  /* Check for KCP-level extracted data */
  char buf[4096];
  int r;
  while ((r = ikcp_recv(k->ikcp, buf, sizeof(buf))) > 0) {
    if (k->on_recv) {
      mem_slice_t s = { .data = buf, .length = (size_t)r };
      k->on_recv(k, &s, peer);
    }
  }
  return 0;
}

static void kcp_tick_task(void* arg1, void* arg2) {
  UNUSED(arg2);
  turbo_kcp_t* k = (turbo_kcp_t*)arg1;
  if (!k->ikcp || k->closing) return;
  
  uint32_t now = (uint32_t)coro_context_now(k->ctx);
  ikcp_update(k->ikcp, now);
  
  /* Schedule next update */
  uint32_t next = ikcp_check(k->ikcp, now);
  uint32_t diff = (next > now) ? (next - now) : 10;
  if (diff > 10) diff = 10; /* Cap at 10ms to match ikcp_nodelay interval */
  
  turbo_timer_start(k->update_timer, on_timer_tick, diff, 0);
}

static void on_timer_tick(turbo_timer_t* timer) {
  turbo_kcp_t* k = (turbo_kcp_t*)turbo_timer_get_data(timer);
  if (!k || !k->ctx || k->closing) return;
  (void)kcp_post_wait(k->ctx, kcp_tick_task, k, NULL);
}

static void turbo_kcp_cleanup_create_failure(turbo_kcp_t *k, int external_ref_held) {
  if (!k) {
    return;
  }

  if (k->update_timer) {
    turbo_timer_stop(k->update_timer);
    turbo_timer_set_data(k->update_timer, NULL);
    turbo_timer_destroy(k->update_timer);
    k->update_timer = NULL;
  }

  if (k->ikcp) {
    ikcp_release(k->ikcp);
    k->ikcp = NULL;
  }

  if (k->fec) {
    turbo_kcp_fec_close(k->fec);
    k->fec = NULL;
  }

  if (external_ref_held && k->ctx) {
    coro_context_release_external(k->ctx);
  }

  free(k);
}

/* ── Public API ───────────────────────────────────────────── */

turbo_kcp_t* turbo_kcp_create(coro_context_t* ctx) {
  turbo_kcp_t* k = calloc(1, sizeof(turbo_kcp_t));
  int external_ref_held = 0;
  if (!k) {
    kcp_record_error(ctx, TURBO_ENOMEM);
    return NULL;
  }
  
  k->ctx = ctx;
  turbo_kcp_fec_config_init(&k->fec_config);
  if (ctx) {
    coro_context_acquire_external(ctx);
    external_ref_held = 1;
  }
  k->ikcp = ikcp_create(12345, k); /* TODO: conv id management */
  if (!k->ikcp) {
    kcp_record_error(ctx, TURBO_ENOMEM);
    turbo_kcp_cleanup_create_failure(k, external_ref_held);
    return NULL;
  }
  
  k->ikcp->output = kcp_low_level_output;
  
  /* Configure KCP for best performance */
  ikcp_nodelay(k->ikcp, 1, 10, 2, 1);
  ikcp_wndsize(k->ikcp, 128, 128);
  /* Seed ikcp internal clock so ikcp_flush() is not a no-op.
   * ikcp_flush() guards on (updated == 0); ikcp_update() sets updated=1
   * on first call and initialises ts_flush. Without this, every ikcp_flush()
   * on the hot path (after ikcp_send / ikcp_input) silently returns early. */
  if (ctx) {
    uint32_t now = (uint32_t)coro_context_now(ctx);
    ikcp_update(k->ikcp, now);
  }
  
  k->update_timer = turbo_timer_create(NULL);
  if (k->update_timer) {
    turbo_timer_set_data(k->update_timer, k);
  } else {
    kcp_record_error(ctx, TURBO_ENOMEM);
    turbo_kcp_cleanup_create_failure(k, external_ref_held);
    return NULL;
  }
  
  kcp_record_error(ctx, 0);
  return k;
}

void turbo_kcp_destroy(turbo_kcp_t* kcp) {
  if (!kcp || kcp->closing) return;
  kcp->closing = 1;
  kcp->connected = 0;
  
  if (kcp->update_timer) {
    turbo_timer_stop(kcp->update_timer);
    turbo_timer_set_data(kcp->update_timer, NULL);
  }
  
  if (kcp->ikcp) {
    ikcp_release(kcp->ikcp);
    kcp->ikcp = NULL;
  }

  if (kcp->fec) {
    turbo_kcp_fec_close(kcp->fec);
    kcp->fec = NULL;
  }
  
  if (kcp->udp) {
    turbo_datagram_set_user_data(kcp->udp, NULL);
    turbo_datagram_destroy(kcp->udp);
    kcp->udp = NULL;
  }
  
  /* Post actual free to happen after any already queued loop tasks */
  if (kcp->ctx) {
    (void)kcp_post_wait(kcp->ctx, kcp_final_free_task, kcp, NULL);
  } else {
    free(kcp);
  }
}

void turbo_kcp_fec_config_default(turbo_kcp_fec_config_t* config) {
  turbo_kcp_fec_config_init(config);
}

int turbo_kcp_fec_backend_available(turbo_kcp_fec_backend_t backend) {
  return turbo_kcp_fec_backend_is_available(backend);
}

int turbo_kcp_set_fec(turbo_kcp_t* kcp, const turbo_kcp_fec_config_t* config) {
  turbo_kcp_fec_state_t* next_fec;
  turbo_kcp_fec_config_t next_config;
  int rc;

  if (!kcp || !config) {
    kcp_record_error(kcp ? kcp->ctx : NULL, TURBO_EINVAL);
    return TURBO_EINVAL;
  }

  if (kcp->udp || kcp->connected || kcp->connecting) {
    kcp_record_error(kcp->ctx, TURBO_EINVAL);
    return TURBO_EINVAL;
  }

  next_config = *config;
  if (!next_config.enabled) {
    turbo_kcp_fec_config_init(&next_config);
    if (kcp->fec) {
      turbo_kcp_fec_close(kcp->fec);
      kcp->fec = NULL;
    }
    kcp->fec_config = next_config;
    kcp_record_error(kcp->ctx, 0);
    return 0;
  }

  next_fec = NULL;
  rc = turbo_kcp_fec_open(&next_config, &next_fec);
  if (rc != 0) {
    kcp_record_error(kcp->ctx, rc);
    return rc;
  }

  if (kcp->fec) {
    turbo_kcp_fec_close(kcp->fec);
  }
  kcp->fec = next_fec;
  kcp->fec_config = next_config;
  kcp_record_error(kcp->ctx, 0);
  return 0;
}

int turbo_kcp_get_fec(turbo_kcp_t* kcp, turbo_kcp_fec_config_t* config) {
  if (!kcp || !config) {
    kcp_record_error(kcp ? kcp->ctx : NULL, TURBO_EINVAL);
    return TURBO_EINVAL;
  }

  *config = kcp->fec_config;
  kcp_record_error(kcp->ctx, 0);
  return 0;
}

int turbo_kcp_bind(turbo_kcp_t* kcp, const char* host, int port,
                   turbo_recv_cb on_recv) {
  if (!kcp || !host) return TURBO_EINVAL;
  
  kcp->on_recv = on_recv;
  
  struct sockaddr_storage local_addr;
  int r = turbo_dns_parse_address(host, port, &local_addr);
  if (r != 0) return r;
  
  if (!kcp->udp) {
    turbo_datagram_kind_t kind = (local_addr.ss_family == AF_INET6) ? TURBO_DATAGRAM_UDP6 : TURBO_DATAGRAM_UDP4;
    kcp->udp = turbo_datagram_create(kcp->ctx, kind);
    if (!kcp->udp) return kcp_last_error_or(kcp->ctx, TURBO_EIO);

    turbo_datagram_set_user_data(kcp->udp, kcp);
  }

  turbo_datagram_set_reuse_port(kcp->udp, kcp->reuse_port);
  r = turbo_datagram_bind(kcp->udp, host, (unsigned short)port);
  if (r != 0) return r;
  
  r = turbo_datagram_recv_start(kcp->udp, on_udp_recv);
  if (r != 0) return r;
  
  turbo_timer_start(kcp->update_timer, on_timer_tick, 10, 0);
  kcp->connected = 1; /* For bind, we consider it "ready" */
  
  return 0;
}

int turbo_kcp_connect(turbo_kcp_t* kcp, const char* host, int port,
                      turbo_connect_cb on_connect, turbo_recv_cb on_recv) {
  if (!kcp || !host) return TURBO_EINVAL;
  
  kcp->on_connect = on_connect;
  kcp->on_recv = on_recv;
  kcp->connecting = 1;
  
  int r = turbo_dns_parse_address(host, port, &kcp->peer_addr);
  if (r != 0) return r;
  kcp->has_peer = 1;
  
  /* Create UDP socket if not existing */
  if (!kcp->udp) {
    turbo_datagram_kind_t kind = (kcp->peer_addr.ss_family == AF_INET6) ? TURBO_DATAGRAM_UDP6 : TURBO_DATAGRAM_UDP4;
    kcp->udp = turbo_datagram_create(kcp->ctx, kind);
    if (!kcp->udp) return kcp_last_error_or(kcp->ctx, TURBO_EIO);
    
    /* Must bind to initialize the backend and get a socket handle.
       Use INADDR_ANY (0.0.0.0 / ::) to allow OS to choose a port. */
    const char* bind_addr = (kcp->peer_addr.ss_family == AF_INET6) ? "::" : "0.0.0.0";
    r = turbo_datagram_bind(kcp->udp, bind_addr, 0);
    if (r != 0) return r;

    turbo_datagram_set_user_data(kcp->udp, kcp);
  }

  /* Start receiving raw UDP */
  r = turbo_datagram_recv_start(kcp->udp, on_udp_recv);
  if (r != 0) return r;
  
  /* Start update timer */
  turbo_timer_start(kcp->update_timer, on_timer_tick, 10, 0);
  
  /* KCP doesn't have a native "connect" handshake at ikcp level,
     but we should signal connection established. */
  kcp->connected = 1;
  kcp->connecting = 0;
  
  if (on_connect) {
    on_connect(kcp, 0, NULL);
  }
  
  return 0;
}

int turbo_kcp_send(turbo_kcp_t* kcp, const char* data, size_t len) {
  if (!kcp || !kcp->ikcp || !kcp->connected) return TURBO_EINVAL;
  int r = ikcp_send(kcp->ikcp, data, (int)len);
  if (r >= 0) {
    /* ikcp_flush() bypasses the ts_flush guard that ikcp_update() imposes.
     * Without this, the send would sit in snd_queue for up to 10ms waiting
     * for the background timer to call ikcp_update(). */
    ikcp_flush(kcp->ikcp);
  }
  return r;
}

void turbo_kcp_close(turbo_kcp_t* kcp) {
  if (!kcp) return;
  kcp->connected = 0;
  if (kcp->update_timer) turbo_timer_stop(kcp->update_timer);
  if (kcp->udp) turbo_datagram_recv_stop(kcp->udp);
}

void turbo_kcp_set_reuse_port(turbo_kcp_t* kcp, int enable) {
  if (!kcp) return;
  kcp->reuse_port = enable ? 1 : 0;
  if (kcp->udp) {
    turbo_datagram_set_reuse_port(kcp->udp, kcp->reuse_port);
  }
}

void turbo_kcp_set_user_data(turbo_kcp_t* kcp, void* user_data) { kcp->user_data = user_data; }
void* turbo_kcp_get_user_data(turbo_kcp_t* kcp) { return kcp->user_data; }

turbo_datagram_t* turbo_kcp_get_datagram(turbo_kcp_t* kcp) {
  return kcp ? kcp->udp : NULL;
}

void turbo_kcp_reset_peer(turbo_kcp_t* kcp) {
  if (!kcp) return;
  /* Only clear the lock flag; do NOT zero peer_addr.
   *
   * ikcp_send() is non-blocking: it queues data for the next ikcp_update()
   * tick (10ms timer) which calls kcp_low_level_output() → sendto(peer_addr).
   * If we zero peer_addr now, the queued response goes to 0.0.0.0:0.
   *
   * With has_peer=0 and peer_addr still holding the previous client's address:
   *   - Pending flush fires correctly when the timer ticks.
   *   - When the next client's first packet arrives, on_udp_recv() takes the
   *     !has_peer branch (line ~122) and overwrites peer_addr atomically. */
  kcp->has_peer = 0;
}
