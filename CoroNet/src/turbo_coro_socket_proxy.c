/**
 * @file turbo_coro_socket_proxy.c
 * @brief Outbound SOCKS5 and HTTP CONNECT tunnels for coroutine stream sockets.
 */

#include "CoroNet/turbo_coro_internal.h"
#include "base64_utils.h"
#include "fmt.h"
#include "turbo_error.h"
#include "turbo_str.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
#endif

enum {
  SOCKS5_VERSION = 0x05,
  SOCKS5_AUTH_NONE = 0x00,
  SOCKS5_AUTH_USERPASS = 0x02,
  SOCKS5_AUTH_UNACCEPTABLE = 0xff,
  SOCKS5_COMMAND_CONNECT = 0x01,
  SOCKS5_ADDRESS_IPV4 = 0x01,
  SOCKS5_ADDRESS_DOMAIN = 0x03,
  SOCKS5_ADDRESS_IPV6 = 0x04,
  HTTP_CONNECT_HEADER_LIMIT = 16 * 1024,
};

typedef struct proxy_reader_s {
  coro_socket_t *socket;
  char *chunk;
  size_t offset;
  size_t length;
  uint64_t deadline_ms;
} proxy_reader_t;

static int proxy_is_stream_transport(turbo_transport_t transport) {
  return transport == TURBO_TCP || transport == TURBO_TLS || transport == TURBO_WEBSOCKET;
}

static int proxy_socket_error(coro_socket_t *s, int error) {
  if (s && s->ctx) s->ctx->last_error = error;
  return error;
}

static int proxy_has_control_character(const char *value) {
  const unsigned char *p = (const unsigned char *)value;

  if (!value) return 0;
  while (*p) {
    if (*p < 0x20U || *p == 0x7fU) return 1;
    ++p;
  }
  return 0;
}

static int proxy_copy_text(char *out, size_t capacity, const char *value, int required) {
  size_t length;

  if (!out || capacity == 0U) return TURBO_EINVAL;
  if (!value) {
    if (required) return TURBO_EINVAL;
    out[0] = '\0';
    return 0;
  }

  length = strlen(value);
  if ((required && length == 0U) || length >= capacity) return TURBO_EINVAL;
  memcpy(out, value, length + 1U);
  return 0;
}

int coro_proxy_settings_copy(coro_proxy_settings_t *out, const coro_proxy_config_t *config) {
  coro_proxy_settings_t next;
  int has_username;
  int has_password;
  int rc;

  if (!out) return TURBO_EINVAL;
  memset(&next, 0, sizeof(next));
  if (!config || config->type == CORO_PROXY_DIRECT) {
    *out = next;
    return 0;
  }
  if (config->type != CORO_PROXY_SOCKS5 && config->type != CORO_PROXY_HTTP_CONNECT) {
    return TURBO_EINVAL;
  }
  if (!config->host || config->host[0] == '\0' || config->port == 0U ||
      proxy_has_control_character(config->host) || strchr(config->host, ' ')) {
    return TURBO_EINVAL;
  }

  has_username = config->username != NULL;
  has_password = config->password != NULL;
  if (has_username != has_password) return TURBO_EINVAL;

  rc = proxy_copy_text(next.host, sizeof(next.host), config->host, 1);
  if (rc != 0) return rc;
  if (has_username) {
    rc = proxy_copy_text(next.username, sizeof(next.username), config->username, 0);
    if (rc != 0) return rc;
    rc = proxy_copy_text(next.password, sizeof(next.password), config->password, 0);
    if (rc != 0) return rc;

    if (config->type == CORO_PROXY_SOCKS5 &&
        (next.username[0] == '\0' || next.password[0] == '\0')) {
      return TURBO_EINVAL;
    }
    if (config->type == CORO_PROXY_HTTP_CONNECT &&
        (strchr(next.username, ':') || proxy_has_control_character(next.username) ||
         proxy_has_control_character(next.password))) {
      return TURBO_EINVAL;
    }
    next.auth_enabled = 1;
  }

  next.type = config->type;
  next.port = config->port;
  *out = next;
  return 0;
}

int coro_socket_set_proxy(coro_socket_t *s, const coro_proxy_config_t *config) {
  coro_proxy_settings_t next;
  int rc;

  if (!s) return TURBO_EINVAL;
  if (!proxy_is_stream_transport(s->transport)) return proxy_socket_error(s, TURBO_ENOTSUP);
  if (s->connected || s->co_wait || s->listener) return proxy_socket_error(s, TURBO_EINVAL);

  rc = coro_proxy_settings_copy(&next, config);
  if (rc != 0) return proxy_socket_error(s, rc);
  s->proxy = next;
  proxy_socket_error(s, 0);
  return 0;
}

int coro_socket_clear_proxy(coro_socket_t *s) { return coro_socket_set_proxy(s, NULL); }

static uint64_t proxy_deadline(coro_socket_t *s, uint64_t timeout_ms) {
  uint64_t now;

  if (!s || !s->loop || timeout_ms == 0U) return 0U;
  now = turbo_loop_now(s->loop);
  if (UINT64_MAX - now < timeout_ms) return UINT64_MAX;
  return now + timeout_ms;
}

static int proxy_apply_remaining_timeout(coro_socket_t *s, uint64_t deadline_ms) {
  uint64_t now;

  if (!s || deadline_ms == 0U) return 0;
  now = turbo_loop_now(s->loop);
  if (now >= deadline_ms) return TURBO_ETIMEDOUT;
  s->timeout_ms = deadline_ms - now;
  return 0;
}

static int proxy_send(coro_socket_t *s, uint64_t deadline_ms, const unsigned char *data,
                      size_t length) {
  int rc = proxy_apply_remaining_timeout(s, deadline_ms);
  if (rc != 0) return rc;
  return coro_socket_send_raw_internal(s, (const char *)data, length);
}

static void proxy_reader_release(proxy_reader_t *reader) {
  if (!reader) return;
  coro_socket_free_recv(reader->chunk);
  reader->chunk = NULL;
  reader->offset = 0U;
  reader->length = 0U;
}

static int proxy_reader_fill(proxy_reader_t *reader) {
  char *chunk = NULL;
  size_t length = 0U;
  int rc;

  if (!reader || !reader->socket) return TURBO_EINVAL;
  proxy_reader_release(reader);
  rc = proxy_apply_remaining_timeout(reader->socket, reader->deadline_ms);
  if (rc != 0) return rc;
  rc = coro_socket_recv_raw_internal(reader->socket, &chunk, &length);
  if (rc != 0) return rc;
  if (!chunk || length == 0U) {
    coro_socket_free_recv(chunk);
    return TURBO_EOF;
  }
  reader->chunk = chunk;
  reader->length = length;
  return 0;
}

static int proxy_reader_read_exact(proxy_reader_t *reader, unsigned char *out, size_t wanted) {
  size_t copied = 0U;

  if (!reader || (!out && wanted != 0U)) return TURBO_EINVAL;
  while (copied < wanted) {
    size_t available;
    size_t take;
    int rc;

    if (reader->offset == reader->length) {
      rc = proxy_reader_fill(reader);
      if (rc != 0) return rc;
    }
    available = reader->length - reader->offset;
    take = wanted - copied;
    if (take > available) take = available;
    memcpy(out + copied, reader->chunk + reader->offset, take);
    reader->offset += take;
    copied += take;
  }
  return 0;
}

static int proxy_stash_received(coro_socket_t *s, const char *data, size_t length) {
  mem_slice_t slice;

  if (length == 0U) return 0;
  memset(&slice, 0, sizeof(slice));
  slice.data = (char *)data;
  slice.length = length;
  coro_deliver_recv(s, &slice);
  return s->status;
}

static int proxy_reader_finish(proxy_reader_t *reader) {
  int rc = 0;

  if (!reader) return TURBO_EINVAL;
  if (reader->chunk && reader->offset < reader->length) {
    rc = proxy_stash_received(reader->socket, reader->chunk + reader->offset,
                              reader->length - reader->offset);
  }
  proxy_reader_release(reader);
  return rc;
}

static int socks5_reply_error(unsigned char reply) {
  switch (reply) {
  case 0x01:
    return TURBO_EIO;
  case 0x02:
    return TURBO_EPERM;
  case 0x03:
    return TURBO_ENETUNREACH;
  case 0x04:
    return TURBO_EHOSTUNREACH;
  case 0x05:
    return TURBO_ECONNREFUSED;
  case 0x06:
    return TURBO_ETIMEDOUT;
  case 0x07:
    return TURBO_ENOTSUP;
  case 0x08:
    return TURBO_EPROTONOSUPPORT;
  default:
    return TURBO_EPROTO;
  }
}

static int socks5_authenticate(coro_socket_t *s, const coro_proxy_settings_t *proxy,
                               proxy_reader_t *reader) {
  unsigned char request[3U + 255U + 255U];
  unsigned char response[2];
  size_t username_length = strlen(proxy->username);
  size_t password_length = strlen(proxy->password);
  size_t offset = 0U;
  int rc;

  request[offset++] = 0x01;
  request[offset++] = (unsigned char)username_length;
  memcpy(request + offset, proxy->username, username_length);
  offset += username_length;
  request[offset++] = (unsigned char)password_length;
  memcpy(request + offset, proxy->password, password_length);
  offset += password_length;

  rc = proxy_send(s, reader->deadline_ms, request, offset);
  if (rc != 0) return rc;
  rc = proxy_reader_read_exact(reader, response, sizeof(response));
  if (rc != 0) return rc;
  return (response[0] == 0x01 && response[1] == 0x00) ? 0 : TURBO_EPERM;
}

static int socks5_send_connect(coro_socket_t *s, const char *target_host, uint16_t target_port,
                               uint64_t deadline_ms) {
  unsigned char request[4U + 1U + 255U + 2U];
  unsigned char address[16];
  size_t host_length = strlen(target_host);
  size_t offset = 0U;

  request[offset++] = SOCKS5_VERSION;
  request[offset++] = SOCKS5_COMMAND_CONNECT;
  request[offset++] = 0x00;
  if (inet_pton(AF_INET, target_host, address) == 1) {
    request[offset++] = SOCKS5_ADDRESS_IPV4;
    memcpy(request + offset, address, 4U);
    offset += 4U;
  } else if (inet_pton(AF_INET6, target_host, address) == 1) {
    request[offset++] = SOCKS5_ADDRESS_IPV6;
    memcpy(request + offset, address, 16U);
    offset += 16U;
  } else {
    if (host_length == 0U || host_length > 255U) return TURBO_EINVAL;
    request[offset++] = SOCKS5_ADDRESS_DOMAIN;
    request[offset++] = (unsigned char)host_length;
    memcpy(request + offset, target_host, host_length);
    offset += host_length;
  }
  request[offset++] = (unsigned char)(target_port >> 8U);
  request[offset++] = (unsigned char)(target_port & 0xffU);
  return proxy_send(s, deadline_ms, request, offset);
}

static int socks5_receive_connect_reply(proxy_reader_t *reader) {
  unsigned char header[4];
  unsigned char discarded[258];
  size_t address_length;
  int rc = proxy_reader_read_exact(reader, header, sizeof(header));

  if (rc != 0) return rc;
  if (header[0] != SOCKS5_VERSION || header[2] != 0x00) return TURBO_EPROTO;
  if (header[1] != 0x00) return socks5_reply_error(header[1]);
  switch (header[3]) {
  case SOCKS5_ADDRESS_IPV4:
    address_length = 4U;
    break;
  case SOCKS5_ADDRESS_IPV6:
    address_length = 16U;
    break;
  case SOCKS5_ADDRESS_DOMAIN:
    rc = proxy_reader_read_exact(reader, discarded, 1U);
    if (rc != 0) return rc;
    address_length = discarded[0];
    break;
  default:
    return TURBO_EPROTO;
  }
  return proxy_reader_read_exact(reader, discarded, address_length + 2U);
}

static int socks5_connect(coro_socket_t *s, const char *target_host, uint16_t target_port,
                          uint64_t deadline_ms) {
  const coro_proxy_settings_t *proxy = &s->proxy;
  proxy_reader_t reader;
  unsigned char negotiation[3];
  unsigned char selection[2];
  int rc;

  memset(&reader, 0, sizeof(reader));
  reader.socket = s;
  reader.deadline_ms = deadline_ms;
  negotiation[0] = SOCKS5_VERSION;
  negotiation[1] = 1;
  negotiation[2] = proxy->auth_enabled ? SOCKS5_AUTH_USERPASS : SOCKS5_AUTH_NONE;

  rc = proxy_send(s, deadline_ms, negotiation, sizeof(negotiation));
  if (rc == 0) rc = proxy_reader_read_exact(&reader, selection, sizeof(selection));
  if (rc == 0 && selection[0] != SOCKS5_VERSION) rc = TURBO_EPROTO;
  if (rc == 0 && selection[1] == SOCKS5_AUTH_UNACCEPTABLE) rc = TURBO_EPERM;
  if (rc == 0 && selection[1] != negotiation[2]) rc = TURBO_EPERM;
  if (rc == 0 && selection[1] == SOCKS5_AUTH_USERPASS) {
    rc = socks5_authenticate(s, proxy, &reader);
  }
  if (rc == 0) rc = socks5_send_connect(s, target_host, target_port, deadline_ms);
  if (rc == 0) rc = socks5_receive_connect_reply(&reader);
  if (rc == 0) rc = proxy_reader_finish(&reader);
  else proxy_reader_release(&reader);
  return rc;
}

static int http_connect_status(const char *header, size_t length) {
  const char *line_end;
  const char *space;
  int status;

  if (!header || length < 12U || memcmp(header, "HTTP/", 5U) != 0) return TURBO_EPROTO;
  line_end = strstr(header, "\r\n");
  if (!line_end) return TURBO_EPROTO;
  space = memchr(header, ' ', (size_t)(line_end - header));
  if (!space || line_end - space < 4 || space[1] < '0' || space[1] > '9' || space[2] < '0' ||
      space[2] > '9' || space[3] < '0' || space[3] > '9' ||
      (space + 4 < line_end && space[4] != ' ' && space[4] != '\t')) {
    return TURBO_EPROTO;
  }
  status = (space[1] - '0') * 100 + (space[2] - '0') * 10 + (space[3] - '0');
  if (status >= 200 && status < 300) return 0;
  if (status == 407) return TURBO_EPERM;
  return TURBO_ECONNREFUSED;
}

static int http_connect_read_response(coro_socket_t *s, uint64_t deadline_ms) {
  char header[HTTP_CONNECT_HEADER_LIMIT + 1U];
  size_t header_length = 0U;

  for (;;) {
    char *chunk = NULL;
    size_t chunk_length = 0U;
    size_t i;
    int rc = proxy_apply_remaining_timeout(s, deadline_ms);

    if (rc != 0) return rc;
    rc = coro_socket_recv_raw_internal(s, &chunk, &chunk_length);
    if (rc != 0) return rc;
    if (!chunk || chunk_length == 0U) {
      coro_socket_free_recv(chunk);
      return TURBO_EOF;
    }

    for (i = 0U; i < chunk_length; ++i) {
      if (header_length == HTTP_CONNECT_HEADER_LIMIT) {
        coro_socket_free_recv(chunk);
        return TURBO_EPROTO;
      }
      header[header_length++] = chunk[i];
      if (header_length >= 4U && memcmp(header + header_length - 4U, "\r\n\r\n", 4U) == 0) {
        int status;
        header[header_length] = '\0';
        status = http_connect_status(header, header_length);
        if (status == 0 && i + 1U < chunk_length) {
          status = proxy_stash_received(s, chunk + i + 1U, chunk_length - i - 1U);
        }
        coro_socket_free_recv(chunk);
        return status;
      }
    }
    coro_socket_free_recv(chunk);
  }
}

static tstr_t http_connect_authority(const char *host, uint16_t port) {
  size_t length = strlen(host);

  if (length >= 2U && host[0] == '[' && host[length - 1U] == ']') {
    return tstr_format("{}:{}", host, port);
  }
  if (strchr(host, ':')) return tstr_format("[{}]:{}", host, port);
  return tstr_format("{}:{}", host, port);
}

static int http_connect(coro_socket_t *s, const char *target_host, uint16_t target_port,
                        uint64_t deadline_ms) {
  const coro_proxy_settings_t *proxy = &s->proxy;
  tstr_t authority = NULL;
  tstr_t request = NULL;
  tstr_t userpass = NULL;
  tn_base64_string_result_t encoded;
  tstr_t updated;
  int rc = TURBO_ENOMEM;

  memset(&encoded, 0, sizeof(encoded));
  authority = http_connect_authority(target_host, target_port);
  if (!authority) goto cleanup;
  request = tstr_format("CONNECT {} HTTP/1.1\r\nHost: {}\r\n", authority, authority);
  if (!request) goto cleanup;

  if (proxy->auth_enabled) {
    userpass = tstr_format("{}:{}", proxy->username, proxy->password);
    if (!userpass) goto cleanup;
    encoded = tn_base64_encode_ex((const uint8_t *)userpass, tstr_len(userpass));
    if (!encoded.ok) {
      rc = encoded.error == TN_BASE64_ERR_NO_MEMORY ? TURBO_ENOMEM : TURBO_EINVAL;
      goto cleanup;
    }
    updated = tstr_append_format(request, "Proxy-Authorization: Basic {}\r\n", encoded.value);
    if (!updated) goto cleanup;
    request = updated;
  }
  updated = tstr_cat(request, "\r\n");
  if (!updated) goto cleanup;
  request = updated;

  rc = proxy_send(s, deadline_ms, (const unsigned char *)request, tstr_len(request));
  if (rc == 0) rc = http_connect_read_response(s, deadline_ms);

cleanup:
  free(encoded.ok ? encoded.value : NULL);
  tstr_free(userpass);
  tstr_free(request);
  tstr_free(authority);
  return rc;
}

static void proxy_restore_failed_transport(coro_socket_t *s, turbo_transport_t requested_transport,
                                           int status) {
  const coro_transport_ops_t *ops;

  if (!s) return;
  ops = s->ops;
  if (s->handle.stream && ops && ops->close) ops->close(s);
  coro_socket_configure_transport_internal(s, requested_transport, 0);
  s->status = status;
  if (s->ctx) s->ctx->last_error = status;
}

int coro_socket_proxy_connect_internal(coro_socket_t *s, const char *connect_host, int port,
                                       const char *request_host) {
  turbo_transport_t requested_transport;
  const char *tls_hostname;
  uint64_t saved_timeout;
  uint64_t deadline_ms;
  int rc;

  if (!s || !connect_host || connect_host[0] == '\0' || port <= 0 || port > 65535 ||
      proxy_has_control_character(connect_host) || strchr(connect_host, ' ')) {
    return proxy_socket_error(s, TURBO_EINVAL);
  }
  requested_transport = s->transport;
  if (requested_transport != TURBO_TCP && requested_transport != TURBO_TLS) {
    return proxy_socket_error(s, TURBO_ENOTSUP);
  }
  if (s->connected) return proxy_socket_error(s, TURBO_EALREADY);
  if (s->handle.stream) return proxy_socket_error(s, TURBO_EBUSY);

  saved_timeout = s->timeout_ms;
  deadline_ms = proxy_deadline(s, saved_timeout);
  coro_socket_configure_transport_internal(s, TURBO_TCP, 0);
  rc = proxy_apply_remaining_timeout(s, deadline_ms);
  if (rc == 0) {
    rc = coro_socket_connect_direct_internal(s, s->proxy.host, s->proxy.port, s->proxy.host);
  }
  if (rc == 0 && s->proxy.type == CORO_PROXY_SOCKS5) {
    rc = socks5_connect(s, connect_host, (uint16_t)port, deadline_ms);
  } else if (rc == 0 && s->proxy.type == CORO_PROXY_HTTP_CONNECT) {
    rc = http_connect(s, connect_host, (uint16_t)port, deadline_ms);
  }

  tls_hostname = (request_host && request_host[0] != '\0') ? request_host : connect_host;
  if (rc == 0 && requested_transport == TURBO_TLS) {
    rc = proxy_apply_remaining_timeout(s, deadline_ms);
    if (rc == 0) rc = coro_socket_upgrade_tls(s, tls_hostname);
  }
  s->timeout_ms = saved_timeout;

  if (rc != 0) {
    proxy_restore_failed_transport(s, requested_transport, rc);
    return rc;
  }
  s->status = 0;
  if (s->ctx) s->ctx->last_error = 0;
  return 0;
}
