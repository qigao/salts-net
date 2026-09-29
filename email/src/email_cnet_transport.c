#include "email_cnet_transport.h"

#include <salts/clock.h>
#include <salts/error_codes.h>
#include <salts_buffer.h>

#include <limits.h>
#include <stdio.h>
#include <string.h>

static int email_cnet_send_bytes(cnet_client *client,
                                 cnet_connection connection,
                                 const void *data,
                                 size_t size) {
  mem_buffer_t *buffer;
  int status;
  if (!client || !data || size == 0u) return SALTS_EINVAL;
  buffer = mem_get_buffer(mem_global(), size);
  if (!buffer) return SALTS_ENOMEM;
  memcpy(mem_buffer_data(buffer), data, size);
  mem_set_used(buffer, size);
  status = cnet_send_buffer(client, connection, buffer);
  mem_buffer_release(buffer);
  return status;
}

static native_io_backend_kind email_cnet_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static void email_cnet_set_error(email_cnet_transport_t *transport, int status, const char *stage) {
  transport->status = status;
  transport->stage = stage;
}

static void email_cnet_on_state(void *user, cnet_connection connection, cnet_connection_state state,
                                const cnet_error *error) {
  email_cnet_transport_t *transport = (email_cnet_transport_t *)user;
  (void)connection;

  if (state == CNET_CONNECTION_TLS_HANDSHAKING) {
    transport->connected = 0;
    transport->tls_handshaking = 1;
    return;
  }
  if (state == CNET_CONNECTION_CONNECTED) {
    transport->connected = 1;
    transport->terminal = 0;
    transport->tls_handshaking = 0;
    return;
  }
  if (state == CNET_CONNECTION_CLOSED || state == CNET_CONNECTION_FAILED) {
    transport->connected = 0;
    transport->terminal = 1;
    transport->terminal_failed = state == CNET_CONNECTION_FAILED;
    transport->tls_handshaking = 0;
    atomic_store_explicit(&transport->active, 0, memory_order_release);
    if (transport->status == SALTS_OK) {
      email_cnet_set_error(transport, error != NULL ? error->status : SALTS_ENOTCONN,
                           error != NULL && error->stage != NULL ? error->stage : "connection");
    }
  }
}

static void email_cnet_on_receive(void *user, cnet_connection connection,
                                  const cnet_receive_view *view) {
  email_cnet_transport_t *transport = (email_cnet_transport_t *)user;
  (void)connection;
  transport->receive_pending = 0;
  transport->receive_offset = 0u;
  transport->receive_size = 0u;

  if (view == NULL || view->kind != CNET_MESSAGE_BYTES || view->data == NULL || view->size == 0u) {
    transport->receive_error = SALTS_EPROTO;
    email_cnet_set_error(transport, SALTS_EPROTO, "receive callback");
    return;
  }
  if (view->size > sizeof(transport->receive_storage)) {
    transport->receive_error = SALTS_EMSGSIZE;
    email_cnet_set_error(transport, SALTS_EMSGSIZE, "receive buffer");
    return;
  }

  memcpy(transport->receive_storage, view->data, view->size);
  transport->receive_size = view->size;
  transport->receive_ready = 1;
}

static void email_cnet_on_send(void *user, cnet_connection connection, size_t size) {
  email_cnet_transport_t *transport = (email_cnet_transport_t *)user;
  (void)connection;
  (void)size;
  transport->send_pending = 0;
}

static uint32_t email_cnet_remaining_ms(uint64_t deadline) {
  const uint64_t now = salts_monotonic_ms();
  uint64_t remaining;
  if (now >= deadline) return 0u;
  remaining = deadline - now;
  return remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
}

static int email_cnet_cancel_if_interrupted(email_cnet_transport_t *transport) {
  const int interrupt_status =
      atomic_load_explicit(&transport->interrupt_status, memory_order_acquire);
  int close_status;
  if (interrupt_status == SALTS_OK) return SALTS_OK;

  transport->connected = 0;
  email_cnet_set_error(transport, interrupt_status, "interrupt");
  if (atomic_load_explicit(&transport->active, memory_order_acquire) != 0 && !transport->terminal) {
    close_status = cnet_close(&transport->client, transport->connection);
    if (close_status != SALTS_OK && close_status != SALTS_EALREADY &&
        close_status != SALTS_ENOENT) {
      email_cnet_set_error(transport, close_status, "interrupt close");
      return close_status;
    }
  }
  return interrupt_status;
}

static int email_cnet_poll(email_cnet_transport_t *transport, uint64_t deadline) {
  size_t events = 0u;
  uint32_t wait_ms;
  int status = email_cnet_cancel_if_interrupted(transport);
  if (status != SALTS_OK) return status;

  wait_ms = email_cnet_remaining_ms(deadline);
  if (wait_ms == 0u) {
    email_cnet_set_error(transport, SALTS_ETIMEDOUT, "deadline");
    if (atomic_load_explicit(&transport->active, memory_order_acquire) != 0 &&
        !transport->terminal) {
      (void)cnet_close(&transport->client, transport->connection);
      transport->connected = 0;
    }
    return SALTS_ETIMEDOUT;
  }

  status = cnet_client_poll(&transport->client, wait_ms, &events);
  if (status != SALTS_OK) {
    email_cnet_set_error(transport, status, "poll");
    return status;
  }
  return email_cnet_cancel_if_interrupted(transport);
}

static int email_cnet_wait_connected(email_cnet_transport_t *transport) {
  const uint64_t deadline = salts_monotonic_ms() + transport->timeout_ms;
  int status;
  while (!transport->connected && !transport->terminal) {
    status = email_cnet_poll(transport, deadline);
    if (status != SALTS_OK) return status;
  }
  if (!transport->connected) {
    return transport->status != SALTS_OK ? transport->status : SALTS_ENOTCONN;
  }
  return SALTS_OK;
}

static int email_cnet_make_uri(char *uri, size_t capacity, const char *host, uint16_t port,
                               int use_tls) {
  int written;
  if (host == NULL || host[0] == '\0' || port == 0u) return SALTS_EINVAL;
  if (strchr(host, ':') != NULL) {
    written =
        snprintf(uri, capacity, "%s://[%s]:%u", use_tls ? "tls" : "tcp", host, (unsigned int)port);
  } else {
    written =
        snprintf(uri, capacity, "%s://%s:%u", use_tls ? "tls" : "tcp", host, (unsigned int)port);
  }
  return written >= 0 && (size_t)written < capacity ? SALTS_OK : SALTS_EMSGSIZE;
}

int email_cnet_transport_init(email_cnet_transport_t *transport, int timeout_ms) {
  cnet_client_config config;
  int status;
  if (transport == NULL || timeout_ms < 0) return SALTS_EINVAL;

  memset(transport, 0, sizeof(*transport));
  transport->timeout_ms = timeout_ms > 0 ? (uint32_t)timeout_ms : EMAIL_CNET_DEFAULT_TIMEOUT_MS;
  atomic_init(&transport->active, 0);
  atomic_init(&transport->interrupt_status, SALTS_OK);
  config = (cnet_client_config){.backend = email_cnet_backend(),
                                .connection_capacity = 1u,
                                .command_capacity = EMAIL_CNET_QUEUE_CAPACITY,
                                .request_capacity = EMAIL_CNET_QUEUE_CAPACITY,
                                .completion_batch_capacity = EMAIL_CNET_QUEUE_CAPACITY,
                                .event_capacity = EMAIL_CNET_QUEUE_CAPACITY,
                                .max_send_bytes = EMAIL_CNET_MAX_SEND_BYTES,
                                .receive_buffer_bytes = EMAIL_CNET_RECEIVE_BYTES,
                                .connect_timeout_ms = transport->timeout_ms,
                                .read_timeout_ms = transport->timeout_ms,
                                .write_timeout_ms = transport->timeout_ms,
                                .tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES,
                                .tls_handshake_timeout_ms = transport->timeout_ms,
                                .command_buffer_bytes = EMAIL_CNET_MAX_SEND_BYTES,
                                .event_buffer_bytes = EMAIL_CNET_RECEIVE_BYTES};
  status = cnet_client_init(&transport->client, &config);
  if (status != SALTS_OK) {
    email_cnet_set_error(transport, status, "client init");
    return status;
  }
  transport->initialized = 1;
  return SALTS_OK;
}

int email_cnet_transport_destroy(email_cnet_transport_t *transport) {
  int destroy_status;
  if (transport == NULL) return SALTS_EINVAL;
  if (!transport->initialized) return SALTS_OK;

  atomic_store_explicit(&transport->interrupt_status, SALTS_OK, memory_order_release);
  (void)cnet_client_stop(&transport->client, EMAIL_CNET_STOP_TIMEOUT_MS);
  destroy_status = cnet_client_destroy(&transport->client);
  if (destroy_status == SALTS_OK) transport->initialized = 0;
  if (destroy_status != SALTS_OK) return destroy_status;
  return SALTS_OK;
}

int email_cnet_transport_connect(email_cnet_transport_t *transport, const char *host, uint16_t port,
                                 int use_tls) {
  char uri[EMAIL_CNET_URI_CAPACITY];
  cnet_connect_options options;
  int status;
  if (transport == NULL || !transport->initialized) return SALTS_EINVAL;
  if (atomic_load_explicit(&transport->active, memory_order_acquire) != 0) return SALTS_EALREADY;

  status = email_cnet_make_uri(uri, sizeof(uri), host, port, use_tls);
  if (status != SALTS_OK) {
    email_cnet_set_error(transport, status, "connect URI");
    return status;
  }

  transport->connection = (cnet_connection){0};
  transport->connected = 0;
  transport->terminal = 0;
  transport->terminal_failed = 0;
  transport->tls_handshaking = 0;
  transport->send_pending = 0;
  transport->receive_pending = 0;
  transport->receive_ready = 0;
  transport->receive_error = SALTS_OK;
  transport->receive_size = 0u;
  transport->receive_offset = 0u;
  transport->status = SALTS_OK;
  transport->stage = NULL;
  atomic_store_explicit(&transport->interrupt_status, SALTS_OK, memory_order_release);

  options = (cnet_connect_options){.uri = uri,
                                   .observer = {.on_state = email_cnet_on_state,
                                                .on_receive = email_cnet_on_receive,
                                                .user = transport,
                                                .on_send = email_cnet_on_send},
                                   .tls = NULL,
                                   .tls_client = NULL};
  status = cnet_connect(&transport->client, &options, &transport->connection);
  if (status != SALTS_OK) {
    email_cnet_set_error(transport, status, "connect admission");
    return status;
  }
  atomic_store_explicit(&transport->active, 1, memory_order_release);
  return email_cnet_wait_connected(transport);
}

int email_cnet_transport_start_tls(email_cnet_transport_t *transport, const char *server_name) {
  cnet_start_tls_options options = CNET_START_TLS_OPTIONS_INIT;
  int status;
  if (transport == NULL || server_name == NULL || server_name[0] == '\0') return SALTS_EINVAL;
  if (!transport->connected || transport->send_pending || transport->receive_pending ||
      transport->receive_offset != transport->receive_size) {
    return SALTS_EBUSY;
  }

  options.server_name = server_name;
  transport->connected = 0;
  transport->tls_handshaking = 1;
  status = cnet_start_tls(&transport->client, transport->connection, &options);
  if (status != SALTS_OK) {
    transport->connected = 1;
    transport->tls_handshaking = 0;
    email_cnet_set_error(transport, status, "STARTTLS admission");
    return status;
  }
  status = email_cnet_wait_connected(transport);
  if (status != SALTS_OK && transport->stage == NULL) {
    email_cnet_set_error(transport, status, "TLS handshake");
  }
  return status;
}

int email_cnet_transport_send(email_cnet_transport_t *transport, const void *data, size_t size) {
  const unsigned char *cursor = (const unsigned char *)data;
  size_t remaining = size;
  if (transport == NULL || (data == NULL && size != 0u)) return SALTS_EINVAL;
  if (!transport->connected) return SALTS_ENOTCONN;

  while (remaining > 0u) {
    const size_t chunk =
        remaining > EMAIL_CNET_MAX_SEND_BYTES ? EMAIL_CNET_MAX_SEND_BYTES : remaining;
    const uint64_t deadline = salts_monotonic_ms() + transport->timeout_ms;
    int status;
    transport->send_pending = 1;
    status = email_cnet_send_bytes(
        &transport->client, transport->connection, cursor, chunk);
    if (status != SALTS_OK) {
      transport->send_pending = 0;
      email_cnet_set_error(transport, status, "send admission");
      return status;
    }
    while (transport->send_pending && !transport->terminal) {
      status = email_cnet_poll(transport, deadline);
      if (status != SALTS_OK) return status;
    }
    if (transport->terminal) {
      return transport->status != SALTS_OK ? transport->status : SALTS_ENOTCONN;
    }
    cursor += chunk;
    remaining -= chunk;
  }
  return SALTS_OK;
}

int email_cnet_transport_receive(email_cnet_transport_t *transport, void *data, size_t capacity,
                                 size_t *out_size) {
  size_t available;
  size_t copied;
  int status;
  uint64_t deadline;
  if (out_size != NULL) *out_size = 0u;
  if (transport == NULL || data == NULL || capacity == 0u || out_size == NULL) {
    return SALTS_EINVAL;
  }
  if (!transport->connected) return SALTS_ENOTCONN;

  if (transport->receive_offset == transport->receive_size) {
    transport->receive_offset = 0u;
    transport->receive_size = 0u;
    transport->receive_ready = 0;
    transport->receive_error = SALTS_OK;
    transport->receive_pending = 1;
    status = cnet_receive(&transport->client, transport->connection, 1u);
    if (status != SALTS_OK) {
      transport->receive_pending = 0;
      email_cnet_set_error(transport, status, "receive admission");
      return status;
    }

    deadline = salts_monotonic_ms() + transport->timeout_ms;
    while (!transport->receive_ready && transport->receive_error == SALTS_OK &&
           !transport->terminal) {
      status = email_cnet_poll(transport, deadline);
      if (status != SALTS_OK) return status;
    }
    if (transport->receive_error != SALTS_OK) return transport->receive_error;
    if (!transport->receive_ready) {
      return transport->status != SALTS_OK ? transport->status : SALTS_ENOTCONN;
    }
  }

  available = transport->receive_size - transport->receive_offset;
  copied = available < capacity ? available : capacity;
  memcpy(data, transport->receive_storage + transport->receive_offset, copied);
  transport->receive_offset += copied;
  if (transport->receive_offset == transport->receive_size) {
    transport->receive_ready = 0;
  }
  *out_size = copied;
  return SALTS_OK;
}

int email_cnet_transport_close(email_cnet_transport_t *transport) {
  uint64_t deadline;
  int status;
  if (transport == NULL || !transport->initialized) return SALTS_EINVAL;
  if (atomic_load_explicit(&transport->active, memory_order_acquire) == 0) return SALTS_OK;

  atomic_store_explicit(&transport->interrupt_status, SALTS_OK, memory_order_release);
  if (!transport->terminal) {
    status = cnet_close(&transport->client, transport->connection);
    if (status != SALTS_OK && status != SALTS_EALREADY && status != SALTS_ENOENT) {
      email_cnet_set_error(transport, status, "close admission");
      return status;
    }
  }

  deadline = salts_monotonic_ms() + transport->timeout_ms;
  while (!transport->terminal) {
    size_t events = 0u;
    const uint32_t wait_ms = email_cnet_remaining_ms(deadline);
    if (wait_ms == 0u) {
      email_cnet_set_error(transport, SALTS_ETIMEDOUT, "close deadline");
      return SALTS_ETIMEDOUT;
    }
    status = cnet_client_poll(&transport->client, wait_ms, &events);
    if (status != SALTS_OK) {
      email_cnet_set_error(transport, status, "close poll");
      return status;
    }
  }

  transport->connection = (cnet_connection){0};
  transport->receive_size = 0u;
  transport->receive_offset = 0u;
  transport->receive_ready = 0;
  transport->receive_pending = 0;
  transport->send_pending = 0;
  return SALTS_OK;
}

int email_cnet_transport_interrupt(email_cnet_transport_t *transport, int status) {
  int expected = SALTS_OK;
  int wake_status;
  if (transport == NULL || status == SALTS_OK) return SALTS_EINVAL;
  if (atomic_load_explicit(&transport->active, memory_order_acquire) == 0) return SALTS_ENOTCONN;
  if (!atomic_compare_exchange_strong_explicit(&transport->interrupt_status, &expected, status,
                                               memory_order_release, memory_order_relaxed)) {
    return SALTS_EALREADY;
  }
  wake_status = cnet_client_wake(&transport->client);
  if (wake_status != SALTS_OK) {
    atomic_store_explicit(&transport->interrupt_status, SALTS_OK, memory_order_release);
  }
  return wake_status;
}

int email_cnet_transport_is_connected(const email_cnet_transport_t *transport) {
  return transport != NULL && transport->connected;
}

int email_cnet_transport_status(const email_cnet_transport_t *transport) {
  return transport != NULL ? transport->status : SALTS_EINVAL;
}

const char *email_cnet_transport_stage(const email_cnet_transport_t *transport) {
  return transport != NULL && transport->stage != NULL ? transport->stage : "transport";
}
