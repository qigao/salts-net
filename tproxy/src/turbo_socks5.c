#include "turbo_socks5.h"
#include "turbo_error.h"
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include <string.h>

typedef struct {
  coro_socket_t *socket;
  unsigned char buf[512];
  size_t off;
  size_t len;
} socks5_reader_t;

static int send_all(coro_socket_t *socket, const unsigned char *buf, size_t len) {
  return coro_socket_send(socket, (const char *)buf, len);
}

static int reader_fill(socks5_reader_t *reader) {
  char *chunk = NULL;
  size_t chunk_len = 0;
  int rc;

  if (reader->off > 0 && reader->off < reader->len) {
    memmove(reader->buf, reader->buf + reader->off, reader->len - reader->off);
    reader->len -= reader->off;
    reader->off = 0;
  } else if (reader->off >= reader->len) {
    reader->off = 0;
    reader->len = 0;
  }

  rc = coro_socket_recv(reader->socket, &chunk, &chunk_len);
  if (rc != 0) return rc;
  if (chunk == NULL || chunk_len == 0) return 0;
  if (reader->len + chunk_len > sizeof(reader->buf)) {
    coro_socket_free_recv(chunk);
    return TURBO_EPROTO;
  }

  memcpy(reader->buf + reader->len, chunk, chunk_len);
  reader->len += chunk_len;
  coro_socket_free_recv(chunk);
  return 0;
}

static int reader_read_exact(socks5_reader_t *reader, unsigned char *out, size_t want) {
  while ((reader->len - reader->off) < want) {
    int rc = reader_fill(reader);
    if (rc != 0) return rc;
    if ((reader->len - reader->off) == 0) continue;
  }

  memcpy(out, reader->buf + reader->off, want);
  reader->off += want;
  return 0;
}

static int send_method_negotiation(coro_socket_t *socket, int auth_required) {
  unsigned char req[4];
  size_t req_len = auth_required ? 4 : 3;

  req[0] = SOCKS5_VERSION;
  req[1] = auth_required ? 2 : 1;
  req[2] = SOCKS5_AUTH_NONE;
  req[3] = SOCKS5_AUTH_USERPASS;
  return send_all(socket, req, req_len);
}

static int recv_method_reply(socks5_reader_t *reader, int auth_required) {
  unsigned char resp[2];
  int rc = reader_read_exact(reader, resp, sizeof(resp));
  if (rc != 0) return rc;
  if (resp[0] != SOCKS5_VERSION) return TURBO_EPROTO;
  if (resp[1] == SOCKS5_AUTH_FAILED) return TURBO_EPERM;
  if (auth_required && resp[1] != SOCKS5_AUTH_USERPASS) return TURBO_EPERM;
  if (!auth_required && resp[1] != SOCKS5_AUTH_NONE && resp[1] != SOCKS5_AUTH_USERPASS) {
    return TURBO_EPERM;
  }
  return (int)resp[1];
}

static int do_userpass_auth(socks5_reader_t *reader, const turbo_socks5_config_t *config) {
  unsigned char req[2 + 255 + 255];
  unsigned char resp[2];
  size_t ulen = strnlen(config->username, sizeof(config->username));
  size_t plen = strnlen(config->password, sizeof(config->password));
  size_t pos = 0;
  int rc;

  if (ulen == 0 || plen == 0 || ulen > 255 || plen > 255) return TURBO_EINVAL;

  req[pos++] = 0x01;
  req[pos++] = (unsigned char)ulen;
  memcpy(req + pos, config->username, ulen);
  pos += ulen;
  req[pos++] = (unsigned char)plen;
  memcpy(req + pos, config->password, plen);
  pos += plen;

  rc = send_all(reader->socket, req, pos);
  if (rc != 0) return rc;

  rc = reader_read_exact(reader, resp, sizeof(resp));
  if (rc != 0) return rc;
  if (resp[0] != 0x01 || resp[1] != 0x00) return TURBO_EPERM;
  return 0;
}

static int send_connect_request(coro_socket_t *socket, const char *target_host,
                                uint16_t target_port) {
  unsigned char req[4 + 1 + 255 + 2];
  unsigned char ipv4[4];
  size_t host_len = strnlen(target_host, 255);
  size_t pos = 0;

  req[pos++] = SOCKS5_VERSION;
  req[pos++] = SOCKS5_CMD_CONNECT;
  req[pos++] = 0x00;

  if (inet_pton(AF_INET, target_host, ipv4) == 1) {
    req[pos++] = SOCKS5_ATYP_IPV4;
    memcpy(req + pos, ipv4, sizeof(ipv4));
    pos += sizeof(ipv4);
  } else {
    if (host_len == 0 || host_len > 255) return TURBO_EINVAL;
    req[pos++] = SOCKS5_ATYP_DOMAIN;
    req[pos++] = (unsigned char)host_len;
    memcpy(req + pos, target_host, host_len);
    pos += host_len;
  }

  req[pos++] = (unsigned char)((target_port >> 8) & 0xFF);
  req[pos++] = (unsigned char)(target_port & 0xFF);
  return send_all(socket, req, pos);
}

static int recv_connect_reply(socks5_reader_t *reader) {
  unsigned char head[4];
  unsigned char addr[256];
  size_t addr_len;
  int rc = reader_read_exact(reader, head, sizeof(head));
  if (rc != 0) return rc;
  if (head[0] != SOCKS5_VERSION) return TURBO_EPROTO;
  if (head[1] != SOCKS5_REP_SUCCESS) return TURBO_ECONNREFUSED;
  if (head[2] != 0x00) return TURBO_EPROTO;

  switch (head[3]) {
  case SOCKS5_ATYP_IPV4:
    addr_len = 4;
    break;
  case SOCKS5_ATYP_IPV6:
    addr_len = 16;
    break;
  case SOCKS5_ATYP_DOMAIN:
    rc = reader_read_exact(reader, addr, 1);
    if (rc != 0) return rc;
    addr_len = addr[0];
    break;
  default:
    return TURBO_EPROTO;
  }

  rc = reader_read_exact(reader, addr, addr_len + 2);
  if (rc != 0) return rc;
  return 0;
}

int turbo_socks5_connect(coro_socket_t *socket, const turbo_socks5_config_t *config,
                         const char *target_host, uint16_t target_port) {
  socks5_reader_t reader = {0};
  int selected_method;
  int rc;

  if (!socket || !config || !target_host || target_host[0] == '\0') {
    return TURBO_EINVAL;
  }

  reader.socket = socket;
  if (config->timeout_ms > 0) {
    coro_socket_set_timeout(socket, (uint64_t)config->timeout_ms);
  }

  rc = send_method_negotiation(socket, config->auth_required);
  if (rc != 0) return rc;

  selected_method = recv_method_reply(&reader, config->auth_required);
  if (selected_method < 0) return selected_method;

  if (selected_method == SOCKS5_AUTH_USERPASS) {
    rc = do_userpass_auth(&reader, config);
    if (rc != 0) return rc;
  }

  rc = send_connect_request(socket, target_host, target_port);
  if (rc != 0) return rc;

  return recv_connect_reply(&reader);
}
