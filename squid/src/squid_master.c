/**
 * @file squid_master.c
 * @brief Squid master process implementation
 *
 * The master process manages multiple listeners across all transport protocols
 * and forwards data between clients and worker processes.
 *
 * Data Flow:
 *   Client <--[transport]--> Master <--[IPC pipe]--> Worker
 *
 * For TCP: Can either pass handle (legacy) or forward data (new mode)
 * For UDP/KCP/TLS/PIPE: Always forward data through master
 */

#include "squid.h"
#include "squid_internal.h"
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declarations */
static void squid_stop_workers(squid_master_t *master);
static int squid_spawn_workers(squid_master_t *master, unsigned int worker_count);
static squid_worker_t *squid_select_worker(squid_master_t *master);
static void squid_send_to_worker(squid_master_t *master, squid_worker_t *worker,
                                  squid_ipc_type_t type, uint64_t conn_id,
                                  uint8_t transport, const void *payload, size_t len);

/* ============================================================================
 * IPC Write Helpers
 * ============================================================================ */

typedef struct {
  uv_write_t req;
  squid_ipc_header_t header;
  char *payload;
} squid_ipc_write_t;

static void squid_ipc_write_cb(uv_write_t *req, int status) {
  squid_ipc_write_t *wr = (squid_ipc_write_t *)req;
  if (status != 0) {
    fprintf(stderr, "squid: IPC write error: %s\n", uv_strerror(status));
  }
  free(wr->payload);
  free(wr);
}

static void squid_send_to_worker(squid_master_t *master, squid_worker_t *worker,
                                  squid_ipc_type_t type, uint64_t conn_id,
                                  uint8_t transport, const void *payload, size_t len) {
  squid_ipc_write_t *wr = (squid_ipc_write_t *)calloc(1, sizeof(*wr));
  if (!wr) return;

  wr->header.type = (uint8_t)type;
  wr->header.transport = transport;
  wr->header.payload_len = (uint32_t)len;
  wr->header.connection_id = conn_id;

  int buf_count = 1;
  uv_buf_t bufs[2];

  /* Try to use shared memory for large payloads */
  if (len > 256 && len <= SQUID_SHM_SLOT_SIZE && master->shm) {
    int slot_id = squid_shm_alloc_slot(master->shm);
    if (slot_id >= 0) {
      /* Write to shared memory */
      if (squid_shm_write_slot(master->shm, slot_id, payload, len) == 0) {
        squid_shm_mark_ready(master->shm, slot_id);

        /* Send header with shm reference */
        wr->header.flags = SQUID_IPC_FLAG_USE_SHM;
        wr->header.shm_slot_id = (int8_t)slot_id;
        bufs[0] = uv_buf_init((char *)&wr->header, sizeof(wr->header));
        buf_count = 1;  /* No inline payload */

        goto send_ipc;
      }
      /* Failed to write to shm, fallback to inline */
      squid_shm_free_slot(master->shm, slot_id);
    }
  }

  /* Fallback: inline payload (old way) */
  wr->header.flags = 0;
  wr->header.shm_slot_id = -1;

  if (len > 0 && payload) {
    wr->payload = (char *)malloc(len);
    if (!wr->payload) {
      free(wr);
      return;
    }
    memcpy(wr->payload, payload, len);
    bufs[1] = uv_buf_init(wr->payload, (unsigned int)len);
    buf_count = 2;
  }

  bufs[0] = uv_buf_init((char *)&wr->header, sizeof(wr->header));

send_ipc:
  int rc = uv_write(&wr->req, (uv_stream_t *)&worker->pipe, bufs, buf_count, squid_ipc_write_cb);
  if (rc != 0) {
    fprintf(stderr, "squid: uv_write failed: %s\n", uv_strerror(rc));
    free(wr->payload);
    free(wr);
  }
}

/* ============================================================================
 * Worker Management
 * ============================================================================ */

static squid_worker_t *squid_select_worker(squid_master_t *master) {
  if (!master || master->worker_count == 0 || master->active_workers == 0)
    return NULL;

  unsigned int attempts = master->worker_count;
  unsigned int start_index = master->rr_counter;

  for (unsigned int i = 0; i < attempts; ++i) {
    unsigned int idx = (start_index + i) % master->worker_count;
    squid_worker_t *candidate = &master->workers[idx];
    if (candidate->running) {
      master->rr_counter = (idx + 1) % master->worker_count;
      return candidate;
    }
  }
  return NULL;
}

static void squid_on_worker_exit(uv_process_t *req, int64_t exit_status, int term_signal) {
  squid_worker_t *worker = (squid_worker_t *)req->data;
  squid_master_t *master = worker ? worker->master : NULL;

  fprintf(stderr, "squid: worker PID %d exited (status=%" PRId64 ", signal=%d)\n",
          req->pid, exit_status, term_signal);

  if (worker) {
    worker->running = 0;
    free(worker->read_buf);
    worker->read_buf = NULL;
    if (!uv_is_closing((uv_handle_t *)&worker->pipe))
      uv_close((uv_handle_t *)&worker->pipe, NULL);
  }

  if (master && master->active_workers > 0) {
    master->active_workers--;
  }

  uv_close((uv_handle_t *)req, NULL);
}

static void squid_worker_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  squid_worker_t *worker = (squid_worker_t *)handle->data;
  squid_alloc_buffer(&worker->read_buf, &worker->read_buf_size, &worker->read_buf_used,
                     suggested_size, buf);
}

static void squid_worker_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  squid_worker_t *worker = (squid_worker_t *)stream->data;
  squid_master_t *master = worker ? worker->master : NULL;
  (void)buf;

  if (nread < 0) {
    if (nread != UV_EOF) {
      fprintf(stderr, "squid: worker pipe read error: %s\n", uv_strerror((int)nread));
    }
    return;
  }

  if (nread == 0)
    return;

  worker->read_buf_used += (size_t)nread;

  /* Process complete messages */
  while (worker->read_buf_used >= sizeof(squid_ipc_header_t)) {
    squid_ipc_header_t *hdr = (squid_ipc_header_t *)worker->read_buf;
    size_t msg_size = sizeof(squid_ipc_header_t) + hdr->payload_len;

    if (worker->read_buf_used < msg_size)
      break;

    /* Handle worker message */
    if (hdr->type == SQUID_IPC_WORKER_SEND && master) {
      squid_conn_entry_t *entry = squid_conn_table_find(master, hdr->connection_id);
      if (entry && entry->connection) {
        const char *payload = worker->read_buf + sizeof(squid_ipc_header_t);
        async_server_send(entry->server, entry->connection, payload, hdr->payload_len);
      }
    }

    /* Shift buffer */
    size_t remaining = worker->read_buf_used - msg_size;
    if (remaining > 0) {
      memmove(worker->read_buf, worker->read_buf + msg_size, remaining);
    }
    worker->read_buf_used = remaining;
  }
}

static int squid_spawn_workers(squid_master_t *master, unsigned int worker_count) {
  if (worker_count == 0) {
    uv_cpu_info_t *info = NULL;
    int cpu_count = 0;
    if (uv_cpu_info(&info, &cpu_count) == 0) {
      uv_free_cpu_info(info, cpu_count);
      if (cpu_count > 0)
        worker_count = (unsigned int)cpu_count;
    }
    if (worker_count == 0)
      worker_count = 1;
  }

  master->workers = (squid_worker_t *)calloc(worker_count, sizeof(squid_worker_t));
  if (!master->workers)
    return UV_ENOMEM;

  char *args[2] = {master->worker_executable, NULL};

  for (unsigned int i = 0; i < worker_count; ++i) {
    squid_worker_t *worker = &master->workers[i];

    int rc = uv_pipe_init(master->loop, &worker->pipe, 1);
    if (rc != 0) {
      fprintf(stderr, "squid: uv_pipe_init failed: %s\n", uv_strerror(rc));
      master->worker_count = i;
      squid_stop_workers(master);
      return rc;
    }

    worker->master = master;
    worker->worker_id = (uint16_t)i;

    uv_stdio_container_t child_stdio[3];
    child_stdio[0].flags = (uv_stdio_flags)(UV_CREATE_PIPE | UV_READABLE_PIPE | UV_WRITABLE_PIPE);
#ifdef _WIN32
    child_stdio[0].flags = (uv_stdio_flags)(child_stdio[0].flags | UV_OVERLAPPED_PIPE);
#endif
    child_stdio[0].data.stream = (uv_stream_t *)&worker->pipe;
    child_stdio[1].flags = UV_IGNORE;
    child_stdio[2].flags = UV_INHERIT_FD;
    child_stdio[2].data.fd = 2;

    uv_process_options_t options = {0};
    options.file = args[0];
    options.args = args;
    options.stdio = child_stdio;
    options.stdio_count = 3;
    options.exit_cb = squid_on_worker_exit;

    rc = uv_spawn(master->loop, &worker->process, &options);
    if (rc != 0) {
      fprintf(stderr, "squid: uv_spawn failed: %s\n", uv_strerror(rc));
      master->worker_count = i;
      squid_stop_workers(master);
      return rc;
    }

    worker->process.data = worker;
    worker->pipe.data = worker;
    worker->running = 1;
    master->worker_count = i + 1;
    master->active_workers = i + 1;

    /* Start reading from worker */
    uv_read_start((uv_stream_t *)&worker->pipe, squid_worker_alloc_cb, squid_worker_read_cb);

    fprintf(stderr, "squid: started worker %u (PID %d)\n", i, worker->process.pid);
  }

  return 0;
}

static void squid_stop_workers(squid_master_t *master) {
  if (!master || !master->workers)
    return;

  for (unsigned int i = 0; i < master->worker_count; ++i) {
    squid_worker_t *worker = &master->workers[i];
    if (worker->running) {
      uv_process_kill(&worker->process, SIGTERM);
      worker->running = 0;
    }
    free(worker->read_buf);
    worker->read_buf = NULL;
    if (!uv_is_closing((uv_handle_t *)&worker->pipe))
      uv_close((uv_handle_t *)&worker->pipe, NULL);
  }

  free(master->workers);
  master->workers = NULL;
  master->worker_count = 0;
  master->active_workers = 0;
}

/* ============================================================================
 * Server Event Handler - Data Forwarding Mode
 * ============================================================================ */

static void squid_server_event_cb(async_server_t *server,
                                  const async_server_event_t *event,
                                  void *user_data) {
  squid_listener_t *listener = (squid_listener_t *)user_data;
  squid_master_t *master = listener ? listener->master : NULL;

  if (!master)
    return;

  switch (event->type) {
  case ASYNC_SERVER_EVENT_CONNECTION: {
    async_server_connection_t *connection = event->connection;

    squid_worker_t *worker = squid_select_worker(master);
    if (!worker) {
      fprintf(stderr, "squid: no workers available\n");
      async_server_close_connection(server, connection);
      return;
    }

    /* Generate connection ID and store in table */
    uint64_t conn_id = squid_generate_connection_id(worker->worker_id);
    squid_conn_entry_t *entry = squid_conn_table_add(master, conn_id, server, connection, worker->worker_id);
    if (!entry) {
      fprintf(stderr, "squid: failed to add connection to table\n");
      async_server_close_connection(server, connection);
      return;
    }

    /* Store connection_id in connection user_data for later lookup */
    async_server_connection_set_user_data(connection, (void *)(uintptr_t)conn_id);

    /* Build CONN_OPEN payload */
    squid_ipc_conn_open_t open_payload = {0};
    async_server_connection_info_t info;
    if (async_server_get_connection_info(connection, &info) == ASYNC_SERVER_STATUS_OK) {
      strncpy(open_payload.remote_address, info.remote_address, sizeof(open_payload.remote_address) - 1);
      open_payload.remote_port = info.remote_port;
      strncpy(open_payload.local_address, info.local_address, sizeof(open_payload.local_address) - 1);
      open_payload.local_port = info.local_port;
    }

    squid_send_to_worker(master, worker, SQUID_IPC_CONN_OPEN, conn_id,
                          (uint8_t)listener->transport, &open_payload, sizeof(open_payload));
    break;
  }

  case ASYNC_SERVER_EVENT_DATA: {
    async_server_connection_t *connection = event->connection;
    uint64_t conn_id = (uint64_t)(uintptr_t)async_server_connection_get_user_data(connection);

    squid_conn_entry_t *entry = squid_conn_table_find(master, conn_id);
    if (!entry) {
      fprintf(stderr, "squid: received data for unknown connection\n");
      return;
    }

    squid_worker_t *worker = &master->workers[entry->worker_id];
    if (!worker->running) {
      fprintf(stderr, "squid: worker not running for connection\n");
      return;
    }

    squid_send_to_worker(master, worker, SQUID_IPC_CONN_DATA, conn_id,
                          (uint8_t)listener->transport, event->data, event->length);
    break;
  }

  case ASYNC_SERVER_EVENT_DISCONNECTION: {
    async_server_connection_t *connection = event->connection;
    uint64_t conn_id = (uint64_t)(uintptr_t)async_server_connection_get_user_data(connection);

    squid_conn_entry_t *entry = squid_conn_table_find(master, conn_id);
    if (entry) {
      squid_worker_t *worker = &master->workers[entry->worker_id];
      if (worker->running) {
        squid_send_to_worker(master, worker, SQUID_IPC_CONN_CLOSE, conn_id,
                              (uint8_t)listener->transport, NULL, 0);
      }
      squid_conn_table_remove(master, conn_id);
    }
    break;
  }

  case ASYNC_SERVER_EVENT_ERROR:
    fprintf(stderr, "squid: listener error: %s\n", event->message ? event->message : "unknown");
    break;

  default:
    break;
  }
}

/* ============================================================================
 * Public API
 * ============================================================================ */

squid_master_t *squid_master_create(uv_loop_t *loop, const char *worker_executable) {
  if (!loop || !worker_executable)
    return NULL;

  squid_master_t *master = (squid_master_t *)calloc(1, sizeof(squid_master_t));
  if (!master)
    return NULL;

  master->loop = loop;
  master->handshake_token = ' ';

  size_t len = strlen(worker_executable);
  if (len >= sizeof(master->worker_executable)) {
    free(master);
    return NULL;
  }
  memcpy(master->worker_executable, worker_executable, len + 1);

  /* Initialize shared memory for zero-copy IPC */
  master->shm = squid_shm_create();
  if (!master->shm) {
    fprintf(stderr, "squid: failed to create shared memory\n");
    free(master);
    return NULL;
  }

  return master;
}

void squid_master_destroy(squid_master_t *master) {
  if (!master)
    return;

  squid_master_stop(master);
  squid_conn_table_clear(master);

  /* Destroy shared memory */
  if (master->shm) {
    squid_shm_destroy(master->shm);
    master->shm = NULL;
  }

  free(master);
}

int squid_master_listen(squid_master_t *master, const char *url) {
  if (!master || !url)
    return UV_EINVAL;

  turbo_address_t addr;
  if (parse_transport_url(url, &addr) != 0) {
    fprintf(stderr, "squid: failed to parse URL: %s\n", url);
    return UV_EINVAL;
  }

  squid_listener_t *listener = (squid_listener_t *)calloc(1, sizeof(*listener));
  if (!listener)
    return UV_ENOMEM;

  listener->transport = addr.transport;
  listener->async_server = async_server_create(squid_server_event_cb, listener);
  if (!listener->async_server) {
    free(listener);
    return UV_ENOMEM;
  }

  /* Apply TLS config if needed */
  if (addr.transport == TURBO_TLS && master->tls_config_set) {
    async_server_tls_config_t tls = {0};
    tls.cert_file = master->tls_config.cert_file;
    tls.key_file = master->tls_config.key_file;
    tls.ca_file = master->tls_config.ca_file;
    tls.verify_peer = master->tls_config.verify_peer;
    tls.cipher_list = master->tls_config.cipher_list;

    if (async_server_set_tls_config(listener->async_server, &tls) != ASYNC_SERVER_STATUS_OK) {
      async_server_destroy(listener->async_server);
      free(listener);
      return UV_EINVAL;
    }
  }

  if (async_server_listen(listener->async_server, url, 128) != ASYNC_SERVER_STATUS_OK) {
    async_server_destroy(listener->async_server);
    free(listener);
    return UV_EINVAL;
  }

  listener->master = master;
  listener->next = master->listeners;
  master->listeners = listener;

  fprintf(stderr, "squid: listening on %s\n", url);

  return 0;
}

int squid_master_set_tls_config(squid_master_t *master, const squid_tls_config_t *config) {
  if (!master || !config)
    return UV_EINVAL;

  master->tls_config = *config;
  master->tls_config_set = 1;
  return 0;
}

int squid_master_start(squid_master_t *master, unsigned int worker_count) {
  if (!master || !master->listeners)
    return UV_EINVAL;

  if (master->worker_count == 0) {
    int rc = squid_spawn_workers(master, worker_count);
    if (rc != 0) {
      squid_master_stop(master);
      return rc;
    }
  }

  fprintf(stderr, "squid: master started with %u workers (data forwarding mode)\n", master->worker_count);
  return 0;
}

void squid_master_stop(squid_master_t *master) {
  if (!master)
    return;

  squid_listener_t *listener = master->listeners;
  while (listener) {
    if (listener->async_server) {
      async_server_stop(listener->async_server);
      async_server_destroy(listener->async_server);
    }
    squid_listener_t *next = listener->next;
    free(listener);
    listener = next;
  }
  master->listeners = NULL;

  squid_stop_workers(master);
  squid_conn_table_clear(master);

  master->rr_counter = 0;
  fprintf(stderr, "squid: master stopped\n");
}
