/**
 * @file turbo_stream_tls.c
 * @brief TLS backend for turbo_stream_t.
 *
 * Implements TURBO_STREAM_TLS on top of a TCP turbo_stream_t via composition.
 * Uses OpenSSL custom BIOs to splice ciphertext into and out of the TLS state
 * machine without an extra memory BIO staging queue.
 *
 * Client support is complete; accepted TCP streams can now be wrapped for
 * server-side TLS handshakes as well.
 */

#include "turbo_stream_internal.h"
#include "CoroNet/turbo_coro_internal.h"
#include "turbo_buffer.h"
#include "turbo_thread.h"
#include "tlog.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#ifndef SSL3_RT_HANDSHAKE
#define SSL3_RT_HANDSHAKE 22
#endif
#ifndef SSL3_MT_CLIENT_HELLO
#define SSL3_MT_CLIENT_HELLO 1
#endif
#ifndef SSL3_MT_SERVER_HELLO
#define SSL3_MT_SERVER_HELLO 2
#endif
#ifndef SSL3_MT_FINISHED
#define SSL3_MT_FINISHED 20
#endif

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
static SSL_CTX *s_server_ctx = NULL;
static int s_ca_configured = 0;
static int s_server_configured = 0;
static SSL_SESSION *s_cached_client_session = NULL;
static char s_cached_client_session_host[256] = {0};
static atomic_int s_tls_protocol_mode;
static turbo_once_t s_tls_cleanup_once = TURBO_ONCE_INIT;
static turbo_once_t s_tls_global_lock_once = TURBO_ONCE_INIT;
static turbo_once_t s_tls_server_ctx_once = TURBO_ONCE_INIT;
static turbo_mutex_t s_tls_global_mutex;
static turbo_mutex_t s_tls_server_ctx_mutex;
static atomic_int s_tls_cleanup_done;
static struct {
  atomic_ullong client_handshakes_started;
  atomic_ullong client_handshakes_completed;
  atomic_ullong client_session_cache_attempts;
  atomic_ullong client_session_reused;
  atomic_ullong client_session_stores;
  atomic_ullong client_handshake_total_ns;
  atomic_ullong client_handshake_bio_write_ns;
  atomic_ullong client_handshake_bio_write_calls;
  atomic_ullong client_handshake_bio_write_bytes;
  atomic_ullong client_handshake_crypto_ns;
  atomic_ullong client_handshake_flush_ns;
  atomic_ullong client_handshake_pump_total_ns;
  atomic_ullong client_handshake_recv_cb_ns;
  atomic_ullong client_handshake_connect_cb_ns;
  atomic_ullong client_handshake_iocp_post_ns;
  atomic_ullong client_handshake_post_drain_ns;
  atomic_ullong client_handshake_waiter_signal_ns;
  atomic_ullong client_handshake_resume_wait_ns;
  atomic_ullong client_handshake_wrap_client_ns;
  atomic_ullong client_handshake_clienthello_to_serverhello_ns;
  atomic_ullong client_handshake_serverhello_to_finished_write_ns;
  atomic_ullong client_handshake_finished_write_to_done_ns;
  atomic_ullong client_handshake_serverhello_to_done_ns;
  atomic_ullong client_handshake_pumps;
  atomic_ullong client_handshakes_tls13;
  atomic_ullong server_handshakes_completed;
  atomic_ullong server_handshake_total_ns;
  atomic_ullong server_handshake_crypto_ns;
  atomic_ullong server_handshake_flush_ns;
  atomic_ullong server_handshake_pump_total_ns;
  atomic_ullong server_handshake_recv_cb_ns;
  atomic_ullong server_handshake_clienthello_to_serverhello_ns;
  atomic_ullong server_handshake_clientfinished_to_done_ns;
  atomic_ullong server_handshake_pumps;
} s_tls_metrics;

typedef struct tls_state_s tls_state_t;

struct turbo_tls_server_context_s {
  atomic_int ref_count;
  SSL_CTX *ctx;
  turbo_tls_client_auth_t client_auth;
};

#define TLS_PLAINTEXT_READ_CHUNK_SIZE 16384U

static void tls_metric_inc(atomic_ullong *counter) {
  atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
}

static void tls_metric_add(atomic_ullong *counter, uint64_t value) {
  atomic_fetch_add_explicit(counter, (unsigned long long)value, memory_order_relaxed);
}

static unsigned long long tls_metric_load(const atomic_ullong *counter) {
  return atomic_load_explicit(counter, memory_order_relaxed);
}

static void tls_global_lock_init(void) {
  turbo_mutex_init(&s_tls_global_mutex);
}

static void tls_global_lock(void) {
  turbo_once(&s_tls_global_lock_once, tls_global_lock_init);
  turbo_mutex_lock(&s_tls_global_mutex);
}

static void tls_global_unlock(void) {
  turbo_mutex_unlock(&s_tls_global_mutex);
}

static void tls_server_ctx_lock_init(void) {
  turbo_mutex_init(&s_tls_server_ctx_mutex);
}

static void tls_server_ctx_lock(void) {
  turbo_once(&s_tls_server_ctx_once, tls_server_ctx_lock_init);
  turbo_mutex_lock(&s_tls_server_ctx_mutex);
}

static void tls_server_ctx_unlock(void) {
  turbo_mutex_unlock(&s_tls_server_ctx_mutex);
}

static void tls_reset_client_session_cache_internal(void) {
  if (s_cached_client_session) {
    SSL_SESSION_free(s_cached_client_session);
    s_cached_client_session = NULL;
  }
  s_cached_client_session_host[0] = '\0';
}

static void tls_store_client_session_for_host(const char *hostname, SSL_SESSION *session) {
  if (!hostname || hostname[0] == '\0' || !session) {
    return;
  }

  tls_global_lock();
  if (session == s_cached_client_session &&
      strcmp(hostname, s_cached_client_session_host) == 0) {
    SSL_SESSION_free(session);
    tls_global_unlock();
    return;
  }

  tls_reset_client_session_cache_internal();
  s_cached_client_session = session;
  strncpy(s_cached_client_session_host, hostname, sizeof(s_cached_client_session_host) - 1);
  s_cached_client_session_host[sizeof(s_cached_client_session_host) - 1] = '\0';
  tls_global_unlock();
  tls_metric_inc(&s_tls_metrics.client_session_stores);
}

static void tls_global_cleanup(void) {
  if (atomic_exchange_explicit(&s_tls_cleanup_done, 1, memory_order_acq_rel) != 0) {
    return;
  }

  tls_global_lock();
  tls_reset_client_session_cache_internal();

  if (s_default_ctx) {
    SSL_CTX_free(s_default_ctx);
    s_default_ctx = NULL;
  }
  s_ca_configured = 0;
  tls_global_unlock();

  tls_server_ctx_lock();
  if (s_server_ctx) {
    SSL_CTX_free(s_server_ctx);
    s_server_ctx = NULL;
  }
  s_server_configured = 0;
  tls_server_ctx_unlock();

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
  OPENSSL_cleanup();
#endif
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((destructor))
static void tls_global_cleanup_destructor(void) {
  tls_global_cleanup();
}
#endif

static void tls_register_global_cleanup(void) {
  (void)atexit(tls_global_cleanup);
}

static const char *tls_get_env_value(const char *name, char *buffer, size_t buffer_size) {
  const char *value;

  if (!name) {
    return NULL;
  }

  value = getenv(name);
  if (value && value[0] != '\0') {
    return value;
  }

#ifdef _WIN32
  if (buffer && buffer_size > 0) {
    DWORD len = GetEnvironmentVariableA(name, buffer, (DWORD)buffer_size);
    if (len > 0 && len < buffer_size) {
      return buffer;
    }
  }
#else
  UNUSED(buffer);
  UNUSED(buffer_size);
#endif

  return NULL;
}

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
  char file_buf[1024];
  char path_buf[1024];
  if (s_ca_configured) return;
  const char *file = tls_get_env_value("TURBONET_TLS_CA_FILE", file_buf, sizeof(file_buf));
  const char *path = tls_get_env_value("TURBONET_TLS_CA_PATH", path_buf, sizeof(path_buf));
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

static int configure_server_ctx_from_env_unlocked(void) {
  char cert_file_buf[1024];
  char key_file_buf[1024];
  const char *cert_file;
  const char *key_file;

  if (s_server_configured) {
    return s_server_ctx ? 0 : TURBO_EINVAL;
  }

  if (!s_server_ctx) {
    return TURBO_EINVAL;
  }

  cert_file = tls_get_env_value("TURBONET_TLS_CERT_FILE",
                                cert_file_buf,
                                sizeof(cert_file_buf));
  key_file = tls_get_env_value("TURBONET_TLS_KEY_FILE",
                               key_file_buf,
                               sizeof(key_file_buf));

  if (!cert_file || cert_file[0] == '\0' || !key_file || key_file[0] == '\0') {
    return TURBO_EINVAL;
  }

  if (SSL_CTX_use_certificate_file(s_server_ctx, cert_file, SSL_FILETYPE_PEM) != 1) {
    return TURBO_EIO;
  }

  if (SSL_CTX_use_PrivateKey_file(s_server_ctx, key_file, SSL_FILETYPE_PEM) != 1) {
    return TURBO_EIO;
  }

  if (SSL_CTX_check_private_key(s_server_ctx) != 1) {
    return TURBO_EIO;
  }

  if (SSL_CTX_set_cipher_list(s_server_ctx, "DEFAULT") != 1) {
    return TURBO_EIO;
  }

#ifdef TLS1_3_VERSION
  if (SSL_CTX_set_ciphersuites(
          s_server_ctx,
          "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256") != 1) {
    return TURBO_EIO;
  }
#endif

  s_server_configured = 1;
  return 0;
}

static int tls_apply_protocol_mode_to_ctx(SSL_CTX *ctx) {
  turbo_tls_protocol_mode_t protocol_mode;

  if (!ctx) {
    return TURBO_EINVAL;
  }

  protocol_mode =
      (turbo_tls_protocol_mode_t)atomic_load_explicit(&s_tls_protocol_mode, memory_order_acquire);

  switch (protocol_mode) {
    case TURBO_TLS_PROTOCOL_DEFAULT:
      if (SSL_CTX_set_min_proto_version(ctx, 0) != 1) {
        return TURBO_EIO;
      }
      if (SSL_CTX_set_max_proto_version(ctx, 0) != 1) {
        return TURBO_EIO;
      }
      return 0;

    case TURBO_TLS_PROTOCOL_TLS13_ONLY:
#ifdef TLS1_3_VERSION
      if (SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) != 1) {
        return TURBO_EIO;
      }
      if (SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION) != 1) {
        return TURBO_EIO;
      }
      return 0;
#else
      return TURBO_ENOTSUP;
#endif
  }

  return TURBO_EINVAL;
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
  int             server_mode;
  turbo_tls_client_auth_t server_client_auth;
  int             client_verify_peer;
  int             client_configured;
  int             client_ctx_owned;
  char            hostname[256]; /* Used for SNI */
  char           *ca_file;
  char           *cert_file;
  char           *key_file;
  char           *key_password;
  char           *cipher_list;

  SSL_CTX        *ctx;    /* OpenSSL context */
  SSL            *ssl;    /* OpenSSL connection object */
  BIO            *rbio;   /* Network -> OpenSSL read BIO */
  BIO            *wbio;   /* OpenSSL -> Network write BIO */
  mem_buffer_t   *pending_plaintext;
  uint64_t        client_handshake_started_ns;
  uint64_t        server_handshake_started_ns;
  uint64_t        client_hello_write_ns;
  uint64_t        client_server_hello_read_ns;
  uint64_t        client_finished_write_ns;
  uint64_t        server_client_hello_read_ns;
  uint64_t        server_server_hello_write_ns;
  uint64_t        server_client_finished_read_ns;
  int             pumping;
  int             close_deferred;
  int             close_requested;

} tls_state_t;

static SSL_CTX *get_default_tls_ctx(void);
static int tls_apply_protocol_mode_to_ctx(SSL_CTX *ctx);
static int tls_on_new_client_session(SSL *ssl, SSL_SESSION *session);
static void tls_install_msg_callback(tls_state_t *st);

static char *tls_strdup_nullable(const char *value) {
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

static void tls_clear_client_config(tls_state_t *st) {
  if (!st) {
    return;
  }

  free(st->ca_file);
  free(st->cert_file);
  free(st->key_file);
  free(st->key_password);
  free(st->cipher_list);
  st->ca_file = NULL;
  st->cert_file = NULL;
  st->key_file = NULL;
  st->key_password = NULL;
  st->cipher_list = NULL;
  st->client_configured = 0;
  st->client_verify_peer = 1;
}

static int tls_copy_client_config(tls_state_t *st, const turbo_tls_client_config_t *config) {
  char *ca_file = NULL;
  char *cert_file = NULL;
  char *key_file = NULL;
  char *key_password = NULL;
  char *cipher_list = NULL;

  if (!st) {
    return TURBO_EINVAL;
  }

  if (config == NULL) {
    tls_clear_client_config(st);
    return 0;
  }

  ca_file = tls_strdup_nullable(config->ca_file);
  if (config->ca_file != NULL && ca_file == NULL) {
    return TURBO_ENOMEM;
  }
  cert_file = tls_strdup_nullable(config->cert_file);
  if (config->cert_file != NULL && cert_file == NULL) {
    free(ca_file);
    return TURBO_ENOMEM;
  }
  key_file = tls_strdup_nullable(config->key_file);
  if (config->key_file != NULL && key_file == NULL) {
    free(cert_file);
    free(ca_file);
    return TURBO_ENOMEM;
  }
  key_password = tls_strdup_nullable(config->key_password);
  if (config->key_password != NULL && key_password == NULL) {
    free(key_file);
    free(cert_file);
    free(ca_file);
    return TURBO_ENOMEM;
  }
  cipher_list = tls_strdup_nullable(config->cipher_list);
  if (config->cipher_list != NULL && cipher_list == NULL) {
    free(key_password);
    free(key_file);
    free(cert_file);
    free(ca_file);
    return TURBO_ENOMEM;
  }

  tls_clear_client_config(st);
  st->ca_file = ca_file;
  st->cert_file = cert_file;
  st->key_file = key_file;
  st->key_password = key_password;
  st->cipher_list = cipher_list;
  st->client_configured = 1;
  st->client_verify_peer = config->verify_peer ? 1 : 0;
  return 0;
}

static int tls_password_cb(char *buf, int size, int rwflag, void *userdata) {
  const char *password = (const char *)userdata;
  size_t length;

  UNUSED(rwflag);
  if (buf == NULL || size <= 0 || password == NULL) {
    return 0;
  }

  length = strlen(password);
  if ((int)length > size - 1) {
    length = (size_t)(size - 1);
  }
  memcpy(buf, password, length);
  buf[length] = '\0';
  return (int)length;
}

int turbo_stream_tls_server_context_create_internal(
    const turbo_tls_server_config_t *config,
    turbo_tls_server_context_t **output) {
  turbo_tls_server_context_t *server_context = NULL;
  SSL_CTX *ctx = NULL;
  int rc;

  if (!output) {
    return TURBO_EINVAL;
  }
  *output = NULL;
  if (!config || config->size != sizeof(*config) ||
      !config->cert_file || config->cert_file[0] == '\0' ||
      !config->key_file || config->key_file[0] == '\0' ||
      (config->client_auth != TURBO_TLS_CLIENT_AUTH_NONE &&
       config->client_auth != TURBO_TLS_CLIENT_AUTH_REQUIRED) ||
      (config->client_auth == TURBO_TLS_CLIENT_AUTH_REQUIRED &&
       (!config->ca_file || config->ca_file[0] == '\0'))) {
    return TURBO_EINVAL;
  }

  turbo_once(&s_tls_cleanup_once, tls_register_global_cleanup);
  ctx = SSL_CTX_new(TLS_server_method());
  if (!ctx) {
    return TURBO_ENOMEM;
  }

  SSL_CTX_set_mode(ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE);
  SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);
  rc = tls_apply_protocol_mode_to_ctx(ctx);
  if (rc != 0) {
    SSL_CTX_free(ctx);
    return rc;
  }

  if (config->key_password && config->key_password[0] != '\0') {
    SSL_CTX_set_default_passwd_cb(ctx, tls_password_cb);
    SSL_CTX_set_default_passwd_cb_userdata(ctx, (void *)config->key_password);
  }
  if (SSL_CTX_use_certificate_chain_file(ctx, config->cert_file) != 1 ||
      SSL_CTX_use_PrivateKey_file(ctx, config->key_file, SSL_FILETYPE_PEM) != 1 ||
      SSL_CTX_check_private_key(ctx) != 1) {
    SSL_CTX_free(ctx);
    return TURBO_EIO;
  }
  SSL_CTX_set_default_passwd_cb(ctx, NULL);
  SSL_CTX_set_default_passwd_cb_userdata(ctx, NULL);

  if (SSL_CTX_set_cipher_list(
          ctx, (config->cipher_list && config->cipher_list[0] != '\0')
                   ? config->cipher_list
                   : "DEFAULT") != 1) {
    SSL_CTX_free(ctx);
    return TURBO_EIO;
  }
#ifdef TLS1_3_VERSION
  if (SSL_CTX_set_ciphersuites(
          ctx,
          "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256") != 1) {
    SSL_CTX_free(ctx);
    return TURBO_EIO;
  }
#endif

  if (config->client_auth == TURBO_TLS_CLIENT_AUTH_REQUIRED) {
    if (SSL_CTX_load_verify_locations(ctx, config->ca_file, NULL) != 1) {
      SSL_CTX_free(ctx);
      return TURBO_EIO;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
  } else {
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
  }

  server_context = (turbo_tls_server_context_t *)calloc(1, sizeof(*server_context));
  if (!server_context) {
    SSL_CTX_free(ctx);
    return TURBO_ENOMEM;
  }
  atomic_init(&server_context->ref_count, 1);
  server_context->ctx = ctx;
  server_context->client_auth = config->client_auth;
  *output = server_context;
  return 0;
}

void turbo_stream_tls_server_context_retain_internal(
    turbo_tls_server_context_t *context) {
  if (context) {
    atomic_fetch_add_explicit(&context->ref_count, 1, memory_order_relaxed);
  }
}

void turbo_stream_tls_server_context_release_internal(
    turbo_tls_server_context_t *context) {
  if (!context) {
    return;
  }
  if (atomic_fetch_sub_explicit(&context->ref_count, 1, memory_order_acq_rel) == 1) {
    SSL_CTX_free(context->ctx);
    context->ctx = NULL;
    free(context);
  }
}

static void tls_release_client_ctx(tls_state_t *st) {
  if (!st) {
    return;
  }

  if (st->ssl) {
    SSL_free(st->ssl);
    st->ssl = NULL;
    st->rbio = NULL;
    st->wbio = NULL;
  }

  if (st->client_ctx_owned && st->ctx) {
    SSL_CTX_free(st->ctx);
  }
  st->ctx = NULL;
  st->client_ctx_owned = 0;
}

static int tls_prepare_client_ssl(tls_state_t *st) {
  SSL_CTX *ctx = NULL;
  SSL *ssl = NULL;
  BIO *rbio = NULL;
  BIO *wbio = NULL;
  int owns_ctx = 0;
  int rc;

  if (!st) {
    return TURBO_EINVAL;
  }

  tls_release_client_ctx(st);
  if (st->server_mode) {
    return TURBO_EINVAL;
  }

  if (!st->client_configured) {
    ctx = get_default_tls_ctx();
    if (!ctx) {
      return TURBO_ENOMEM;
    }
  } else {
    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
      return TURBO_ENOMEM;
    }
    owns_ctx = 1;

    SSL_CTX_set_mode(ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE);
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_CLIENT);
    SSL_CTX_sess_set_new_cb(ctx, tls_on_new_client_session);
    SSL_CTX_set_verify(ctx, st->client_verify_peer ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);
    if (st->client_verify_peer) {
      SSL_CTX_set_default_verify_paths(ctx);
#ifdef _WIN32
      load_windows_cert_store(ctx, "ROOT");
      load_windows_cert_store(ctx, "CA");
#endif
      if (st->ca_file != NULL && st->ca_file[0] != '\0' &&
          SSL_CTX_load_verify_locations(ctx, st->ca_file, NULL) != 1) {
        SSL_CTX_free(ctx);
        return TURBO_EIO;
      }
    }
    rc = tls_apply_protocol_mode_to_ctx(ctx);
    if (rc != 0) {
      SSL_CTX_free(ctx);
      return rc;
    }
    if (st->cipher_list != NULL && st->cipher_list[0] != '\0' &&
        SSL_CTX_set_cipher_list(ctx, st->cipher_list) != 1) {
      SSL_CTX_free(ctx);
      return TURBO_EIO;
    }
    if (st->key_password != NULL && st->key_password[0] != '\0') {
      SSL_CTX_set_default_passwd_cb(ctx, tls_password_cb);
      SSL_CTX_set_default_passwd_cb_userdata(ctx, st->key_password);
    }
    if (st->cert_file != NULL && st->cert_file[0] != '\0' &&
        SSL_CTX_use_certificate_file(ctx, st->cert_file, SSL_FILETYPE_PEM) != 1) {
      SSL_CTX_free(ctx);
      return TURBO_EIO;
    }
    if (st->key_file != NULL && st->key_file[0] != '\0' &&
        SSL_CTX_use_PrivateKey_file(ctx, st->key_file, SSL_FILETYPE_PEM) != 1) {
      SSL_CTX_free(ctx);
      return TURBO_EIO;
    }
    if ((st->cert_file != NULL && st->cert_file[0] != '\0') ||
        (st->key_file != NULL && st->key_file[0] != '\0')) {
      if (SSL_CTX_check_private_key(ctx) != 1) {
        SSL_CTX_free(ctx);
        return TURBO_EIO;
      }
    }
  }

  ssl = SSL_new(ctx);
  if (!ssl) {
    if (owns_ctx) {
      SSL_CTX_free(ctx);
    }
    return TURBO_ENOMEM;
  }

  rbio = BIO_new(BIO_s_mem());
  wbio = BIO_new(BIO_s_mem());
  if (!rbio || !wbio) {
    if (rbio) {
      BIO_free(rbio);
    }
    if (wbio) {
      BIO_free(wbio);
    }
    SSL_free(ssl);
    if (owns_ctx) {
      SSL_CTX_free(ctx);
    }
    return TURBO_ENOMEM;
  }

  SSL_set_bio(ssl, rbio, wbio);
  st->ctx = ctx;
  st->client_ctx_owned = owns_ctx;
  st->ssl = ssl;
  st->rbio = rbio;
  st->wbio = wbio;
  tls_install_msg_callback(st);
  return 0;
}

/* ── Forward declarations ─────────────────────────────────── */

static void tls_on_tcp_connect(void *handle, int status, void *peer);
static void tls_on_tcp_close(void *handle);
static void tls_on_tcp_write_complete(turbo_stream_t *tcp, int status);
/* turbo_recv_cb signature: int(void*, const mem_slice_t*, void*) */
static int  tls_on_tcp_recv_cb(void *handle, const mem_slice_t *slice, void *peer);
static int  tls_deliver_or_queue_plaintext(tls_state_t *st, const char *data, size_t len);
static int  tls_flush_pending_plaintext(tls_state_t *st);
static int  tls_reserve_pending_plaintext(tls_state_t *st, size_t needed);
static mem_buffer_t *tls_alloc_plaintext_chunk(tls_state_t *st, size_t *payload_capacity);
static int  tls_deliver_or_queue_plaintext_chunk(tls_state_t *st, mem_buffer_t *chunk, size_t len);

static void tls_reset_flight_markers(tls_state_t *st) {
  if (!st) {
    return;
  }

  st->client_hello_write_ns = 0;
  st->client_server_hello_read_ns = 0;
  st->client_finished_write_ns = 0;
  st->server_client_hello_read_ns = 0;
  st->server_server_hello_write_ns = 0;
  st->server_client_finished_read_ns = 0;
}

static void tls_on_handshake_msg(int write_p, int version, int content_type,
                                 const void *buf, size_t len, SSL *ssl, void *arg) {
  const unsigned char *bytes = (const unsigned char *)buf;
  tls_state_t *st = (tls_state_t *)arg;
  uint64_t now;
  int hs_type;

  UNUSED(version);
  UNUSED(ssl);

  if (!st || content_type != SSL3_RT_HANDSHAKE || !bytes || len == 0) {
    return;
  }

  hs_type = (int)bytes[0];
  now = turbo_hrtime();

  if (!st->server_mode) {
    if (write_p) {
      if (hs_type == SSL3_MT_CLIENT_HELLO && st->client_hello_write_ns == 0) {
        st->client_hello_write_ns = now;
      } else if (hs_type == SSL3_MT_FINISHED &&
                 st->client_finished_write_ns == 0) {
        st->client_finished_write_ns = now;
      }
    } else if (hs_type == SSL3_MT_SERVER_HELLO &&
               st->client_server_hello_read_ns == 0) {
      st->client_server_hello_read_ns = now;
    }
    return;
  }

  if (!write_p) {
    if (hs_type == SSL3_MT_CLIENT_HELLO && st->server_client_hello_read_ns == 0) {
      st->server_client_hello_read_ns = now;
    } else if (hs_type == SSL3_MT_FINISHED &&
               st->server_client_finished_read_ns == 0) {
      st->server_client_finished_read_ns = now;
    }
    return;
  }

  if (hs_type == SSL3_MT_SERVER_HELLO && st->server_server_hello_write_ns == 0) {
    st->server_server_hello_write_ns = now;
  }
}

static void tls_install_msg_callback(tls_state_t *st) {
  if (!st || !st->ssl) {
    return;
  }

  SSL_set_app_data(st->ssl, st);
  SSL_set_msg_callback(st->ssl, tls_on_handshake_msg);
  SSL_set_msg_callback_arg(st->ssl, st);
}

static void tls_remove_msg_callback(tls_state_t *st) {
  if (!st || !st->ssl) {
    return;
  }

  SSL_set_msg_callback(st->ssl, NULL);
  SSL_set_msg_callback_arg(st->ssl, NULL);
}

static tls_state_t *tls_handshake_client_state_for_stream(turbo_stream_t *s) {
  tls_state_t *st;

  if (!s) {
    return NULL;
  }

  if (s->kind == TURBO_STREAM_TLS) {
    st = (tls_state_t *)s->backend_data;
    if (!st) {
      return NULL;
    }
    if (st->server_mode || st->client_handshake_started_ns == 0) {
      return NULL;
    }
    return st;
  }

  if (s->on_connect != tls_on_tcp_connect || s->on_close != tls_on_tcp_close) {
    return NULL;
  }

  st = (tls_state_t *)s->user_data;
  if (!st || st->tcp != s) {
    return NULL;
  }
  if (st->server_mode || st->state != TLS_ST_HANDSHAKING ||
      st->client_handshake_started_ns == 0) {
    return NULL;
  }
  return st;
}

void turbo_stream_tls_note_resume_wait(turbo_stream_t *s, uint64_t value_ns) {
  if (!s || value_ns == 0) {
    return;
  }
  tls_metric_add(&s_tls_metrics.client_handshake_resume_wait_ns, value_ns);
}

void turbo_stream_tls_note_wrap_client_time(uint64_t value_ns) {
  if (value_ns == 0) {
    return;
  }
  tls_metric_add(&s_tls_metrics.client_handshake_wrap_client_ns, value_ns);
}

void turbo_stream_tls_note_iocp_timing(turbo_stream_t *s, uint64_t iocp_post_ns,
                                       uint64_t post_drain_ns) {
  if (!tls_handshake_client_state_for_stream(s)) {
    return;
  }
  if (iocp_post_ns != 0) {
    tls_metric_add(&s_tls_metrics.client_handshake_iocp_post_ns, iocp_post_ns);
  }
  if (post_drain_ns != 0) {
    tls_metric_add(&s_tls_metrics.client_handshake_post_drain_ns, post_drain_ns);
  }
}

void turbo_stream_tls_note_waiter_signal(turbo_stream_t *s, uint64_t value_ns) {
  if (!s || value_ns == 0) {
    return;
  }
  tls_metric_add(&s_tls_metrics.client_handshake_waiter_signal_ns, value_ns);
}

static void tls_detach_tcp(tls_state_t *st) {
  turbo_stream_t *tcp;

  if (!st || !st->tcp) {
    return;
  }

  tcp = st->tcp;
  st->tcp = NULL;
  tcp->user_data = NULL;
  tcp->managed = 0;
  tcp->on_recv = NULL;
  tcp->on_connect = NULL;
  tcp->on_close = NULL;
  tcp->on_write_complete = NULL;
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

  tls_release_client_ctx(st);
  if (st->pending_plaintext) {
    mem_unref(st->pending_plaintext);
  }
  st->pending_plaintext = NULL;
  tls_clear_client_config(st);
  free(st);
}

static int tls_reserve_pending_plaintext(tls_state_t *st, size_t needed) {
  mem_buffer_t *new_buf;
  size_t current_cap;
  size_t new_cap;
  size_t used;

  if (!st || !st->outer || !st->outer->arena) {
    return TURBO_EINVAL;
  }
  if (needed == 0) {
    return 0;
  }

  current_cap = st->pending_plaintext ? st->pending_plaintext->capacity : 0;
  if (current_cap >= needed) {
    return 0;
  }

  new_cap = current_cap ? current_cap : MEM_HANDSHAKE_BUFFER_SIZE;
  while (new_cap < needed) {
    new_cap *= 2U;
  }

  new_buf = mem_get_buffer(st->outer->arena, new_cap);
  if (!new_buf) {
    return TURBO_ENOMEM;
  }

  used = st->pending_plaintext ? st->pending_plaintext->used : 0;
  if (used > 0) {
    memcpy(new_buf->data, st->pending_plaintext->data, used);
    mem_set_used(new_buf, used);
  }

  if (st->pending_plaintext) {
    mem_unref(st->pending_plaintext);
  }
  st->pending_plaintext = new_buf;
  return 0;
}

static int tls_deliver_or_queue_plaintext(tls_state_t *st, const char *data, size_t len) {
  size_t queued;

  if (!st || !data || len == 0) {
    return 0;
  }

  if (st->outer->on_recv) {
    mem_slice_t sl = { .data = (char *)data, .length = len, .buffer = NULL };
    return st->outer->on_recv(st->outer, &sl, NULL);
  }

  queued = st->pending_plaintext ? st->pending_plaintext->used : 0;
  if (tls_reserve_pending_plaintext(st, queued + len) != 0) {
    return TURBO_ENOMEM;
  }

  memcpy(st->pending_plaintext->data + queued, data, len);
  mem_set_used(st->pending_plaintext, queued + len);
  return 0;
}

static mem_buffer_t *tls_alloc_plaintext_chunk(tls_state_t *st, size_t *payload_capacity) {
  mem_buffer_t *chunk;
  coro_recv_header_t *hdr;
  size_t total_size;

  if (!st || !st->outer || !st->outer->arena) {
    return NULL;
  }

  total_size = sizeof(coro_recv_header_t) + TLS_PLAINTEXT_READ_CHUNK_SIZE;
  chunk = mem_get_buffer(st->outer->arena, total_size);
  if (!chunk) {
    return NULL;
  }

  hdr = (coro_recv_header_t *)chunk->data;
  hdr->magic = CORO_RECV_MAGIC_POOLED;
  hdr->size = 0;
  hdr->owner = chunk;
  mem_set_used(chunk, sizeof(coro_recv_header_t));

  if (payload_capacity) {
    *payload_capacity = TLS_PLAINTEXT_READ_CHUNK_SIZE;
  }
  return chunk; 
}

static int tls_deliver_or_queue_plaintext_chunk(tls_state_t *st, mem_buffer_t *chunk, size_t len) {
  coro_recv_header_t *hdr;
  mem_slice_t sl;

  if (!st || !chunk || len == 0) {
    return 0;
  }

  hdr = (coro_recv_header_t *)chunk->data;
  hdr->size = len;
  mem_set_used(chunk, sizeof(coro_recv_header_t) + len);

  if (st->outer->on_recv) {
    sl.data = chunk->data + sizeof(coro_recv_header_t);
    sl.length = len;
    sl.buffer = chunk;
    return st->outer->on_recv(st->outer, &sl, NULL);
  }

  return tls_deliver_or_queue_plaintext(st,
                                        chunk->data + sizeof(coro_recv_header_t),
                                        len);
}

static int tls_is_closed_or_deferred(const tls_state_t *st) {
  return !st || st->close_deferred || st->state == TLS_ST_CLOSED;
}

static void tls_finish_close(tls_state_t *st) {
  turbo_stream_t *outer;

  if (!st) {
    return;
  }

  if (st->tcp) {
    st->tcp->on_recv = NULL;
    st->tcp->on_connect = NULL;
    st->tcp->on_write_complete = NULL;
    turbo_stream_close(st->tcp);
    return;
  }

  outer = st->outer;
  if (outer) {
    outer->backend_data = NULL;
  }
  st->outer = NULL;
  tls_free_state(st);
  if (outer) {
    turbo_stream_finalize_close(outer);
  }
}

static void tls_pump_leave(tls_state_t *st) {
  turbo_stream_t *outer;

  if (!st) {
    return;
  }

  if (st->pumping > 0) {
    st->pumping--;
  }

  if (st->pumping == 0 && st->close_deferred) {
    outer = st->outer;
    st->outer = NULL;
    tls_free_state(st);
    if (outer) {
      turbo_stream_finalize_close(outer);
    }
    return;
  }

  if (st->pumping == 0 && st->close_requested) {
    st->close_requested = 0;
    tls_finish_close(st);
  }
}

static int tls_flush_pending_plaintext(tls_state_t *st) {
  int close_req;
  mem_slice_t sl;

  if (!st || !st->outer->on_recv || !st->pending_plaintext ||
      st->pending_plaintext->used == 0) {
    return 0;
  }

  sl.data = st->pending_plaintext->data;
  sl.length = st->pending_plaintext->used;
  sl.buffer = NULL;

  close_req = st->outer->on_recv(st->outer, &sl, NULL);
  mem_set_used(st->pending_plaintext, 0);

  if (close_req) {
    turbo_stream_close(st->outer);
    return TURBO_ECONNABORTED;
  }

  return 0;
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
static int tls_write_plaintext(tls_state_t *st, const char *data, size_t len,
                               int flush_network);

static void tls_apply_cached_client_session(tls_state_t *st) {
  int rc;

  if (!st || st->server_mode || !st->ssl) {
    return;
  }

  tls_global_lock();
  if (!s_cached_client_session || st->hostname[0] == '\0') {
    tls_global_unlock();
    return;
  }
  if (strcmp(st->hostname, s_cached_client_session_host) != 0) {
    tls_global_unlock();
    return;
  }
  tls_metric_inc(&s_tls_metrics.client_session_cache_attempts);
  rc = SSL_set_session(st->ssl, s_cached_client_session);
  tls_global_unlock();
  if (rc != 1) {
    ERR_clear_error();
  }
}

static void tls_cache_client_session(tls_state_t *st) {
  SSL_SESSION *session;

  if (!st || st->server_mode || !st->ssl || st->hostname[0] == '\0') {
    return;
  }

  session = SSL_get1_session(st->ssl);
  if (!session) {
    return;
  }

  tls_store_client_session_for_host(st->hostname, session);
}

static void tls_mark_client_handshake_start(tls_state_t *st) {
  if (!st || st->server_mode) {
    return;
  }

  tls_reset_flight_markers(st);
  st->client_handshake_started_ns = turbo_hrtime();
}

static void tls_mark_server_handshake_start(tls_state_t *st) {
  if (!st || !st->server_mode) {
    return;
  }

  tls_reset_flight_markers(st);
  st->server_handshake_started_ns = turbo_hrtime();
}

static int tls_on_new_client_session(SSL *ssl, SSL_SESSION *session) {
  tls_state_t *st = (tls_state_t *)SSL_get_app_data(ssl);

  if (!st || st->server_mode || !session || st->hostname[0] == '\0') {
    return 1;
  }

  /* OpenSSL supplies the callback with one reference owned by the application. */
  tls_store_client_session_for_host(st->hostname, session);
  return 1;
}

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
                                 const char *hostname, int server_mode,
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

  st->server_mode = server_mode ? 1 : 0;

  if (server_mode) {
    SSL_set_accept_state(st->ssl);
  } else {
    int rc = tls_configure_hostname(st);
    if (rc != 0) {
      return rc;
    }
    tls_apply_cached_client_session(st);
    SSL_set_connect_state(st->ssl);
    tls_metric_inc(&s_tls_metrics.client_handshakes_started);
  }

  st->tcp = tcp;
  st->tcp->user_data = st;
  st->tcp->managed = 1;
  st->tcp->on_connect = NULL;
  st->tcp->on_close = tls_on_tcp_close;
  st->tcp->on_write_complete = tls_on_tcp_write_complete;
  turbo_stream_recv_stop(st->tcp);

  st->state = TLS_ST_HANDSHAKING;
  if (!st->server_mode) {
    tls_mark_client_handshake_start(st);
  } else {
    tls_mark_server_handshake_start(st);
  }
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
  SSL_CTX *ctx;

  turbo_once(&s_tls_cleanup_once, tls_register_global_cleanup);

  tls_global_lock();
  if (s_default_ctx) {
    if (tls_apply_protocol_mode_to_ctx(s_default_ctx) != 0) {
      tls_global_unlock();
      return NULL;
    }
    configure_ca_from_env();
    ctx = s_default_ctx;
    tls_global_unlock();
    return ctx;
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
    SSL_CTX_set_session_cache_mode(s_default_ctx, SSL_SESS_CACHE_CLIENT);
    SSL_CTX_sess_set_new_cb(s_default_ctx, tls_on_new_client_session);
    SSL_CTX_set_verify(s_default_ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_default_verify_paths(s_default_ctx);
    if (tls_apply_protocol_mode_to_ctx(s_default_ctx) != 0) {
      SSL_CTX_free(s_default_ctx);
      s_default_ctx = NULL;
      tls_global_unlock();
      return NULL;
    }
    configure_ca_from_env();
#ifdef _WIN32
    configure_ca_from_windows_store();
#endif
  }
  ctx = s_default_ctx;
  tls_global_unlock();
  return ctx;
}

static SSL_CTX *get_default_tls_server_ctx(void) {
  SSL_CTX *ctx;
  int rc = 0;

  turbo_once(&s_tls_cleanup_once, tls_register_global_cleanup);

  tls_server_ctx_lock();

  if (s_server_ctx) {
    if (!s_server_configured) {
      rc = configure_server_ctx_from_env_unlocked();
    }
    ctx = (rc == 0) ? s_server_ctx : NULL;
    tls_server_ctx_unlock();
    return ctx;
  }
#if OPENSSL_VERSION_NUMBER < 0x10100000L
  SSL_library_init();
  SSL_load_error_strings();
#endif
  s_server_ctx = SSL_CTX_new(TLS_server_method());
  if (s_server_ctx) {
    SSL_CTX_set_mode(s_server_ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_CTX_set_mode(s_server_ctx, SSL_MODE_ENABLE_PARTIAL_WRITE);
    SSL_CTX_set_session_cache_mode(s_server_ctx, SSL_SESS_CACHE_SERVER);
    rc = tls_apply_protocol_mode_to_ctx(s_server_ctx);
    if (rc == 0) {
      rc = configure_server_ctx_from_env_unlocked();
    }
    if (rc != 0) {
      SSL_CTX_free(s_server_ctx);
      s_server_ctx = NULL;
      s_server_configured = 0;
    }
  }

  ctx = s_server_ctx;
  tls_server_ctx_unlock();
  return ctx;
}

/* ── Pump Logic (where the magic happens) ─────────────────── */

/**
 * @brief Extract encrypted bytes from wbio and send them over tcp.
 */
static void tls_flush_wbio_to_network(tls_state_t *st) {
  if (!st->wbio || !st->tcp) return;

  char buf[MEM_SEND_BUFFER_SIZE];
  int pending;
  while ((pending = BIO_pending(st->wbio)) > 0) {
    size_t chunk_size = ((size_t)pending > sizeof(buf)) ? sizeof(buf) : (size_t)pending;
    mem_buffer_t *send_buf = turbo_stream_get_send_buffer(st->tcp, chunk_size);
    if (send_buf) {
      int n = BIO_read(st->wbio, send_buf->data, (int)chunk_size);
      if (n > 0) {
        mem_set_used(send_buf, (size_t)n);
        (void)turbo_stream_send_buffer(st->tcp, send_buf, (size_t)n);
        mem_unref(send_buf);
        continue;
      }
      mem_unref(send_buf);
      break;
    }

    {
      int n = BIO_read(st->wbio, buf, (int)chunk_size);
      if (n > 0) {
        (void)turbo_stream_send(st->tcp, buf, (size_t)n);
      } else {
        break;
      }
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
  if (!st || !st->ssl || st->state == TLS_ST_INIT || st->state == TLS_ST_CONNECTING_TCP ||
      st->state == TLS_ST_CLOSED) {
    return;
  }

  st->pumping++;

  /* 1. Drive Handshake */
  if (st->state == TLS_ST_HANDSHAKING) {
    uint64_t pump_start_ns = 0;
    uint64_t crypto_start_ns = 0;
    uint64_t flush_start_ns = 0;
    int r;
    if (!st->server_mode) {
      pump_start_ns = turbo_hrtime();
      tls_metric_inc(&s_tls_metrics.client_handshake_pumps);
      crypto_start_ns = turbo_hrtime();
      r = SSL_connect(st->ssl);
      tls_metric_add(&s_tls_metrics.client_handshake_crypto_ns,
                     turbo_hrtime() - crypto_start_ns);
      flush_start_ns = turbo_hrtime();
      tls_flush_wbio_to_network(st);
      tls_metric_add(&s_tls_metrics.client_handshake_flush_ns,
                     turbo_hrtime() - flush_start_ns);
    } else {
      pump_start_ns = turbo_hrtime();
      tls_metric_inc(&s_tls_metrics.server_handshake_pumps);
      crypto_start_ns = turbo_hrtime();
      r = SSL_accept(st->ssl);
      tls_metric_add(&s_tls_metrics.server_handshake_crypto_ns,
                     turbo_hrtime() - crypto_start_ns);
      flush_start_ns = turbo_hrtime();
      tls_flush_wbio_to_network(st);
      tls_metric_add(&s_tls_metrics.server_handshake_flush_ns,
                     turbo_hrtime() - flush_start_ns);
    }

    if (r == 1) {
      /* Handshake complete! */
      st->state = TLS_ST_OPEN;
      st->outer->connected = 1;
      if (!st->server_mode) {
        tls_metric_inc(&s_tls_metrics.client_handshakes_completed);
        if (st->client_handshake_started_ns != 0) {
          tls_metric_add(&s_tls_metrics.client_handshake_total_ns,
                         turbo_hrtime() - st->client_handshake_started_ns);
          st->client_handshake_started_ns = 0;
        }
        if (st->client_hello_write_ns != 0 && st->client_server_hello_read_ns != 0 &&
            st->client_server_hello_read_ns >= st->client_hello_write_ns) {
          tls_metric_add(&s_tls_metrics.client_handshake_clienthello_to_serverhello_ns,
                         st->client_server_hello_read_ns - st->client_hello_write_ns);
          if (st->client_finished_write_ns != 0 &&
              st->client_finished_write_ns >= st->client_server_hello_read_ns) {
            tls_metric_add(
                &s_tls_metrics.client_handshake_serverhello_to_finished_write_ns,
                st->client_finished_write_ns - st->client_server_hello_read_ns);
            tls_metric_add(&s_tls_metrics.client_handshake_finished_write_to_done_ns,
                           turbo_hrtime() - st->client_finished_write_ns);
          }
          tls_metric_add(&s_tls_metrics.client_handshake_serverhello_to_done_ns,
                         turbo_hrtime() - st->client_server_hello_read_ns);
        }
        if (SSL_session_reused(st->ssl) == 1) {
          tls_metric_inc(&s_tls_metrics.client_session_reused);
        }
#ifdef TLS1_3_VERSION
        if (SSL_version(st->ssl) == TLS1_3_VERSION) {
          tls_metric_inc(&s_tls_metrics.client_handshakes_tls13);
        }
#endif
      } else {
        tls_metric_inc(&s_tls_metrics.server_handshakes_completed);
        if (st->server_handshake_started_ns != 0) {
          tls_metric_add(&s_tls_metrics.server_handshake_total_ns,
                         turbo_hrtime() - st->server_handshake_started_ns);
          st->server_handshake_started_ns = 0;
        }
        if (st->server_client_hello_read_ns != 0 &&
            st->server_server_hello_write_ns != 0 &&
            st->server_server_hello_write_ns >= st->server_client_hello_read_ns) {
          tls_metric_add(&s_tls_metrics.server_handshake_clienthello_to_serverhello_ns,
                         st->server_server_hello_write_ns - st->server_client_hello_read_ns);
        }
        if (st->server_client_finished_read_ns != 0 &&
            turbo_hrtime() >= st->server_client_finished_read_ns) {
          tls_metric_add(&s_tls_metrics.server_handshake_clientfinished_to_done_ns,
                         turbo_hrtime() - st->server_client_finished_read_ns);
        }
      }
      tls_remove_msg_callback(st);
      tls_cache_client_session(st);
      if (st->outer->on_connect) {
        st->outer->on_connect(st->outer, 0, NULL);
      }
      if (tls_is_closed_or_deferred(st)) {
        if (pump_start_ns != 0) {
          if (!st->server_mode) {
            tls_metric_add(&s_tls_metrics.client_handshake_pump_total_ns,
                           turbo_hrtime() - pump_start_ns);
          } else {
            tls_metric_add(&s_tls_metrics.server_handshake_pump_total_ns,
                           turbo_hrtime() - pump_start_ns);
          }
        }
        tls_pump_leave(st);
        return;
      }
      if (pump_start_ns != 0) {
        if (!st->server_mode) {
          tls_metric_add(&s_tls_metrics.client_handshake_pump_total_ns,
                         turbo_hrtime() - pump_start_ns);
        } else {
          tls_metric_add(&s_tls_metrics.server_handshake_pump_total_ns,
                         turbo_hrtime() - pump_start_ns);
        }
      }
      /* Fall through to process any early application data */
    } else {
      int err = SSL_get_error(st->ssl, r);
      if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        if (pump_start_ns != 0) {
          if (!st->server_mode) {
            tls_metric_add(&s_tls_metrics.client_handshake_pump_total_ns,
                           turbo_hrtime() - pump_start_ns);
          } else {
            tls_metric_add(&s_tls_metrics.server_handshake_pump_total_ns,
                           turbo_hrtime() - pump_start_ns);
          }
        }
        tls_pump_leave(st);
        return; /* Wait for more network I/O */
      } else {
        /* Handshake protocol error */
        st->client_handshake_started_ns = 0;
        st->server_handshake_started_ns = 0;
        tls_remove_msg_callback(st);
        tls_log_handshake_failure(st, r);
        st->state = TLS_ST_CLOSING;
        if (st->outer->on_connect) {
          st->outer->on_connect(st->outer, TURBO_ECONNABORTED, NULL);
        }
        turbo_stream_close(st->outer);
        if (pump_start_ns != 0) {
          if (!st->server_mode) {
            tls_metric_add(&s_tls_metrics.client_handshake_pump_total_ns,
                           turbo_hrtime() - pump_start_ns);
          } else {
            tls_metric_add(&s_tls_metrics.server_handshake_pump_total_ns,
                           turbo_hrtime() - pump_start_ns);
          }
        }
        tls_pump_leave(st);
        return;
      }
    }
  }

  /* 2. Drive Open (Read Decrypted Data) */
  if (st->state == TLS_ST_OPEN) {
    int n;
    if (tls_flush_pending_plaintext(st) != 0) {
      tls_pump_leave(st);
      return;
    }
    if (tls_is_closed_or_deferred(st)) {
      tls_pump_leave(st);
      return;
    }
    for (;;) {
      int close_req;
      mem_buffer_t *chunk;
      size_t payload_capacity = 0;

      chunk = tls_alloc_plaintext_chunk(st, &payload_capacity);
      if (!chunk) {
        turbo_stream_close(st->outer);
        tls_pump_leave(st);
        return;
      }

      n = SSL_read(st->ssl,
                   chunk->data + sizeof(coro_recv_header_t),
                   (int)payload_capacity);
      if (n <= 0) {
        mem_unref(chunk);
        break;
      }

      close_req = tls_deliver_or_queue_plaintext_chunk(st, chunk, (size_t)n);
      mem_unref(chunk);
      if (tls_is_closed_or_deferred(st)) {
        tls_pump_leave(st);
        return;
      }
      if (close_req == TURBO_ENOMEM) {
        turbo_stream_close(st->outer);
        tls_pump_leave(st);
        return;
      }
      if (close_req != 0) {
        turbo_stream_close(st->outer);
        tls_pump_leave(st);
        return;
      }
    }

    if (tls_is_closed_or_deferred(st)) {
      tls_pump_leave(st);
      return;
    }

    int err = SSL_get_error(st->ssl, n);
    if (err == SSL_ERROR_ZERO_RETURN) {
      /* Clean shutdown from peer (close_notify) */
      turbo_stream_close(st->outer);
      tls_pump_leave(st);
      return;
    } else if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
      /* Protocol error / abrupt disconnect */
      turbo_stream_close(st->outer);
      tls_pump_leave(st);
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

  tls_pump_leave(st);
}

/* ── TCP Callbacks ────────────────────────────────────────── */

static int tls_on_tcp_recv_cb(void *handle, const mem_slice_t *slice, void *peer) {
  (void)peer;
  turbo_stream_t *tcp = (turbo_stream_t *)handle;
  tls_state_t    *st;
  uint64_t cb_start_ns = 0;
  uint64_t bio_start_ns = 0;
  int server_mode = 0;
  if (!tcp) return 0;
  st = (tls_state_t *)tcp->user_data;
  if (!st || !st->outer || !st->rbio) return 0;
  server_mode = st->server_mode;
  if (!slice || !slice->data || slice->length == 0) {
    turbo_stream_close(st->outer);
    return 0;
  }

  /* Push encrypted network bytes into rbio */
  if (st->state == TLS_ST_HANDSHAKING) {
    cb_start_ns = turbo_hrtime();
    if (!st->server_mode) {
      bio_start_ns = turbo_hrtime();
    }
  }
  {
    int written = BIO_write(st->rbio, slice->data, (int)slice->length);
    if (written <= 0) {
      /* Shouldn't happen with memory BIOs unless OOM */
      return -1;
    }
  }
  if (bio_start_ns != 0) {
    tls_metric_add(&s_tls_metrics.client_handshake_bio_write_ns,
                   turbo_hrtime() - bio_start_ns);
    tls_metric_inc(&s_tls_metrics.client_handshake_bio_write_calls);
    tls_metric_add(&s_tls_metrics.client_handshake_bio_write_bytes, (uint64_t)slice->length);
  }

  /* Drive TLS state machine to consume the data */
  tls_pump(st);
  if (cb_start_ns != 0) {
    if (!server_mode) {
      tls_metric_add(&s_tls_metrics.client_handshake_recv_cb_ns,
                     turbo_hrtime() - cb_start_ns);
    } else {
      tls_metric_add(&s_tls_metrics.server_handshake_recv_cb_ns,
                     turbo_hrtime() - cb_start_ns);
    }
  }
  return 0;
}

static void tls_on_tcp_connect(void *handle, int status, void *peer) {
  (void)peer;
  turbo_stream_t *tcp = (turbo_stream_t *)handle;
  tls_state_t    *st;
  uint64_t cb_start_ns = 0;
  if (!tcp) return;
  st = (tls_state_t *)tcp->user_data;
  if (!st || !st->outer) return;

  if (status != 0) {
    st->state = TLS_ST_CLOSED;
    if (st->outer->on_connect)
      st->outer->on_connect(st->outer, status, NULL);
    return;
  }

  st->state = TLS_ST_HANDSHAKING;
  tls_mark_client_handshake_start(st);
  if (!st->server_mode) {
    cb_start_ns = turbo_hrtime();
  }
  
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
  if (cb_start_ns != 0) {
    tls_metric_add(&s_tls_metrics.client_handshake_connect_cb_ns,
                   turbo_hrtime() - cb_start_ns);
  }
}

static void tls_on_tcp_close(void *handle) {
  turbo_stream_t *tcp = (turbo_stream_t *)handle;
  tls_state_t    *st;
  turbo_stream_t *outer;
  if (!tcp) return;
  st = (tls_state_t *)tcp->user_data;
  tcp->managed = 0;
  tcp->destroyed = 1;
  tcp->on_recv = NULL;
  tcp->on_connect = NULL;
  tcp->on_write_complete = NULL;
  if (!st) return;

  outer = st->outer;
  if (outer) outer->backend_data = NULL;
  tcp->user_data = NULL;
  st->tcp = NULL;
  st->state = TLS_ST_CLOSED;

  if (st->pumping > 0) {
    st->close_deferred = 1;
    return;
  }

  st->outer = NULL;
  tls_free_state(st);

  if (outer) {
    turbo_stream_finalize_close(outer);
  }
}

static void tls_on_tcp_write_complete(turbo_stream_t *tcp, int status) {
  tls_state_t *st;

  if (!tcp) {
    return;
  }
  st = (tls_state_t *)tcp->user_data;
  if (st && st->outer && st->outer->on_write_complete) {
    st->outer->on_write_complete(st->outer, status);
  }
}

/* ── Backend vtable implementation ───────────────────────── */

static int tls_init(turbo_stream_t *s) {
  tls_state_t *st = NULL;
  int rc = 0;

  st = (tls_state_t *)calloc(1, sizeof(tls_state_t));
  if (!st) return TURBO_ENOMEM;

  st->outer = s;
  st->state = TLS_ST_INIT;
  st->client_verify_peer = 1;
  rc = tls_prepare_client_ssl(st);
  if (rc != 0) {
    goto fail;
  }

  s->backend_data = st;
  return 0;

fail:
  tls_free_state(st);
  return rc;
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
  st->tcp->on_write_complete = tls_on_tcp_write_complete;
  if (s->send_hwm_bytes) {
    int rc = turbo_stream_set_send_hwm(st->tcp, s->send_hwm_bytes);
    if (rc != 0) {
      tls_drop_inner_tcp(st);
      return rc;
    }
  }
  if (s->tcp_keepalive_configured) {
    int rc = turbo_stream_set_tcp_keepalive(st->tcp, &s->tcp_keepalive_config);
    if (rc != 0) {
      tls_drop_inner_tcp(st);
      return rc;
    }
  }
  if (s->linger_configured) {
    int rc = turbo_stream_set_linger(st->tcp, &s->linger_config);
    if (rc != 0) {
      tls_drop_inner_tcp(st);
      return rc;
    }
  }

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
  tls_apply_cached_client_session(st);
  tls_metric_inc(&s_tls_metrics.client_handshakes_started);

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
  return tls_attach_tcp_stream(tls_stream, tcp_stream, hostname, 0,
                               on_connect, on_close);
}

int turbo_stream_tls_wrap_server(turbo_stream_t *tls_stream,
                                 turbo_stream_t *tcp_stream,
                                 turbo_connect_cb on_connect,
                                 turbo_close_cb on_close) {
  return turbo_stream_tls_wrap_server_with_context(
      tls_stream, tcp_stream, NULL, on_connect, on_close);
}

int turbo_stream_tls_wrap_server_with_context(
    turbo_stream_t *tls_stream, turbo_stream_t *tcp_stream,
    turbo_tls_server_context_t *server_context,
    turbo_connect_cb on_connect, turbo_close_cb on_close) {
  tls_state_t *st;
  int rc;

  if (!tls_stream || !tcp_stream) {
    return TURBO_EINVAL;
  }

  st = (tls_state_t *)tls_stream->backend_data;
  if (!st) {
    return TURBO_EINVAL;
  }

  tls_release_client_ctx(st);
  st->server_client_auth = TURBO_TLS_CLIENT_AUTH_NONE;
  if (server_context) {
    if (!server_context->ctx || SSL_CTX_up_ref(server_context->ctx) != 1) {
      return TURBO_EIO;
    }
    st->ctx = server_context->ctx;
    st->client_ctx_owned = 1;
    st->server_client_auth = server_context->client_auth;
  } else {
    st->ctx = get_default_tls_server_ctx();
    if (!st->ctx) {
      return TURBO_ENOMEM;
    }
    st->client_ctx_owned = 0;
  }

  st->ssl = SSL_new(st->ctx);
  if (!st->ssl) {
    return TURBO_ENOMEM;
  }
  tls_install_msg_callback(st);

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
    rc = TURBO_ENOMEM;
    goto fail;
  }

  SSL_set_bio(st->ssl, st->rbio, st->wbio);
  return tls_attach_tcp_stream(tls_stream, tcp_stream, NULL, 1, on_connect, on_close);

fail:
  tls_free_state(st);
  tls_stream->backend_data = NULL;
  return rc;
}

int turbo_stream_tls_set_client_config_internal(turbo_stream_t *s,
                                                const turbo_tls_client_config_t *config) {
  tls_state_t *st;
  int rc;

  if (!s || s->kind != TURBO_STREAM_TLS) {
    return TURBO_EINVAL;
  }

  st = (tls_state_t *)s->backend_data;
  if (!st) {
    return TURBO_EINVAL;
  }
  if (st->server_mode || st->tcp != NULL || st->state != TLS_ST_INIT) {
    return TURBO_EBUSY;
  }

  rc = tls_copy_client_config(st, config);
  if (rc != 0) {
    return rc;
  }
  return tls_prepare_client_ssl(st);
}

int turbo_stream_tls_export_channel_binding_internal(const turbo_stream_t *stream,
                                                      uint8_t *output,
                                                      size_t output_len) {
  static const char exporter_label[] = "EXPORTER-Channel-Binding";
  static const size_t channel_binding_size = 32U;
  tls_state_t *st;

  if (!output || output_len != channel_binding_size) {
    return TURBO_EINVAL;
  }
  memset(output, 0, output_len);

  if (!stream || stream->kind != TURBO_STREAM_TLS) {
    return TURBO_EINVAL;
  }

  st = (tls_state_t *)stream->backend_data;
  if (!st || !st->ssl || st->state != TLS_ST_OPEN) {
    return TURBO_ENOTCONN;
  }
  if (SSL_version(st->ssl) != TLS1_3_VERSION) {
    return TURBO_EPROTONOSUPPORT;
  }
  if (!st->server_mode &&
      (!st->client_verify_peer || SSL_get_verify_result(st->ssl) != X509_V_OK)) {
    return TURBO_EPERM;
  }

  if (SSL_export_keying_material(st->ssl, output, output_len,
                                 exporter_label, sizeof(exporter_label) - 1U,
                                 NULL, 0U, 1) != 1) {
    memset(output, 0, output_len);
    return TURBO_EIO;
  }

  return 0;
}

int turbo_stream_tls_get_verified_peer_certificate_sha256_internal(
    const turbo_stream_t *stream, char *output, size_t output_len) {
  static const char hex[] = "0123456789abcdef";
  tls_state_t *st;
  X509 *peer;
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  size_t i;

  if (!output || output_len != CORO_TLS_PEER_CERT_SHA256_CAPACITY) {
    return TURBO_EINVAL;
  }
  memset(output, 0, output_len);
  if (!stream || stream->kind != TURBO_STREAM_TLS) {
    return TURBO_EINVAL;
  }

  st = (tls_state_t *)stream->backend_data;
  if (!st || !st->ssl || st->state != TLS_ST_OPEN) {
    return TURBO_ENOTCONN;
  }
  if ((st->server_mode &&
       st->server_client_auth != TURBO_TLS_CLIENT_AUTH_REQUIRED) ||
      (!st->server_mode && !st->client_verify_peer) ||
      SSL_get_verify_result(st->ssl) != X509_V_OK) {
    return TURBO_EPERM;
  }

  peer = SSL_get1_peer_certificate(st->ssl);
  if (!peer) {
    return TURBO_ENOENT;
  }
  if (X509_digest(peer, EVP_sha256(), digest, &digest_len) != 1 ||
      digest_len != 32U) {
    X509_free(peer);
    return TURBO_EIO;
  }
  X509_free(peer);

  memcpy(output, "sha256:", 7U);
  for (i = 0; i < digest_len; ++i) {
    output[7U + (i * 2U)] = hex[(digest[i] >> 4) & 0x0fU];
    output[8U + (i * 2U)] = hex[digest[i] & 0x0fU];
  }
  output[CORO_TLS_PEER_CERT_SHA256_CAPACITY - 1U] = '\0';
  return 0;
}

static int tls_connect_pipe(turbo_stream_t *s, const char *name) {
  UNUSED(s); UNUSED(name);
  return TURBO_EINVAL; /* TLS over pipe is theoretically possible, but API expects INET */
}

static int tls_send(turbo_stream_t *s, const char *data, size_t len) {
  tls_state_t *st = (tls_state_t *)s->backend_data;
  if (!st || st->state != TLS_ST_OPEN) return TURBO_ENOTCONN;
  return tls_write_plaintext(st, data, len, 1);
}

static int tls_write_plaintext(tls_state_t *st, const char *data, size_t len,
                               int flush_network) {
  size_t offset = 0;

  if (!st || !data) {
    return TURBO_EINVAL;
  }
  if (len == 0) {
    return 0;
  }

  while (offset < len) {
    size_t remaining = len - offset;
    int chunk = (remaining > (size_t)INT_MAX) ? INT_MAX : (int)remaining;
    int n = SSL_write(st->ssl, data + offset, chunk);

    if (n > 0) {
      offset += (size_t)n;
      if (flush_network) {
        tls_flush_wbio_to_network(st);
      }
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
  mem_buffer_t *buf;
  int rc;

  if (!st || !st->tcp) return 0;

  /* Drain queued plaintext through tls_send() so partial writes and
   * WANT_READ/WANT_WRITE are handled consistently. */
  while ((buf = s->send_head) != NULL) {
    rc = tls_write_plaintext(st, buf->data, buf->used, 0);
    if (rc != 0) {
      return rc;
    }

    s->send_head = buf->next;
    if (!s->send_head) s->send_tail = NULL;
    s->send_queued -= buf->used;
    buf->next = NULL;
    mem_unref(buf);
  }

  tls_flush_wbio_to_network(st);
  return turbo_stream_flush(st->tcp);
}

static int tls_recv_start(turbo_stream_t *s) {
  /* on_recv is cached in s->on_recv; underlying TCP read was started 
     during handshake. Nothing more to do. */
  tls_state_t *st = (tls_state_t *)s->backend_data;
  if (st) {
    return tls_flush_pending_plaintext(st);
  }
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
    if (st->pumping == 0) {
      tls_pump(st); /* Triggers SSL_shutdown and flushing */
    }
  }

  if (st->pumping > 0) {
    st->close_requested = 1;
    return;
  }

  tls_finish_close(st);
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

CXX_C_API int turbo_stream_tls_set_protocol_mode(turbo_tls_protocol_mode_t mode) {
  int rc;

  if (mode != TURBO_TLS_PROTOCOL_DEFAULT && mode != TURBO_TLS_PROTOCOL_TLS13_ONLY) {
    return TURBO_EINVAL;
  }

  tls_global_lock();
  atomic_store_explicit(&s_tls_protocol_mode, mode, memory_order_release);
  tls_reset_client_session_cache_internal();

  if (s_default_ctx) {
    rc = tls_apply_protocol_mode_to_ctx(s_default_ctx);
    if (rc != 0) {
      tls_global_unlock();
      return rc;
    }
  }
  tls_global_unlock();

  tls_server_ctx_lock();
  if (s_server_ctx) {
    rc = tls_apply_protocol_mode_to_ctx(s_server_ctx);
    if (rc != 0) {
      tls_server_ctx_unlock();
      return rc;
    }
  }
  tls_server_ctx_unlock();

  return 0;
}

CXX_C_API turbo_tls_protocol_mode_t turbo_stream_tls_get_protocol_mode(void) {
  return (turbo_tls_protocol_mode_t)atomic_load_explicit(&s_tls_protocol_mode,
                                                        memory_order_acquire);
}

CXX_C_API void turbo_stream_tls_reset_client_session_cache(void) {
  tls_global_lock();
  tls_reset_client_session_cache_internal();
  tls_global_unlock();
}

CXX_C_API void turbo_stream_tls_thread_cleanup(void) {
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
  OPENSSL_thread_stop();
#endif
}

CXX_C_API void turbo_stream_tls_global_cleanup(void) {
  tls_global_cleanup();
}

CXX_C_API void turbo_stream_tls_get_metrics(turbo_tls_metrics_t *metrics) {
  if (!metrics) {
    return;
  }

  metrics->client_handshakes_started =
      (uint64_t)tls_metric_load(&s_tls_metrics.client_handshakes_started);
  metrics->client_handshakes_completed =
      (uint64_t)tls_metric_load(&s_tls_metrics.client_handshakes_completed);
  metrics->client_session_cache_attempts =
      (uint64_t)tls_metric_load(&s_tls_metrics.client_session_cache_attempts);
  metrics->client_session_reused =
      (uint64_t)tls_metric_load(&s_tls_metrics.client_session_reused);
  metrics->client_session_stores =
      (uint64_t)tls_metric_load(&s_tls_metrics.client_session_stores);
  metrics->client_handshake_total_ns =
      (uint64_t)tls_metric_load(&s_tls_metrics.client_handshake_total_ns);
  metrics->client_handshake_bio_write_ns =
      (uint64_t)tls_metric_load(&s_tls_metrics.client_handshake_bio_write_ns);
  metrics->client_handshake_bio_write_calls =
      (uint64_t)tls_metric_load(&s_tls_metrics.client_handshake_bio_write_calls);
  metrics->client_handshake_bio_write_bytes =
      (uint64_t)tls_metric_load(&s_tls_metrics.client_handshake_bio_write_bytes);
  metrics->client_handshake_crypto_ns =
      (uint64_t)tls_metric_load(&s_tls_metrics.client_handshake_crypto_ns);
  metrics->client_handshake_flush_ns =
      (uint64_t)tls_metric_load(&s_tls_metrics.client_handshake_flush_ns);
  metrics->client_handshake_pump_total_ns =
      (uint64_t)tls_metric_load(&s_tls_metrics.client_handshake_pump_total_ns);
  metrics->client_handshake_recv_cb_ns =
      (uint64_t)tls_metric_load(&s_tls_metrics.client_handshake_recv_cb_ns);
  metrics->client_handshake_connect_cb_ns =
      (uint64_t)tls_metric_load(&s_tls_metrics.client_handshake_connect_cb_ns);
  metrics->client_handshake_iocp_post_ns =
      (uint64_t)tls_metric_load(&s_tls_metrics.client_handshake_iocp_post_ns);
  metrics->client_handshake_post_drain_ns =
      (uint64_t)tls_metric_load(&s_tls_metrics.client_handshake_post_drain_ns);
  metrics->client_handshake_waiter_signal_ns =
      (uint64_t)tls_metric_load(&s_tls_metrics.client_handshake_waiter_signal_ns);
  metrics->client_handshake_resume_wait_ns =
      (uint64_t)tls_metric_load(&s_tls_metrics.client_handshake_resume_wait_ns);
  metrics->client_handshake_wrap_client_ns =
      (uint64_t)tls_metric_load(&s_tls_metrics.client_handshake_wrap_client_ns);
  metrics->client_handshake_clienthello_to_serverhello_ns =
      (uint64_t)tls_metric_load(&s_tls_metrics.client_handshake_clienthello_to_serverhello_ns);
  metrics->client_handshake_serverhello_to_finished_write_ns =
      (uint64_t)tls_metric_load(&s_tls_metrics.client_handshake_serverhello_to_finished_write_ns);
  metrics->client_handshake_finished_write_to_done_ns =
      (uint64_t)tls_metric_load(&s_tls_metrics.client_handshake_finished_write_to_done_ns);
  metrics->client_handshake_serverhello_to_done_ns =
      (uint64_t)tls_metric_load(&s_tls_metrics.client_handshake_serverhello_to_done_ns);
  metrics->client_handshake_pumps =
      (uint64_t)tls_metric_load(&s_tls_metrics.client_handshake_pumps);
  metrics->client_handshakes_tls13 =
      (uint64_t)tls_metric_load(&s_tls_metrics.client_handshakes_tls13);
  metrics->server_handshakes_completed =
      (uint64_t)tls_metric_load(&s_tls_metrics.server_handshakes_completed);
  metrics->server_handshake_total_ns =
      (uint64_t)tls_metric_load(&s_tls_metrics.server_handshake_total_ns);
  metrics->server_handshake_crypto_ns =
      (uint64_t)tls_metric_load(&s_tls_metrics.server_handshake_crypto_ns);
  metrics->server_handshake_flush_ns =
      (uint64_t)tls_metric_load(&s_tls_metrics.server_handshake_flush_ns);
  metrics->server_handshake_pump_total_ns =
      (uint64_t)tls_metric_load(&s_tls_metrics.server_handshake_pump_total_ns);
  metrics->server_handshake_recv_cb_ns =
      (uint64_t)tls_metric_load(&s_tls_metrics.server_handshake_recv_cb_ns);
  metrics->server_handshake_clienthello_to_serverhello_ns =
      (uint64_t)tls_metric_load(&s_tls_metrics.server_handshake_clienthello_to_serverhello_ns);
  metrics->server_handshake_clientfinished_to_done_ns =
      (uint64_t)tls_metric_load(&s_tls_metrics.server_handshake_clientfinished_to_done_ns);
  metrics->server_handshake_pumps =
      (uint64_t)tls_metric_load(&s_tls_metrics.server_handshake_pumps);
}

CXX_C_API void turbo_stream_tls_reset_metrics(void) {
  atomic_store_explicit(&s_tls_metrics.client_handshakes_started, 0, memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.client_handshakes_completed, 0, memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.client_session_cache_attempts, 0, memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.client_session_reused, 0, memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.client_session_stores, 0, memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.client_handshake_total_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.client_handshake_bio_write_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.client_handshake_bio_write_calls, 0, memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.client_handshake_bio_write_bytes, 0, memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.client_handshake_crypto_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.client_handshake_flush_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.client_handshake_pump_total_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.client_handshake_recv_cb_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.client_handshake_connect_cb_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.client_handshake_iocp_post_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.client_handshake_post_drain_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.client_handshake_waiter_signal_ns, 0,
                        memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.client_handshake_resume_wait_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.client_handshake_wrap_client_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.client_handshake_clienthello_to_serverhello_ns, 0,
                        memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.client_handshake_serverhello_to_finished_write_ns, 0,
                        memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.client_handshake_finished_write_to_done_ns, 0,
                        memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.client_handshake_serverhello_to_done_ns, 0,
                        memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.client_handshake_pumps, 0, memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.client_handshakes_tls13, 0, memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.server_handshakes_completed, 0, memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.server_handshake_total_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.server_handshake_crypto_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.server_handshake_flush_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.server_handshake_pump_total_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.server_handshake_recv_cb_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.server_handshake_clienthello_to_serverhello_ns, 0,
                        memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.server_handshake_clientfinished_to_done_ns, 0,
                        memory_order_relaxed);
  atomic_store_explicit(&s_tls_metrics.server_handshake_pumps, 0, memory_order_relaxed);
}
