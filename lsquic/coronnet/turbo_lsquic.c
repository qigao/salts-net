#include "CoroNet/turbo_lsquic.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "turbo_error.h"

struct turbo_lsquic_s {
  coro_context_t *context;
  turbo_datagram_t *datagram;
  lsquic_engine_t *engine;
  struct sockaddr_storage local_addr;
  void *peer_ctx;
  int closing;
};

static atomic_flag g_lsquic_global_lock = ATOMIC_FLAG_INIT;
static unsigned g_lsquic_global_refs;
static int g_lsquic_global_initialized;

static void turbo_lsquic_global_lock(void) {
  while (atomic_flag_test_and_set_explicit(&g_lsquic_global_lock,
                                           memory_order_acquire)) {
  }
}

static void turbo_lsquic_global_unlock(void) {
  atomic_flag_clear_explicit(&g_lsquic_global_lock, memory_order_release);
}

static int turbo_lsquic_global_acquire(void) {
  int rc = 0;

  turbo_lsquic_global_lock();
  if (!g_lsquic_global_initialized) {
    rc = lsquic_global_init(LSQUIC_GLOBAL_CLIENT | LSQUIC_GLOBAL_SERVER);
    if (rc == 0) {
      g_lsquic_global_initialized = 1;
    }
  }
  if (rc == 0) {
    ++g_lsquic_global_refs;
  }
  turbo_lsquic_global_unlock();
  return rc;
}

static void turbo_lsquic_global_release(void) {
  turbo_lsquic_global_lock();
  if (g_lsquic_global_refs > 0) {
    --g_lsquic_global_refs;
    if (g_lsquic_global_refs == 0 && g_lsquic_global_initialized) {
      lsquic_global_cleanup();
      g_lsquic_global_initialized = 0;
    }
  }
  turbo_lsquic_global_unlock();
}

static void turbo_lsquic_set_send_errno(int rc) {
  int native_error;

  if (rc == 0) {
    errno = 0;
    return;
  }

  switch (rc) {
  case TURBO_EADDRINUSE:
    errno = EADDRINUSE;
    return;
  case TURBO_EADDRNOTAVAIL:
    errno = EADDRNOTAVAIL;
    return;
  case TURBO_ECONNREFUSED:
    errno = ECONNREFUSED;
    return;
  case TURBO_ECONNRESET:
    errno = ECONNRESET;
    return;
  case TURBO_EHOSTUNREACH:
    errno = EHOSTUNREACH;
    return;
  case TURBO_EINVAL:
    errno = EINVAL;
    return;
  case TURBO_EMSGSIZE:
    errno = EMSGSIZE;
    return;
  case TURBO_ENETDOWN:
    errno = ENETDOWN;
    return;
  case TURBO_ENETUNREACH:
    errno = ENETUNREACH;
    return;
  case TURBO_ENOBUFS:
    errno = ENOBUFS;
    return;
  case TURBO_ENOMEM:
    errno = ENOMEM;
    return;
  case TURBO_ENOTCONN:
    errno = ENOTCONN;
    return;
  case TURBO_EPIPE:
    errno = EPIPE;
    return;
  case TURBO_ETIMEDOUT:
    errno = ETIMEDOUT;
    return;
  default:
    native_error = rc < 0 ? -rc : rc;
    errno = native_error > 0 && native_error < 4000 ? native_error : EIO;
  }
}

int turbo_lsquic_packets_out(void *packets_out_ctx,
                             const struct lsquic_out_spec *out_spec,
                             unsigned n_packets_out) {
  turbo_lsquic_t *adapter = (turbo_lsquic_t *)packets_out_ctx;
  unsigned sent_packets = 0;

  if (!adapter || adapter->closing || !adapter->datagram || !out_spec) {
    errno = EINVAL;
    return -1;
  }

  for (unsigned packet_index = 0; packet_index < n_packets_out;
       ++packet_index) {
    const struct lsquic_out_spec *spec = &out_spec[packet_index];
    size_t total_size = 0;
    mem_buffer_t *buffer;

    if (!spec->dest_sa || !spec->iov || spec->iovlen == 0) {
      errno = EINVAL;
      return sent_packets > 0 ? (int)sent_packets : -1;
    }

    for (size_t iov_index = 0; iov_index < spec->iovlen; ++iov_index) {
      const struct iovec *iov = &spec->iov[iov_index];
      if (iov->iov_len > SIZE_MAX - total_size ||
          (iov->iov_len > 0 && !iov->iov_base)) {
        errno = EOVERFLOW;
        return sent_packets > 0 ? (int)sent_packets : -1;
      }
      total_size += iov->iov_len;
    }

    if (total_size == 0) {
      errno = EINVAL;
      return sent_packets > 0 ? (int)sent_packets : -1;
    }

    buffer = turbo_datagram_get_send_buffer(adapter->datagram, total_size);
    if (!buffer) {
      errno = ENOMEM;
      return sent_packets > 0 ? (int)sent_packets : -1;
    }

    total_size = 0;
    for (size_t iov_index = 0; iov_index < spec->iovlen; ++iov_index) {
      const struct iovec *iov = &spec->iov[iov_index];
      memcpy(buffer->data + total_size, iov->iov_base, iov->iov_len);
      total_size += iov->iov_len;
    }
    mem_set_used(buffer, total_size);

    int rc = turbo_datagram_sendto_buffer(adapter->datagram, spec->dest_sa,
                                          buffer, total_size);
    mem_unref(buffer);
    if (rc != 0) {
      turbo_lsquic_set_send_errno(rc);
      return sent_packets > 0 ? (int)sent_packets : -1;
    }
    ++sent_packets;
  }

  errno = 0;
  return (int)sent_packets;
}

static int turbo_lsquic_packet_in(void *handle, const mem_slice_t *data,
                                  void *peer) {
  turbo_datagram_t *datagram = (turbo_datagram_t *)handle;
  turbo_lsquic_t *adapter;

  if (!datagram) {
    return 0;
  }
  adapter = (turbo_lsquic_t *)turbo_datagram_get_user_data(datagram);

  if (!adapter || !adapter->engine || !data || !data->data || data->length == 0 ||
      !peer) {
    return 0;
  }

  (void)lsquic_engine_packet_in(adapter->engine,
                                (const unsigned char *)data->data,
                                data->length,
                                (const struct sockaddr *)&adapter->local_addr,
                                (const struct sockaddr *)peer,
                                adapter->peer_ctx, 0);
  lsquic_engine_process_conns(adapter->engine);
  return 0;
}

int turbo_lsquic_create(const turbo_lsquic_config_t *config,
                        turbo_lsquic_t **out_adapter) {
  struct lsquic_engine_api engine_api;
  turbo_lsquic_t *adapter;
  int rc;

  if (!config || !out_adapter || !config->context || !config->bind_host ||
      !config->engine_api || !config->engine_api->ea_stream_if) {
    return TURBO_EINVAL;
  }
  *out_adapter = NULL;

  adapter = (turbo_lsquic_t *)calloc(1, sizeof(*adapter));
  if (!adapter) {
    return TURBO_ENOMEM;
  }
  adapter->context = config->context;
  adapter->peer_ctx = config->peer_ctx;

  if (turbo_lsquic_global_acquire() != 0) {
    free(adapter);
    return TURBO_EIO;
  }

  adapter->datagram = turbo_datagram_create(config->context,
                                            config->datagram_kind);
  if (!adapter->datagram) {
    rc = coro_context_get_last_error(config->context);
    turbo_lsquic_global_release();
    free(adapter);
    return rc != 0 ? rc : TURBO_EIO;
  }

  rc = turbo_datagram_bind(adapter->datagram, config->bind_host,
                           config->bind_port);
  if (rc != 0) {
    turbo_datagram_destroy(adapter->datagram);
    turbo_lsquic_global_release();
    free(adapter);
    return rc;
  }

  rc = turbo_datagram_get_local_addr(adapter->datagram, &adapter->local_addr);
  if (rc != 0) {
    turbo_datagram_destroy(adapter->datagram);
    turbo_lsquic_global_release();
    free(adapter);
    return rc;
  }

  engine_api = *config->engine_api;
  engine_api.ea_packets_out = turbo_lsquic_packets_out;
  engine_api.ea_packets_out_ctx = adapter;
  adapter->engine = lsquic_engine_new(config->engine_flags, &engine_api);
  if (!adapter->engine) {
    turbo_datagram_destroy(adapter->datagram);
    turbo_lsquic_global_release();
    free(adapter);
    return TURBO_EIO;
  }

  /* Pending completions use the datagram as their stable callback handle. */
  turbo_datagram_set_user_data(adapter->datagram, adapter);
  rc = turbo_datagram_recv_start(adapter->datagram, turbo_lsquic_packet_in);
  if (rc != 0) {
    turbo_datagram_set_user_data(adapter->datagram, NULL);
    lsquic_engine_destroy(adapter->engine);
    turbo_datagram_destroy(adapter->datagram);
    turbo_lsquic_global_release();
    free(adapter);
    return rc;
  }

  *out_adapter = adapter;
  return TURBO_OK;
}

void turbo_lsquic_destroy(turbo_lsquic_t *adapter) {
  lsquic_engine_t *engine;

  if (!adapter) {
    return;
  }

  adapter->closing = 1;
  turbo_datagram_set_user_data(adapter->datagram, NULL);
  engine = adapter->engine;
  adapter->engine = NULL;
  turbo_datagram_recv_stop(adapter->datagram);
  if (engine) {
    lsquic_engine_destroy(engine);
  }
  if (adapter->datagram) {
    turbo_datagram_destroy(adapter->datagram);
  }
  turbo_lsquic_global_release();
  free(adapter);
}

void turbo_lsquic_process(turbo_lsquic_t *adapter) {
  if (adapter && !adapter->closing && adapter->engine) {
    lsquic_engine_process_conns(adapter->engine);
  }
}

void turbo_lsquic_send_unsent(turbo_lsquic_t *adapter) {
  if (adapter && !adapter->closing && adapter->engine) {
    lsquic_engine_send_unsent_packets(adapter->engine);
  }
}

lsquic_engine_t *turbo_lsquic_engine(turbo_lsquic_t *adapter) {
  return adapter ? adapter->engine : NULL;
}

turbo_datagram_t *turbo_lsquic_datagram(turbo_lsquic_t *adapter) {
  return adapter ? adapter->datagram : NULL;
}
