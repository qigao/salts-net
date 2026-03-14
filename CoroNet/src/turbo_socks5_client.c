/**
 * @file turbo_socks5_client.c
 * @brief SOCKS5 client implementation
 */

#include "turbo_socks5.h"
#include "tlog.h"
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

/* ── Helper Functions ────────────────────────────────────────── */

/**
 * @brief Receive exact amount of data using coro_socket (blocking/coro-aware)
 */
static int recv_all(coro_socket_t *s, uint8_t *buf, size_t len) {
  size_t received = 0;
  while (received < len) {
    char *data = NULL;
    size_t n = 0;
    int err = coro_socket_recv(s, &data, &n);
    if (err != 0 || n == 0) return -1;

    size_t remaining = len - received;
    if (n > remaining) {
      TLOG_ERROR("SOCKS5: Proxy sent %zu bytes, expected max %zu during handshake", n, remaining);
      coro_socket_free_recv(data);
      return -1;
    }

    memcpy(buf + received, data, n);
    received += n;
    coro_socket_free_recv(data);
  }
  return 0;
}

/**
 * @brief Check if string is IPv4 address
 */
static int is_ipv4(const char *host) {
  struct in_addr addr;
  return inet_pton(AF_INET, host, &addr) == 1;
}

/* ── SOCKS5 Protocol Implementation ─────────────────────────── */

/**
 * @brief Method negotiation (step 1)
 */
static int socks5_method_negotiation(coro_socket_t *s, const turbo_socks5_config_t *config) {
  uint8_t req[4];
  uint8_t resp[2];

  /* Build request: VER | NMETHODS | METHODS */
  req[0] = SOCKS5_VERSION;
  if (config->auth_required) {
    req[1] = 2;  /* 2 methods */
    req[2] = SOCKS5_AUTH_NONE;
    req[3] = SOCKS5_AUTH_USERPASS;
    if (coro_socket_send(s, (const char*)req, 4) != 0) return -1;
  } else {
    req[1] = 1;  /* 1 method */
    req[2] = SOCKS5_AUTH_NONE;
    if (coro_socket_send(s, (const char*)req, 3) != 0) return -1;
  }

  /* Read response: VER | METHOD */
  if (recv_all(s, resp, 2) != 0) return -1;

  if (resp[0] != SOCKS5_VERSION) {
    TLOG_ERROR("SOCKS5: Invalid version in response: %d", resp[0]);
    return -1;
  }

  if (resp[1] == SOCKS5_AUTH_FAILED) {
    TLOG_ERROR("SOCKS5: No acceptable authentication methods");
    return -1;
  }

  return resp[1];  /* Return selected method */
}

/**
 * @brief Username/password authentication (step 2, if needed)
 */
static int socks5_authenticate(coro_socket_t *s, const turbo_socks5_config_t *config) {
  uint8_t req[513];  /* 1 + 1 + 255 + 1 + 255 */
  uint8_t resp[2];
  size_t pos = 0;

  size_t ulen = strlen(config->username);
  size_t plen = strlen(config->password);

  if (ulen > 255 || plen > 255) {
    TLOG_ERROR("SOCKS5: Username or password too long");
    return -1;
  }

  /* Build request: VER | ULEN | UNAME | PLEN | PASSWD */
  req[pos++] = 0x01;  /* Auth version */
  req[pos++] = (uint8_t)ulen;
  memcpy(req + pos, config->username, ulen);
  pos += ulen;
  req[pos++] = (uint8_t)plen;
  memcpy(req + pos, config->password, plen);
  pos += plen;

  if (coro_socket_send(s, (const char*)req, pos) != 0) return -1;

  /* Read response: VER | STATUS */
  if (recv_all(s, resp, 2) != 0) return -1;

  if (resp[1] != 0) {
    TLOG_ERROR("SOCKS5: Authentication failed");
    return -1;
  }

  return 0;
}

/**
 * @brief Send connect request (step 3)
 */
static int socks5_send_connect_request(coro_socket_t *s, const char *host, uint16_t port) {
  uint8_t req[262];  /* 4 + 1 + 255 + 2 */
  size_t pos = 0;

  /* VER | CMD | RSV | ATYP */
  req[pos++] = SOCKS5_VERSION;
  req[pos++] = SOCKS5_CMD_CONNECT;
  req[pos++] = 0x00;  /* Reserved */

  if (is_ipv4(host)) {
    /* IPv4 address */
    req[pos++] = SOCKS5_ATYP_IPV4;
    struct in_addr addr;
    inet_pton(AF_INET, host, &addr);
    memcpy(req + pos, &addr, 4);
    pos += 4;
  } else {
    /* Domain name */
    size_t len = strlen(host);
    if (len > 255) {
      TLOG_ERROR("SOCKS5: Hostname too long");
      return -1;
    }
    req[pos++] = SOCKS5_ATYP_DOMAIN;
    req[pos++] = (uint8_t)len;
    memcpy(req + pos, host, len);
    pos += len;
  }

  /* Port (network byte order) */
  uint16_t nport = htons(port);
  memcpy(req + pos, &nport, 2);
  pos += 2;

  return coro_socket_send(s, (const char*)req, pos);
}

/**
 * @brief Read connect reply (step 4)
 */
static int socks5_read_connect_reply(coro_socket_t *s) {
  uint8_t resp[10];  /* Minimum: VER | REP | RSV | ATYP | ADDR(4) | PORT(2) */

  /* Read fixed part: VER | REP | RSV | ATYP */
  if (recv_all(s, resp, 4) != 0) return -1;

  if (resp[0] != SOCKS5_VERSION) {
    TLOG_ERROR("SOCKS5: Invalid version in reply: %d", resp[0]);
    return -1;
  }

  if (resp[1] != SOCKS5_REP_SUCCESS) {
    TLOG_ERROR("SOCKS5: Connect failed with code: %d", resp[1]);
    return -1;
  }

  uint8_t atyp = resp[3];
  size_t addr_len;

  /* Read address based on type */
  switch (atyp) {
    case SOCKS5_ATYP_IPV4:
      addr_len = 4;
      break;
    case SOCKS5_ATYP_IPV6:
      addr_len = 16;
      break;
    case SOCKS5_ATYP_DOMAIN:
      /* Read domain length first */
      if (recv_all(s, resp, 1) != 0) return -1;
      addr_len = resp[0];
      break;
    default:
      TLOG_ERROR("SOCKS5: Unsupported address type: %d", atyp);
      return -1;
  }

  /* Read address + port */
  if (recv_all(s, resp, addr_len + 2) != 0) return -1;

  return 0;
}

/* ── Public API ──────────────────────────────────────────────── */

int turbo_socks5_connect(coro_socket_t *s,
                         const turbo_socks5_config_t *config,
                         const char *target_host,
                         uint16_t target_port) {
  if (!s || !config || !target_host) return -1;

  /* Step 1: Method negotiation */
  int method = socks5_method_negotiation(s, config);
  if (method < 0) return -1;

  /* Step 2: Authentication (if required) */
  if (method == SOCKS5_AUTH_USERPASS) {
    if (socks5_authenticate(s, config) != 0) return -1;
  }

  /* Step 3: Send connect request */
  if (socks5_send_connect_request(s, target_host, target_port) != 0) return -1;

  /* Step 4: Read connect reply */
  if (socks5_read_connect_reply(s) != 0) return -1;

  TLOG_INFO("SOCKS5: Connected to %s:%d via proxy", target_host, target_port);
  return 0;
}
