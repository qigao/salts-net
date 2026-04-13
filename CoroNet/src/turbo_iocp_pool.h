/**
 * @file turbo_iocp_pool.h
 * @brief Shared IOCP completion port pool for a coro_context_t.
 *
 * DESIGN (Good taste):
 * - ONE IOCP port shared by all stream/datagram sockets on a context
 * - Fixed worker thread count (auto-detect or user-specified, capped at 4)
 * - Disruptor MPMC ring for worker→loop completion delivery (lock-free)
 * - Completion key = pointer to iocp_op_t (no per-socket key indirection)
 * - Eliminates thread-per-connection overhead: O(N) threads → O(1)
 *
 * THREAD SAFETY:
 * - Worker threads: claim+publish to disruptor (lock-free multi-producer)
 * - Loop thread: consume from disruptor (single-consumer drain)
 * - Socket submission (WSASend/WSARecv): always on loop thread
 */

#ifndef TURBO_IOCP_POOL_H
#define TURBO_IOCP_POOL_H

#ifdef _WIN32

#include <windows.h>
#include <winsock2.h>
#include <stdint.h>

/* Forward declaration — full definition in turbo_coro_internal.h */
typedef struct coro_context_s coro_context_t;

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaque pool handle ───────────────────────────────────── */

typedef struct iocp_pool_s iocp_pool_t;

/* ── Completion op: the envelope every IOCP completion carries ── */

typedef enum {
  IOCP_OP_STREAM_CONNECT = 1,
  IOCP_OP_STREAM_SEND    = 2,
  IOCP_OP_STREAM_RECV    = 3,
  IOCP_OP_STREAM_ACCEPT  = 4,
  IOCP_OP_DG_SEND        = 5,
  IOCP_OP_DG_RECV        = 6,
} iocp_op_kind_t;

/**
 * @brief IOCP overlapped operation.
 *
 * OVERLAPPED must be the first field so CONTAINING_RECORD works.
 * The 'owner' is a void* discriminated by 'kind' — stream or datagram state.
 */
typedef struct iocp_op_s {
  OVERLAPPED overlapped;       /* must be first for CONTAINING_RECORD */
  iocp_op_kind_t kind;
  void *owner;                 /* per-socket state pointer */
  struct iocp_op_s *next;      /* intrusive list for batch processing */

  /* I/O fields */
  struct mem_buffer_s *buffer;
  int owns_buffer;
  size_t length;
  DWORD bytes_transferred;
  int status;
  DWORD flags;
  WSABUF wsabuf;
  uint64_t completed_ns;
  uint64_t tick_post_request_ns;

  /* Accept-specific */
  SOCKET client_socket;
  uint8_t accept_buf[(sizeof(struct sockaddr_storage) + 16) * 2];

  /* Datagram peer address */
  struct sockaddr_storage addr;
  int addr_len;
} iocp_op_t;

/* ── Pool lifecycle ───────────────────────────────────────── */

/**
 * @brief Create a shared IOCP pool for a context.
 *
 * @param ctx          Owning context (pool lifetime <= context lifetime)
 * @param num_workers  Worker thread count (0 = auto, capped at 4)
 * @return Pool handle or NULL on failure
 */
iocp_pool_t *iocp_pool_create(coro_context_t *ctx, int num_workers);

/**
 * @brief Destroy the pool: signal stop, join workers, free memory.
 */
void iocp_pool_destroy(iocp_pool_t *pool);

/* ── Socket registration ──────────────────────────────────── */

/**
 * @brief Associate a socket handle with the shared IOCP port.
 *
 * @param pool    Shared pool
 * @param socket  Socket handle to associate
 * @return 0 on success, negative error code on failure
 */
int iocp_pool_associate(iocp_pool_t *pool, SOCKET socket);

/**
 * @brief Get the underlying IOCP port handle (for AcceptEx etc.).
 */
HANDLE iocp_pool_get_port(iocp_pool_t *pool);

/* ── Completion drain (called from loop thread) ───────────── */

/**
 * @brief Drain all pending completions and invoke handlers.
 *
 * Called from coro_context_run(). Returns number of ops processed.
 * Each op is dispatched to the appropriate stream/datagram handler.
 *
 * @param pool  Shared pool
 * @return Number of ops drained
 */
int iocp_pool_drain(iocp_pool_t *pool);

/**
 * @brief Check if there are pending completions to drain.
 */
int iocp_pool_has_pending(iocp_pool_t *pool);

/**
 * @brief Get the owning context.
 */
coro_context_t *iocp_pool_get_ctx(iocp_pool_t *pool);

/* ── Completion dispatch callbacks (implemented by stream/datagram) ── */

/**
 * @brief Process a single completed IOCP op on the loop thread.
 *
 * Called by iocp_pool_drain for each dequeued completion.
 * The implementation lives in turbo_stream_iocp.c / turbo_datagram_iocp.c
 * and dispatches based on op->kind + op->owner.
 */
void iocp_pool_dispatch_op(iocp_op_t *op);

/**
 * @brief Increment the pool-wide inflight I/O counter.
 */
void iocp_pool_inflight_inc(iocp_pool_t *pool);

/**
 * @brief Decrement the pool-wide inflight I/O counter.
 */
void iocp_pool_inflight_dec(iocp_pool_t *pool);

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */
#endif /* TURBO_IOCP_POOL_H */
