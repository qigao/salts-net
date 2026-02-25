#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "internal.h"
#include "stats.h"
#include "turbo_dns.h"
#include "tlog.h"
#include "turbo_tls.h"
#include <uv.h>

/* Include uvtls headers - these would need to be available */
#include <openssl/bio.h>
#include <openssl/conf.h>
#include <openssl/engine.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#ifdef _WIN32
  #include <winsock2.h>
  #define sleep_ms(ms) Sleep(ms)
#else
  #include <unistd.h>
  #define sleep_ms(ms) usleep((ms) * 1000)
#endif

/* Forward declarations for pool synchronization */
extern void turbo_tls_sync_lock(void);
extern void turbo_tls_sync_unlock(void);

static void turbo_tls_debug_ssl_errors(const char *context) {
  unsigned long code;
  while ((code = ERR_peek_error()) != 0) {
    char buf[256];
    ERR_error_string_n(code, buf, sizeof(buf));
    TLOG_DEBUG("{}: {}", context, buf);
    (void)ERR_get_error();
  }
}

static void turbo_tls_debug_uv_error(const char *context, int err) {
  const char *msg = uv_strerror(err);
  TLOG_DEBUG("{}: {} ({:d})", context, msg ? msg : "unknown", err);
}

/* OpenSSL version compatibility */
#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
  #define BIO_get_data(b) ((b)->ptr)
  #define BIO_set_data(b, p) ((b)->ptr = p)
  #define BIO_set_init(b, i) ((b)->init = i)
  #define BIO_get_shutdown(b) ((b)->shutdown)
  #define BIO_set_shutdown(b, s) ((b)->shutdown = s)
  #define ASN1_STRING_get0_data ASN1_STRING_data
#endif

#if (OPENSSL_VERSION_NUMBER >= 0x10100000L || LIBRESSL_VERSION_NUMBER >= 0x20302000L)
  #define TURBO_TLS_METHOD TLS_method
#else
  #define TURBO_TLS_METHOD SSLv23_method
#endif

#define TURBO_TLS_SUGGESTED_READ_SIZE 16384
#define TURBO_TLS_STACK_BUFS_COUNT 16

/* UV error codes if not available */
#ifndef UV_ENOMEM
  #define UV_ENOMEM (-12)
#endif
#ifndef UV_EINVAL
  #define UV_EINVAL (-22)
#endif
#ifndef UV_EOF
  #define UV_EOF (-4095)
#endif

/* Stats macros - simple stubs if not available */
#ifndef TURBO_STATS_INC
  #define TURBO_STATS_INC(name)                                                                    \
    do {                                                                                           \
      (void)(name);                                                                                \
    } while (0)
#endif
#ifndef TURBO_STATS_ADD
  #define TURBO_STATS_ADD(name, value)                                                             \
    do {                                                                                           \
      (void)(name);                                                                                \
      (void)(value);                                                                               \
    } while (0)
#endif
#ifndef TURBO_STATS_SET
  #define TURBO_STATS_SET(name, value)                                                             \
    do {                                                                                           \
      (void)(name);                                                                                \
      (void)(value);                                                                               \
    } while (0)
#endif
#ifndef TURBO_STATS_RECORD
  #define TURBO_STATS_RECORD(name, value)                                                          \
    do {                                                                                           \
      (void)(name);                                                                                \
      (void)(value);                                                                               \
    } while (0)
#endif

/* Arena-based memory pool for TLS */
typedef struct turbo_tls_arena_pool_s {
  turbo_arena_t arena;
  turbo_arena_buffer_t *current_buffer;
  size_t buffer_size;
  int ret;
} turbo_tls_arena_pool_t;

typedef struct turbo_tls_arena_pos_s {
  turbo_arena_buffer_t *buffer;
  size_t offset;
} turbo_tls_arena_pos_t;

/* TLS session implementation */
typedef struct turbo_tls_session_s {
  SSL *ssl;
  BIO *incoming_bio;
  BIO *outgoing_bio;
} turbo_tls_session_t;

/* Send operation for zero-copy */
typedef struct turbo_tls_send_op_s {
  uv_write_t req;
  turbo_arena_slice_t *slices;
  size_t slice_count;
  turbo_tls_client_t *client;
  struct turbo_tls_send_op_s *next;
  turbo_tls_arena_pos_t commit_pos;
} turbo_tls_send_op_t;

/* Send operation pool */
static turbo_tls_send_op_t *g_tls_send_op_pool = NULL;
static size_t g_tls_send_op_pool_size = 0;
static const size_t MAX_TLS_SEND_OP_POOL_SIZE = 256;

/* Library initialization */
static uv_once_t lib_init_guard__ = UV_ONCE_INIT;

static void lib_cleanup(void) {
  RAND_cleanup();
  ENGINE_cleanup();
  CONF_modules_unload(1);
  CONF_modules_free();
  EVP_cleanup();
  ERR_free_strings();
  CRYPTO_cleanup_all_ex_data();
  CRYPTO_set_locking_callback(NULL);
  CRYPTO_set_id_callback(NULL);
#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
  ERR_remove_thread_state(NULL);
#endif
}

static void lib_init(void) {
  SSL_library_init();
  SSL_load_error_strings();
  OpenSSL_add_all_algorithms();
  atexit(lib_cleanup);
}

/* Arena pool operations */
static int turbo_tls_arena_pool_init(turbo_tls_arena_pool_t *pool) {
  pool->buffer_size = 65536; /* 64KB default buffer size */
  pool->ret = -1;
  pool->current_buffer = NULL;

  return turbo_arena_init(&pool->arena, pool->buffer_size);
}

static void turbo_tls_arena_pool_destroy(turbo_tls_arena_pool_t *pool) {
  if (pool) {
    if (pool->current_buffer) {
      turbo_arena_buffer_unref(pool->current_buffer);
      pool->current_buffer = NULL;
    }
    turbo_arena_free(&pool->arena);
  }
}

static size_t turbo_tls_arena_pool_available(turbo_tls_arena_pool_t *pool) {
  if (!pool->current_buffer)
    return 0;
  if (pool->current_buffer->used > pool->current_buffer->capacity) {
    pool->current_buffer->used = 0;
    return 0;
  }
  return pool->current_buffer->used;
}

static int turbo_tls_arena_pool_read(turbo_tls_arena_pool_t *pool, char *out, int len) {
  if (!pool->current_buffer || pool->current_buffer->used == 0) {
    return 0; /* No data available - return 0, BIO will check pool->ret */
  }
  if (pool->current_buffer->used > pool->current_buffer->capacity) {
    pool->current_buffer->used = 0;
    return 0;
  }

  size_t available = pool->current_buffer->used;
  size_t to_read = (size_t)len < available ? (size_t)len : available;

  memcpy(out, pool->current_buffer->data, to_read);

  /* Move remaining data to front */
  if (to_read < available) {
    memmove(pool->current_buffer->data, pool->current_buffer->data + to_read, available - to_read);
  }
  pool->current_buffer->used -= to_read;

  return (int)to_read;
}

static void turbo_tls_arena_pool_write(turbo_tls_arena_pool_t *pool, const char *data, int len) {
  if (len <= 0)
    return;

  if (pool->current_buffer && pool->current_buffer->used > pool->current_buffer->capacity) {
    turbo_arena_buffer_unref(pool->current_buffer);
    pool->current_buffer = NULL;
  }

  size_t existing_used = 0;
  if (pool->current_buffer) {
    if (pool->current_buffer->used <= pool->current_buffer->capacity) {
      existing_used = pool->current_buffer->used;
    } else {
      turbo_arena_buffer_unref(pool->current_buffer);
      pool->current_buffer = NULL;
    }
  }

  /* Ensure we have a current buffer with enough space */
  if (!pool->current_buffer || turbo_arena_buffer_remaining(pool->current_buffer) < (size_t)len) {

    /* Get a new buffer */
    size_t needed_size = existing_used + (size_t)len;
    if (needed_size < pool->buffer_size)
      needed_size = pool->buffer_size;
    turbo_arena_buffer_t *new_buffer = turbo_arena_get_buffer(&pool->arena, needed_size);

    if (!new_buffer) {
      return; /* Out of memory */
    }

    /* Copy existing data to new buffer if any */
    if (pool->current_buffer && existing_used > 0) {
      size_t copy_size = existing_used;
      if (copy_size <= new_buffer->capacity) {
        memcpy(new_buffer->data, pool->current_buffer->data, copy_size);
        new_buffer->used = copy_size;
      }
    }

    /* Release old buffer and use new one */
    if (pool->current_buffer) {
      turbo_arena_buffer_unref(pool->current_buffer);
    }
    pool->current_buffer = new_buffer;
    pool->ret = -1;
  }

  /* Append new data */
  char *write_ptr = turbo_arena_buffer_write_ptr(pool->current_buffer);
  memcpy(write_ptr, data, (size_t)len);
  pool->current_buffer->used += (size_t)len;
  pool->ret = -1;
}

static void turbo_tls_arena_pool_reset(turbo_tls_arena_pool_t *pool) {
  if (pool->current_buffer) {
    pool->current_buffer->used = 0;
    pool->ret = -1;
  }
}

static turbo_tls_arena_pos_t turbo_tls_arena_pool_get_blocks(turbo_tls_arena_pool_t *pool,
                                                             turbo_tls_arena_pos_t start_pos,
                                                             uv_buf_t *bufs, int *bufs_count) {
  turbo_tls_arena_pos_t pos = {NULL, 0};

  if (!pool->current_buffer || pool->current_buffer->used == 0 || *bufs_count <= 0) {
    *bufs_count = 0;
    return pos;
  }

  /* Calculate the data that was added since start_pos */
  size_t start_offset = (start_pos.buffer == pool->current_buffer) ? start_pos.offset : 0;
  size_t available_data = pool->current_buffer->used;

  if (available_data <= start_offset) {
    *bufs_count = 0;
    return pos;
  }

  /* Return only the new data since start_pos */
  size_t new_data_size = available_data - start_offset;
  bufs[0] = uv_buf_init(pool->current_buffer->data + start_offset, (unsigned int)new_data_size);
  *bufs_count = 1;

  pos.buffer = pool->current_buffer;
  pos.offset = pool->current_buffer->used;

  return pos;
}

static void turbo_tls_arena_pool_commit(turbo_tls_arena_pool_t *pool, turbo_tls_arena_pos_t pos) {
  /* Mark data as consumed up to the commit position */
  if (pool->current_buffer && pos.buffer == pool->current_buffer) {
    /* Move remaining data to front if any */
    if (pos.offset < pool->current_buffer->used) {
      size_t remaining = pool->current_buffer->used - pos.offset;
      memmove(pool->current_buffer->data, pool->current_buffer->data + pos.offset, remaining);
      pool->current_buffer->used = remaining;
    } else {
      /* All data consumed */
      pool->current_buffer->used = 0;
    }
  }
}

/* Get send operation from pool */
static turbo_tls_send_op_t *get_tls_send_op(turbo_tls_client_t *client) {
  turbo_tls_send_op_t *op = NULL;

  turbo_tls_sync_lock();
  if (g_tls_send_op_pool && g_tls_send_op_pool_size > 0) {
    op = g_tls_send_op_pool;
    g_tls_send_op_pool = op->next;
    g_tls_send_op_pool_size--;
    TURBO_STATS_INC("tls.send_ops_reused");
  }
  turbo_tls_sync_unlock();

  if (!op) {
    op = (turbo_tls_send_op_t *)malloc(sizeof(turbo_tls_send_op_t));
    if (op) {
      TURBO_STATS_INC("tls.send_ops_allocated");
    } else {
      return NULL; // NULL check
    }
  }

  memset(op, 0, sizeof(*op));
  op->client = client;

  return op;
}

/* Return send operation to pool (thread-safe) */
static void return_tls_send_op(turbo_tls_send_op_t *op) {
  if (!op)
    return;

  /* Release all slices */
  if (op->slices) {
    for (size_t i = 0; i < op->slice_count; i++) {
      turbo_arena_slice_release(&op->slices[i]);
    }
    free(op->slices);
    op->slices = NULL;
  }

  turbo_tls_sync_lock();
  if (g_tls_send_op_pool_size < MAX_TLS_SEND_OP_POOL_SIZE) {
    op->next = g_tls_send_op_pool;
    g_tls_send_op_pool = op;
    g_tls_send_op_pool_size++;
    turbo_tls_sync_unlock();
    TURBO_STATS_INC("tls.send_ops_pooled");
  } else {
    turbo_tls_sync_unlock();
    free(op);
    TURBO_STATS_INC("tls.send_ops_freed");
  }
}

/* Forward declarations */
static void on_tls_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);

/* BIO operations for arena pool */
static int arena_pool_bio_create(BIO *bio);
static int arena_pool_bio_destroy(BIO *bio);
static int arena_pool_bio_read(BIO *bio, char *out, int len);
static int arena_pool_bio_write(BIO *bio, const char *data, int len);
static int arena_pool_bio_puts(BIO *bio, const char *str);
static int arena_pool_bio_gets(BIO *bio, char *out, int size);
static long arena_pool_bio_ctrl(BIO *bio, int cmd, long num, void *ptr);

#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
const BIO_METHOD method__ = {BIO_TYPE_MEM,           "Arena Pool",
                             arena_pool_bio_write,   arena_pool_bio_read,
                             arena_pool_bio_puts,    arena_pool_bio_gets,
                             arena_pool_bio_ctrl,    arena_pool_bio_create,
                             arena_pool_bio_destroy, NULL};
#else
static BIO_METHOD *method__ = NULL;
static void arena_pool_bio_init(void) {
  method__ = BIO_meth_new(BIO_TYPE_MEM, "arena pool");
  if (method__) {
    BIO_meth_set_write(method__, arena_pool_bio_write);
    BIO_meth_set_read(method__, arena_pool_bio_read);
    BIO_meth_set_puts(method__, arena_pool_bio_puts);
    BIO_meth_set_gets(method__, arena_pool_bio_gets);
    BIO_meth_set_ctrl(method__, arena_pool_bio_ctrl);
    BIO_meth_set_create(method__, arena_pool_bio_create);
    BIO_meth_set_destroy(method__, arena_pool_bio_destroy);
  }
}
#endif

static uv_once_t arena_pool_init_guard__ = UV_ONCE_INIT;

static void arena_pool_bio_init_once(void) {
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
  uv_once(&arena_pool_init_guard__, arena_pool_bio_init);
#endif
}

static turbo_tls_arena_pool_t *arena_pool_from_bio(BIO *bio) {
  void *data = BIO_get_data(bio);
  assert(data && "BIO data field should not be NULL");
  return (turbo_tls_arena_pool_t *)data;
}

static int arena_pool_bio_create(BIO *bio) {
  BIO_set_shutdown(bio, 1);
  BIO_set_init(bio, 1);
  return 1;
}

static int arena_pool_bio_destroy(BIO *bio) {
  if (bio == NULL)
    return 0;
  turbo_tls_arena_pool_destroy(arena_pool_from_bio(bio));
  return 1;
}

static int arena_pool_bio_read(BIO *bio, char *out, int len) {
  int bytes;
  turbo_tls_arena_pool_t *pool;
  BIO_clear_retry_flags(bio);

  pool = arena_pool_from_bio(bio);
  TLOG_DEBUG("BIO read: requesting {:d} bytes from pool={}", len, (void *)pool);
  bytes = turbo_tls_arena_pool_read(pool, out, len);
  TLOG_DEBUG("BIO read: got {:d} bytes from pool={}", bytes, (void *)pool);

  if (bytes == 0) {
    /* No data read from pool */
    if (pool->ret != 0) {
      /* Pool wants us to retry - set retry flag and return 0 */
      BIO_set_retry_read(bio);
      TLOG_DEBUG("BIO read: no data, set retry flag, returning 0");
    } else {
      /* Pool indicates true EOF */
      TLOG_DEBUG("BIO read: true EOF from pool");
    }
    bytes = 0;
  }

  return bytes;
}

static int arena_pool_bio_write(BIO *bio, const char *data, int len) {
  BIO_clear_retry_flags(bio);
  turbo_tls_arena_pool_t *pool = arena_pool_from_bio(bio);
  TLOG_DEBUG("BIO write: {:d} bytes to pool={}", len, (void *)pool);
  turbo_tls_arena_pool_write(pool, data, len);
  return len;
}

static int arena_pool_bio_puts(BIO *bio, const char *str) {
  return arena_pool_bio_write(bio, str, (int)strlen(str));
}

static int arena_pool_bio_gets(BIO *bio, char *out, int size) {
  /* Not implemented for TLS use case */
  (void)bio;
  (void)out;
  (void)size;
  return -1;
}

static long arena_pool_bio_ctrl(BIO *bio, int cmd, long num, void *ptr) {
  long ret = 1;
  turbo_tls_arena_pool_t *pool = arena_pool_from_bio(bio);

  switch (cmd) {
  case BIO_CTRL_RESET:
    turbo_tls_arena_pool_reset(pool);
    break;
  case BIO_CTRL_EOF:
    ret = (turbo_tls_arena_pool_available(pool) == 0);
    break;
  case BIO_C_SET_BUF_MEM_EOF_RETURN:
    pool->ret = (int)num;
    break;
  case BIO_CTRL_INFO:
    ret = (long)turbo_tls_arena_pool_available(pool);
    if (ptr != NULL) {
      *(void **)ptr = NULL;
    }
    break;
  case BIO_CTRL_GET_CLOSE:
    ret = BIO_get_shutdown(bio);
    break;
  case BIO_CTRL_SET_CLOSE:
    BIO_set_shutdown(bio, (int)num);
    break;
  case BIO_CTRL_WPENDING:
    ret = 0;
    break;
  case BIO_CTRL_PENDING:
    ret = (long)turbo_tls_arena_pool_available(pool);
    break;
  case BIO_CTRL_DUP:
  case BIO_CTRL_FLUSH:
    ret = 1;
    break;
  default:
    ret = 0;
    break;
  }
  return ret;
}

static BIO *create_bio(turbo_tls_arena_pool_t *pool) {
#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
  BIO *bio = BIO_new((BIO_METHOD *)&method__);
#else
  BIO *bio = BIO_new(method__);
#endif
  BIO_set_data(bio, pool);
  return bio;
}

/* Certificate and key loading */
static X509 *load_cert(const char *cert, size_t length) {
  BIO *bio;
  X509 *x509;
  if (length > INT_MAX)
    return NULL;

  bio = BIO_new_mem_buf(cert, (int)length);
  if (bio == NULL)
    return NULL;

  x509 = PEM_read_bio_X509(bio, NULL, NULL, NULL);
  BIO_free_all(bio);
  return x509;
}

static EVP_PKEY *load_key(const char *key, size_t length) {
  BIO *bio;
  EVP_PKEY *pkey;
  if (length > INT_MAX)
    return NULL;

  bio = BIO_new_mem_buf(key, (int)length);
  if (bio == NULL)
    return NULL;

  pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
  BIO_free_all(bio);
  return pkey;
}

/* TLS session management */
static turbo_tls_session_t *turbo_tls_session_create(SSL_CTX *ssl_ctx,
                                                     turbo_tls_arena_pool_t *incoming,
                                                     turbo_tls_arena_pool_t *outgoing) {
  turbo_tls_session_t *session = (turbo_tls_session_t *)malloc(sizeof(turbo_tls_session_t));
  if (!session)
    return NULL;

#if (OPENSSL_VERSION_NUMBER >= 0x10100000L || LIBRESSL_VERSION_NUMBER >= 0x20302000L)
  SSL_CTX_up_ref(ssl_ctx);
#endif

  session->ssl = SSL_new(ssl_ctx);
  if (!session->ssl) {
    free(session);
    return NULL;
  }

  session->incoming_bio = create_bio(incoming);
  session->outgoing_bio = create_bio(outgoing);

  SSL_set_bio(session->ssl, session->incoming_bio, session->outgoing_bio);
  return session;
}

/* Write completion callback */
static void on_tls_write_complete(uv_write_t *req, int status) {
  turbo_tls_send_op_t *op =
      (turbo_tls_send_op_t *)((char *)req - offsetof(turbo_tls_send_op_t, req));
  turbo_tls_client_t *client = op->client;

  client->write_in_progress = 0;

  if (status == 0) {
    size_t total_bytes = 0;
    for (size_t i = 0; i < op->slice_count; i++) {
      total_bytes += op->slices[i].length;
    }

    TURBO_STATS_ADD("tls.bytes_sent", total_bytes);
    TURBO_STATS_INC("tls.messages_sent");
    TURBO_STATS_INC("tls.zero_copy_sends");
    TURBO_STATS_RECORD("tls.send_size", total_bytes);
  } else {
    TURBO_STATS_INC("tls.send_errors");
  }

  /* Commit arena pool changes */
  turbo_tls_arena_pool_commit((turbo_tls_arena_pool_t *)client->outgoing_ring, op->commit_pos);

  return_tls_send_op(op);

  /* Continue sending if more data queued */
  if (client->send_queue_head && !client->closing) {
    turbo_tls_flush(client);
  }
}

/* Handshake write callback */
static void on_handshake_write(uv_write_t *req, int status) {
  turbo_tls_client_t *client = (turbo_tls_client_t *)req->data;
  turbo_tls_arena_pool_commit((turbo_tls_arena_pool_t *)client->outgoing_ring,
                              *(turbo_tls_arena_pos_t *)client->commit_pos);
  free(req);

  if (status != 0) {
    TURBO_STATS_INC("tls.handshake_errors");
    if (client->handshake_done_cb) {
      client->handshake_done_cb(client, status);
    }
  }
}

/* Perform TLS handshake */
static int do_handshake(turbo_tls_client_t *client) {
  turbo_tls_session_t *session = (turbo_tls_session_t *)client->impl;
  turbo_tls_arena_pool_t *outgoing = (turbo_tls_arena_pool_t *)client->outgoing_ring;

  /* Capture the starting position properly */
  turbo_tls_arena_pos_t start_pos = {outgoing->current_buffer,
                                     outgoing->current_buffer ? outgoing->current_buffer->used : 0};
  size_t start_size = turbo_tls_arena_pool_available(outgoing);

  TLOG_DEBUG("handshake starting client={} is_server={:d}", (void *)client,
             SSL_is_server(session->ssl));
  int rc = SSL_do_handshake(session->ssl);
  TLOG_DEBUG("handshake step client={} rc={:d}", (void *)client, rc);
  if (rc <= 0) {
    int err = SSL_get_error(session->ssl, rc);
    if (err == SSL_ERROR_WANT_READ) {
      TLOG_DEBUG("handshake wants read client={}", (void *)client);
      /* This is normal - handshake needs more incoming data */
    } else if (err == SSL_ERROR_WANT_WRITE) {
      TLOG_DEBUG("handshake wants write client={}", (void *)client);
      /* This is normal - handshake wants to send data */
    } else {
      TLOG_DEBUG("handshake error client={} err={:d}", (void *)client, err);
      turbo_tls_debug_ssl_errors("handshake error detail");
      TURBO_STATS_INC("tls.handshake_errors");
      return TURBO_TLS_EHANDSHAKE;
    }
  }

  TLOG_DEBUG("handshake buffer start={} client={}", start_size, (void *)client);
  size_t new_size = turbo_tls_arena_pool_available(outgoing);
  TLOG_DEBUG("handshake buffer new={} client={}", new_size, (void *)client);

  /* Check if SSL generated outgoing data that needs to be sent */
  if (new_size > start_size) {
    TLOG_DEBUG("handshake produced {} bytes for client={}", new_size - start_size, (void *)client);
    uv_write_t *req = (uv_write_t *)malloc(sizeof(uv_write_t));
    if (!req)
      return UV_ENOMEM;

    req->data = client;

    uv_buf_t buf;
    int bufs_count = 1;
    turbo_tls_arena_pos_t commit_pos =
        turbo_tls_arena_pool_get_blocks(outgoing, start_pos, &buf, &bufs_count);

    /* Store commit position */
    if (!client->commit_pos) {
      client->commit_pos = malloc(sizeof(turbo_tls_arena_pos_t));
      if (!client->commit_pos) {
        free(req);
        return UV_ENOMEM;
      }
    }
    *(turbo_tls_arena_pos_t *)client->commit_pos = commit_pos;

    int write_rc = uv_write(req, (uv_stream_t *)&client->handle, &buf, (unsigned int)bufs_count,
                            on_handshake_write);
    if (write_rc != 0) {
      free(req);
      return write_rc;
    }
  }

  return 0;
} /* Cert
 ificate verification */
static int verify_certificate(turbo_tls_client_t *client) {
  turbo_tls_session_t *session = (turbo_tls_session_t *)client->impl;
  int verify_flags = client->context->verify_flags;

  if (!verify_flags)
    return 0;

  X509 *peer_cert = SSL_get_peer_certificate(session->ssl);
  if (peer_cert == NULL) {
    return TURBO_TLS_ENOPEERCERT;
  }

  if (verify_flags & TURBO_TLS_VERIFY_PEER_CERT) {
    long rc = SSL_get_verify_result(session->ssl);
    if (rc != X509_V_OK) {
      X509_free(peer_cert);
      return TURBO_TLS_EBADPEERCERT;
    }
  }

  /* Hostname verification using X509_check_host */
  if (verify_flags & TURBO_TLS_VERIFY_PEER_IDENT) {
    if (client->hostname[0] == '\0') {
      X509_free(peer_cert);
      return TURBO_TLS_EBADPEERIDENT;
    }

#if OPENSSL_VERSION_NUMBER >= 0x10200000L
    /* OpenSSL 1.0.2+ has X509_check_host */
    int check_result =
        X509_check_host(peer_cert, client->hostname, strlen(client->hostname), 0, NULL);
    if (check_result != 1) {
      X509_free(peer_cert);
      TLOG_DEBUG("hostname verification failed for '{:s}', result={:d}", client->hostname,
                 check_result);
      return TURBO_TLS_EBADPEERIDENT;
    }
    TLOG_DEBUG("hostname verification succeeded for '{:s}'", client->hostname);
#else
    /* Fallback for older OpenSSL: basic CN check */
    X509_NAME *subject = X509_get_subject_name(peer_cert);
    if (subject) {
      char cn[256] = {0};
      int cn_len = X509_NAME_get_text_by_NID(subject, NID_commonName, cn, sizeof(cn) - 1);
      if (cn_len <= 0 || strcmp(cn, client->hostname) != 0) {
        X509_free(peer_cert);
        TLOG_DEBUG("hostname verification failed: CN='{:s}' != hostname='{:s}'", cn,
                   client->hostname);
        return TURBO_TLS_EBADPEERIDENT;
      }
      TLOG_DEBUG("hostname verification succeeded (CN match): '{:s}'", cn);
    } else {
      X509_free(peer_cert);
      return TURBO_TLS_EBADPEERIDENT;
    }
#endif
  }

  X509_free(peer_cert);
  return 0;
}

/* Receive buffer allocation */
static void alloc_tls_recv_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  turbo_tls_client_t *client = (turbo_tls_client_t *)handle->data;
  if (!client || client->closing) {
    buf->base = NULL;
    buf->len = 0;
    return;
  }

  /* Use ping-pong buffers for zero-copy receives */
  if (client->recv_buffer2 && client->recv_toggle == 0) {
    buf->base = client->recv_buffer2->data;
    buf->len = (unsigned int)client->recv_buffer2->capacity;
    client->recv_toggle = 1;
  } else if (client->recv_buffer1) {
    buf->base = client->recv_buffer1->data;
    buf->len = (unsigned int)client->recv_buffer1->capacity;
    client->recv_toggle = 0;
  } else {
    buf->base = NULL;
    buf->len = 0;
  }
}

/* Process decrypted TLS data */
static void do_tls_read(turbo_tls_client_t *client) {
  turbo_tls_session_t *session = (turbo_tls_session_t *)client->impl;

  while (client->on_recv) {
    int nread;

    /* Always use ping-pong receive buffers for consistency */
    turbo_arena_buffer_t *recv_buffer =
        (client->recv_toggle == 1) ? client->recv_buffer2 : client->recv_buffer1;
    if (!recv_buffer) {
      TLOG_DEBUG("no receive buffer available for client={}", (void *)client);
      return;
    }

    uv_buf_t buf = uv_buf_init(recv_buffer->data, (unsigned int)recv_buffer->capacity);

    TLOG_DEBUG("SSL_read attempting to read {:d} bytes for client={}", (int)buf.len,
               (void *)client);
    nread = SSL_read(session->ssl, buf.base, (int)buf.len);
    TLOG_DEBUG("SSL_read returned {:d} for client={}", nread, (void *)client);
    if (nread <= 0) {
      int error = SSL_get_error(session->ssl, nread);
      if (error == SSL_ERROR_WANT_READ) {
        TLOG_DEBUG("SSL_read wants more data for client={}", (void *)client);
        /* Wait for next read */
        return;
      } else {
        /* Create error slice */
        TLOG_DEBUG("SSL_read error for client={} err={:d}", (void *)client, error);
        turbo_tls_debug_ssl_errors("SSL_read error detail");
        turbo_arena_slice_t error_slice = {NULL, 0, NULL};
        client->on_recv(client, &error_slice, NULL);
        return;
      }
    }

    TLOG_DEBUG("decrypted {} bytes for client={}", nread, (void *)client);
    TURBO_STATS_ADD("tls.bytes_received", (size_t)nread);
    TURBO_STATS_INC("tls.messages_received");
    TURBO_STATS_INC("tls.zero_copy_receives");
    TURBO_STATS_RECORD("tls.recv_size", (size_t)nread);

    /* Create slice for received data - buffer should always match now */
    TLOG_DEBUG("calling on_recv with {} bytes for client={}", nread, (void *)client);
    turbo_arena_buffer_set_used(recv_buffer, (size_t)nread);
    turbo_arena_slice_t slice = turbo_arena_buffer_slice(recv_buffer, 0, (size_t)nread);

    int should_close = client->on_recv(client, &slice, NULL);
    turbo_arena_slice_release(&slice);

    if (should_close) {
      turbo_tls_client_close(client);
      return;
    }

    /* Toggle to other buffer for next read */
    client->recv_toggle = 1 - client->recv_toggle;
  }
}

/* Handshake read callback */
static void on_handshake_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  turbo_tls_client_t *client = (turbo_tls_client_t *)stream->data;
  turbo_tls_session_t *session = (turbo_tls_session_t *)client->impl;

  if ((nread == UV_EOF && !SSL_is_init_finished(session->ssl)) || (nread != UV_EOF && nread < 0)) {
    TLOG_DEBUG("handshake read error client={} nread={}", (void *)client, nread);
    turbo_tls_debug_uv_error("handshake read uv error", (int)nread);
    uv_read_stop(stream);
    TURBO_STATS_INC("tls.handshake_errors");
    if (client->handshake_done_cb) {
      client->handshake_done_cb(client, (int)nread);
    }
    return;
  } else if (nread > 0) {
    TLOG_DEBUG("handshake received %zd bytes for client={}", nread, (void *)client);
    /* Write received data to incoming arena pool */
    turbo_tls_arena_pool_write((turbo_tls_arena_pool_t *)client->incoming_ring, buf->base,
                               (int)nread);

    int rc = do_handshake(client);
    if (rc != 0 && rc != UV_EAGAIN) { /* UV_EAGAIN means write is pending */
      uv_read_stop(stream);
      if (client->handshake_done_cb) {
        client->handshake_done_cb(client, rc);
      }
      return;
    }

    /* Check if handshake is now complete */
    if (SSL_is_init_finished(session->ssl)) {
      uv_read_stop(stream);
      client->handshake_complete = 1;
      TLOG_DEBUG("handshake complete for client={} is_server={}", (void *)client,
                 SSL_is_server(session->ssl));
      TURBO_STATS_INC("tls.handshakes_completed");

      int verify_result = verify_certificate(client);
      if (verify_result == 0) {
        /* Switch to normal data processing */
        int read_rc =
            uv_read_start((uv_stream_t *)&client->handle, alloc_tls_recv_buffer, on_tls_read);
        if (read_rc == 0) {
          if (client->on_connect) {
            TLOG_DEBUG("handshake verify ok for client={}", (void *)client);
            client->on_connect(client, 0, client->server);
          }
          /* Process any pending decrypted data */
          do_tls_read(client);
        } else {
          TLOG_DEBUG("failed to start normal reads client={} rc={}", (void *)client, read_rc);
          if (client->handshake_done_cb) {
            client->handshake_done_cb(client, read_rc);
          }
        }
      } else {
        TLOG_DEBUG("handshake verify failed for client={} code={}", (void *)client, verify_result);
        if (client->handshake_done_cb) {
          client->handshake_done_cb(client, verify_result);
        }
      }
    }
  } else if (nread == 0) {
    /* No data received, but check if handshake completed anyway */
    if (SSL_is_init_finished(session->ssl) && !client->handshake_complete) {
      uv_read_stop(stream);
      client->handshake_complete = 1;
      TLOG_DEBUG("handshake complete (no data) for client={}", (void *)client);
      TURBO_STATS_INC("tls.handshakes_completed");

      int verify_result = verify_certificate(client);
      if (verify_result == 0 && client->on_connect) {
        client->on_connect(client, 0, client->server);
      } else if (verify_result != 0 && client->handshake_done_cb) {
        client->handshake_done_cb(client, verify_result);
      }
    }
  }
}

/* Regular read callback after handshake */
static void on_tls_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  turbo_tls_client_t *client = (turbo_tls_client_t *)stream->data;
  turbo_tls_session_t *session = (turbo_tls_session_t *)client->impl;

  TLOG_DEBUG("on_tls_read called: client={} nread=%zd is_server={} "
             "handshake_complete={}",
             (void *)client, nread, SSL_is_server(session->ssl), client->handshake_complete);

  if (nread < 0) {
    TLOG_DEBUG("uv_read error client={} nread=%zd", (void *)client, nread);
    turbo_tls_debug_uv_error("uv_read error detail", (int)nread);
    TURBO_STATS_INC("tls.recv_errors");
    if (client->on_recv) {
      turbo_arena_slice_t error_slice = {NULL, 0, NULL};
      client->on_recv(client, &error_slice, NULL);
    }
    return;
  } else if (nread > 0) {
    /* Write received data to incoming arena pool */
    turbo_tls_arena_pool_write((turbo_tls_arena_pool_t *)client->incoming_ring, buf->base,
                               (int)nread);
    do_tls_read(client);
  }
}

/* Handle close callback */
static void on_tls_handle_closed(uv_handle_t *handle) {
  turbo_tls_client_t *client = (turbo_tls_client_t *)handle->data;
  if (!client)
    return;

  /* Update connection count */
  if (client->server) {
    if (client->server->active_connections > 0) {
      client->server->active_connections--;
    }
    TURBO_STATS_SET("tls.active_connections", client->server->active_connections);
    TURBO_STATS_INC("tls.connections_closed");

    if (client->server->on_close) {
      client->server->on_close(client);
    }
  } else if (client->on_close) {
    client->on_close(client);
  }

  /* Cleanup TLS session */
  if (client->impl) {
    turbo_tls_session_t *session = (turbo_tls_session_t *)client->impl;
#if (OPENSSL_VERSION_NUMBER >= 0x10100000L || LIBRESSL_VERSION_NUMBER >= 0x20302000L)
    SSL_CTX_free(SSL_get_SSL_CTX(session->ssl));
#endif
    SSL_free(session->ssl);
    free(session);
  }

  /* Cleanup arena pools */
  if (client->incoming_ring) {
    turbo_tls_arena_pool_destroy((turbo_tls_arena_pool_t *)client->incoming_ring);
    free(client->incoming_ring);
  }
  if (client->outgoing_ring) {
    turbo_tls_arena_pool_destroy((turbo_tls_arena_pool_t *)client->outgoing_ring);
    free(client->outgoing_ring);
  }
  if (client->commit_pos) {
    free(client->commit_pos);
  }

  /* Cleanup client resources */
  if (client->recv_buffer1) {
    turbo_arena_buffer_unref(client->recv_buffer1);
  }
  if (client->recv_buffer2) {
    turbo_arena_buffer_unref(client->recv_buffer2);
  }
  if (client->write_iov) {
    free(client->write_iov);
  }

  /* Free send queue */
  turbo_arena_buffer_t *current = client->send_queue_head;
  while (current) {
    turbo_arena_buffer_t *next = current->next;
    turbo_arena_buffer_unref(current);
    current = next;
  }

  turbo_arena_free(&client->arena);
  free(client);
}

/* New TLS connection callback */
static void on_tls_new_connection(uv_stream_t *server_stream, int status) {
  if (status < 0) {
    TURBO_STATS_INC("tls.accept_errors");
    return;
  }

  turbo_tls_server_t *server = (turbo_tls_server_t *)server_stream->data;
  TLOG_DEBUG("server accepted connection (status={})", status);
  if (!server)
    return;

  TURBO_STATS_INC("tls.connections_accepted");

  /* Create new client */
  turbo_tls_client_t *client = (turbo_tls_client_t *)calloc(1, sizeof(*client));
  if (!client)
    return;

  client->server = server;
  client->context = server->context;
  client->on_recv = server->on_recv;
  client->on_connect = server->on_connect;
  client->on_close = server->on_close;

  /* Initialize client arena */
  if (turbo_arena_init(&client->arena, 0) != 0) {
    free(client);
    return;
  }

  /* Initialize TCP handle */
  if (uv_tcp_init(server->loop, &client->handle) != 0) {
    turbo_arena_free(&client->arena);
    free(client);
    return;
  }

  client->handle.data = client;

  /* Accept the connection */
  if (uv_accept(server_stream, (uv_stream_t *)&client->handle) != 0) {
    uv_close((uv_handle_t *)&client->handle, on_tls_handle_closed);
    return;
  }

  /* Initialize TLS components */
  arena_pool_bio_init_once();

  /* Setup arena pools */
  client->incoming_ring = malloc(sizeof(turbo_tls_arena_pool_t));
  client->outgoing_ring = malloc(sizeof(turbo_tls_arena_pool_t));

  if (!client->incoming_ring || !client->outgoing_ring ||
      turbo_tls_arena_pool_init((turbo_tls_arena_pool_t *)client->incoming_ring) != 0 ||
      turbo_tls_arena_pool_init((turbo_tls_arena_pool_t *)client->outgoing_ring) != 0) {
    uv_close((uv_handle_t *)&client->handle, on_tls_handle_closed);
    return;
  }

  /* Create TLS session */
  client->impl = turbo_tls_session_create((SSL_CTX *)server->context->impl,
                                          (turbo_tls_arena_pool_t *)client->incoming_ring,
                                          (turbo_tls_arena_pool_t *)client->outgoing_ring);
  if (!client->impl) {
    uv_close((uv_handle_t *)&client->handle, on_tls_handle_closed);
    return;
  }

  /* Setup receive buffers */
  size_t recv_buf_size = 16384; /* Default read buffer size */
  client->recv_buffer1 = turbo_arena_get_buffer(&client->arena, recv_buf_size);
  client->recv_buffer2 = turbo_arena_get_buffer(&client->arena, recv_buf_size);

  if (!client->recv_buffer1) {
    uv_close((uv_handle_t *)&client->handle, on_tls_handle_closed);
    return;
  }

  /* Setup write IOV array */
  client->write_iov_capacity = 16; /* Default IOV capacity */
  client->write_iov = (uv_buf_t *)malloc(client->write_iov_capacity * sizeof(uv_buf_t));
  if (!client->write_iov) {
    turbo_tls_client_close(client);
    return;
  }

  /* Set server mode for TLS */
  turbo_tls_session_t *session = (turbo_tls_session_t *)client->impl;
  SSL_set_accept_state(session->ssl);

  /* Enable TCP_NODELAY */
  uv_tcp_nodelay(&client->handle, 1);

  /* Start TLS handshake */
  client->handshake_done_cb = NULL; /* Server mode doesn't need explicit handshake callback */

  if (uv_read_start((uv_stream_t *)&client->handle, alloc_tls_recv_buffer, on_handshake_read) ==
      0) {
    server->active_connections++;
    TURBO_STATS_SET("tls.active_connections", server->active_connections);
    TURBO_STATS_INC("tls.connections_established");

    /* For server mode, trigger initial handshake processing */
    int handshake_rc = do_handshake(client);
    if (handshake_rc != 0 && handshake_rc != UV_EAGAIN) {
      TLOG_DEBUG("server initial handshake failed client={} rc={}", (void *)client, handshake_rc);
      uv_close((uv_handle_t *)&client->handle, on_tls_handle_closed);
    }
  } else {
    uv_close((uv_handle_t *)&client->handle, on_tls_handle_closed);
  }
}
/* TLS
context management implementation */
int turbo_tls_context_init(turbo_tls_context_t *context, int flags) {
  SSL_CTX *ssl_ctx;

  if (flags & TURBO_TLS_CONTEXT_LIB_INIT) {
    uv_once(&lib_init_guard__, lib_init);
  }

  ssl_ctx = SSL_CTX_new(TURBO_TLS_METHOD());
  if (!ssl_ctx) {
    return UV_ENOMEM;
  }

  context->impl = ssl_ctx;
  context->verify_flags = TURBO_TLS_VERIFY_PEER_CERT;

  SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, NULL);
  return 0;
}

void turbo_tls_context_destroy(turbo_tls_context_t *context) {
  if (context && context->impl) {
    SSL_CTX_free((SSL_CTX *)context->impl);
    context->impl = NULL;
  }
}

void turbo_tls_context_set_verify_flags(turbo_tls_context_t *context, int verify_flags) {
  if (context) {
    context->verify_flags = verify_flags;
  }
}

int turbo_tls_context_add_trusted_certs(turbo_tls_context_t *context, const char *cert,
                                        size_t length) {
  if (!context || !cert || length == 0)
    return TURBO_TLS_EINVAL;

  int ncerts = 0;
  X509 *x509;
  X509_STORE *trusted_store = SSL_CTX_get_cert_store((SSL_CTX *)context->impl);

  BIO *bio = BIO_new_mem_buf(cert, (int)length);
  if (bio == NULL) {
    return UV_ENOMEM;
  }

  while ((x509 = PEM_read_bio_X509(bio, NULL, NULL, NULL)) != NULL) {
    X509_STORE_add_cert(trusted_store, x509);
    X509_free(x509);
    ncerts++;
  }

  BIO_free_all(bio);
  return ncerts == 0 ? TURBO_TLS_EINVAL : 0;
}

int turbo_tls_context_set_cert(turbo_tls_context_t *context, const char *cert, size_t length) {
  if (!context || !cert || length == 0)
    return TURBO_TLS_EINVAL;

  X509 *x509 = load_cert(cert, length);
  if (x509 == NULL) {
    return TURBO_TLS_EINVAL;
  }

  SSL_CTX_use_certificate((SSL_CTX *)context->impl, x509);
  X509_free(x509);
  return 0;
}

int turbo_tls_context_set_private_key(turbo_tls_context_t *context, const char *key,
                                      size_t length) {
  if (!context || !key || length == 0)
    return TURBO_TLS_EINVAL;

  EVP_PKEY *pkey = load_key(key, length);
  if (pkey == NULL) {
    return TURBO_TLS_EINVAL;
  }

  SSL_CTX_use_PrivateKey((SSL_CTX *)context->impl, pkey);
  EVP_PKEY_free(pkey);
  return 0;
}

int turbo_tls_server_start(turbo_tls_server_t *server, turbo_recv_cb on_recv,
                           turbo_connect_cb on_connect, turbo_close_cb on_close) {
  if (!server || !server->handle)
    return UV_EINVAL;

  server->on_recv = on_recv;
  server->on_connect = on_connect;
  server->on_close = on_close;

  int backlog = 128; /* Default backlog */
  return uv_listen((uv_stream_t *)server->handle, backlog, on_tls_new_connection);
}

void turbo_tls_server_stop(turbo_tls_server_t *server) {
  if (!server)
    return;

  if (server->handle) {
    if (!uv_is_closing((uv_handle_t *)server->handle)) {
      uv_close((uv_handle_t *)server->handle, NULL);
    }
    free(server->handle);
    server->handle = NULL;
  }

  turbo_arena_free(&server->arena);
  TURBO_STATS_INC("tls.servers_stopped");
}

/* Client lifecycle implementation */
turbo_tls_client_t *turbo_tls_client_create(uv_loop_t *loop, turbo_tls_context_t *context) {
  if (!loop || !context)
    return NULL;

  turbo_tls_client_t *client = (turbo_tls_client_t *)calloc(1, sizeof(*client));
  if (!client)
    return NULL;

  client->is_client_mode = 1;
  client->context = context;

  /* Initialize client arena */
  if (turbo_arena_init(&client->arena, 0) != 0) {
    free(client);
    return NULL;
  }

  /* Initialize TCP handle */
  if (uv_tcp_init(loop, &client->handle) != 0) {
    turbo_arena_free(&client->arena);
    free(client);
    return NULL;
  }

  client->handle.data = client;

  /* Setup receive buffers */
  size_t recv_buf_size = 16384;
  client->recv_buffer1 = turbo_arena_get_buffer(&client->arena, recv_buf_size);
  client->recv_buffer2 = turbo_arena_get_buffer(&client->arena, recv_buf_size);

  /* Setup write IOV array */
  client->write_iov_capacity = 16;
  client->write_iov = (uv_buf_t *)malloc(client->write_iov_capacity * sizeof(uv_buf_t));
  if (!client->write_iov) {
    turbo_arena_free(&client->arena);
    free(client);
    return NULL;
  }

  return client;
}

int turbo_tls_client_set_hostname(turbo_tls_client_t *client, const char *hostname, size_t length) {
  if (!client || !hostname || length == 0 || length >= sizeof(client->hostname)) {
    return UV_EINVAL;
  }

  memcpy(client->hostname, hostname, length);
  client->hostname[length] = '\0';

  return 0;
}

/* Client connect callback */
static void on_tls_client_connected(uv_connect_t *req, int status) {
  TLOG_DEBUG("on_tls_client_connected status={} req={}", status, (void *)req);
  turbo_tls_client_t *client = (turbo_tls_client_t *)req->handle->data;
  free(req);

  if (!client)
    return;

  if (status == 0) {
    TURBO_STATS_INC("tls.client_connections_established");
    TLOG_DEBUG("client TCP connection established client={}", (void *)client);

    /* Initialize TLS components */
    arena_pool_bio_init_once();

    /* Setup arena pools */
    client->incoming_ring = malloc(sizeof(turbo_tls_arena_pool_t));
    client->outgoing_ring = malloc(sizeof(turbo_tls_arena_pool_t));

    if (!client->incoming_ring || !client->outgoing_ring ||
        turbo_tls_arena_pool_init((turbo_tls_arena_pool_t *)client->incoming_ring) != 0 ||
        turbo_tls_arena_pool_init((turbo_tls_arena_pool_t *)client->outgoing_ring) != 0) {
      turbo_tls_client_close(client);
      return;
    }

    /* Create TLS session */
    client->impl = turbo_tls_session_create((SSL_CTX *)client->context->impl,
                                            (turbo_tls_arena_pool_t *)client->incoming_ring,
                                            (turbo_tls_arena_pool_t *)client->outgoing_ring);
    if (!client->impl) {
      turbo_tls_client_close(client);
      return;
    }

    /* Set client mode for TLS */
    turbo_tls_session_t *session = (turbo_tls_session_t *)client->impl;
    SSL_set_connect_state(session->ssl);

    /* Set hostname for SNI */
    if (client->hostname[0] != '\0') {
      SSL_set_tlsext_host_name(session->ssl, client->hostname);
    }

    /* Enable TCP_NODELAY */
    uv_tcp_nodelay(&client->handle, 1);

    /* Start TLS handshake */
    if (uv_read_start((uv_stream_t *)&client->handle, alloc_tls_recv_buffer, on_handshake_read) ==
        0) {
      /* Trigger initial handshake for client mode */
      int handshake_rc = do_handshake(client);
      if (handshake_rc != 0 && handshake_rc != UV_EAGAIN) {
        TLOG_DEBUG("initial handshake failed client={} rc={}", (void *)client, handshake_rc);
        turbo_tls_client_close(client);
      }
    } else {
      turbo_tls_client_close(client);
    }
  } else {
    turbo_tls_debug_uv_error("client TCP connection failed", status);
    TURBO_STATS_INC("tls.client_connection_errors");
    turbo_tls_client_close(client);
  }
}

int turbo_tls_client_connect(turbo_tls_client_t *client, const char *host, unsigned short port,
                             turbo_recv_cb on_recv, turbo_connect_cb on_connect,
                             turbo_close_cb on_close) {
  if (!client || !host)
    return UV_EINVAL;

  client->on_recv = on_recv;
  client->on_connect = on_connect;
  client->on_close = on_close;

  /* Try to parse as IP address first */
  struct sockaddr_storage addr;
  int rc;

  /* Try IPv4 */
  struct sockaddr_in addr4;
  rc = uv_ip4_addr(host, (int)port, &addr4);
  if (rc == 0) {
    memcpy(&addr, &addr4, sizeof(addr4));
  } else {
    /* Try IPv6 */
    struct sockaddr_in6 addr6;
    rc = uv_ip6_addr(host, (int)port, &addr6);
    if (rc == 0) {
      memcpy(&addr, &addr6, sizeof(addr6));
    } else {
      /* Not an IP address - caller must resolve hostname first */
      return UV_EINVAL;
    }
  }

  uv_connect_t *connect_req = (uv_connect_t *)malloc(sizeof(uv_connect_t));
  if (!connect_req)
    return UV_ENOMEM;

  TLOG_DEBUG("client_connect client={} host=%s:%u", (void *)client, host, (unsigned)port);

  int connect_rc = uv_tcp_connect(connect_req, &client->handle, (const struct sockaddr *)&addr,
                                  on_tls_client_connected);
  if (connect_rc != 0) {
    TLOG_DEBUG("uv_tcp_connect failed client={} rc={}", (void *)client, connect_rc);
  } else {
    TLOG_DEBUG("uv_tcp_connect pending client={}", (void *)client);
  }
  return connect_rc;
}

void turbo_tls_client_close(turbo_tls_client_t *client) {
  if (!client || client->closing)
    return;

  client->closing = 1;
  uv_read_stop((uv_stream_t *)&client->handle);

  if (!uv_is_closing((uv_handle_t *)&client->handle)) {
    uv_close((uv_handle_t *)&client->handle, on_tls_handle_closed);
  }
}

/* Zero-copy send operations */
turbo_arena_buffer_t *turbo_tls_get_send_buffer(turbo_tls_client_t *client, size_t min_size) {
  if (!client)
    return NULL;
  return turbo_arena_get_pooled_buffer(&client->arena, min_size);
}

int turbo_tls_send_buffer(turbo_tls_client_t *client, turbo_arena_buffer_t *buffer, size_t length) {
  if (!client || !buffer || client->closing)
    return UV_EINVAL;
  if (length > buffer->used)
    return UV_EINVAL;

  /* Add to send queue */
  buffer->next = NULL;
  if (client->send_queue_tail) {
    client->send_queue_tail->next = buffer;
  } else {
    client->send_queue_head = buffer;
  }
  client->send_queue_tail = buffer;
  client->send_queue_bytes += length;

  /* Reference the buffer */
  turbo_arena_buffer_ref(buffer);

  /* Flush if not already writing */
  if (!client->write_in_progress) {
    return turbo_tls_flush(client);
  }

  return 0;
}

void turbo_tls_discard_buffer(turbo_tls_client_t *client, turbo_arena_buffer_t *buffer) {
  if (!client || !buffer)
    return;
  turbo_arena_buffer_unref(buffer);
}

int turbo_tls_send(turbo_tls_client_t *client, const char *data, size_t length) {
  if (!client || !data || length == 0)
    return UV_EINVAL;

  turbo_arena_buffer_t *buffer = turbo_tls_get_send_buffer(client, length);
  if (!buffer)
    return UV_ENOMEM;

  memcpy(buffer->data, data, length);
  turbo_arena_buffer_set_used(buffer, length);

  int rc = turbo_tls_send_buffer(client, buffer, length);
  turbo_arena_buffer_unref(buffer);

  if (rc == 0) {
    TURBO_STATS_INC("tls.copy_sends");
  }

  return rc;
}

int turbo_tls_sendv(turbo_tls_client_t *client, const turbo_tls_iovec_t *iov, size_t iovcnt) {
  if (!client || !iov || iovcnt == 0)
    return UV_EINVAL;

  int rc = 0;

  /* Queue all buffers using zero-copy wrappers (NO MEMCPY!) */
  for (size_t i = 0; i < iovcnt && rc == 0; i++) {
    if (iov[i].len > 0 && iov[i].data) {
      /* Wrap user buffer - ZERO COPY! */
      turbo_arena_buffer_t *buffer = turbo_arena_wrap_external(
          (void *)iov[i].data, iov[i].len, NULL, /* No free callback - user manages memory */
          NULL);
      if (!buffer) {
        rc = UV_ENOMEM;
        break;
      }

      rc = turbo_tls_send_buffer(client, buffer, iov[i].len);
      turbo_arena_buffer_unref(buffer);
    }
  }

  /* Flush all queued buffers atomically */
  if (rc == 0) {
    rc = turbo_tls_flush(client);
  }

  if (rc == 0) {
    TURBO_STATS_INC("tls.scatter_gather_sends");
    TURBO_STATS_INC("tls.zero_copy_sendv");
  }

  return rc;
}

int turbo_tls_read_start(turbo_tls_client_t *client,
                         void (*alloc_cb)(turbo_tls_client_t *, size_t, uv_buf_t *),
                         turbo_recv_cb read_cb) {
  if (!client)
    return UV_EINVAL;

  client->alloc_cb = alloc_cb;
  client->on_recv = read_cb;

  do_tls_read(client); /* Process existing data */

  /* If do_tls_read delivered data synchronously, on_recv was cleared by read_stop.
     Skip uv_read_start — the coroutine already has its data. */
  if (!client->on_recv)
    return 0;

  return uv_read_start((uv_stream_t *)&client->handle, alloc_tls_recv_buffer, on_tls_read);
}

int turbo_tls_read_stop(turbo_tls_client_t *client) {
  if (!client)
    return UV_EINVAL;
  client->on_recv = NULL;
  return uv_read_stop((uv_stream_t *)&client->handle);
}

int turbo_tls_flush(turbo_tls_client_t *client) {
  if (!client || client->closing || client->write_in_progress)
    return UV_EINVAL;
  if (!client->send_queue_head)
    return 0;

  turbo_tls_session_t *session = (turbo_tls_session_t *)client->impl;
  turbo_tls_arena_pool_t *outgoing = (turbo_tls_arena_pool_t *)client->outgoing_ring;

  /* Capture starting position of outgoing buffer */
  turbo_tls_arena_pos_t start_pos = {outgoing->current_buffer,
                                     outgoing->current_buffer ? outgoing->current_buffer->used : 0};
  size_t start_size = turbo_tls_arena_pool_available(outgoing);

  /* Encrypt only the first queued entry to avoid arena overflow on large payloads.
     on_tls_write_complete will call flush again for remaining entries. */
  turbo_arena_buffer_t *current = client->send_queue_head;
  TLOG_DEBUG("SSL_write encrypting %zu bytes for client={}", current->used, (void *)client);
  int ssl_written = SSL_write(session->ssl, current->data, (int)current->used);
  if (ssl_written <= 0) {
    int ssl_error = SSL_get_error(session->ssl, ssl_written);
    TLOG_DEBUG("SSL_write failed for client={} err={}", (void *)client, ssl_error);
    turbo_tls_debug_ssl_errors("SSL_write error detail");
    return TURBO_TLS_EHANDSHAKE;
  }
  size_t plaintext_bytes = current->used;

  /* Advance send queue past the processed entry */
  client->send_queue_head = current->next;
  if (!client->send_queue_head)
    client->send_queue_tail = NULL;
  client->send_queue_bytes -= current->used;
  turbo_arena_buffer_unref(current);

  TLOG_DEBUG("encrypted %zu plaintext bytes for client={}", plaintext_bytes, (void *)client);

  /* Check if SSL generated encrypted data to send */
  size_t new_size = turbo_tls_arena_pool_available(outgoing);
  if (new_size <= start_size) {
    TLOG_DEBUG("no encrypted data generated for client={}", (void *)client);
    return 0; /* No encrypted data to send */
  }

  TLOG_DEBUG("sending %zu encrypted bytes for client={}", new_size - start_size, (void *)client);

  /* Send the encrypted data */
  turbo_tls_send_op_t *op = get_tls_send_op(client);
  if (!op)
    return UV_ENOMEM;

  op->req.data = op;
  op->client = client;

  uv_buf_t buf;
  int bufs_count = 1;
  op->commit_pos = turbo_tls_arena_pool_get_blocks(outgoing, start_pos, &buf, &bufs_count);

  client->write_in_progress = 1;
  int rc = uv_write(&op->req, (uv_stream_t *)&client->handle, &buf, (unsigned int)bufs_count,
                    on_tls_write_complete);

  if (rc != 0) {
    TLOG_DEBUG("uv_write failed for client={} rc={}", (void *)client, rc);
    client->write_in_progress = 0;
    return_tls_send_op(op);
    return rc;
  }

  TURBO_STATS_INC("tls.flush_calls");
  return 0;
}

/* Statistics and monitoring - simplified implementations */
void turbo_tls_get_stats(const turbo_tls_server_t *server, turbo_tls_stats_t *stats) {
  if (!server || !stats)
    return;
  memset(stats, 0, sizeof(*stats));

  /* Get arena statistics */
  turbo_arena_get_stats(&server->arena, &stats->arena_stats);

  /* Basic stats - would need proper implementation */
  stats->bytes_sent = 0;
  stats->bytes_received = 0;
  stats->active_connections = server->active_connections;
}

void turbo_tls_reset_stats(turbo_tls_server_t *server) {
  if (!server)
    return;

  /* Reset stats - would need proper implementation */
  (void)server; /* Suppress unused parameter warning */
}

void turbo_tls_trim_memory(turbo_tls_server_t *server) {
  if (!server)
    return;
  turbo_arena_trim(&server->arena);
}

size_t turbo_tls_get_memory_usage(const turbo_tls_server_t *server) {
  if (!server)
    return 0;

  turbo_arena_stats_t stats;
  turbo_arena_get_stats(&server->arena, &stats);
  return stats.total_allocated;
}

/* Cleanup global pools */
void turbo_tls_cleanup_pools(void) {
  while (g_tls_send_op_pool) {
    turbo_tls_send_op_t *op = g_tls_send_op_pool;
    g_tls_send_op_pool = op->next;
    free(op);
  }
  g_tls_send_op_pool_size = 0;

  TURBO_STATS_INC("tls.pools_cleaned");
}

/* Server lifecycle */
int turbo_tls_server_init(turbo_tls_server_t *server, uv_loop_t *loop, turbo_tls_context_t *context,
                          const char *host, unsigned short port) {
  if (!server || !loop || !context)
    return TURBO_TLS_EINVAL;

  memset(server, 0, sizeof(*server));

  server->loop = loop;
  server->context = context;
  server->active_connections = 0;

  /* Initialize arena */
  int rc = turbo_arena_init(&server->arena, 0);
  if (rc != 0)
    return rc;

  /* Initialize TCP handle */
  server->handle = (uv_tcp_t *)malloc(sizeof(uv_tcp_t));
  if (!server->handle) {
    turbo_arena_free(&server->arena);
    return UV_ENOMEM;
  }

  rc = uv_tcp_init(loop, server->handle);
  if (rc != 0) {
    free(server->handle);
    turbo_arena_free(&server->arena);
    return rc;
  }

  server->handle->data = server;

  /* Bind to address */
  struct sockaddr_in addr;
  rc = uv_ip4_addr(host, port, &addr);
  if (rc != 0) {
    free(server->handle);
    turbo_arena_free(&server->arena);
    return rc;
  }

  rc = uv_tcp_bind(server->handle, (const struct sockaddr *)&addr, 0);
  if (rc != 0) {
    free(server->handle);
    turbo_arena_free(&server->arena);
    return rc;
  }

  return 0;
}

int turbo_tls_client_get_peer_cert_pem(turbo_tls_client_t *client, char *buffer, size_t *length) {
  if (!client || !client->impl || !length)
    return TURBO_TLS_EINVAL;

  turbo_tls_session_t *session = (turbo_tls_session_t *)client->impl;
  X509 *peer_cert = SSL_get_peer_certificate(session->ssl);
  if (!peer_cert)
    return TURBO_TLS_ENOPEERCERT;

  BIO *bio = BIO_new(BIO_s_mem());
  if (!bio) {
    X509_free(peer_cert);
    return TURBO_TLS_UNKNOWN;
  }

  if (PEM_write_bio_X509(bio, peer_cert) != 1) {
    BIO_free(bio);
    X509_free(peer_cert);
    return TURBO_TLS_UNKNOWN;
  }

  char *data;
  long len = BIO_get_mem_data(bio, &data);

  if (buffer == NULL) {
    *length = (size_t)len + 1;
    BIO_free(bio);
    X509_free(peer_cert);
    return 0;
  }

  if (*length < (size_t)len + 1) {
    *length = (size_t)len + 1;
    BIO_free(bio);
    X509_free(peer_cert);
    return TURBO_TLS_EINVAL;
  }

  memcpy(buffer, data, (size_t)len);
  buffer[len] = '\0';
  *length = (size_t)len;

  BIO_free(bio);
  X509_free(peer_cert);
  return 0;
}
