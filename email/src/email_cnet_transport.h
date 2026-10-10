#ifndef EMAIL_CNET_TRANSPORT_H
#define EMAIL_CNET_TRANSPORT_H

#include <cnet/cnet.h>
#include <cnet/manager.h>

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

enum {
  EMAIL_CNET_RECEIVE_BYTES = 16 * 1024,
  EMAIL_CNET_MAX_SEND_BYTES = 64 * 1024,
  EMAIL_CNET_QUEUE_CAPACITY = 8,
  EMAIL_CNET_URI_CAPACITY = 512,
  EMAIL_CNET_DEFAULT_TIMEOUT_MS = 30000,
  EMAIL_CNET_STOP_TIMEOUT_MS = 5000
};

typedef struct email_cnet_transport_s {
  cnet_client client;
  cnet_manager manager;
  cnet_managed_connection managed;
  cnet_connection connection;
  uint32_t timeout_ms;
  int initialized;
  int connected;
  int terminal;
  int terminal_failed;
  int tls_handshaking;
  int send_pending;
  int receive_pending;
  int receive_ready;
  int receive_error;
  int status;
  const char *stage;
  unsigned char receive_storage[EMAIL_CNET_RECEIVE_BYTES];
  size_t receive_size;
  size_t receive_offset;
  atomic_int active;
  atomic_int interrupt_status;
} email_cnet_transport_t;

int email_cnet_transport_init(email_cnet_transport_t *transport, int timeout_ms);
int email_cnet_transport_destroy(email_cnet_transport_t *transport);
int email_cnet_transport_connect(email_cnet_transport_t *transport, const char *host, uint16_t port,
                                 int use_tls);
int email_cnet_transport_start_tls(email_cnet_transport_t *transport, const char *server_name);
int email_cnet_transport_send(email_cnet_transport_t *transport, const void *data, size_t size);
int email_cnet_transport_receive(email_cnet_transport_t *transport, void *data, size_t capacity,
                                 size_t *out_size);
int email_cnet_transport_close(email_cnet_transport_t *transport);
int email_cnet_transport_interrupt(email_cnet_transport_t *transport, int status);
int email_cnet_transport_is_connected(const email_cnet_transport_t *transport);
int email_cnet_transport_status(const email_cnet_transport_t *transport);
const char *email_cnet_transport_stage(const email_cnet_transport_t *transport);

#endif
