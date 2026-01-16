/**
 * Unified Address Parsing for TurboNet
 * "Eliminate special cases, don't add them" - Linus
 *
 * Uses the RFC-compliant URI parser to provide unified addressing
 * across all transport protocols. No more special cases!
 */
#include "turbo_url.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include "stb_sprintf.h"
#include <string.h>
#include "turbo_logger.h"
#include <uv.h>
#include <stc/cstr.h>

// Define the structure for scheme-port mapping
typedef struct {
  const char *scheme;
  int default_port;
  turbo_transport_t transport;
} transport_scheme_t;

#define i_static
#define i_type SchemeMap
#define i_key_str
#define i_val transport_scheme_t
#include <stc/hmap.h>

// Global hashmap for transport schemes
static SchemeMap g_scheme_map = {0};
static int g_scheme_init = 0;

// Initialize the hashmap with default schemes and ports
static void initialize_scheme_map(void) {
  if (g_scheme_init) {
    return;
  }

  g_scheme_map = SchemeMap_init();

  const transport_scheme_t schemes[] = {
      {"tcp", 80, TURBO_TCP},       {"tls", 443, TURBO_TLS},
      {"ssl", 443, TURBO_TLS},      {"https", 443, TURBO_TLS},
      {"http", 80, TURBO_TCP},      {"ws", 80, TURBO_TCP},
      {"wss", 443, TURBO_TLS},      {"udp", 53, TURBO_UDP},
      {"pipe", 0, TURBO_PIPE},      {"kcp", 7000, TURBO_KCP},
      {"quic", 443, TURBO_QUIC},    {"http3", 443, TURBO_QUIC},
      {NULL, 0, 0}};

  for (int i = 0; schemes[i].scheme; i++) {
    SchemeMap_insert(&g_scheme_map, cstr_from(schemes[i].scheme), schemes[i]);
  }
  g_scheme_init = 1;
}

// Free the hashmap
static void free_scheme_map() {
  if (g_scheme_init) {
    SchemeMap_drop(&g_scheme_map);
    g_scheme_init = 0;
  }
}

// Helper to convert string to lower case - avoids non-standard stricmp
static void to_lower(char *str) {
  if (!str) {
    return;
  }
  for (; *str; ++str) {
    *str = (char)tolower((unsigned char)*str);
  }
}

static int parse_and_validate_url(const char *url, uri_t **parsed_url) {
  if (turbo_parse_uri((const uint8_t *)url, strlen(url), parsed_url) != 0 ||
      !turbo_uri_is_valid(*parsed_url)) {
    TLOG_ERROR("Invalid URL format: {}", url);
    if (*parsed_url) {
      turbo_free_uri(parsed_url);
    }
    return TURBO_EINVAL_TRANSPORT;
  }

  int port = turbo_uri_port(*parsed_url);
  if (port > 65535 || port < 0) {
    TLOG_ERROR("Invalid port number: {} (must be 0-65535)", port);
    turbo_free_uri(parsed_url);
    return TURBO_EINVAL_TRANSPORT;
  }

  return 0;
}

static int get_transport_info(const char *scheme, int *default_port,
                              turbo_transport_t *transport) {
  if (!scheme)
    return TURBO_EINVAL_TRANSPORT;

  char lower_scheme[32];
  strncpy(lower_scheme, scheme, sizeof(lower_scheme) - 1);
  lower_scheme[sizeof(lower_scheme) - 1] = '\0';
  to_lower(lower_scheme);

  const SchemeMap_value *found = SchemeMap_get(&g_scheme_map, lower_scheme);

  if (!found) {
    static const char FMT_UNSUPPORTED[48] = "Unsupported transport scheme: {}";
    TLOG_ERROR(FMT_UNSUPPORTED, scheme);
    return TURBO_EINVAL_TRANSPORT;
  }

  *default_port = found->second.default_port;
  *transport = found->second.transport;

  return 0;
}

static void build_address_path(turbo_address_t *addr, const uri_t *parsed_url) {
  addr->path[0] = '\0';
  const char *host = turbo_uri_host(parsed_url);
  const char *path = turbo_uri_path(parsed_url);
  const char *query = turbo_uri_query(parsed_url);

  if (addr->transport == TURBO_PIPE) {
#ifdef _WIN32
    char tmp[1024];
    const char *name = (path && path[0] == '/') ? path + 1 : path;

    static const char FMT_PATH_REMOTE[32] = "\\\\%s\\pipe\\%s";
    static const char FMT_PATH_LOCAL[32] = "\\\\.\\pipe\\%s";

    if (host[0] == '\0' || strcmp(host, ".") == 0) {
      stbsp_snprintf(tmp, (int)sizeof(tmp), FMT_PATH_LOCAL, name ? name : "");
    } else {
      stbsp_snprintf(tmp, (int)sizeof(tmp), FMT_PATH_REMOTE, host, name ? name : "");
    }
    strncpy(addr->path, tmp, sizeof(addr->path) - 1);
    addr->path[sizeof(addr->path) - 1] = '\0';
#else
    if (path && path[0]) {
      strncpy(addr->path, path, sizeof(addr->path) - 1);
      addr->path[sizeof(addr->path) - 1] = '\0';
    }
#endif
  } else {
    if ((!path || path[0] == '\0') && (!query || query[0] == '\0')) {
      return;
    }
    if (!query || query[0] == '\0') {
      strncpy(addr->path, path, sizeof(addr->path) - 1);
    } else {
      static const char FMT_PATH_QUERY[32] = "%s?%s";
      stbsp_snprintf(addr->path, (int)sizeof(addr->path), FMT_PATH_QUERY, path ? path : "", query);
    }
    addr->path[sizeof(addr->path) - 1] = '\0';
  }
}

// Parse URL into transport and connection details
int parse_transport_url(const char *url, turbo_address_t *addr) {
  static uv_once_t once_guard = UV_ONCE_INIT;
  uv_once(&once_guard, initialize_scheme_map);

  if (!url || !addr) {
    return UV_EINVAL;
  }
  memset(addr, 0, sizeof(*addr));

  uri_t *parsed_url = NULL;
  int err = parse_and_validate_url(url, &parsed_url);
  if (err) {
    return err;
  }

  int default_port;
  turbo_transport_t transport;
  err = get_transport_info(turbo_uri_scheme(parsed_url), &default_port, &transport);
  if (err) {
    turbo_free_uri(&parsed_url);
    return err;
  }

  addr->transport = transport;
  addr->host_type = turbo_uri_host_type(parsed_url);
  const char *host = turbo_uri_host(parsed_url);
  if (host && host[0]) {
    strncpy(addr->host, host, sizeof(addr->host) - 1);
    addr->host[sizeof(addr->host) - 1] = '\0';
  }
  int port = turbo_uri_port(parsed_url);
  addr->port = (port > 0) ? port : default_port;

  build_address_path(addr, parsed_url);

  addr->valid = true;
  turbo_free_uri(&parsed_url);
  return 0;
}
