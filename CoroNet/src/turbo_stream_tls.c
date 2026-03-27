/**
 * @file turbo_stream_tls.c
 * @brief TLS backend for turbo_stream_t.
 *
 * Implements TURBO_STREAM_TLS on top of a TCP turbo_stream_t via composition.
 * Uses OpenSSL Memory BIOs (BIO_s_mem) to intercept and encrypt/decrypt data
 * asynchronously completely isolated from the event loop type.
 *
 * Client-only for now (Phase 1).
 */

#include "turbo_stream_internal.h"
#include "turbo_buffer.h"
#include "tlog.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <wincrypt.h>
#else
#  include <arpa/inet.h>
#endif

/* Global lock internally in modern OpenSSL, but static initialization flag for context. */
static SSL_CTX *s_default_ctx = NULL;
static int s_ca_configured = 0;

#ifdef _WIN32
static int load_windows_cert_store(SSL_CTX *ctx, const char *store_name) {
  HCERTSTORE store;
  PCCERT_CONTEXT cert = NULL;
  X509_STORE *x509_store;

  if (!ctx || !store_name) {
    return 0;
  }

  store = CertOpenSystemStoreA(0, store_name);
  if (!store) {
    return 0;
  }

  x509_store = SSL_CTX_get_cert_store(ctx);
  if (!x509_store) {
    CertCloseStore(store, 0);
    return 0;
  }

  while ((cert = CertEnumCertificatesInStore(store, cert)) != NULL) {
    const unsigned char *encoded = cert->pbCertEncoded;
    X509 *x509 = d2i_X509(NULL, &encoded, cert->cbCertEncoded);
    if (!x509) {
      ERR_clear_error();
      continue;
    }

    if (X509_STORE_add_cert(x509_store, x509) != 1) {
      unsigned long err = ERR_peek_last_error();
      if (ERR_GET_LIB(err) == ERR_LIB_X509 &&
          ERR_GET_REASON(err) == X509_R_CERT_ALREADY_IN_HASH_TABLE) {
        ERR_clear_error();
      }
    }

    X509_free(x509);
  }

  CertCloseStore(store, 0);
  return 1;
}

static void configure_ca_from_windows_store(void) {
  if (s_ca_configured || !s_default_ctx) {
    return;
  }

  if (load_windows_cert_store(s_default_ctx, "ROOT")) {
    load_windows_cert_store(s_default_ctx, "CA");
    s_ca_configured = 1;
  }
}
#endif

static void configure_ca_from_env(void) {
  if (s_ca_configured) return;
  const char *file = getenv("TURBONET_TLS_CA_FILE");
  const char *path = getenv("TURBONET_TLS_CA_PATH");
  if ((file == NULL || file[0] == '\0') &&
      (path == NULL || path[0] == '\0')) {
    return;
  }

  if (s_default_ctx) {
    SSL_CTX_load_verify_locations(s_default_ctx,
                                  (file && file[0]) ? file : NULL,
                                  (path && path[0]) ? path : NULL);
    s_ca_configured = 1;
  }
}

/* ── State machine ────────────────────────────────────────── */

typedef enum {
  TLS_ST_INIT = 0,
  TLS_ST_CONNECTING_TCP = 1,
  TLS_ST_HANDSHAKING = 2,
  TLS_ST_OPEN = 3,
  TLS_ST_CLOSING = 4,
  TLS_ST_CLOSED = 5,
} tls_state_e;

typedef struct tls_state_s {
  turbo_stream_t *outer;  /* back-pointer to the outer handle */
  turbo_stream_t *tcp;    /* owned inner TCP stream */

  tls_state_e     state;
  char            hostname[256]; /* Used for SNI */

  SSL_CTX        *ctx;    /* OpenSSL context */
  SSL            *ssl;    /* OpenSSL connection object */
  BIO            *rbio;   /* Network -> OpenSSL read BIO */
  BIO            *wbio;   /* OpenSSL -> Network write BIO */

} tls_state_t;

/* ── Forward declarations ─────────────────────────────────── */

static void tls_on_tcp_connect(void *handle, int status, void *peer);
static void tls_on_tcp_close(void *handle);
/* turbo_recv_cb signature: int(void*, const mem_slice_t*, void*) */
static int  tls_on_tcp_recv_cb(void *handle, const mem_slice_t *slice, void *peer);

static void tls_detach_tcp(tls_state_t *st) {
  turbo_stream_t *tcp;

  if (!st || !st->tcp) {
    return;
  }

  tcp = st->tcp;
  st->tcp = NULL;
  tcp->user_data = NULL;
  tcp->managed = 0;
  tcp->on_connect = NULL;
  tcp->on_close = NULL;
}

static void tls_drop_inner_tcp(tls_state_t *st) {
  turbo_stream_t *tcp;

  if (!st || !st->tcp) {
    return;
  }

  tcp = st->tcp;
  tls_detach_tcp(st);
  turbo_stream_destroy(tcp);
}

static void tls_free_state(tls_state_t *st) {
  if (!st) {
    return;
  }

  if (st->ssl) {
    SSL_free(st->ssl);
    st->ssl = NULL;
  }

  st->rbio = NULL;
  st->wbio = NULL;
  free(st);
}

static int tls_configure_hostname(tls_state_t *st) {
  if (!st || !st->ssl || st->hostname[0] == '\0') {
    return 0;
  }

  if (SSL_set_tlsext_host_name(st->ssl, st->hostname) != 1) {
    return TURBO_EIO;
  }

  if (SSL_set1_host(st->ssl, st->hostname) != 1) {
    return TURBO_EIO;
  }

  return 0;
}

static void tls_pump(tls_state_t *st);
static void tls_flush_wbio_to_network(tls_state_t *st);

static void tls_log_handshake_failure(tls_state_t *st, int ssl_rc) {
  int ssl_err;
  long verify_rc;
  unsigned long openssl_err;
  char openssl_buf[256];
  const char *host;

  if (!st || !st->ssl) {
    return;
  }

  ssl_err = SSL_get_error(st->ssl, ssl_rc);
  verify_rc = SSL_get_verify_result(st->ssl);
  openssl_err = ERR_peek_last_error();
  host = st->hostname[0] ? st->hostname : "(unset)";

  if (verify_rc != X509_V_OK) {
    TLOG_ERROR("TLS handshake failed for {}: verify={} ({})",
               host, verify_rc, X509_verify_cert_error_string(verify_rc));
    return;
  }

  if (openssl_err != 0) {
    ERR_error_string_n(openssl_err, openssl_buf, sizeof(openssl_buf));
    TLOG_ERROR("TLS handshake failed for {}: ssl_error={} openssl={}",
               host, ssl_err, openssl_buf);
    return;
  }

  TLOG_ERROR("TLS handshake failed for {}: ssl_error={} (no OpenSSL detail)",
             host, ssl_err);
}

static int tls_attach_tcp_stream(turbo_stream_t *outer, turbo_stream_t *tcp,
                                 const char *hostname,
                                 turbo_connect_cb on_connect,
                                 turbo_close_cb on_close) {
  tls_state_t *st;

  if (!outer || !tcp || tcp->kind == TURBO_STREAM_TLS) {
    return TURBO_EINVAL;
  }

  st = (tls_state_t *)outer->backend_data;
  if (!st || st->tcp) {
    return TURBO_EINVAL;
  }

  if (!tcp->connected) {
    return TURBO_ENOTCONN;
  }

  outer->on_connect = on_connect;
  outer->on_close = on_close;

  if (hostname && hostname[0] != '\0') {
    strncpy(st->hostname, hostname, sizeof(st->hostname) - 1);
    st->hostname[sizeof(st->hostname) - 1] = '\0';
  }

  {
    int rc = tls_configure_hostname(st);
    if (rc != 0) {
      return rc;
    }
  }

  st->tcp = tcp;
  st->tcp->user_data = st;
  st->tcp->managed = 1;
  st->tcp->on_connect = NULL;
  st->tcp->on_close = tls_on_tcp_close;
  turbo_stream_recv_stop(st->tcp);

  st->state = TLS_ST_HANDSHAKING;
  outer->connected = 0;

  {
    int rc = turbo_stream_recv_start(st->tcp, tls_on_tcp_recv_cb);
    if (rc != 0) {
      tls_detach_tcp(st);
      st->state = TLS_ST_CLOSED;
      return rc;
    }
  }

  tls_pump(st);
  return 0;
}

/* ── Initialization ───────────────────────────────────────── */

static SSL_CTX *get_default_tls_ctx(void) {
  if (s_default_ctx) {
    configure_ca_from_env();
    return s_default_ctx;
  }
  /* Auto-init for older OpenSSL just in case, modern ignores it */
#if OPENSSL_VERSION_NUMBER < 0x10100000L
  SSL_library_init();
  SSL_load_error_strings();
#endif
  s_default_ctx = SSL_CTX_new(TLS_client_method());
  if (s_default_ctx) {
    SSL_CTX_set_mode(s_default_ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_CTX_set_mode(s_default_ctx, SSL_MODE_ENABLE_PARTIAL_WRITE);
    SSL_CTX_set_verify(s_default_ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_default_verify_paths(s_default_ctx);
    configure_ca_from_env();
#ifdef _WIN32
    configure_ca_from_windows_store();
#endif
  }
  return s_default_ctx;
}

/* ── Pump Logic (where the magic happens) ─────────────────── */

/**
 * @brief Extract encrypted bytes from wbio and send them over tcp.
 */
static void tls_flush_wbio_to_network(tls_state_t *st) {
  if (!st->wbio || !st->tcp) return;

  char buf[4096];
  int pending;
  while ((pending = BIO_pending(st->wbio)) > 0) {
    int n = BIO_read(st->wbio, buf, sizeof(buf));
    if (n > 0) {
      turbo_stream_send(st->tcp, buf, (size_t)n);
    } else {
      break;
    }
  }
}

/**
 * @brief Drive the TLS state machine.
 *
 * Called whenever TCP resolves its connect, TCP receives data, or upper
 * layer sends data.
 */
static void tls_pump(tls_state_t *st) {
  if (!st || !st->ssl || st->state == TLS_ST_INIT || st->state == TLS_ST_CONNECTING_TCP || st->state == TLS_ST_CLOSED) {
    return;
  }

  /* 1. Drive Handshake */
  if (st->state == TLS_ST_HANDSHAKING) {
    int r = SSL_connect(st->ssl);
    tls_flush_wbio_to_network(st);

    if (r == 1) {
      /* Handshake complete! */
      st->state = TLS_ST_OPEN;
      st->outer->connected = 1;
      if (st->outer->on_connect) {
        st->outer->on_connect(st->outer, 0, NULL);
      }
      /* Fall through to process any early application data */
    } else {
      int err = SSL_get_error(st->ssl, r);
      if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        return; /* Wait for more network I/O */
      } else {
        /* Handshake protocol error */
        tls_log_handshake_failure(st, r);
        st->state = TLS_ST_CLOSING;
        if (st->outer->on_connect) {
          st->outer->on_connect(st->outer, TURBO_ECONNABORTED, NULL);
        }
        turbo_stream_close(st->outer);
        return;
      }
    }
  }

  /* 2. Drive Open (Read Decrypted Data) */
  if (st->state == TLS_ST_OPEN) {
    char buf[4096];
    int n;
    while ((n = SSL_read(st->ssl, buf, sizeof(buf))) > 0) {
      /* Deliver to upper layer */
      if (st->outer->on_recv) {
        mem_slice_t sl = { .data = buf, .length = (size_t)n, .buffer = NULL };
        int close_req = st->outer->on_recv(st->outer, &sl, NULL);
        if (close_req) {
          turbo_stream_close(st->outer);
          return;
        }
      }
    }
    
    int err = SSL_get_error(st->ssl, n);
    if (err == SSL_ERROR_ZERO_RETURN) {
      /* Clean shutdown from peer (close_notify) */
      turbo_stream_close(st->outer);
      return;
    } else if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
      /* Protocol error / abrupt disconnect */
      turbo_stream_close(st->outer);
      return;
    }

    /* SSL_read could have triggered renegotiation/ticket updates */
    tls_flush_wbio_to_network(st);
  }
  
  /* 3. Drive Close */
  if (st->state == TLS_ST_CLOSING) {
    int r = SSL_shutdown(st->ssl);
    tls_flush_wbio_to_network(st);
    if (r == 1) {
      /* TCP close will follow shortly */
    }
  }
}

/* ── TCP Callbacks ────────────────────────────────────────── */

static int tls_on_tcp_recv_cb(void *handle, const mem_slice_t *slice, void *peer) {
  (void)peer;
  turbo_stream_t *tcp = (turbo_stream_t *)handle;
  tls_state_t    *st  = (tls_state_t *)tcp->user_data;
  if (!st || !st->rbio || !slice || !slice->data || slice->length == 0) return 0;

  /* Push encrypted network bytes into rbio */
  int written = BIO_write(st->rbio, slice->data, (int)slice->length);
  if (written <= 0) {
    /* Shouldn't happen with memory BIOs unless OOM */
    return -1;
  }

  /* Drive TLS state machine to consume the data */
  tls_pump(st);
  return 0;
}

static void tls_on_tcp_connect(void *handle, int status, void *peer) {
  (void)peer;
  turbo_stream_t *tcp = (turbo_stream_t *)handle;
  tls_state_t    *st  = (tls_state_t *)tcp->user_data;
  if (!st) return;

  if (status != 0) {
    st->state = TLS_ST_CLOSED;
    if (st->outer->on_connect)
      st->outer->on_connect(st->outer, status, NULL);
    return;
  }

  st->state = TLS_ST_HANDSHAKING;
  
  /* Start reading incoming TCP data to fuel the handshake */
  {
    int rc = turbo_stream_recv_start(st->tcp, tls_on_tcp_recv_cb);
    if (rc != 0) {
      st->state = TLS_ST_CLOSED;
      if (st->outer->on_connect) {
        st->outer->on_connect(st->outer, rc, NULL);
      }
      turbo_stream_close(st->tcp);
      return;
    }
  }
  
  /* First pump triggers SSL_connect and sends ClientHello */
  tls_pump(st);
}

static void tls_on_tcp_close(void *handle) {
  turbo_stream_t *tcp = (turbo_stream_t *)handle;
  tls_state_t    *st  = (tls_state_t *)tcp->user_data;
  turbo_stream_t *outer;
  if (!st) return;

  outer = st->outer;
  if (outer) outer->backend_data = NULL;
  tcp->user_data = NULL;
  tls_free_state(st);

  if (outer) {
    turbo_stream_finalize_close(outer);
  }
}

/* ── Backend vtable implementation ───────────────────────── */

static int tls_init(turbo_stream_t *s) {
  tls_state_t *st = (tls_state_t *)calloc(1, sizeof(tls_state_t));
  if (!st) return TURBO_ENOMEM;

  st->outer = s;
  st->state = TLS_ST_INIT;
  st->ctx   = get_default_tls_ctx();
  if (!st->ctx) {
    free(st);
    return TURBO_ENOMEM;
  }

  st->ssl = SSL_new(st->ctx);
  if (!st->ssl) {
    free(st);
    return TURBO_ENOMEM;
  }

  /* Create Memory BIOs: network->SSL (rbio), SSL->network (wbio) */
  st->rbio = BIO_new(BIO_s_mem());
  st->wbio = BIO_new(BIO_s_mem());
  if (!st->rbio || !st->wbio) {
    if (st->rbio) {
      BIO_free(st->rbio);
      st->rbio = NULL;
    }
    if (st->wbio) {
      BIO_free(st->wbio);
      st->wbio = NULL;
    }
    tls_free_state(st);
    return TURBO_ENOMEM;
  }
  
  /* SSL_set_bio takes ownership of the BIOs */
  SSL_set_bio(st->ssl, st->rbio, st->wbio);

  s->backend_data = st;
  return 0;
}

static int tls_connect(turbo_stream_t *s, const struct sockaddr *addr) {
  tls_state_t *st = (tls_state_t *)s->backend_data;
  if (!st || !addr) return TURBO_EINVAL;

  /* Create inner TCP stream */
  turbo_stream_kind_t inner_kind = (addr->sa_family == AF_INET6)
                                   ? TURBO_STREAM_TCP6 : TURBO_STREAM_TCP4;
  st->tcp = turbo_stream_create(s->ctx, inner_kind);
  if (!st->tcp) return TURBO_ENOMEM;

  st->tcp->user_data = st;
  st->tcp->managed   = 1; /* Owned by tls_state_t */

  /* Inform OpenSSL of the hostname for SNI Extension, only if not already set */
  if (st->hostname[0] == '\0') {
    /* Resolve string for SNI and hostname verification */
    if (addr->sa_family == AF_INET) {
      struct sockaddr_in *a4 = (struct sockaddr_in *)addr;
      inet_ntop(AF_INET, &a4->sin_addr, st->hostname, sizeof(st->hostname));
    } else {
      struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)addr;
      inet_ntop(AF_INET6, &a6->sin6_addr, st->hostname, sizeof(st->hostname));
    }
  }
  {
    int rc = tls_configure_hostname(st);
    if (rc != 0) {
      tls_drop_inner_tcp(st);
      return rc;
    }
  }

  st->state = TLS_ST_CONNECTING_TCP;
  {
    int rc = turbo_stream_connect_addr(st->tcp, addr,
                                       tls_on_tcp_connect,
                                       tls_on_tcp_close);
    if (rc != 0) {
      tls_drop_inner_tcp(st);
    }
    return rc;
  }
}

int turbo_stream_tls_wrap_client(turbo_stream_t *tls_stream,
                                 turbo_stream_t *tcp_stream,
                                 const char *hostname,
                                 turbo_connect_cb on_connect,
                                 turbo_close_cb on_close) {
  return tls_attach_tcp_stream(tls_stream, tcp_stream, hostname,
                               on_connect, on_close);
}

static int tls_connect_pipe(turbo_stream_t *s, const char *name) {
  UNUSED(s); UNUSED(name);
  return TURBO_EINVAL; /* TLS over pipe is theoretically possible, but API expects INET */
}

static int tls_send(turbo_stream_t *s, const char *data, size_t len) {
  tls_state_t *st = (tls_state_t *)s->backend_data;
  if (!st || st->state != TLS_ST_OPEN) return TURBO_ENOTCONN;

  if (len == 0) return 0;

  size_t offset = 0;
  while (offset < len) {
    size_t remaining = len - offset;
    int chunk = (remaining > (size_t)INT_MAX) ? INT_MAX : (int)remaining;
    int n = SSL_write(st->ssl, data + offset, chunk);

    if (n > 0) {
      offset += (size_t)n;
      tls_flush_wbio_to_network(st);
      continue;
    }

    tls_flush_wbio_to_network(st);

    {
      int err = SSL_get_error(st->ssl, n);
      if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        continue;
      }
    }

    return TURBO_ECONNABORTED;
  }

  return 0;
}

static int tls_flush(turbo_stream_t *s) {
  tls_state_t *st = (tls_state_t *)s->backend_data;
  if (!st || !st->tcp) return 0;

  /* Drain any pending plaintext from the arena queue into OpenSSL */
  while (s->send_head) {
    mem_buffer_t *buf = s->send_head;
    s->send_head = buf->next;
    if (!s->send_head) s->send_tail = NULL;
    s->send_queued -= buf->used;
    buf->next = NULL;

    SSL_write(st->ssl, buf->data, (int)buf->used);
    mem_unref(buf);
  }

  tls_flush_wbio_to_network(st);
  return turbo_stream_flush(st->tcp);
}

static int tls_recv_start(turbo_stream_t *s) {
  /* on_recv is cached in s->on_recv; underlying TCP read was started 
     during handshake. Nothing more to do. */
  (void)s;
  return 0;
}

static void tls_recv_stop(turbo_stream_t *s) {
  tls_state_t *st = (tls_state_t *)s->backend_data;
  if (st && st->tcp) turbo_stream_recv_stop(st->tcp);
}

static void tls_close(turbo_stream_t *s) {
  tls_state_t *st = (tls_state_t *)s->backend_data;
  if (!st) { turbo_stream_finalize_close(s); return; }

  /* Initiate Graceful TLS Shutdown if currently open */
  if (st->state == TLS_ST_OPEN || st->state == TLS_ST_HANDSHAKING) {
    st->state = TLS_ST_CLOSING;
    tls_pump(st); /* Triggers SSL_shutdown and flushing */
  }

  if (st->tcp) {
    turbo_stream_close(st->tcp); /* triggers async close -> tls_on_tcp_close */
  } else {
    s->backend_data = NULL;
    tls_free_state(st);
    turbo_stream_finalize_close(s);
  }
}

static int tls_get_local(turbo_stream_t *s, struct sockaddr_storage *a) {
  tls_state_t *st = (tls_state_t *)s->backend_data;
  if (!st || !st->tcp) return TURBO_ENOTSUP;
  return turbo_stream_get_local_addr(st->tcp, a);
}

static int tls_get_peer(turbo_stream_t *s, struct sockaddr_storage *a) {
  tls_state_t *st = (tls_state_t *)s->backend_data;
  if (!st || !st->tcp) return TURBO_ENOTSUP;
  return turbo_stream_get_peer_addr(st->tcp, a);
}

/* Listener ops — TLS server accept is Phase 2 */
static int  tls_bind(turbo_stream_listener_t *l, const struct sockaddr *a) {
  UNUSED(l); UNUSED(a); return TURBO_ENOTSUP;
}
static int  tls_bind_pipe(turbo_stream_listener_t *l, const char *n) {
  UNUSED(l); UNUSED(n); return TURBO_ENOTSUP;
}
static int  tls_listen(turbo_stream_listener_t *l, int b) {
  UNUSED(l); UNUSED(b); return TURBO_ENOTSUP;
}
static void tls_listener_close(turbo_stream_listener_t *l) {
  turbo_stream_listener_finalize_close(l);
}

/* ── Cleanup callback for finalize_close ──────────────────── */

/* Since finalize_close cleans up the backend_data, we should hook a custom cleanup. 
 * Actually, turbo_stream_close does not free backend_data! The backend is responsible 
 * for its own freeing during close, or we do it. But turbo_stream.c doesn't free backend_data!
 * In turbo_stream_close, we must make sure memory is freed.
 * Wait, `pipe_state_t` frees itself inside `pw_close`. 
 * So here we must free st->ssl and st in tls_close... BUT we can only free it after 
 * we know it's fully closed, which is `tls_on_tcp_close`.
 */

/* ── Vtable ───────────────────────────────────────────────── */

const turbo_stream_backend_ops_t turbo_stream_tls_ops = {
  .init           = tls_init,
  .connect        = tls_connect,
  .connect_pipe   = tls_connect_pipe,
  .send           = tls_send,
  .flush          = tls_flush,
  .recv_start     = tls_recv_start,
  .recv_stop      = tls_recv_stop,
  .close          = tls_close,
  .get_local_addr = tls_get_local,
  .get_peer_addr  = tls_get_peer,
  .bind           = tls_bind,
  .bind_pipe      = tls_bind_pipe,
  .listen         = tls_listen,
  .listener_close = tls_listener_close,
};

CXX_C_API void turbo_stream_tls_set_sni(turbo_stream_t *s, const char *hostname) {
  if (!s || s->kind != TURBO_STREAM_TLS) return;
  tls_state_t *st = (tls_state_t *)s->backend_data;
  if (!st || !hostname) return;
  strncpy(st->hostname, hostname, sizeof(st->hostname) - 1);
  st->hostname[sizeof(st->hostname) - 1] = '\0';
}
