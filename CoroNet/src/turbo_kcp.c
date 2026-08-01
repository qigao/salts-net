#include "CoroNet/turbo_kcp.h"
#include "CoroNet/turbo_coro_internal.h"
#include "CoroNet/turbo_coro_context.h"
#include "CoroNet/turbo_datagram.h"
#include "CoroNet/turbo_dns.h"
#include "ikcp.h"
#include "platform.h"
#include "turbo_kcp_fec_internal.h"
#include "turbo_kcp_secure_internal.h"
#include "turbo_error.h"
#include "turbo_crypto.h"
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
  int is_server;
  int config_set;
  uint64_t last_handshake_send_ms;
  turbo_kcp_config_t config;
  turbo_kcp_secure_state_t secure;
  turbo_kcp_fec_state_t* fec;
  char* secure_send_buffer;
  char* secure_receive_buffer;
  size_t secure_buffer_size;
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
static int kcp_low_level_output(const char* buf, int len, ikcpcb* ikcp,
                                void* user);

static int kcp_reset_protocol(turbo_kcp_t* k, IUINT32 conv) {
  ikcpcb* next;
  if (!k || conv == 0U) return TURBO_EINVAL;
  next = ikcp_create(conv, k);
  if (!next) return TURBO_ENOMEM;
  next->output = kcp_low_level_output;
  ikcp_nodelay(next, 1, k->config.interval_ms, k->config.fast_resend,
               k->config.no_congestion_window);
  ikcp_wndsize(next, k->config.send_window, k->config.receive_window);
  if (ikcp_setmtu(next, k->config.mtu) != 0) {
    ikcp_release(next);
    return TURBO_EINVAL;
  }
  ikcp_update(next, (uint32_t)coro_context_now(k->ctx));
  if (k->ikcp) ikcp_release(k->ikcp);
  k->ikcp = next;
  return TURBO_OK;
}

static IUINT32 kcp_session_conv(const turbo_kcp_secure_state_t* secure) {
  IUINT32 conv = (IUINT32)(secure->session_epoch ^
                          (secure->session_epoch >> 32U));
  return conv == 0U ? 1U : conv;
}

static int kcp_activate_session(turbo_kcp_t* k) {
  int rc;
  if (!k || !k->secure.established || !k->fec) return TURBO_EINVAL;
  rc = turbo_kcp_fec_set_session(k->fec, k->secure.session_epoch,
                                 k->secure.fec_key);
  if (rc == TURBO_OK) rc = kcp_reset_protocol(k, kcp_session_conv(&k->secure));
  return rc;
}

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
  size_t record_size = 0;
  int rc;
  UNUSED(ikcp);
  turbo_kcp_t* k = (turbo_kcp_t*)user;
  if (!k->udp || k->closing || !k->secure.established || !k->fec ||
      !k->secure_send_buffer)
    return -1;
  rc = turbo_kcp_secure_seal(&k->secure, buf, (size_t)len,
                             k->secure_send_buffer, k->secure_buffer_size,
                             &record_size);
  if (rc != TURBO_OK) return rc;
  return turbo_kcp_fec_send_data(k->fec, k->udp,
                                 (const struct sockaddr*)&k->peer_addr,
                                 k->secure_send_buffer, record_size);
}

typedef struct kcp_fec_input_s {
  turbo_kcp_t* k;
  int delivered;
} kcp_fec_input_t;

static int kcp_deliver_input(void* user, const char* data, size_t len) {
  kcp_fec_input_t* input = (kcp_fec_input_t*)user;
  size_t plain_size = 0;
  int rc;
  if (!input || !input->k || !input->k->ikcp || !data || len == 0) {
    return TURBO_EINVAL;
  }

  rc = turbo_kcp_secure_open(&input->k->secure, data, len,
                             input->k->secure_receive_buffer,
                             input->k->secure_buffer_size, &plain_size);
  if (rc != TURBO_OK) return rc;
  rc = ikcp_input(input->k->ikcp, input->k->secure_receive_buffer,
                  (long)plain_size);
  if (rc != 0) return TURBO_EPROTO;
  input->delivered++;
  return 0;
}

static int on_udp_recv(void* handle, const mem_slice_t* slice, void* peer) {
  turbo_kcp_t* k = (turbo_kcp_t*)turbo_datagram_get_user_data((turbo_datagram_t*)handle);
  kcp_fec_input_t fec_input;
  const struct sockaddr_storage* incoming;
  uint8_t handshake[TURBO_KCP_SECURE_HANDSHAKE_SIZE];
  int input_rc;

  if (!k || !k->ikcp || k->closing) return 0;

  if (!slice || !slice->data || slice->length == 0) {
    k->connected = 0;
    if (k->on_recv) {
      k->on_recv(k, NULL, NULL);
    }
    return 0;
  }
  if (!peer) return 0;
  incoming = (const struct sockaddr_storage*)peer;

  if (turbo_kcp_secure_is_handshake((const uint8_t*)slice->data,
                                    slice->length)) {
    if (k->is_server) {
      if (k->has_peer &&
          !sockaddr_storage_equal(&k->peer_addr, incoming))
        return 0;
      input_rc = turbo_kcp_secure_accept_client_hello(
          &k->secure, (const uint8_t*)slice->data, slice->length, handshake);
      if (input_rc != TURBO_OK) return 0;
      input_rc = kcp_activate_session(k);
      if (input_rc != TURBO_OK) return 0;
      memcpy(&k->peer_addr, incoming, sizeof(*incoming));
      k->has_peer = 1;
      (void)turbo_datagram_sendto(
          k->udp, (const struct sockaddr*)&k->peer_addr,
          (const char*)handshake, sizeof(handshake));
      return 0;
    }
    if (!sockaddr_storage_equal(&k->peer_addr, incoming)) return 0;
    input_rc = turbo_kcp_secure_accept_server_hello(
        &k->secure, (const uint8_t*)slice->data, slice->length);
    if (input_rc != TURBO_OK || kcp_activate_session(k) != TURBO_OK) return 0;
    k->connected = 1;
    k->connecting = 0;
    if (k->on_connect) k->on_connect(k, TURBO_OK, peer);
    return 0;
  }

  if (!k->secure.established || !k->has_peer ||
      !sockaddr_storage_equal(&k->peer_addr, incoming))
    return 0;

  memset(&fec_input, 0, sizeof(fec_input));
  fec_input.k = k;
  input_rc = turbo_kcp_fec_receive_frame(k->fec, slice, kcp_deliver_input,
                                         &fec_input);
  if (input_rc != 0 || fec_input.delivered == 0) return 0;

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
  uint8_t hello[TURBO_KCP_SECURE_HANDSHAKE_SIZE];
  if (!k->ikcp || k->closing) return;
  
  uint32_t now = (uint32_t)coro_context_now(k->ctx);
  if (k->connecting && !k->is_server &&
      (k->last_handshake_send_ms == 0U ||
       (uint64_t)now - k->last_handshake_send_ms >=
           k->config.handshake_retry_ms) &&
      turbo_kcp_secure_build_client_hello(&k->secure, hello) == TURBO_OK) {
    (void)turbo_datagram_sendto(
        k->udp, (const struct sockaddr*)&k->peer_addr, (const char*)hello,
        sizeof(hello));
    k->last_handshake_send_ms = now;
  }
  if (k->secure.established) ikcp_update(k->ikcp, now);
  
  /* Schedule next update */
  uint32_t next = k->secure.established ? ikcp_check(k->ikcp, now) : now + 10U;
  uint32_t diff = (next > now) ? (next - now) : k->config.interval_ms;
  if (diff > k->config.interval_ms) diff = k->config.interval_ms;
  
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
  turbo_kcp_secure_wipe(&k->secure);
  turbo_kcp_config_wipe(&k->config);
  free(k->secure_send_buffer);
  free(k->secure_receive_buffer);

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
  turbo_kcp_config_default(&k->config);
  if (ctx) {
    coro_context_acquire_external(ctx);
    external_ref_held = 1;
  }
  k->ikcp = ikcp_create(1U, k);
  if (!k->ikcp) {
    kcp_record_error(ctx, TURBO_ENOMEM);
    turbo_kcp_cleanup_create_failure(k, external_ref_held);
    return NULL;
  }
  
  k->ikcp->output = kcp_low_level_output;
  
  /* Configure KCP for best performance */
  ikcp_nodelay(k->ikcp, 1, k->config.interval_ms, k->config.fast_resend,
               k->config.no_congestion_window);
  ikcp_wndsize(k->ikcp, k->config.send_window, k->config.receive_window);
  (void)ikcp_setmtu(k->ikcp, k->config.mtu);
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
  turbo_timer_t* update_timer;

  if (!kcp || kcp->closing) return;
  kcp->closing = 1;
  kcp->connected = 0;

  /* The native timer is the only producer of KCP tick posts. Destroying it
   * waits for an in-flight callback, so every tick it can publish is ordered
   * before the final-free post below. */
  update_timer = kcp->update_timer;
  kcp->update_timer = NULL;
  if (update_timer) {
    turbo_timer_destroy(update_timer);
  }

  if (kcp->ikcp) {
    ikcp_release(kcp->ikcp);
    kcp->ikcp = NULL;
  }

  if (kcp->fec) {
    turbo_kcp_fec_close(kcp->fec);
    kcp->fec = NULL;
  }
  turbo_kcp_secure_wipe(&kcp->secure);
  turbo_kcp_config_wipe(&kcp->config);
  free(kcp->secure_send_buffer);
  kcp->secure_send_buffer = NULL;
  free(kcp->secure_receive_buffer);
  kcp->secure_receive_buffer = NULL;
  
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

void turbo_kcp_config_default(turbo_kcp_config_t* config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->mtu = 1200U;
  config->send_window = 256U;
  config->receive_window = 256U;
  config->interval_ms = 5U;
  config->handshake_retry_ms = 200U;
  config->fast_resend = 2U;
  config->no_congestion_window = 1U;
  turbo_kcp_fec_config_init(&config->fec);
}

void turbo_kcp_config_wipe(turbo_kcp_config_t* config) {
  if (config) turbo_crypto_wipe(config, sizeof(*config));
}

int turbo_kcp_fec_backend_available(turbo_kcp_fec_backend_t backend) {
  return turbo_kcp_fec_backend_is_available(backend);
}

int turbo_kcp_set_config(turbo_kcp_t* kcp, const turbo_kcp_config_t* config) {
  turbo_kcp_fec_state_t* next_fec;
  char* next_send;
  char* next_receive;
  size_t buffer_size;
  uint8_t key_bits = 0U;
  size_t key_index;
  int rc;

  if (!kcp || !config) {
    kcp_record_error(kcp ? kcp->ctx : NULL, TURBO_EINVAL);
    return TURBO_EINVAL;
  }

  if (kcp->udp || kcp->connected || kcp->connecting) {
    kcp_record_error(kcp->ctx, TURBO_EINVAL);
    return TURBO_EINVAL;
  }

  for (key_index = 0; key_index < TURBO_KCP_PSK_SIZE; ++key_index)
    key_bits |= config->pre_shared_key[key_index];
  if (key_bits == 0U || config->mtu < 576U || config->send_window == 0U ||
      config->receive_window == 0U || config->interval_ms == 0U ||
      config->interval_ms > 100U || config->handshake_retry_ms == 0U ||
      config->fast_resend > 255U ||
      config->fec.max_payload_size <
          (uint32_t)config->mtu + TURBO_KCP_SECURE_RECORD_OVERHEAD) {
    kcp_record_error(kcp->ctx, TURBO_EINVAL);
    return TURBO_EINVAL;
  }

  next_fec = NULL;
  rc = turbo_kcp_fec_open(&config->fec, &next_fec);
  if (rc != 0) {
    kcp_record_error(kcp->ctx, rc);
    return rc;
  }

  buffer_size = (size_t)config->fec.max_payload_size;
  next_send = (char*)malloc(buffer_size);
  next_receive = (char*)malloc(buffer_size);
  if (!next_send || !next_receive) {
    free(next_send);
    free(next_receive);
    turbo_kcp_fec_close(next_fec);
    kcp_record_error(kcp->ctx, TURBO_ENOMEM);
    return TURBO_ENOMEM;
  }
  if (kcp->fec) turbo_kcp_fec_close(kcp->fec);
  free(kcp->secure_send_buffer);
  free(kcp->secure_receive_buffer);
  kcp->fec = next_fec;
  kcp->secure_send_buffer = next_send;
  kcp->secure_receive_buffer = next_receive;
  kcp->secure_buffer_size = buffer_size;
  kcp->config = *config;
  kcp->config_set = 1;
  kcp_record_error(kcp->ctx, 0);
  return 0;
}

int turbo_kcp_get_config(turbo_kcp_t* kcp, turbo_kcp_config_t* config) {
  if (!kcp || !config) {
    kcp_record_error(kcp ? kcp->ctx : NULL, TURBO_EINVAL);
    return TURBO_EINVAL;
  }

  *config = kcp->config;
  kcp_record_error(kcp->ctx, 0);
  return 0;
}

int turbo_kcp_bind(turbo_kcp_t* kcp, const char* host, int port,
                   turbo_recv_cb on_recv) {
  if (!kcp || !host || !kcp->config_set || !kcp->fec) return TURBO_EINVAL;
  
  kcp->on_recv = on_recv;
  kcp->is_server = 1;
  if (turbo_kcp_secure_init(&kcp->secure, TURBO_KCP_SECURE_SERVER,
                            kcp->config.pre_shared_key) != TURBO_OK)
    return TURBO_EINVAL;
  
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
  uint8_t hello[TURBO_KCP_SECURE_HANDSHAKE_SIZE];
  if (!kcp || !host || !kcp->config_set || !kcp->fec) return TURBO_EINVAL;
  
  kcp->on_connect = on_connect;
  kcp->on_recv = on_recv;
  kcp->connecting = 1;
  kcp->is_server = 0;
  if (turbo_kcp_secure_init(&kcp->secure, TURBO_KCP_SECURE_CLIENT,
                            kcp->config.pre_shared_key) != TURBO_OK)
    return TURBO_EINVAL;
  
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

  /* Receive authenticated KCP transport datagrams from UDP. */
  r = turbo_datagram_recv_start(kcp->udp, on_udp_recv);
  if (r != 0) return r;
  
  /* Start update timer */
  turbo_timer_start(kcp->update_timer, on_timer_tick, 10, 0);
  
  if (turbo_kcp_secure_build_client_hello(&kcp->secure, hello) != TURBO_OK)
    return TURBO_EIO;
  r = turbo_datagram_sendto(kcp->udp,
                            (const struct sockaddr*)&kcp->peer_addr,
                            (const char*)hello, sizeof(hello));
  if (r != TURBO_OK) return r;
  kcp->last_handshake_send_ms = coro_context_now(kcp->ctx);
  
  return 0;
}

int turbo_kcp_send(turbo_kcp_t* kcp, const char* data, size_t len) {
  if (!kcp || !kcp->ikcp || !kcp->connected ||
      !kcp->secure.established || !kcp->has_peer)
    return TURBO_EINVAL;
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
  kcp->connecting = 0;
  turbo_kcp_secure_wipe(&kcp->secure);
  turbo_kcp_fec_clear_session(kcp->fec);
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
  if (!kcp || !kcp->is_server) return;
  turbo_kcp_secure_wipe(&kcp->secure);
  turbo_kcp_fec_clear_session(kcp->fec);
  (void)turbo_kcp_secure_init(&kcp->secure, TURBO_KCP_SECURE_SERVER,
                              kcp->config.pre_shared_key);
  kcp->has_peer = 0;
}
