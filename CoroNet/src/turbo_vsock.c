/**
 * @file turbo_vsock.c
 * @brief Portable VSOCK endpoint adapter over the native stream backends.
 */

#include "CoroNet/turbo_coro_internal.h"
#include "turbo_stream_internal.h"

#include <string.h>

#if defined(__linux__) && TURBO_HAS_VSOCK
  #include <linux/vm_sockets.h>
  #include <sys/socket.h>
  #include <unistd.h>
#endif

int turbo_vsock_endpoint_to_sockaddr(const turbo_vsock_endpoint_t *endpoint, int for_bind,
                                     struct sockaddr_storage *addr, size_t *addr_len) {
  if (!endpoint || !addr || !addr_len) return TURBO_EINVAL;
  if (!for_bind &&
      (endpoint->cid == TURBO_VSOCK_CID_ANY || endpoint->port == TURBO_VSOCK_PORT_ANY)) {
    return TURBO_EINVAL;
  }

#if defined(__linux__) && TURBO_HAS_VSOCK
  {
    struct sockaddr_vm *vm = (struct sockaddr_vm *)addr;
    memset(addr, 0, sizeof(*addr));
    vm->svm_family = AF_VSOCK;
    vm->svm_cid = endpoint->cid;
    vm->svm_port = endpoint->port;
    *addr_len = sizeof(*vm);
  }
  return TURBO_OK;
#else
  UNUSED(for_bind);
  memset(addr, 0, sizeof(*addr));
  *addr_len = 0u;
  return TURBO_EPROTONOSUPPORT;
#endif
}

int turbo_vsock_endpoint_from_sockaddr(const struct sockaddr *addr, size_t addr_len,
                                       turbo_vsock_endpoint_t *endpoint) {
  if (!addr || !endpoint) return TURBO_EINVAL;

#if defined(__linux__) && TURBO_HAS_VSOCK
  if (addr_len < sizeof(struct sockaddr_vm) || addr->sa_family != AF_VSOCK) {
    return TURBO_EINVAL;
  }
  {
    const struct sockaddr_vm *vm = (const struct sockaddr_vm *)addr;
    endpoint->cid = vm->svm_cid;
    endpoint->port = vm->svm_port;
  }
  return TURBO_OK;
#else
  UNUSED(addr_len);
  return TURBO_EPROTONOSUPPORT;
#endif
}

int turbo_vsock_is_available(void) {
#if defined(__linux__) && TURBO_HAS_VSOCK
  int fd = socket(AF_VSOCK, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  close(fd);
  return 1;
#else
  return 0;
#endif
}

int turbo_stream_connect_vsock(turbo_stream_t *s, const turbo_vsock_endpoint_t *endpoint,
                               turbo_connect_cb on_connect, turbo_close_cb on_close) {
  struct sockaddr_storage addr;
  size_t addr_len;
  int rc;

  if (!s || s->kind != TURBO_STREAM_VSOCK) return TURBO_EINVAL;
  rc = turbo_vsock_endpoint_to_sockaddr(endpoint, 0, &addr, &addr_len);
  if (rc != 0) return rc;
  return turbo_stream_connect_addr_ex(s, (const struct sockaddr *)&addr, addr_len, on_connect,
                                      on_close);
}

turbo_stream_listener_t *turbo_stream_listen_vsock(coro_context_t *ctx,
                                                   const turbo_vsock_endpoint_t *endpoint,
                                                   int backlog, turbo_accept_cb on_accept) {
  return turbo_stream_listen_vsock_with_data(ctx, endpoint, backlog, on_accept, NULL);
}

turbo_stream_listener_t *turbo_stream_listen_vsock_with_data(coro_context_t *ctx,
                                                             const turbo_vsock_endpoint_t *endpoint,
                                                             int backlog, turbo_accept_cb on_accept,
                                                             void *user_data) {
  struct sockaddr_storage addr;
  size_t addr_len;
  int rc;

  if (!ctx || !endpoint || !on_accept) {
    return NULL;
  }
  rc = turbo_vsock_endpoint_to_sockaddr(endpoint, 1, &addr, &addr_len);
  if (rc != 0) {
    ctx->last_error = rc;
    return NULL;
  }
  return turbo_stream_listen_ex(ctx, TURBO_STREAM_VSOCK, (const struct sockaddr *)&addr, addr_len,
                                backlog, on_accept, 0, user_data);
}

int turbo_stream_get_local_vsock_endpoint(turbo_stream_t *s, turbo_vsock_endpoint_t *endpoint) {
  struct sockaddr_storage addr;
  int rc;

  if (!s || s->kind != TURBO_STREAM_VSOCK || !endpoint) return TURBO_EINVAL;
  rc = turbo_stream_get_local_addr(s, &addr);
  if (rc != 0) return rc;
  return turbo_vsock_endpoint_from_sockaddr((const struct sockaddr *)&addr, sizeof(addr), endpoint);
}

int turbo_stream_get_peer_vsock_endpoint(turbo_stream_t *s, turbo_vsock_endpoint_t *endpoint) {
  struct sockaddr_storage addr;
  int rc;

  if (!s || s->kind != TURBO_STREAM_VSOCK || !endpoint) return TURBO_EINVAL;
  rc = turbo_stream_get_peer_addr(s, &addr);
  if (rc != 0) return rc;
  return turbo_vsock_endpoint_from_sockaddr((const struct sockaddr *)&addr, sizeof(addr), endpoint);
}
