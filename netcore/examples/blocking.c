#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

typedef struct uv_client uv_handle;

struct uv_client {
  uv_tcp_t tcp;
  uv_connect_t *connect_req;
  int connected;
  int pending_writes;
  int close_pending;
  int blocking_mode;
  int reading;
  struct {
    struct queued_write {
      uv_write_t req;
      uv_buf_t *bufs; // Array of bufs for scatter-gather
      unsigned int nbufs;
      struct queued_write *next;
    } *head, *tail;
  } write_queue;
  struct {
    struct queued_read {
      char *data;
      size_t len;
      size_t capacity;
      struct queued_read *next;
    } *head, *tail;
    size_t total_bytes;
  } read_queue;
};

static uv_loop_t *loop;

static void on_connect(uv_connect_t *req, int status);
static void on_write(uv_write_t *req, int status);
static void on_shutdown(uv_shutdown_t *req, int status);
static void on_close(uv_handle_t *handle);
static void on_alloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf);
static void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);
static void drain_write_queue(uv_handle *h);
static void maybe_close(uv_handle *h);
static void run_loop_once(uv_handle *h, int until_closed);
static void free_queued_write(struct queued_write *qw);

// Alloc and read callbacks (unchanged)
static void on_alloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  (void)handle;
  char *chunk = malloc(suggested_size);
  if (chunk == NULL) {
    buf->base = NULL;
    buf->len = 0;
    return;
  }
  buf->base = chunk;
  buf->len = suggested_size;
}

static void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  uv_handle *h = (uv_handle *)stream->data;
  if (nread < 0) {
    if (nread != UV_EOF) {
      fprintf(stderr, "Read error: %s\n", uv_strerror(nread));
    }
    free(buf->base);
    uv_read_stop(stream);
    h->reading = 0;
    maybe_close(h);
    return;
  }
  if (nread == 0) {
    free(buf->base);
    return;
  }
  struct queued_read *qr = malloc(sizeof(struct queued_read));
  if (qr == NULL) {
    free(buf->base);
    return;
  }
  qr->data = malloc(nread);
  if (qr->data == NULL) {
    free(qr);
    free(buf->base);
    return;
  }
  memcpy(qr->data, buf->base, nread);
  qr->len = nread;
  qr->capacity = nread;
  qr->next = NULL;
  if (h->read_queue.head == NULL) {
    h->read_queue.head = h->read_queue.tail = qr;
  } else {
    h->read_queue.tail->next = qr;
    h->read_queue.tail = qr;
  }
  h->read_queue.total_bytes += nread;
  free(buf->base);
  printf("Received %zd bytes (queue now: %zu total)\n", nread, h->read_queue.total_bytes);
}

// Initialization (updated for scatter-gather write queue)
uv_handle *uv_init(void) {
  if (!loop) {
    loop = uv_default_loop();
  }
  uv_handle *h = calloc(1, sizeof(uv_handle));
  if (h == NULL) {
    return NULL;
  }
  h->tcp.data = h;
  if (uv_tcp_init(loop, &h->tcp) < 0) {
    free(h);
    return NULL;
  }
  h->connect_req = malloc(sizeof(uv_connect_t));
  if (h->connect_req == NULL) {
    uv_close((uv_handle_t *)&h->tcp, on_close);
    free(h);
    return NULL;
  }
  return h;
}

void uv_set_blocking(uv_handle *h, int blocking) {
  if (h)
    h->blocking_mode = !!blocking;
}

void uv_config(uv_handle *h, int type) {
  (void)type;
  uv_tcp_nodelay(&h->tcp, 1);
  uv_tcp_keepalive(&h->tcp, 1, 60);
}

int uv_connect(uv_handle *h, const char *endpoint) {
  if (h == NULL || endpoint == NULL)
    return UV_EINVAL;
  char host[256];
  int port;
  if (sscanf(endpoint, "%255[^:]:%d", host, &port) != 2) {
    return UV_EINVAL;
  }
  struct sockaddr_in addr;
  int r = uv_ip4_addr(host, port, &addr);
  if (r < 0)
    return r;
  h->connect_req->data = h;
  r = uv_tcp_connect(h->connect_req, &h->tcp, (const struct sockaddr *)&addr, on_connect);
  if (r < 0)
    return r;
  if (h->blocking_mode) {
    run_loop_once(h, 0);
  }
  return r;
}

static void on_connect(uv_connect_t *req, int status) {
  uv_handle *h = (uv_handle *)req->data;
  if (status < 0) {
    fprintf(stderr, "Connection failed: %s\n", uv_strerror(status));
    uv_close((uv_handle_t *)&h->tcp, on_close);
    return;
  }
  printf("Connected successfully.\n");
  h->connected = 1;
  h->tcp.data = h;
  drain_write_queue(h);
  if (uv_read_start((uv_stream_t *)&h->tcp, on_alloc, on_read) < 0) {
    fprintf(stderr, "Failed to start reading\n");
  } else {
    h->reading = 1;
  }
  // Don't free here - will be freed in on_close
  // Mark as NULL so on_close knows it's been consumed
  h->connect_req = NULL;
  free(req);
}

// Enhanced uv_send: scatter-gather for text/binary (bufs can be single for simple cases)
int uv_send(uv_handle *h, const uv_buf_t *bufs, unsigned int nbufs) {
  if (h == NULL || bufs == NULL || nbufs == 0)
    return UV_EINVAL;
  if (h->connected) {
    // Immediate scatter-write
    struct queued_write *qw = malloc(sizeof(struct queued_write));
    if (qw == NULL)
      return UV_ENOMEM;
    qw->bufs = malloc(sizeof(uv_buf_t) * nbufs);
    if (qw->bufs == NULL) {
      free(qw);
      return UV_ENOMEM;
    }
    // Copy bufs (shallow copy bases/lens; caller owns data)
    memcpy(qw->bufs, bufs, sizeof(uv_buf_t) * nbufs);
    qw->nbufs = nbufs;
    qw->req.data = qw;
    h->pending_writes++;
    int r = uv_write(&qw->req, (uv_stream_t *)&h->tcp, qw->bufs, nbufs, on_write);
    if (r < 0) {
      h->pending_writes--;
      free_queued_write(qw);
      return r;
    }
    if (h->blocking_mode) {
      run_loop_once(h, 0);
    }
    return r;
  } else {
    // Queue the scatter list
    struct queued_write *qw = malloc(sizeof(struct queued_write));
    if (qw == NULL)
      return UV_ENOMEM;
    qw->bufs = malloc(sizeof(uv_buf_t) * nbufs);
    if (qw->bufs == NULL) {
      free(qw);
      return UV_ENOMEM;
    }
    memcpy(qw->bufs, bufs, sizeof(uv_buf_t) * nbufs);
    qw->nbufs = nbufs;
    qw->next = NULL;
    if (h->write_queue.head == NULL) {
      h->write_queue.head = h->write_queue.tail = qw;
    } else {
      h->write_queue.tail->next = qw;
      h->write_queue.tail = qw;
    }
    return 0;
  }
}

// Backward compat: simple send for single text/binary buffer
int uv_send_simple(uv_handle *h, const void *data, size_t len) {
  uv_buf_t buf = uv_buf_init((char *)data, len);
  return uv_send(h, &buf, 1);
}

ssize_t uv_read(uv_handle *h, char *buffer, size_t maxlen) {
  if (h == NULL || buffer == NULL || maxlen == 0)
    return UV_EINVAL;

  // In blocking mode, wait for data to arrive
  if (h->blocking_mode && h->read_queue.head == NULL) {
    // Keep running the loop until we get data or connection closes
    int max_iterations = 100; // Prevent infinite loop
    while (h->read_queue.head == NULL && h->connected && max_iterations-- > 0) {
      if (uv_run(loop, UV_RUN_ONCE) == 0) {
        break; // No more events
      }
    }
  }

  if (h->read_queue.head == NULL) {
    return 0; // No data available
  }

  struct queued_read *qr = h->read_queue.head;
  size_t to_copy = (qr->len < maxlen) ? qr->len : maxlen;
  memcpy(buffer, qr->data, to_copy);
  h->read_queue.total_bytes -= to_copy;
  qr->len -= to_copy;
  memmove(qr->data, qr->data + to_copy, qr->len);
  if (qr->len == 0) {
    h->read_queue.head = qr->next;
    if (h->read_queue.head == NULL)
      h->read_queue.tail = NULL;
    free(qr->data);
    free(qr);
  }
  return (ssize_t)to_copy;
}

// Amp'd up: gather-read into multiple bufs (fills sequentially from queue)
ssize_t uv_readv(uv_handle *h, uv_buf_t *bufs, unsigned int nbufs) {
  if (h == NULL || bufs == NULL || nbufs == 0)
    return UV_EINVAL;

  // In blocking mode, wait for initial data
  if (h->blocking_mode && h->read_queue.head == NULL) {
    int max_iterations = 100;
    while (h->read_queue.head == NULL && h->connected && max_iterations-- > 0) {
      if (uv_run(loop, UV_RUN_ONCE) == 0) {
        break;
      }
    }
  }

  ssize_t total_read = 0;
  for (unsigned int i = 0; i < nbufs; i++) {
    if (h->read_queue.head == NULL) {
      break; // No more data available
    }
    size_t to_copy =
        (h->read_queue.head->len < bufs[i].len) ? h->read_queue.head->len : bufs[i].len;
    memcpy(bufs[i].base, h->read_queue.head->data, to_copy);
    h->read_queue.total_bytes -= to_copy;
    h->read_queue.head->len -= to_copy;
    memmove(h->read_queue.head->data, h->read_queue.head->data + to_copy, h->read_queue.head->len);
    total_read += to_copy;
    bufs[i].len -= to_copy;
    if (h->read_queue.head->len == 0) {
      struct queued_read *qr = h->read_queue.head;
      h->read_queue.head = qr->next;
      if (h->read_queue.head == NULL)
        h->read_queue.tail = NULL;
      free(qr->data);
      free(qr);
    }
  }
  return total_read;
}

static void drain_write_queue(uv_handle *h) {
  while (h->write_queue.head != NULL && h->connected) {
    struct queued_write *qw = h->write_queue.head;
    h->write_queue.head = qw->next;
    if (h->write_queue.head == NULL)
      h->write_queue.tail = NULL;
    h->pending_writes++;
    qw->req.data = qw;
    if (uv_write(&qw->req, (uv_stream_t *)&h->tcp, qw->bufs, qw->nbufs, on_write) < 0) {
      h->pending_writes--;
      free_queued_write(qw);
    }
  }
}

static void free_queued_write(struct queued_write *qw) {
  if (qw) {
    free(qw->bufs);
    free(qw);
  }
}

static void on_write(uv_write_t *req, int status) {
  if (status < 0) {
    fprintf(stderr, "Write failed: %s\n", uv_strerror(status));
  } else {
    printf("Data sent successfully (%u bufs).\n", ((struct queued_write *)req->data)->nbufs);
  }
  uv_handle *h = (uv_handle *)((uv_stream_t *)req->handle)->data;
  h->pending_writes--;
  struct queued_write *qw = (struct queued_write *)req->data;
  free_queued_write(qw);
  maybe_close(h);
}

// Renamed wrapper to avoid shadowing libuv's uv_close
void uv_handle_close(uv_handle *h) {
  if (h == NULL)
    return;
  if (h->reading) {
    uv_read_stop((uv_stream_t *)&h->tcp);
    h->reading = 0;
  }
  h->close_pending = 1;
  maybe_close(h);
  if (h->blocking_mode) {
    run_loop_once(h, 1);
  }
}

static void maybe_close(uv_handle *h) {
  if (h->close_pending && h->pending_writes == 0 && h->write_queue.head == NULL) {
    uv_shutdown_t *sreq = malloc(sizeof(uv_shutdown_t));
    if (sreq == NULL) {
      uv_close((uv_handle_t *)&h->tcp, on_close);
      return;
    }
    sreq->data = h;
    if (uv_shutdown(sreq, (uv_stream_t *)&h->tcp, on_shutdown) < 0) {
      free(sreq);
      uv_close((uv_handle_t *)&h->tcp, on_close);
    }
  }
}

static void on_shutdown(uv_shutdown_t *req, int status) {
  (void)status;
  uv_handle *h = (uv_handle *)req->data;
  uv_close((uv_handle_t *)&h->tcp, on_close);
  free(req);
}

static void on_close(uv_handle_t *hh) {
  uv_handle *h = (uv_handle *)hh->data;
  while (h->read_queue.head != NULL) {
    struct queued_read *qr = h->read_queue.head;
    h->read_queue.head = qr->next;
    free(qr->data);
    free(qr);
  }
  h->read_queue.total_bytes = 0;
  while (h->write_queue.head != NULL) {
    struct queued_write *qw = h->write_queue.head;
    h->write_queue.head = qw->next;
    free_queued_write(qw);
  }
  // Only free connect_req if it wasn't already freed in on_connect
  if (h->connect_req != NULL) {
    free(h->connect_req);
  }
  free(h);
  printf("Connection closed.\n");
}

static void run_loop_once(uv_handle *h, int until_closed) {
  if (until_closed) {
    // Run until handle is no longer active
    while (uv_is_active((uv_handle_t *)&h->tcp)) {
      uv_run(loop, UV_RUN_ONCE);
    }
  } else {
    // Run until connected or data available
    while (!h->connected && h->read_queue.total_bytes == 0) {
      if (uv_run(loop, UV_RUN_ONCE) == 0) {
        break; // No more events
      }
    }
  }
}

void uv_run_blocking(uv_handle *h) {
  if (h)
    run_loop_once(h, 1);
}

// Example: Text and binary scatter-gather sends/reads
int main(void) {
  uv_handle *handle = uv_init();
  if (handle == NULL) {
    fprintf(stderr, "Failed to initialize handle\n");
    return 1;
  }

  uv_set_blocking(handle, 1);
  uv_config(handle, 1);

  printf("Connecting to 127.0.0.1:8080...\n");
  if (uv_connect(handle, "127.0.0.1:8080") < 0) {
    fprintf(stderr, "Failed to connect\n");
    uv_handle_close(handle);
    return 1;
  }

  if (!handle->connected) {
    fprintf(stderr, "Connection failed - not connected after blocking connect\n");
    uv_handle_close(handle);
    return 1;
  }

  // Text send (simple)
  printf("Sending: 'Hello server!'\n");
  if (uv_send_simple(handle, "Hello server!", strlen("Hello server!")) < 0) {
    fprintf(stderr, "Failed to send text\n");
  }

  // Wait a bit for echo response
  printf("Waiting for echo response...\n");
  char recv_buf[128];
  ssize_t bytes = uv_read(handle, recv_buf, sizeof(recv_buf) - 1);
  if (bytes > 0) {
    recv_buf[bytes] = '\0';
    printf("Received echo: '%s' (%zd bytes)\n", recv_buf, bytes);
  } else {
    printf("No data received (bytes=%zd)\n", bytes);
  }

  // Binary scatter-gather send: e.g., header + payload
  printf("\nSending binary scatter-gather (header + payload)...\n");
  char header[4] = {0x01, 0x02, 0x03, 0x04};
  char payload[] = "Binary data here";
  uv_buf_t bufs[2] = {uv_buf_init(header, sizeof(header)), uv_buf_init(payload, strlen(payload))};
  if (uv_send(handle, bufs, 2) < 0) {
    fprintf(stderr, "Failed to send binary data\n");
  }

  // Read binary echo response
  printf("Waiting for binary echo response...\n");
  char recv_buf2[128];
  bytes = uv_read(handle, recv_buf2, sizeof(recv_buf2));
  if (bytes > 0) {
    printf("Received binary echo: %zd bytes\n", bytes);
    printf("  Header: %02x %02x %02x %02x\n", (unsigned char)recv_buf2[0],
           (unsigned char)recv_buf2[1], (unsigned char)recv_buf2[2], (unsigned char)recv_buf2[3]);
    if (bytes > 4) {
      recv_buf2[bytes] = '\0';
      printf("  Payload: '%s'\n", recv_buf2 + 4);
    }
  } else {
    printf("No binary data received (bytes=%zd)\n", bytes);
  }

  printf("\nClosing connection...\n");
  uv_handle_close(handle);

  return 0;
}