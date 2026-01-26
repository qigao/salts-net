/**
 * @file squid_worker.c
 * @brief Squid worker process implementation
 *
 * Worker processes receive connection events and data from the master via IPC.
 * They process application logic and send responses back through the master.
 *
 * IPC Protocol:
 *   Master -> Worker: CONN_OPEN, CONN_DATA, CONN_CLOSE
 *   Worker -> Master: WORKER_SEND
 */

#include "squid.h"
#include "squid_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

/* ============================================================================
 * Worker Context
 *
 * NOTE: Global state limits each process to one worker instance.
 * This is intentional - Squid spawns separate processes per worker.
 * ============================================================================ */

typedef struct {
  uv_loop_t *loop;
  uv_pipe_t pipe;
  char *read_buf;
  size_t read_buf_size;
  size_t read_buf_used;
  squid_shm_t *shm;  /* Shared memory (attached after fork) */
} squid_worker_ctx_t;

static squid_worker_ctx_t g_worker_ctx;
static squid_worker_callbacks_t g_worker_cbs;

/* ============================================================================
 * Worker API for sending responses
 * ============================================================================ */

typedef struct {
  uv_write_t req;
  squid_ipc_header_t header;
  char *payload;
} worker_write_t;

static void worker_write_cb(uv_write_t *req, int status) {
  worker_write_t *wr = (worker_write_t *)req;
  if (status != 0) {
    fprintf(stderr, "squid worker: write error: %s\n", uv_strerror(status));
  }
  free(wr->payload);
  free(wr);
}

int squid_worker_send(uint64_t connection_id, const void *data, size_t len) {
  if (!data || len == 0)
    return -1;

  worker_write_t *wr = (worker_write_t *)calloc(1, sizeof(*wr));
  if (!wr)
    return -1;

  wr->header.type = SQUID_IPC_WORKER_SEND;
  wr->header.payload_len = (uint32_t)len;
  wr->header.connection_id = connection_id;

  wr->payload = (char *)malloc(len);
  if (!wr->payload) {
    free(wr);
    return -1;
  }
  memcpy(wr->payload, data, len);

  uv_buf_t bufs[2];
  bufs[0] = uv_buf_init((char *)&wr->header, sizeof(wr->header));
  bufs[1] = uv_buf_init(wr->payload, (unsigned int)len);

  int rc = uv_write(&wr->req, (uv_stream_t *)&g_worker_ctx.pipe, bufs, 2, worker_write_cb);
  if (rc != 0) {
    free(wr->payload);
    free(wr);
    return rc;
  }

  return 0;
}

int squid_worker_close_connection(uint64_t connection_id) {
  /* Send close request to master - master will close the actual connection */
  worker_write_t *wr = (worker_write_t *)calloc(1, sizeof(*wr));
  if (!wr)
    return -1;

  wr->header.type = SQUID_IPC_CONN_CLOSE;
  wr->header.payload_len = 0;
  wr->header.connection_id = connection_id;

  uv_buf_t buf = uv_buf_init((char *)&wr->header, sizeof(wr->header));

  int rc = uv_write(&wr->req, (uv_stream_t *)&g_worker_ctx.pipe, &buf, 1, worker_write_cb);
  if (rc != 0) {
    free(wr);
    return rc;
  }

  return 0;
}

/* ============================================================================
 * IPC Message Processing
 * ============================================================================ */

static void worker_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  squid_worker_ctx_t *ctx = (squid_worker_ctx_t *)handle->data;
  squid_alloc_buffer(&ctx->read_buf, &ctx->read_buf_size, &ctx->read_buf_used,
                     suggested_size, buf);
}

static void worker_process_message(squid_ipc_header_t *hdr, const char *payload) {
  switch (hdr->type) {
  case SQUID_IPC_CONN_OPEN: {
    if (hdr->payload_len >= sizeof(squid_ipc_conn_open_t)) {
      squid_ipc_conn_open_t *open = (squid_ipc_conn_open_t *)payload;
      if (g_worker_cbs.on_connection) {
        squid_connection_t conn = {0};
        conn.id = hdr->connection_id;
        conn.transport = hdr->transport;
        strncpy(conn.remote_address, open->remote_address, sizeof(conn.remote_address) - 1);
        conn.remote_port = open->remote_port;
        strncpy(conn.local_address, open->local_address, sizeof(conn.local_address) - 1);
        conn.local_port = open->local_port;
        g_worker_cbs.on_connection(&conn, g_worker_cbs.user_data);
      }
    }
    break;
  }

  case SQUID_IPC_CONN_DATA: {
    if (g_worker_cbs.on_data) {
      g_worker_cbs.on_data(hdr->connection_id, payload, hdr->payload_len, g_worker_cbs.user_data);
    }
    break;
  }

  case SQUID_IPC_CONN_CLOSE: {
    if (g_worker_cbs.on_close) {
      g_worker_cbs.on_close(hdr->connection_id, g_worker_cbs.user_data);
    }
    break;
  }

  default:
    fprintf(stderr, "squid worker: unknown message type %d\n", hdr->type);
    break;
  }
}

static void worker_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  squid_worker_ctx_t *ctx = (squid_worker_ctx_t *)stream->data;
  (void)buf;

  if (nread < 0) {
    if (nread != UV_EOF) {
      fprintf(stderr, "squid worker: pipe read error: %s\n", uv_strerror((int)nread));
    }
    uv_close((uv_handle_t *)stream, NULL);
    return;
  }

  if (nread == 0)
    return;

  ctx->read_buf_used += (size_t)nread;

  /* Process complete messages */
  while (ctx->read_buf_used >= sizeof(squid_ipc_header_t)) {
    squid_ipc_header_t *hdr = (squid_ipc_header_t *)ctx->read_buf;

    /* Check if payload is in shared memory or inline */
    int use_shm = (hdr->flags & SQUID_IPC_FLAG_USE_SHM) != 0;
    size_t msg_size = sizeof(squid_ipc_header_t);
    if (!use_shm) {
      msg_size += hdr->payload_len;  /* Inline payload follows header */
    }

    /* Wait for complete message */
    if (ctx->read_buf_used < msg_size)
      break;

    const char *payload = NULL;
    char *shm_buf = NULL;

    if (use_shm && hdr->shm_slot_id >= 0 && ctx->shm) {
      /* Read from shared memory */
      shm_buf = (char *)malloc(hdr->payload_len);
      if (shm_buf) {
        ssize_t bytes_read = squid_shm_read_slot(ctx->shm, hdr->shm_slot_id,
                                                   shm_buf, hdr->payload_len);
        if (bytes_read > 0) {
          payload = shm_buf;
        }
        squid_shm_free_slot(ctx->shm, hdr->shm_slot_id);  /* Release slot */
      }
    } else {
      /* Inline payload */
      payload = ctx->read_buf + sizeof(squid_ipc_header_t);
    }

    if (payload) {
      worker_process_message(hdr, payload);
    }

    free(shm_buf);  /* Free temp buffer if used */

    /* Shift buffer */
    size_t remaining = ctx->read_buf_used - msg_size;
    if (remaining > 0) {
      memmove(ctx->read_buf, ctx->read_buf + msg_size, remaining);
    }
    ctx->read_buf_used = remaining;
  }
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int squid_worker_run(uv_loop_t *loop, uv_file pipe_fd,
                     const squid_worker_callbacks_t *callbacks) {
  memset(&g_worker_ctx, 0, sizeof(g_worker_ctx));
  memset(&g_worker_cbs, 0, sizeof(g_worker_cbs));

  if (callbacks) {
    g_worker_cbs = *callbacks;
  }

  g_worker_ctx.loop = loop ? loop : uv_default_loop();

  /* Attach to shared memory (created by master) */
  g_worker_ctx.shm = squid_shm_attach(NULL);
  if (!g_worker_ctx.shm) {
    fprintf(stderr, "squid worker: failed to attach shared memory\n");
    return -1;
  }

  if (pipe_fd == (uv_file)-1)
    pipe_fd = 0; /* stdin */

  int rc = uv_pipe_init(g_worker_ctx.loop, &g_worker_ctx.pipe, 1);
  if (rc != 0) {
    fprintf(stderr, "squid worker: uv_pipe_init failed: %s\n", uv_strerror(rc));
    return rc;
  }

  g_worker_ctx.pipe.data = &g_worker_ctx;

  rc = uv_pipe_open(&g_worker_ctx.pipe, pipe_fd);
  if (rc != 0) {
    fprintf(stderr, "squid worker: uv_pipe_open failed: %s\n", uv_strerror(rc));
    uv_close((uv_handle_t *)&g_worker_ctx.pipe, NULL);
    return rc;
  }

  rc = uv_read_start((uv_stream_t *)&g_worker_ctx.pipe, worker_alloc_cb, worker_read_cb);
  if (rc != 0) {
    fprintf(stderr, "squid worker: uv_read_start failed: %s\n", uv_strerror(rc));
    uv_close((uv_handle_t *)&g_worker_ctx.pipe, NULL);
    return rc;
  }

  fprintf(stderr, "squid worker: started (data forwarding mode)\n");

  if (!loop) {
    return uv_run(g_worker_ctx.loop, UV_RUN_DEFAULT);
  }

  return 0;
}

void squid_worker_stop(void) {
  if (!uv_is_closing((uv_handle_t *)&g_worker_ctx.pipe)) {
    uv_close((uv_handle_t *)&g_worker_ctx.pipe, NULL);
  }

  /* Detach shared memory */
  if (g_worker_ctx.shm) {
    /* Note: Worker doesn't destroy, only detaches */
    if (g_worker_ctx.shm->base_addr) {
#ifdef _WIN32
      UnmapViewOfFile(g_worker_ctx.shm->base_addr);
#else
      munmap(g_worker_ctx.shm->base_addr, g_worker_ctx.shm->total_size);
#endif
    }
    free(g_worker_ctx.shm);
    g_worker_ctx.shm = NULL;
  }

  free(g_worker_ctx.read_buf);
  g_worker_ctx.read_buf = NULL;
  g_worker_ctx.read_buf_size = 0;
  g_worker_ctx.read_buf_used = 0;
}

/* Legacy API - kept for backward compatibility but deprecated */
void squid_worker_close_client(uv_tcp_t *client) {
  if (!client)
    return;
  uv_close((uv_handle_t *)client, NULL);
}
