#include "turbo_stream_internal.h"

#include "turbo_error.h"

#include <limits.h>

#ifdef _WIN32
  #include <mstcpip.h>
#else
  #include <errno.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/socket.h>
#endif

static int stream_apply_linger(turbo_stream_native_socket_t socket,
                               const turbo_socket_linger_config_t *config) {
  struct linger value;

  if (!config) return TURBO_EINVAL;
  value.l_onoff = config->enabled ? 1 : 0;
  value.l_linger = config->enabled ? (int)((config->timeout_ms + 999u) / 1000u) : 0;
#ifdef _WIN32
  if (setsockopt(socket, SOL_SOCKET, SO_LINGER, (const char *)&value, sizeof(value)) != 0) {
    return -(int)WSAGetLastError();
  }
#else
  if (setsockopt(socket, SOL_SOCKET, SO_LINGER, &value, sizeof(value)) != 0) {
    return -errno;
  }
#endif
  return 0;
}

static int stream_apply_tcp_keepalive(turbo_stream_native_socket_t socket,
                                      const turbo_tcp_keepalive_config_t *config) {
  int enabled;

  if (!config) return TURBO_EINVAL;
  enabled = config->enabled ? 1 : 0;
#ifdef _WIN32
  if (setsockopt(socket, SOL_SOCKET, SO_KEEPALIVE, (const char *)&enabled, sizeof(enabled)) != 0) {
    return -(int)WSAGetLastError();
  }
  if (enabled && (config->idle_ms || config->interval_ms || config->count)) {
    struct tcp_keepalive keepalive;
    DWORD bytes_returned = 0;
    if (config->count) return TURBO_ENOTSUP;
    keepalive.onoff = 1;
    keepalive.keepalivetime = config->idle_ms ? config->idle_ms : 7200000u;
    keepalive.keepaliveinterval = config->interval_ms ? config->interval_ms : 1000u;
    if (WSAIoctl(socket, SIO_KEEPALIVE_VALS, &keepalive, sizeof(keepalive), NULL, 0,
                 &bytes_returned, NULL, NULL) != 0) {
      return -(int)WSAGetLastError();
    }
  }
#else
  if (setsockopt(socket, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled)) != 0) {
    return -errno;
  }
  if (enabled && config->idle_ms) {
#ifdef TCP_KEEPIDLE
    int seconds = (int)((config->idle_ms + 999u) / 1000u);
    if (setsockopt(socket, IPPROTO_TCP, TCP_KEEPIDLE, &seconds, sizeof(seconds)) != 0) {
      return -errno;
    }
#else
    return TURBO_ENOTSUP;
#endif
  }
  if (enabled && config->interval_ms) {
#ifdef TCP_KEEPINTVL
    int seconds = (int)((config->interval_ms + 999u) / 1000u);
    if (setsockopt(socket, IPPROTO_TCP, TCP_KEEPINTVL, &seconds, sizeof(seconds)) != 0) {
      return -errno;
    }
#else
    return TURBO_ENOTSUP;
#endif
  }
  if (enabled && config->count) {
#ifdef TCP_KEEPCNT
    int count = (int)config->count;
    if (setsockopt(socket, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count)) != 0) {
      return -errno;
    }
#else
    return TURBO_ENOTSUP;
#endif
  }
#endif
  return 0;
}

static int stream_apply_socket_buffer(turbo_stream_native_socket_t socket, int option,
                                      size_t bytes) {
  int value;
  if (bytes == 0u || bytes > (size_t)INT_MAX) return TURBO_ERANGE;
  value = (int)bytes;
#ifdef _WIN32
  if (setsockopt(socket, SOL_SOCKET, option, (const char *)&value, sizeof(value)) != 0) {
    return -(int)WSAGetLastError();
  }
#else
  if (setsockopt(socket, SOL_SOCKET, option, &value, sizeof(value)) != 0) {
    return -errno;
  }
#endif
  return 0;
}

int turbo_stream_apply_native_socket_options(turbo_stream_t *s,
                                             turbo_stream_native_socket_t socket) {
  int rc;

  if (!s) return TURBO_EINVAL;
  if (s->socket_recv_buffer_configured) {
    rc = stream_apply_socket_buffer(socket, SO_RCVBUF, s->socket_recv_buffer_bytes);
    if (rc != 0) return rc;
  }
  if (s->socket_send_buffer_configured) {
    rc = stream_apply_socket_buffer(socket, SO_SNDBUF, s->socket_send_buffer_bytes);
    if (rc != 0) return rc;
  }
  if (s->tcp_keepalive_configured) {
    rc = stream_apply_tcp_keepalive(socket, &s->tcp_keepalive_config);
    if (rc != 0) return rc;
  }
  if (s->linger_configured) {
    rc = stream_apply_linger(socket, &s->linger_config);
    if (rc != 0) return rc;
  }
  return 0;
}
