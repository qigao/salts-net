/**
 * @file turbo_coro_socket_core.c
 * @brief Core socket implementation - lifecycle, DNS, timeout, ref counting.
 *
 * DESIGN:
 * - Transport-agnostic logic shared by all socket types
 * - Reference counting ensures safe async cleanup
 * - DNS resolution with timeout support
 * - Transport-specific ops are in separate files
 */

#include "CoroNet/turbo_coro_internal.h"
#include "CoroNet/turbo_kcp.h"
#include "tlog.h"
#include "turbo_error.h"
#include "turbo_zstd.h"
#include "turbo_stream_internal.h"
#include <stdlib.h>
#include <string.h>

#define CORO_ZSTD_FRAME_MAGIC 0x5A535443U /* 'ZSTC' */
#define CORO_ZSTD_FRAME_HEADER_LEN 12U
#define CORO_ZSTD_MAX_FRAME_DATA_LEN ((size_t)UINT32_MAX)

static void zstd_store_u32_be(char *p, uint32_t v) {
  p[0] = (char)((v >> 24) & 0xffU);
  p[1] = (char)((v >> 16) & 0xffU);
  p[2] = (char)((v >> 8) & 0xffU);
  p[3] = (char)(v & 0xffU);
}

static uint32_t zstd_load_u32_be(const char *p) {
  return ((uint32_t)(unsigned char)p[0] << 24) |
         ((uint32_t)(unsigned char)p[1] << 16) |
         ((uint32_t)(unsigned char)p[2] << 8) |
         (uint32_t)(unsigned char)p[3];
}

/* ── External transport ops (defined in separate files) ────── */
extern const coro_transport_ops_t transport_ops_tcp;
extern const coro_transport_ops_t transport_ops_pipe;
extern const coro_transport_ops_t udp_client_ops;
extern const coro_transport_ops_t transport_ops_kcp;
extern const coro_transport_ops_t transport_ops_tls;
extern const coro_transport_ops_t transport_ops_ws;

const coro_transport_ops_t *transport_ops_table[TURBO_TRANSPORT_MAX] = {
    [TURBO_TCP] = &transport_ops_tcp,   [TURBO_TLS] = &transport_ops_tls,
    [TURBO_KCP] = &transport_ops_kcp,   [TURBO_UDP] = &udp_client_ops,
    [TURBO_PIPE] = &transport_ops_pipe, [TURBO_WEBSOCKET] = &transport_ops_ws,
};

/* ── Forward declarations ─────────────────────────────────── */
static void socket_record_error(coro_context_t *ctx, int err);
static void socket_destroy_shell(coro_socket_t *s);
static turbo_datagram_t *coro_socket_multicast_datagram(coro_socket_t *s);
static void coro_socket_reset_recv_compression_state(coro_socket_t *s);

static void socket_record_error(coro_context_t *ctx, int err) {
  if (ctx) {
    ctx->last_error = err;
  }
}

static int socket_return_error(coro_socket_t *s, int err) {
  socket_record_error(s ? s->ctx : NULL, err);
  return err;
}

static int socket_is_tcp_backed(const coro_socket_t *s) {
  return s && (s->transport == TURBO_TCP || s->transport == TURBO_TLS ||
               s->transport == TURBO_WEBSOCKET);
}

static int socket_is_stream_backed(const coro_socket_t *s) {
  return s && (s->transport == TURBO_TCP || s->transport == TURBO_TLS ||
               s->transport == TURBO_WEBSOCKET || s->transport == TURBO_PIPE);
}

static mem_buffer_t *socket_return_buffer_error(coro_socket_t *s, int err) {
  socket_record_error(s ? s->ctx : NULL, err);
  return NULL;
}

static char *socket_strdup_nullable(const char *value) {
  size_t len;
  char *copy;

  if (value == NULL) {
    return NULL;
  }

  len = strlen(value);
  copy = (char *)malloc(len + 1U);
  if (copy == NULL) {
    return NULL;
  }

  memcpy(copy, value, len + 1U);
  return copy;
}

static void socket_clear_tls_client_config(coro_socket_t *s) {
  if (!s) {
    return;
  }

  free(s->tls_ca_file);
  free(s->tls_cert_file);
  free(s->tls_key_file);
  free(s->tls_key_password);
  free(s->tls_cipher_list);
  s->tls_ca_file = NULL;
  s->tls_cert_file = NULL;
  s->tls_key_file = NULL;
  s->tls_key_password = NULL;
  s->tls_cipher_list = NULL;
  s->tls_client_configured = 0;
  s->tls_verify_peer = 1;
}

static void socket_clear_tls_server_context(coro_socket_t *s) {
  if (!s || !s->tls_server_context) {
    return;
  }
  turbo_stream_tls_server_context_release_internal(s->tls_server_context);
  s->tls_server_context = NULL;
}

static int socket_copy_tls_client_config(coro_socket_t *s, const turbo_tls_client_config_t *config) {
  char *ca_file = NULL;
  char *cert_file = NULL;
  char *key_file = NULL;
  char *key_password = NULL;
  char *cipher_list = NULL;

  if (!s) {
    return TURBO_EINVAL;
  }

  if (config == NULL) {
    socket_clear_tls_client_config(s);
    return 0;
  }

  ca_file = socket_strdup_nullable(config->ca_file);
  if (config->ca_file != NULL && ca_file == NULL) {
    return TURBO_ENOMEM;
  }
  cert_file = socket_strdup_nullable(config->cert_file);
  if (config->cert_file != NULL && cert_file == NULL) {
    free(ca_file);
    return TURBO_ENOMEM;
  }
  key_file = socket_strdup_nullable(config->key_file);
  if (config->key_file != NULL && key_file == NULL) {
    free(cert_file);
    free(ca_file);
    return TURBO_ENOMEM;
  }
  key_password = socket_strdup_nullable(config->key_password);
  if (config->key_password != NULL && key_password == NULL) {
    free(key_file);
    free(cert_file);
    free(ca_file);
    return TURBO_ENOMEM;
  }
  cipher_list = socket_strdup_nullable(config->cipher_list);
  if (config->cipher_list != NULL && cipher_list == NULL) {
    free(key_password);
    free(key_file);
    free(cert_file);
    free(ca_file);
    return TURBO_ENOMEM;
  }

  socket_clear_tls_client_config(s);
  s->tls_ca_file = ca_file;
  s->tls_cert_file = cert_file;
  s->tls_key_file = key_file;
  s->tls_key_password = key_password;
  s->tls_cipher_list = cipher_list;
  s->tls_client_configured = 1;
  s->tls_verify_peer = config->verify_peer ? 1 : 0;
  return 0;
}

static int socket_last_error_or(coro_socket_t *s, int fallback) {
  return (s && s->ctx && s->ctx->last_error != 0) ? s->ctx->last_error : fallback;
}

void coro_socket_configure_transport_internal(coro_socket_t *s, turbo_transport_t transport,
                                              int connected) {
  if (!s) return;

  s->transport = transport;
  s->ops =
      (transport >= 0 && transport < TURBO_TRANSPORT_MAX) ? transport_ops_table[transport] : NULL;
  s->connected = connected ? 1 : 0;
  s->status = 0;
}

static void socket_destroy_shell(coro_socket_t *s) {
  if (!s) return;

  if (s->timer) {
    turbo_timer_stop(s->timer);
    turbo_timer_destroy(s->timer);
    s->timer = NULL;
  }

  coro_socket_reset_recv_compression_state(s);
  socket_clear_tls_client_config(s);
  socket_clear_tls_server_context(s);
  free(s);
}

static void coro_socket_cleanup_create_failure(coro_socket_t *s) {
  if (!s) {
    return;
  }

  if (s->timer) {
    turbo_timer_stop(s->timer);
    turbo_timer_destroy(s->timer);
    s->timer = NULL;
  }

  coro_socket_reset_recv_compression_state(s);
  socket_clear_tls_client_config(s);
  socket_clear_tls_server_context(s);
  free(s);
}

static void coro_socket_reset_recv_compression_state(coro_socket_t *s) {
  if (!s) return;

  free(s->recv_compression_cache);
  s->recv_compression_cache = NULL;
  s->recv_compression_cache_len = 0U;
  s->recv_compression_header_len = 0U;
  s->recv_compression_expected_compressed_len = 0U;
  s->recv_compression_expected_uncompressed_len = 0U;
}

/* ── Reference Counting ───────────────────────────────────── */

void retain_client(coro_socket_t *client) {
  if (!client) {
    return;
  }
  atomic_fetch_add_explicit(&client->ref_count, 1, memory_order_relaxed);
}

void release_client(coro_socket_t *client) {
  int old_ref;

  if (!client) {
    return;
  }

  old_ref = (int)atomic_fetch_sub_explicit(&client->ref_count, 1, memory_order_acq_rel);
  if (old_ref == 1) {
    atomic_thread_fence(memory_order_acquire);
    /* Clean up TLS context (TODO: migrate) */

    /* Clean up listener socket */
    if (client->listener) {
      coro_socket_t *l = client->listener;
      client->listener = NULL;
      coro_socket_destroy(l);
    }

    /* Free transport handles */
    if ((client->transport == TURBO_TCP || client->transport == TURBO_TLS ||
         client->transport == TURBO_PIPE || client->transport == TURBO_WEBSOCKET) &&
        client->handle.stream) {
      turbo_stream_destroy(client->handle.stream);
      client->handle.stream = NULL;
    }

    if (client->transport == TURBO_UDP && client->handle.datagram && client->owns_handle) {
      turbo_datagram_destroy(client->handle.datagram);
      client->handle.datagram = NULL;
    }

    if (client->transport == TURBO_KCP && client->handle.kcp && client->owns_handle) {
      turbo_kcp_destroy(client->handle.kcp);
      client->handle.kcp = NULL;
    }

    coro_socket_reset_recv_compression_state(client);

    /* Free TCP listener state */
    if ((client->transport == TURBO_TCP || client->transport == TURBO_WEBSOCKET) &&
        client->native_tcp_state) {
      free(client->native_tcp_state);
      client->native_tcp_state = NULL;
    }

    socket_clear_tls_client_config(client);
    socket_clear_tls_server_context(client);
    free(client);
  }
}

void coro_socket_release_destroy_wait_handoff(coro_socket_t *s) {
  if (!s || !s->destroy_wait_handoff) {
    return;
  }

  s->destroy_wait_handoff = 0;
  release_client(s);
}

void coro_socket_release_destroy_wait_guard(coro_socket_t *s) {
  if (!s || !s->destroy_wait_guard_ref) {
    return;
  }

  s->destroy_wait_guard_ref = 0;
  release_client(s);
}

static void release_accepted_transport_ref_if_orphaned(coro_socket_t *s) {
  if (!s || !s->accepted_ref || s->close_pending) {
    return;
  }

  s->accepted_ref = 0;
  release_client(s);
}

/* ── Timeout Management ───────────────────────────────────── */

enum {
  CORO_TIMEOUT_IDLE = 0,
  CORO_TIMEOUT_ARMED = 1,
  CORO_TIMEOUT_POSTED = 2,
  CORO_TIMEOUT_CANCELED_POSTED = 3,
};

static void on_timer_fired_bounce(void *arg1, void *arg2) {
  (void)arg2;
  coro_socket_t *s = (coro_socket_t *)arg1;

  if (s->timer_active == CORO_TIMEOUT_IDLE) {
    return; /* Already cancelled and released via stop_timeout_timer */
  }
  if (s->timer_active == CORO_TIMEOUT_CANCELED_POSTED) {
    s->timer_active = CORO_TIMEOUT_IDLE;
    release_client(s);
    return;
  }
  if (!s->co_wait) {
    s->timer_active = CORO_TIMEOUT_IDLE;
    release_client(s);
    return;
  }

  s->timed_out = 1;
  s->status = TURBO_ETIMEDOUT;
  s->timer_active = CORO_TIMEOUT_IDLE; /* Clear flag before resume to avoid race */
  if (s->co_wait) coro_resume_waiter(s);
  release_client(s);
}

static void on_timer_fired(turbo_timer_t *timer) {
  coro_socket_t *s = (coro_socket_t *)turbo_timer_get_data(timer);
  int rc;

  if (!s) {
    return;
  }
  if (s->timer_active != CORO_TIMEOUT_ARMED) {
    return;
  }
  s->timer_active = CORO_TIMEOUT_POSTED;

  rc = coro_post(s->ctx, on_timer_fired_bounce, s, NULL);
  if (rc != 0) {
    on_timer_fired_bounce(s, NULL);
  }
}

void start_timeout_timer(coro_socket_t *s) {
  s->timed_out = 0;
  if (s->timeout_ms > 0 && s->timer && !s->timer_active) {
    s->timer_active = CORO_TIMEOUT_ARMED;
    retain_client(s);
    turbo_timer_start(s->timer, on_timer_fired, s->timeout_ms, 0);
  }
}

void stop_timeout_timer(coro_socket_t *s) {
  if (s->timer_active == CORO_TIMEOUT_ARMED) {
    s->timer_active = CORO_TIMEOUT_IDLE;
    turbo_timer_stop(s->timer);
    release_client(s);
    return;
  }
  if (s->timer_active == CORO_TIMEOUT_POSTED) {
    s->timer_active = CORO_TIMEOUT_CANCELED_POSTED;
    turbo_timer_stop(s->timer);
  }
}

typedef struct {
  coro_socket_t *socket;
  char *data;
  size_t len;
  int has_slice;
} recv_post_t;

static void recv_post_cb(void *arg1, void *arg2) {
  recv_post_t *post = (recv_post_t *)arg1;
  (void)arg2;

  if (post != NULL && post->socket != NULL) {
    if (post->has_slice) {
      mem_slice_t slice;
      slice.data = post->data;
      slice.length = post->len;
      slice.buffer = NULL;
      coro_deliver_recv(post->socket, &slice);
    } else {
      coro_deliver_recv(post->socket, NULL);
    }
    coro_resume_waiter_with_handoff(post->socket);
    release_client(post->socket);
  }
  if (post != NULL) {
    free(post->data);
    free(post);
  }
}

/* ── Transport Bridge Logic ───────────────────────────────── */

void coro_socket_handle_transport_recv(coro_socket_t *s, const mem_slice_t *slice) {
  if (!s) return;

  if (!s->co_wait || s->timed_out) {
    coro_deliver_recv(s, slice);
    return;
  }

  stop_timeout_timer(s);
  if (s->co_is_scheduled && s->ctx != NULL && coro_context_current() != s->ctx) {
    recv_post_t *post = (recv_post_t *)calloc(1, sizeof(*post));
    if (post != NULL) {
      post->socket = s;
      if (slice != NULL) {
        post->has_slice = 1;
        post->len = slice->length;
        if (slice->length > 0U) {
          post->data = (char *)malloc(slice->length);
          if (post->data == NULL) {
            free(post);
            post = NULL;
          } else {
            memcpy(post->data, slice->data, slice->length);
          }
        }
      }
    }
    if (post != NULL) {
      retain_client(s);
      if (coro_post(s->ctx, recv_post_cb, post, NULL) == 0) {
        return;
      }
      release_client(s);
      free(post->data);
      free(post);
    }
  }

  coro_deliver_recv(s, slice);
  coro_resume_waiter_with_handoff(s);
  if (!s->destroy_wait_handoff) {
    release_client(s);
  }
}

void coro_socket_handle_transport_connect(coro_socket_t *s, int status) {
  if (!s) return;

  if (status == 0) s->connected = 1;
  s->status = status;

  if (s->co_wait && !s->timed_out) {
    stop_timeout_timer(s);
    coro_resume_waiter_with_handoff(s);
  }

  if (!s->destroy_wait_handoff) {
    release_client(s); /* Balance retain from tcp_connect */
  }
}

void coro_socket_handle_transport_close(coro_socket_t *s) {
  int resumed_waiter = 0;
  int release_close_ref = 0;
  int release_wait_ref = 0;

  if (!s) return;

  if (s->transport == TURBO_TCP || s->transport == TURBO_TLS || s->transport == TURBO_PIPE ||
      s->transport == TURBO_WEBSOCKET)
    s->handle.stream = NULL;

  if (s->transport == TURBO_UDP) s->handle.datagram = NULL;

  s->connected = 0;
  if (s->co_wait) {
    stop_timeout_timer(s);
    s->status = (s->status == 0) ? TURBO_EOF : s->status;
    coro_resume_waiter_with_handoff(s);
    resumed_waiter = 1;
  } else {
    s->peer_eof_pending = 1;
  }

  if (s->co_write_wait) {
    s->write_status = TURBO_EOF;
    coro_resume_co(s->ctx, s->co_write_wait);
    s->co_write_wait = NULL;
  }

  if (s->close_pending) {
    s->close_pending = 0;
    release_close_ref = 1;
  }

  if (resumed_waiter) {
    release_wait_ref = !s->destroy_wait_handoff;
  }

  if (release_wait_ref) {
    release_client(s); /* Balance retain from recv/connect wait */
  }

  if (release_close_ref) {
    release_client(s); /* Balance retain from tcp_close */
  }
}

void coro_client_wake_eof(coro_socket_t *client) {
  if (!client || !client->co_wait) return;
  stop_timeout_timer(client);
  client->connected = 0;
  client->status = TURBO_EOF;
  client->recv_data = NULL;
  client->recv_len = 0;
  retain_client(client);
  coro_resume_waiter_with_handoff(client);
  if (!client->destroy_wait_handoff) {
    release_client(client); /* Balance pending recv wait retain */
  }
  release_client(client);
}

/* ── DNS Resolution ───────────────────────────────────────── */

typedef struct {
  coro_socket_t *s;
  turbo_dns_result_t results[TURBO_DNS_MAX_RESULTS * 2];
  size_t count;
  int status;
} dns_bounce_t;

static void on_dns_resolved_bounce(void *arg1, void *arg2) {
  dns_bounce_t *b = (dns_bounce_t *)arg1;
  coro_socket_t *s = b->s;
  (void)arg2;

  if (s->timed_out) {
    free(b);
    release_client(s);
    return;
  }

  stop_timeout_timer(s);
  s->status = b->status;
  s->resolved_ip_count = 0;
  s->resolved_ip[0] = '\0';
  if (b->status == 0) {
    for (size_t i = 0; i < b->count && i < TURBO_DNS_MAX_RESULTS; i++) {
      strncpy(s->resolved_ips[i], b->results[i].ip, sizeof(s->resolved_ips[i]) - 1);
      s->resolved_ips[i][sizeof(s->resolved_ips[i]) - 1] = '\0';
      s->resolved_ip_count++;
    }
    if (s->resolved_ip_count > 0) {
      strncpy(s->resolved_ip, s->resolved_ips[0], sizeof(s->resolved_ip) - 1);
      s->resolved_ip[sizeof(s->resolved_ip) - 1] = '\0';
    }
  }

  if (s->co_wait) {
    coro_resume_waiter_with_handoff(s);
  }

  free(b);
  if (!s->destroy_wait_handoff) {
    release_client(s);
  }
}

static void on_dns_resolved(const char *hostname, const turbo_dns_result_t *results, size_t count,
                            int status, void *user_data) {
  coro_socket_t *s = (coro_socket_t *)user_data;
  int rc;
  UNUSED(hostname);

  /* This is called from a background thread MUST use post for thread safety */
  dns_bounce_t *b = (dns_bounce_t *)calloc(1, sizeof(dns_bounce_t));
  if (!b) {
    rc = coro_socket_interrupt_wait(s, TURBO_ENOMEM);
    if (rc != 0) {
      TLOG_ERROR("coro_socket_connect: failed to report DNS ENOMEM");
      abort();
    }
    return;
  }

  b->s = s;
  b->status = status;
  if (results && count > 0) {
    if (count > TURBO_DNS_MAX_RESULTS * 2) count = TURBO_DNS_MAX_RESULTS * 2;
    memcpy(b->results, results, count * sizeof(turbo_dns_result_t));
    b->count = count;
  }

  rc = coro_post(s->ctx, on_dns_resolved_bounce, b, NULL);
  if (rc != 0) {
    s->status = rc;
    free(b);
    TLOG_ERROR("coro_socket_connect: DNS bounce post failed");
    abort();
  }
}

static uint64_t coro_socket_connect_deadline_ms(coro_socket_t *s) {
  uint64_t now;

  if (!s || !s->loop || s->timeout_ms == 0) return 0;
  now = turbo_loop_now(s->loop);
  if (UINT64_MAX - now < s->timeout_ms) return UINT64_MAX;
  return now + s->timeout_ms;
}

static uint64_t coro_socket_connect_remaining_ms(coro_socket_t *s, uint64_t deadline_ms) {
  uint64_t now;

  if (!s || deadline_ms == 0) return 0;
  now = turbo_loop_now(s->loop);
  if (now >= deadline_ms) return 0;
  return deadline_ms - now;
}

static void coro_socket_reset_retry_state(coro_socket_t *s) {
  if (!s) return;

  s->connected = 0;
  s->status = 0;
  s->peer_eof_pending = 0;
  s->timed_out = 0;
  s->co_wait = NULL;

  if ((s->transport == TURBO_TCP || s->transport == TURBO_TLS || s->transport == TURBO_PIPE ||
       s->transport == TURBO_WEBSOCKET) &&
      s->handle.stream && !s->handle.stream->closing) {
    turbo_stream_set_user_data(s->handle.stream, NULL);
    turbo_stream_destroy(s->handle.stream);
    s->handle.stream = NULL;
  }
}

static int coro_socket_connect_resolved(coro_socket_t *s, const char *host, int port,
                                        uint64_t deadline_ms) {
  uint64_t saved_timeout = s ? s->timeout_ms : 0;
  int last_status = TURBO_EAI_FAIL;

  if (!s) return TURBO_EINVAL;

  if (s->resolved_ip_count == 0) {
    return s->ops->connect(s, host, port);
  }

  for (size_t i = 0; i < s->resolved_ip_count; i++) {
    uint64_t remaining_ms = coro_socket_connect_remaining_ms(s, deadline_ms);

    if (deadline_ms != 0) {
      if (remaining_ms == 0) {
        last_status = TURBO_ETIMEDOUT;
        break;
      }
      s->timeout_ms = remaining_ms;
    }

    strncpy(s->resolved_ip, s->resolved_ips[i], sizeof(s->resolved_ip) - 1);
    s->resolved_ip[sizeof(s->resolved_ip) - 1] = '\0';
    retain_client(s);
    last_status = s->ops->connect(s, host, port);
    s->timeout_ms = saved_timeout;
    release_client(s);

    if (last_status == 0) {
      return 0;
    }

    if (i + 1 < s->resolved_ip_count) {
      coro_socket_reset_retry_state(s);
    }
  }

  s->timeout_ms = saved_timeout;
  return last_status;
}

/* ── Socket Creation ──────────────────────────────────────── */

typedef int (*coro_socket_protocol_init_fn)(coro_socket_t *s, coro_socket_type_t type);

typedef struct coro_socket_protocol_desc_s {
  coro_socket_type_t type;
  turbo_transport_t transport;
  const coro_transport_ops_t *ops;
  turbo_dns_pref_t dns_pref;
  int owns_handle;
  coro_socket_protocol_init_fn init;
} coro_socket_protocol_desc_t;

static int socket_init_pipe(coro_socket_t *s, coro_socket_type_t type) {
  UNUSED(type);

  s->handle.stream = turbo_stream_create(s->ctx, TURBO_STREAM_PIPE);
  if (!s->handle.stream) {
    return socket_last_error_or(s, TURBO_ENOMEM);
  }
  turbo_stream_set_user_data(s->handle.stream, s);
  s->handle.stream->managed = 1;
  return 0;
}

static int socket_init_udp(coro_socket_t *s, coro_socket_type_t type) {
  turbo_datagram_kind_t kind;

  kind = (type == CORO_SOCKET_UDP_V6) ? TURBO_DATAGRAM_UDP6 : TURBO_DATAGRAM_UDP4;
  s->handle.datagram = turbo_datagram_create(s->ctx, kind);
  if (!s->handle.datagram) {
    return socket_last_error_or(s, TURBO_ENOMEM);
  }
  turbo_datagram_set_user_data(s->handle.datagram, s);
  return 0;
}

static const coro_socket_protocol_desc_t g_socket_protocols[] = {
    {CORO_SOCKET_TCP_V4, TURBO_TCP, &transport_ops_tcp, TURBO_DNS_IPV4_ONLY, 1, NULL},
    {CORO_SOCKET_TCP_V6, TURBO_TCP, &transport_ops_tcp, TURBO_DNS_IPV6_ONLY, 1, NULL},
    {CORO_SOCKET_TLS, TURBO_TLS, &transport_ops_tls, TURBO_DNS_ANY, 1, NULL},
    {CORO_SOCKET_UDP_V4, TURBO_UDP, &udp_client_ops, TURBO_DNS_IPV4_ONLY, 1, socket_init_udp},
    {CORO_SOCKET_UDP_V6, TURBO_UDP, &udp_client_ops, TURBO_DNS_IPV6_ONLY, 1, socket_init_udp},
    {CORO_SOCKET_KCP, TURBO_KCP, &transport_ops_kcp, TURBO_DNS_ANY, 1, NULL},
    {CORO_SOCKET_PIPE, TURBO_PIPE, &transport_ops_pipe, TURBO_DNS_ANY, 1, socket_init_pipe},
};

static const coro_socket_protocol_desc_t *socket_protocol_desc(coro_socket_type_t type) {
  size_t i;

  for (i = 0; i < sizeof(g_socket_protocols) / sizeof(g_socket_protocols[0]); ++i) {
    if (g_socket_protocols[i].type == type) {
      return &g_socket_protocols[i];
    }
  }
  return NULL;
}

coro_socket_t *coro_socket_create_shell(coro_context_t *ctx, turbo_transport_t transport,
                                        const coro_transport_ops_t *ops) {
  coro_socket_t *s;

  if (!ctx || !ctx->arena || !ops) {
    return NULL;
  }

  s = calloc(1, sizeof(coro_socket_t));
  if (!s) {
    return NULL;
  }

  s->loop = ctx->loop;
  s->ctx = ctx;
  atomic_init(&s->ref_count, 1);
  s->arena = ctx->arena;
  s->transport = transport;
  s->ops = ops;
  s->owns_handle = 0;
  s->dns_pref = TURBO_DNS_ANY;
  s->recv_compression_level = turbo_zstd_default_level();
  s->recv_compression_auto = 0;

  s->timer = turbo_timer_create(NULL);
  if (!s->timer) {
    coro_socket_cleanup_create_failure(s);
    return NULL;
  }
  turbo_timer_set_data(s->timer, s);
  return s;
}

coro_socket_t *coro_socket_create(coro_context_t *ctx, coro_socket_type_t type) {
  const coro_socket_protocol_desc_t *desc;
  coro_socket_t *s;
  int rc;

  /* Always use context's arena - no fallback to global pool.
     Good taste: eliminate special cases, context is always required. */
  if (!ctx || !ctx->arena) {
    socket_record_error(ctx, TURBO_EINVAL);
    return NULL; /* Fail fast: context with arena is mandatory */
  }

  desc = socket_protocol_desc(type);
  if (!desc) {
    socket_record_error(ctx, TURBO_EPROTONOSUPPORT);
    return NULL;
  }

  s = coro_socket_create_shell(ctx, desc->transport, desc->ops);
  if (!s) {
    socket_record_error(ctx, TURBO_ENOMEM);
    return NULL;
  }

  s->dns_pref = desc->dns_pref;
  s->owns_handle = desc->owns_handle ? 1 : 0;

  if (desc->init) {
    rc = desc->init(s, type);
    if (rc != 0) {
      socket_record_error(ctx, rc);
      socket_destroy_shell(s);
      return NULL;
    }
  }

  /* Initial reference is held by the caller. */
  socket_record_error(ctx, 0);
  return s;
}

/* ── Socket Connect ───────────────────────────────────────── */

int coro_socket_connect_direct_internal(coro_socket_t *s, const char *connect_host, int port,
                                        const char *request_host) {
  uint64_t deadline_ms;
  const char *target_host;

  if (!s || !connect_host) return TURBO_EINVAL;
  target_host = (request_host && request_host[0] != '\0') ? request_host : connect_host;

  /* If transport is already set (e.g. by coro_socket_create), use it.
     Good taste: don't re-resolve what we already know. */
  turbo_transport_t transport = s->transport;
  if (transport >= TURBO_TRANSPORT_MAX) {
    transport = TURBO_TCP;
  }

  if (transport >= TURBO_TRANSPORT_MAX || !transport_ops_table[transport]) {
    return TURBO_EPROTONOSUPPORT;
  }

  coro_socket_configure_transport_internal(s, transport, 0);
  s->resolved_ip_count = 0;
  s->resolved_ip[0] = '\0';
  deadline_ms = coro_socket_connect_deadline_ms(s);

  if (transport == TURBO_PIPE) {
    return s->ops->connect(s, connect_host, port);
  }

  /* DNS resolution for host-based protocols */
  struct sockaddr_storage probe;
  if (turbo_dns_parse_address(connect_host, 0, &probe) == 0) {
    /* Already an IP address */
    strncpy(s->resolved_ip, connect_host, sizeof(s->resolved_ip) - 1);
    s->resolved_ip[sizeof(s->resolved_ip) - 1] = '\0';
    strncpy(s->resolved_ips[0], connect_host, sizeof(s->resolved_ips[0]) - 1);
    s->resolved_ips[0][sizeof(s->resolved_ips[0]) - 1] = '\0';
    s->resolved_ip_count = 1;
  } else {
    /* Need DNS resolution */
    int r;
    turbo_dns_init();
    s->dns_initialized = 1;
    retain_client(s);
    coro_set_wait(s);

    r = turbo_dns_resolve_async_results2(s->loop, connect_host, s->dns_pref, on_dns_resolved, s,
                                         &s->dns_query);

    if (r != 0) {
      s->co_wait = NULL;
      release_client(s);
      return r;
    }

    start_timeout_timer(s);
    coro_yield();
    s->dns_query = NULL;

    {
      int timed_out = s->timed_out;
      int status = s->status;
      coro_socket_release_destroy_wait_handoff(s);

      if (timed_out) return TURBO_ETIMEDOUT;
      if (status != 0) return status;
    }
  }

  if (s->connect_policy) {
    turbo_dns_result_t results[TURBO_DNS_MAX_RESULTS];
    size_t i;
    int policy_rc;
    memset(results, 0, sizeof(results));
    for (i = 0; i < s->resolved_ip_count; ++i) {
      strncpy(results[i].ip, s->resolved_ips[i], sizeof(results[i].ip) - 1);
      results[i].family = strchr(results[i].ip, ':') ? AF_INET6 : AF_INET;
    }
    policy_rc = s->connect_policy(connect_host, port, results, s->resolved_ip_count,
                                  s->connect_policy_user_data);
    if (policy_rc != 0) return policy_rc;
  }

  return coro_socket_connect_resolved(s, target_host, port, deadline_ms);
}

static int coro_socket_connect_host_impl(coro_socket_t *s, const char *connect_host, int port,
                                         const char *request_host) {
  if (s && s->proxy.type != CORO_PROXY_DIRECT) {
    return coro_socket_proxy_connect_internal(s, connect_host, port, request_host);
  }
  return coro_socket_connect_direct_internal(s, connect_host, port, request_host);
}

int coro_socket_set_connect_policy(coro_socket_t *s, coro_socket_connect_policy_fn policy,
                                   void *user_data) {
  if (!s || (policy && (s->co_wait || s->connected))) return TURBO_EINVAL;
  s->connect_policy = policy;
  s->connect_policy_user_data = user_data;
  return 0;
}

int coro_socket_connect_host_ex(coro_socket_t *s, const char *connect_host, int port,
                                const char *request_host) {
  return coro_socket_connect_host_impl(s, connect_host, port, request_host);
}

int coro_socket_connect(coro_socket_t *s, const char *host, int port) {
  return coro_socket_connect_host_impl(s, host, port, host);
}

int coro_socket_connect_pipe(coro_socket_t *s, const char *path) {
  if (!s || !path) return socket_return_error(s, TURBO_EINVAL);

  coro_socket_configure_transport_internal(s, TURBO_PIPE, 0);
  if (!s->ops || !s->ops->connect) {
    return socket_return_error(s, TURBO_ENOTSUP);
  }
  return s->ops->connect(s, path, 0);
}

/* WebSocket connect lives in turbo_coro_socket_ws.c. */

int coro_socket_set_tls_client_config(coro_socket_t *s, const turbo_tls_client_config_t *config) {
  turbo_tls_client_config_t applied;
  int rc;

  if (!s) {
    return TURBO_EINVAL;
  }

  rc = socket_copy_tls_client_config(s, config);
  if (rc != 0) {
    return socket_return_error(s, rc);
  }

  if (s->handle.stream == NULL ||
      (s->transport != TURBO_TLS && s->transport != TURBO_WEBSOCKET)) {
    return socket_return_error(s, 0);
  }

  memset(&applied, 0, sizeof(applied));
  if (s->tls_client_configured) {
    applied.ca_file = s->tls_ca_file;
    applied.cert_file = s->tls_cert_file;
    applied.key_file = s->tls_key_file;
    applied.key_password = s->tls_key_password;
    applied.cipher_list = s->tls_cipher_list;
    applied.verify_peer = s->tls_verify_peer;
    rc = turbo_stream_tls_set_client_config(s->handle.stream, &applied);
  } else {
    rc = turbo_stream_tls_set_client_config(s->handle.stream, NULL);
  }

  return socket_return_error(s, rc);
}

int coro_socket_set_tls_server_config(coro_socket_t *s,
                                      const turbo_tls_server_config_t *config) {
  turbo_tls_server_context_t *next = NULL;
  int rc;

  if (!s || (s->transport != TURBO_TLS && s->transport != TURBO_TCP)) {
    return socket_return_error(s, TURBO_EINVAL);
  }
  if (s->connected || s->co_wait || s->listener || s->handle.stream) {
    return socket_return_error(s, TURBO_EBUSY);
  }

  if (config) {
    rc = turbo_stream_tls_server_context_create_internal(config, &next);
    if (rc != 0) {
      return socket_return_error(s, rc);
    }
  }

  socket_clear_tls_server_context(s);
  s->tls_server_context = next;
  return socket_return_error(s, 0);
}

int coro_socket_set_ws_server_config(coro_socket_t *s,
                                     const coro_ws_server_config_t *config) {
  size_t path_len = 0;
  size_t subprotocol_len = 0;

  if (!s || (s->transport != TURBO_TCP && s->transport != TURBO_TLS)) {
    return socket_return_error(s, TURBO_EINVAL);
  }
  if (s->connected || s->co_wait || s->listener || s->handle.stream) {
    return socket_return_error(s, TURBO_EBUSY);
  }
  if (!config) {
    s->ws_server_configured = 0;
    s->ws_server_path[0] = '\0';
    s->ws_server_subprotocol[0] = '\0';
    s->ws_server_max_message_size = 0;
    s->ws_server_binary_only = 0;
    return socket_return_error(s, 0);
  }
  if (config->size != sizeof(*config)) {
    return socket_return_error(s, TURBO_EINVAL);
  }
  if (config->path) {
    path_len = strlen(config->path);
    if (path_len == 0 || config->path[0] != '/') {
      return socket_return_error(s, TURBO_EINVAL);
    }
    if (path_len >= sizeof(s->ws_server_path)) {
      return socket_return_error(s, TURBO_ERANGE);
    }
  }
  if (config->subprotocol) {
    subprotocol_len = strlen(config->subprotocol);
    if (subprotocol_len == 0 ||
        subprotocol_len >= sizeof(s->ws_server_subprotocol)) {
      return socket_return_error(
          s, subprotocol_len == 0 ? TURBO_EINVAL : TURBO_ERANGE);
    }
  }

  s->ws_server_path[0] = '\0';
  s->ws_server_subprotocol[0] = '\0';
  if (path_len > 0) memcpy(s->ws_server_path, config->path, path_len + 1U);
  if (subprotocol_len > 0) {
    memcpy(s->ws_server_subprotocol, config->subprotocol,
           subprotocol_len + 1U);
  }
  s->ws_server_max_message_size = config->max_message_size;
  s->ws_server_binary_only = config->binary_only ? 1 : 0;
  s->ws_server_configured = 1;
  return socket_return_error(s, 0);
}

/* ── Socket I/O ───────────────────────────────────────────── */

static int coro_socket_recv_compression_append(coro_socket_t *s, const char *chunk, size_t chunk_len) {
  size_t new_len;
  char *new_cache;

  if (!s || !chunk || chunk_len == 0U) {
    return 0;
  }

  if (s->recv_compression_cache_len > (SIZE_MAX - chunk_len)) {
    return TURBO_ENOMEM;
  }

  new_len = s->recv_compression_cache_len + chunk_len;
  if (s->recv_compression_cache == NULL) {
    new_cache = (char *)malloc(new_len);
  } else {
    new_cache = (char *)realloc(s->recv_compression_cache, new_len);
  }
  if (!new_cache) {
    return TURBO_ENOMEM;
  }

  memcpy(new_cache + s->recv_compression_cache_len, chunk, chunk_len);
  s->recv_compression_cache = new_cache;
  s->recv_compression_cache_len = new_len;
  s->recv_compression_header_len =
      (new_len >= CORO_ZSTD_FRAME_HEADER_LEN) ? CORO_ZSTD_FRAME_HEADER_LEN : new_len;
  return 0;
}

static void coro_socket_compact_recv_cache(coro_socket_t *s, size_t consumed) {
  size_t remaining;

  if (!s || consumed == 0U || s->recv_compression_cache == NULL) {
    return;
  }

  if (consumed >= s->recv_compression_cache_len) {
    free(s->recv_compression_cache);
    s->recv_compression_cache = NULL;
    s->recv_compression_cache_len = 0U;
    s->recv_compression_header_len = 0U;
    return;
  }

  remaining = s->recv_compression_cache_len - consumed;
  memmove(s->recv_compression_cache, s->recv_compression_cache + consumed, remaining);
  s->recv_compression_cache_len = remaining;
  s->recv_compression_header_len = 0U;
}

static int coro_socket_recv_compressed_payload(coro_socket_t *s, char **data, size_t *len) {
  uint32_t magic;
  uint32_t compressed_len_u32;
  uint32_t uncompressed_len_u32;
  size_t payload_len_needed;
  size_t decompressed_len;
  size_t compressed_len;

  if (!s || !data || !len || !s->recv_compression_cache) {
    return 0;
  }

  if (s->recv_compression_cache_len < CORO_ZSTD_FRAME_HEADER_LEN) {
    return 0;
  }

  magic = zstd_load_u32_be(s->recv_compression_cache);
  if (magic != CORO_ZSTD_FRAME_MAGIC) {
    coro_socket_reset_recv_compression_state(s);
    return TURBO_EPROTO;
  }

  compressed_len_u32 = zstd_load_u32_be(s->recv_compression_cache + 4);
  uncompressed_len_u32 = zstd_load_u32_be(s->recv_compression_cache + 8);
  compressed_len = (size_t)compressed_len_u32;
  decompressed_len = (size_t)uncompressed_len_u32;

  if (compressed_len_u32 > CORO_ZSTD_MAX_FRAME_DATA_LEN) {
    coro_socket_reset_recv_compression_state(s);
    return TURBO_EPROTO;
  }
  if (decompressed_len > CORO_ZSTD_MAX_FRAME_DATA_LEN) {
    coro_socket_reset_recv_compression_state(s);
    return TURBO_EPROTO;
  }

  payload_len_needed = CORO_ZSTD_FRAME_HEADER_LEN + compressed_len;
  if (s->recv_compression_cache_len < payload_len_needed) {
    s->recv_compression_expected_compressed_len = compressed_len;
    s->recv_compression_expected_uncompressed_len = decompressed_len;
    s->recv_compression_header_len = CORO_ZSTD_FRAME_HEADER_LEN;
    return 0;
  }

  if (decompressed_len > 0U) {
    size_t decompressed;
    char *payload = s->recv_compression_cache + CORO_ZSTD_FRAME_HEADER_LEN;
    size_t frame_total = payload_len_needed;
    size_t out_capacity = decompressed_len;
    char *frame_base;

    frame_base = (char *)malloc(sizeof(coro_recv_header_t) + out_capacity);
    if (!frame_base) {
      return TURBO_ENOMEM;
    }

    {
      int rc = turbo_zstd_decompress(
          frame_base + sizeof(coro_recv_header_t), decompressed_len,
          &decompressed, payload, compressed_len);
      if (rc != TURBO_OK) {
        free(frame_base);
        coro_socket_reset_recv_compression_state(s);
        return rc;
      }
    }
    if (decompressed != decompressed_len) {
      free(frame_base);
      coro_socket_reset_recv_compression_state(s);
      return TURBO_EPROTO;
    }

    coro_socket_compact_recv_cache(s, frame_total);
    s->recv_compression_expected_compressed_len = 0U;
    s->recv_compression_expected_uncompressed_len = 0U;
    coro_recv_header_store_before_data(frame_base + sizeof(coro_recv_header_t), 0U, out_capacity, NULL);
    *data = frame_base + sizeof(coro_recv_header_t);
    *len = out_capacity;
    return 1;
  }

  if (compressed_len != 0U) {
    coro_socket_reset_recv_compression_state(s);
    return TURBO_EPROTO;
  }

  *data = (char *)malloc(sizeof(coro_recv_header_t));
  if (!*data) {
    return TURBO_ENOMEM;
  }
  coro_recv_header_store_before_data(*data + sizeof(coro_recv_header_t), 0U, 0U,
                                    NULL);
  *data += sizeof(coro_recv_header_t);
  *len = 0U;
  coro_socket_compact_recv_cache(s, payload_len_needed);
  s->recv_compression_expected_compressed_len = 0U;
  s->recv_compression_expected_uncompressed_len = 0U;
  return 1;
}

static int coro_socket_send_compressed_internal(coro_socket_t *s, const char *d, size_t l,
                                              int level) {
  size_t expected_len;
  size_t compressed_cap;
  size_t compressed_len;
  char *frame;
  char *payload;
  int rc;
  size_t frame_cap;
  uint32_t uncompressed_u32;
  uint32_t compressed_u32;

  if (l > (size_t)UINT32_MAX) {
    return TURBO_EPROTO;
  }
  uncompressed_u32 = (uint32_t)l;

  rc = turbo_zstd_compress_bound(l, &compressed_cap);
  if (rc != TURBO_OK) {
    return rc;
  }

  if (compressed_cap > (size_t)(UINT32_MAX - CORO_ZSTD_FRAME_HEADER_LEN)) {
    return TURBO_EPROTO;
  }

  frame_cap = compressed_cap + CORO_ZSTD_FRAME_HEADER_LEN;
  frame = (char *)malloc(frame_cap);
  if (!frame) {
    return TURBO_ENOMEM;
  }

  payload = frame + CORO_ZSTD_FRAME_HEADER_LEN;
  rc = turbo_zstd_compress(payload, compressed_cap, &compressed_len, d, l,
                           level);
  if (rc != TURBO_OK) {
    free(frame);
    return rc;
  }
  if (compressed_len > (size_t)UINT32_MAX) {
    free(frame);
    return TURBO_EPROTO;
  }
  compressed_u32 = (uint32_t)compressed_len;

  expected_len = CORO_ZSTD_FRAME_HEADER_LEN + compressed_len;
  zstd_store_u32_be(frame, CORO_ZSTD_FRAME_MAGIC);
  zstd_store_u32_be(frame + 4, compressed_u32);
  zstd_store_u32_be(frame + 8, uncompressed_u32);

  rc = s->ops->send(s, frame, expected_len);
  free(frame);
  return rc;
}

int coro_socket_send(coro_socket_t *s, const char *d, size_t l) {
  if (!s || !d || l == 0) return socket_return_error(s, TURBO_EINVAL);
  if (!s->ops || !s->ops->send) return socket_return_error(s, TURBO_ENOTSUP);
  if (s->recv_compression_auto && s->recv_compression_level > 0) {
    return socket_return_error(s, coro_socket_send_compressed_internal(s, d, l, s->recv_compression_level));
  }
  return coro_socket_send_raw_internal(s, d, l);
}

int coro_socket_send_raw_internal(coro_socket_t *s, const char *d, size_t l) {
  if (!s || !d || l == 0) return socket_return_error(s, TURBO_EINVAL);
  if (!s->ops || !s->ops->send) return socket_return_error(s, TURBO_ENOTSUP);
  return s->ops->send(s, d, l);
}

int coro_socket_send_compressed(coro_socket_t *s, const char *d, size_t l) {
  if (!s || !d || l == 0) return socket_return_error(s, TURBO_EINVAL);
  if (!s->ops || !s->ops->send) return socket_return_error(s, TURBO_ENOTSUP);
  if (s->recv_compression_level <= 0) return socket_return_error(s, TURBO_ENOTSUP);
  return socket_return_error(s, coro_socket_send_compressed_internal(s, d, l, s->recv_compression_level));
}

int coro_socket_recv_compressed(coro_socket_t *s, char **data, size_t *len) {
  if (!s || !data || !len) return socket_return_error(s, TURBO_EINVAL);
  if (!s->ops || !s->ops->recv_start) return socket_return_error(s, TURBO_ENOTSUP);
  if (s->recv_compression_level <= 0) return socket_return_error(s, TURBO_ENOTSUP);

  *data = NULL;
  *len = 0U;

  for (;;) {
    int parsed = coro_socket_recv_compressed_payload(s, data, len);
    if (parsed != 0) {
      return socket_return_error(s, parsed < 0 ? parsed : 0);
    }

    {
      char *chunk = NULL;
      size_t chunk_len = 0U;
      int rc = coro_socket_recv_raw_internal(s, &chunk, &chunk_len);

      if (rc != 0) {
        if (chunk) {
          coro_socket_free_recv(chunk);
        }
        return socket_return_error(s, rc);
      }

      if (chunk == NULL) {
        if (s->recv_compression_cache_len > 0U) {
          coro_socket_reset_recv_compression_state(s);
          return socket_return_error(s, TURBO_EPROTO);
        }
        return 0;
      }

      rc = coro_socket_recv_compression_append(s, chunk, chunk_len);
      coro_socket_free_recv(chunk);
      if (rc != 0) {
        return socket_return_error(s, rc);
      }
    }
  }
}

int coro_socket_set_compression_level(coro_socket_t *s, int level) {
  if (!s) {
    return socket_return_error(s, TURBO_EINVAL);
  }

  if (level < 0) {
    return socket_return_error(s, TURBO_EINVAL);
  }
  if (level > 0) {
    int min_level = turbo_zstd_min_level();
    int max_level = turbo_zstd_max_level();
    if (level < min_level || level > max_level) {
      return socket_return_error(s, TURBO_EINVAL);
    }
  }

  if (s->recv_compression_level != level) {
    if (s->recv_compression_cache != NULL) {
      coro_socket_reset_recv_compression_state(s);
    }
    s->recv_compression_level = level;
  }
  s->recv_compression_auto = level > 0 ? 1 : 0;

  return 0;
}

int coro_socket_send_owned_recv(coro_socket_t *s, char *d, size_t l) {
  int rc;

  if (!s || !d || l == 0) return socket_return_error(s, TURBO_EINVAL);
  if (!s->ops || !s->ops->send) return socket_return_error(s, TURBO_ENOTSUP);

  if (s->ops->send_owned_recv) {
    rc = s->ops->send_owned_recv(s, d, l);
    if (rc != TURBO_ENOTSUP) {
      return socket_return_error(s, rc);
    }
  }

  rc = s->ops->send(s, d, l);
  coro_socket_free_recv(d);
  return socket_return_error(s, rc);
}

int coro_socket_sendv(coro_socket_t *s, const turbo_iovec_t *iov, size_t iovcnt) {
  if (!s || !iov || iovcnt == 0u) return socket_return_error(s, TURBO_EINVAL);
  if (!s->ops || !s->ops->sendv) return socket_return_error(s, TURBO_ENOTSUP);
  return socket_return_error(s, s->ops->sendv(s, iov, iovcnt));
}

mem_buffer_t *coro_socket_get_send_buffer(coro_socket_t *s, size_t min_size) {
  if (!s) return NULL;
  if (!s->ops || !s->ops->get_send_buffer) {
    return socket_return_buffer_error(s, TURBO_ENOTSUP);
  }
  return s->ops->get_send_buffer(s, min_size);
}

int coro_socket_send_buffer(coro_socket_t *s, mem_buffer_t *buffer, size_t len) {
  if (!s || !buffer) return socket_return_error(s, TURBO_EINVAL);
  if (!s->ops || !s->ops->send_buffer) return socket_return_error(s, TURBO_ENOTSUP);
  return s->ops->send_buffer(s, buffer, len);
}

int coro_socket_recv(coro_socket_t *s, char **data, size_t *len) {
  if (s && s->recv_compression_auto && s->recv_compression_level > 0) {
    return coro_socket_recv_compressed(s, data, len);
  }
  return coro_socket_recv_raw_internal(s, data, len);
}

int coro_socket_recv_raw_internal(coro_socket_t *s, char **data, size_t *len) {
  if (!s || !data || !len) return socket_return_error(s, TURBO_EINVAL);
  if (!s->ops || !s->ops->recv_start) return socket_return_error(s, TURBO_ENOTSUP);
  if (s->pending_recv_interrupt) {
    int status = s->pending_recv_interrupt_status;
    s->pending_recv_interrupt = 0;
    s->pending_recv_interrupt_status = 0;
    *data = NULL;
    *len = 0u;
    return status;
  }
  /* Return buffered data if available */
  if (s->recv_data) {
    int ret;
    *data = s->recv_data;
    *len = s->recv_len;
    s->recv_data = NULL;
    s->recv_len = 0;
    ret = s->status;
    if (ret == TURBO_ETIMEDOUT || ret == TURBO_ECANCELED || ret == TURBO_EINTR) {
      s->status = 0;
      s->timed_out = 0;
      ret = 0;
    }
    if (*data != NULL) {
      s->status = 0;
      s->timed_out = 0;
      return 0;
    }
    return ret == TURBO_EOF ? 0 : ret;
  }

  if (s->status != 0) {
    int ret = s->status;
    if (ret == TURBO_EINTR) {
      s->status = 0;
    }
    return ret;
  }

  if (s->peer_eof_pending) {
    return TURBO_EOF;
  }

  if (s->recv_call_inflight || (s->co_wait && s->co_wait != coro_running())) {
    return socket_return_error(s, TURBO_EBUSY);
  }

  s->recv_call_inflight = 1;
  retain_client(s);
  coro_set_wait(s);
  start_timeout_timer(s);

  int r = s->ops->recv_start(s);
  if (r != 0 && r != TURBO_EALREADY) {
    stop_timeout_timer(s);
    s->co_wait = NULL;
    s->recv_call_inflight = 0;
    release_client(s);
    return r;
  }

  if (s->co_wait) {
    coro_yield();
  }

  {
    char *recv_data = s->recv_data;
    size_t recv_len = s->recv_len;
    int status = s->status;
    int timed_out = s->timed_out;
    int ret;

    s->recv_data = NULL;
    s->recv_len = 0;
    s->recv_call_inflight = 0;
    coro_socket_release_destroy_wait_handoff(s);
    if (timed_out) {
      s->timed_out = 0;
      if (status == TURBO_ETIMEDOUT) {
        s->status = 0;
      }
      release_client(s);
    }

    *data = recv_data;
    *len = recv_len;
    if (recv_data != NULL) {
      s->status = 0;
      s->timed_out = 0;
      ret = 0;
      coro_socket_release_destroy_wait_guard(s);
      return ret;
    }
    if (status == TURBO_EINTR) {
      s->status = 0;
    }
    if (recv_data != NULL && status == TURBO_EOF) {
      ret = 0;
    } else {
      ret = status;
    }
    coro_socket_release_destroy_wait_guard(s);
    return ret;
  }
}

static void coro_socket_interrupt_wait_cb(void *arg1, void *arg2) {
  coro_socket_t *s = (coro_socket_t *)arg1;
  int status = (int)(intptr_t)arg2;
  int waiter_interrupted = 0;

  if (!s) {
    return;
  }

  if (s->co_wait) {
    waiter_interrupted = 1;
    stop_timeout_timer(s);
    s->timed_out = 0;
    if (status != 0 || s->status == 0) {
      s->status = status;
    }

    coro_resume_waiter_with_handoff(s);

    /* Drop the pending recv reference unless it was handed to the waiter. */
    if (!s->destroy_wait_handoff) {
      release_client(s);
    }
  }

  if (s->co_write_wait && status != TURBO_EINTR) {
    waiter_interrupted = 1;
    coro_t *co = s->co_write_wait;
    s->write_status = status;
    s->co_write_wait = NULL;
    coro_resume_co(s->ctx, co);
  }

  if (!waiter_interrupted &&
      (!s->pending_recv_interrupt || status != TURBO_OK)) {
    s->pending_recv_interrupt = 1;
    s->pending_recv_interrupt_status = status;
  }

  release_client(s);
}

int coro_socket_interrupt_wait(coro_socket_t *s, int status) {
  int rc;

  if (!s || !s->ctx) {
    return TURBO_EINVAL;
  }

  retain_client(s);
  rc = coro_post(s->ctx, coro_socket_interrupt_wait_cb, s, (void *)(intptr_t)status);
  if (rc != 0) {
    release_client(s);
    return rc;
  }

  return 0;
}

int coro_socket_sendto(coro_socket_t *s, const char *d, size_t l, const struct sockaddr *a) {
  struct sockaddr_storage addr;
  turbo_datagram_t *dg;

  if (!s || s->transport != TURBO_UDP || !d || l == 0 || !a) {
    return TURBO_EINVAL;
  }

  dg = coro_socket_multicast_datagram(s);
  if (!dg) {
    return socket_last_error_or(s, TURBO_EINVAL);
  }

  if (turbo_datagram_get_local_addr(dg, &addr) != 0) {
    int rc = turbo_datagram_bind(dg, NULL, 0);
    if (rc != 0) {
      return rc;
    }
  }

  return turbo_datagram_sendto(dg, a, d, l);
}

static turbo_datagram_t *coro_socket_multicast_datagram(coro_socket_t *s) {
  if (!s || s->transport != TURBO_UDP) {
    return NULL;
  }

  if (s->listener && s->listener->transport == TURBO_UDP && s->listener->handle.datagram) {
    return s->listener->handle.datagram;
  }

  if (s->handle.datagram) {
    return s->handle.datagram;
  }

  return NULL;
}

int coro_socket_join_multicast(coro_socket_t *s, const char *group, const char *iface) {
  turbo_datagram_t *dg = coro_socket_multicast_datagram(s);

  if (!dg || !group) {
    return !group ? TURBO_EINVAL : socket_last_error_or(s, TURBO_EINVAL);
  }

  return turbo_datagram_join_multicast(dg, group, iface);
}

int coro_socket_leave_multicast(coro_socket_t *s, const char *group, const char *iface) {
  turbo_datagram_t *dg = coro_socket_multicast_datagram(s);

  if (!dg || !group) {
    return !group ? TURBO_EINVAL : socket_last_error_or(s, TURBO_EINVAL);
  }

  return turbo_datagram_leave_multicast(dg, group, iface);
}

int coro_socket_set_multicast_loop(coro_socket_t *s, int on) {
  turbo_datagram_t *dg = coro_socket_multicast_datagram(s);

  if (!dg) {
    return socket_last_error_or(s, TURBO_EINVAL);
  }

  return turbo_datagram_set_multicast_loop(dg, on);
}

int coro_socket_set_multicast_ttl(coro_socket_t *s, int ttl) {
  turbo_datagram_t *dg = coro_socket_multicast_datagram(s);

  if (!dg) {
    return socket_last_error_or(s, TURBO_EINVAL);
  }

  return turbo_datagram_set_multicast_ttl(dg, ttl);
}

int coro_socket_set_broadcast(coro_socket_t *s, int on) {
  turbo_datagram_t *dg = coro_socket_multicast_datagram(s);

  if (!dg) {
    return socket_last_error_or(s, TURBO_EINVAL);
  }

  return turbo_datagram_set_broadcast(dg, on);
}

int coro_socket_recvfrom(coro_socket_t *s, char **data, size_t *len,
                         struct sockaddr_storage *addr) {
  int r = coro_socket_recv(s, data, len);
  if (r == 0 && addr) {
    memcpy(addr, &s->peer_addr, sizeof(struct sockaddr_storage));
  }
  return r;
}

/* ── Socket Server Operations ─────────────────────────────── */

int coro_socket_bind(coro_socket_t *s, const struct sockaddr *a) {
  if (!s || !a) return socket_return_error(s, TURBO_EINVAL);
  if (!s->ops || !s->ops->bind) return socket_return_error(s, TURBO_ENOTSUP);

  return s->ops->bind(s, a);
}

int coro_socket_listen(coro_socket_t *s, int b) {
  if (!s) return socket_return_error(s, TURBO_EINVAL);
  if (!s->ops || !s->ops->listen) return socket_return_error(s, TURBO_ENOTSUP);

  return s->ops->listen(s, b);
}

int coro_socket_accept(coro_socket_t *s, coro_socket_t **n) {
  if (!s || !n) return socket_return_error(s, TURBO_EINVAL);
  if (!s->ops || !s->ops->accept) return socket_return_error(s, TURBO_ENOTSUP);

  return s->ops->accept(s, n);
}

void coro_socket_set_reuse_port(coro_socket_t *s, int enable) {
  if (!s) {
    return;
  }

  s->reuse_port = enable ? 1 : 0;
}

int coro_socket_apply_stream_options(coro_socket_t *s) {
  int rc;
  if (!s || !s->handle.stream) return TURBO_EINVAL;
  if (s->socket_recv_buffer_bytes) {
    rc = turbo_stream_set_recv_buffer_size(s->handle.stream, s->socket_recv_buffer_bytes);
    if (rc != 0) return socket_return_error(s, rc);
  }
  if (s->socket_send_buffer_bytes) {
    rc = turbo_stream_set_send_buffer_size(s->handle.stream, s->socket_send_buffer_bytes);
    if (rc != 0) return socket_return_error(s, rc);
  }
  if (s->send_hwm_bytes) {
    rc = turbo_stream_set_send_hwm(s->handle.stream, s->send_hwm_bytes);
    if (rc != 0) return socket_return_error(s, rc);
  }
  if (s->tcp_keepalive_configured) {
    rc = turbo_stream_set_tcp_keepalive(s->handle.stream, &s->tcp_keepalive_config);
    if (rc != 0) return socket_return_error(s, rc);
  }
  if (s->linger_configured) {
    rc = turbo_stream_set_linger(s->handle.stream, &s->linger_config);
    if (rc != 0) return socket_return_error(s, rc);
  }
  return 0;
}

int coro_socket_inherit_stream_options(coro_socket_t *child, const coro_socket_t *parent) {
  if (!child || !parent) return TURBO_EINVAL;
  child->send_hwm_bytes = parent->send_hwm_bytes;
  child->socket_recv_buffer_bytes = parent->socket_recv_buffer_bytes;
  child->socket_send_buffer_bytes = parent->socket_send_buffer_bytes;
  child->tcp_keepalive_config = parent->tcp_keepalive_config;
  child->tcp_keepalive_configured = parent->tcp_keepalive_configured;
  child->linger_config = parent->linger_config;
  child->linger_configured = parent->linger_configured;
  return child->handle.stream ? coro_socket_apply_stream_options(child) : 0;
}

int coro_socket_set_tcp_keepalive(coro_socket_t *s,
                                  const turbo_tcp_keepalive_config_t *config) {
  int rc;
  if (!s || !config) return socket_return_error(s, TURBO_EINVAL);
  if (!socket_is_tcp_backed(s)) return socket_return_error(s, TURBO_ENOTSUP);
  s->tcp_keepalive_config = *config;
  s->tcp_keepalive_configured = 1;
  if (s->handle.stream) {
    rc = turbo_stream_set_tcp_keepalive(s->handle.stream, config);
    if (rc != 0) return socket_return_error(s, rc);
  }
  return 0;
}

int coro_socket_set_linger(coro_socket_t *s, const turbo_socket_linger_config_t *config) {
  int rc;
  if (!s || !config) return socket_return_error(s, TURBO_EINVAL);
  if (!socket_is_tcp_backed(s)) return socket_return_error(s, TURBO_ENOTSUP);
  s->linger_config = *config;
  s->linger_configured = 1;
  if (s->handle.stream) {
    rc = turbo_stream_set_linger(s->handle.stream, config);
    if (rc != 0) return socket_return_error(s, rc);
  }
  return 0;
}

int coro_socket_set_recv_buffer_size(coro_socket_t *s, size_t bytes) {
  int rc;
  if (!s || bytes == 0u) return socket_return_error(s, TURBO_EINVAL);
  if (!socket_is_tcp_backed(s)) return socket_return_error(s, TURBO_ENOTSUP);
  if (bytes > (size_t)INT32_MAX) return socket_return_error(s, TURBO_ERANGE);
  s->socket_recv_buffer_bytes = bytes;
  if (s->handle.stream) {
    rc = turbo_stream_set_recv_buffer_size(s->handle.stream, bytes);
    if (rc != 0) return socket_return_error(s, rc);
  }
  return 0;
}

int coro_socket_set_send_buffer_size(coro_socket_t *s, size_t bytes) {
  int rc;
  if (!s || bytes == 0u) return socket_return_error(s, TURBO_EINVAL);
  if (!socket_is_tcp_backed(s)) return socket_return_error(s, TURBO_ENOTSUP);
  if (bytes > (size_t)INT32_MAX) return socket_return_error(s, TURBO_ERANGE);
  s->socket_send_buffer_bytes = bytes;
  if (s->handle.stream) {
    rc = turbo_stream_set_send_buffer_size(s->handle.stream, bytes);
    if (rc != 0) return socket_return_error(s, rc);
  }
  return 0;
}

int coro_socket_set_send_hwm(coro_socket_t *s, size_t bytes) {
  int rc;
  if (!s) return socket_return_error(s, TURBO_EINVAL);
  if (!socket_is_stream_backed(s)) return socket_return_error(s, TURBO_ENOTSUP);
  s->send_hwm_bytes = bytes;
  if (s->handle.stream) {
    rc = turbo_stream_set_send_hwm(s->handle.stream, bytes);
    if (rc != 0) return socket_return_error(s, rc);
  }
  return 0;
}

/* ── Socket Cleanup ───────────────────────────────────────── */

void coro_socket_destroy(coro_socket_t *s) {
  if (!s) return;
  if (s->destroyed) return;
  s->destroyed = 1;

  /* Wake waiting coroutines */
  if (s->co_wait) {
    stop_timeout_timer(s);
    s->timed_out = 0;
    if (!s->destroy_wait_handoff) {
      s->destroy_wait_handoff = 1;
      s->destroy_wait_guard_ref = 1;
      retain_client(s);
    }
    s->status = TURBO_ECANCELED;
    coro_resume_waiter(s);
  }
  if (s->co_write_wait) {
    s->write_status = TURBO_ECANCELED;
    coro_t *co = s->co_write_wait;
    s->co_write_wait = NULL;
    coro_resume_co(s->ctx, co);
  }

  /* Start closing the transport if it's not already */
  if (s->ops && s->ops->close) {
    s->ops->close(s);
  }
  release_accepted_transport_ref_if_orphaned(s);

  /* Close WebSocket server */

  /* Stop server admission and accepted tasks before releasing the owner. */
  if (s->listener || s->accept_loop_active || s->server_tasks) {
    (void)coro_socket_server_stop(s);
  }

  /* Close timer */
  if (s->timer) {
    turbo_timer_stop(s->timer);
    turbo_timer_destroy(s->timer);
    s->timer = NULL;
  }

  /* Cancel DNS query */
  if (s->dns_query) {
    turbo_dns_cancel(s->dns_query);
    s->dns_query = NULL;
  }

  release_client(s); /* Release creator reference */
}

/* ── Utility Functions ────────────────────────────────────── */

void coro_socket_free_recv(void *d) {
  char *base;
  coro_recv_header_t hdr;

  if (!d) return;

  base = (char *)d - sizeof(coro_recv_header_t);
  coro_recv_header_load_from_data(d, &hdr);
  if (hdr.magic == CORO_RECV_MAGIC_POOLED) {
    mem_unref(hdr.owner);
    return;
  }

  if (hdr.magic == CORO_RECV_MAGIC) {
    /* Memory is from context arena (raw alloc), do NOT free.
       It will be reclaimed when context is destroyed. */
    return;
  }

  free(base); /* Free the whole block (header (magic=0) + data) */
}

void coro_socket_set_timeout(coro_socket_t *s, uint64_t t) {
  if (!s) return;
  s->timeout_ms = t;
  if (t == 0) {
    stop_timeout_timer(s);
  }
}

coro_context_t *coro_socket_get_context(coro_socket_t *s) { return s ? s->ctx : NULL; }

void coro_socket_set_user_data(coro_socket_t *s, void *d) {
  if (s) {
    s->user_data = d;
  }
}

void *coro_socket_get_user_data(coro_socket_t *s) { return s ? s->user_data : NULL; }

turbo_tcp_backend_t coro_socket_get_tcp_backend(const coro_socket_t *s) {
  if (s && s->transport == TURBO_TCP && s->ctx) {
    return s->ctx->tcp_backend;
  }
#ifdef _WIN32
  return TURBO_TCP_BACKEND_IOCP;
#elif defined(__linux__)
  return TURBO_TCP_BACKEND_EPOLL;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  return TURBO_TCP_BACKEND_KQUEUE;
#else
  return (turbo_tcp_backend_t)0;
#endif
}

turbo_udp_backend_t coro_socket_get_udp_backend(const coro_socket_t *s) {
  turbo_udp_backend_t preferred;

  if (!s || s->transport != TURBO_UDP) {
    return TURBO_UDP_BACKEND_AUTO;
  }

  preferred = s->ctx ? s->ctx->udp_backend : TURBO_UDP_BACKEND_AUTO;
  if (preferred != TURBO_UDP_BACKEND_AUTO) {
    return preferred;
  }

#ifdef _WIN32
  return TURBO_UDP_BACKEND_IOCP;
#elif defined(__linux__) && TURBO_HAS_IO_URING
  return TURBO_UDP_BACKEND_IO_URING;
#elif defined(__linux__) || defined(__ANDROID__)
  return TURBO_UDP_BACKEND_EPOLL;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  return TURBO_UDP_BACKEND_KQUEUE;
#else
  return TURBO_UDP_BACKEND_AUTO;
#endif
}

int coro_socket_get_local_address(coro_socket_t *s, struct sockaddr_storage *a) {
  if (!s || !a) {
    return TURBO_EINVAL;
  }
  if (!s->ops) {
    return TURBO_ENOSYS;
  }
  return s->ops->get_local_addr ? s->ops->get_local_addr(s, a) : TURBO_ENOSYS;
}
