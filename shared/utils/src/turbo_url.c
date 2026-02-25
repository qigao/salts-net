/**
 * Unified Address Parsing for TurboNet
 * "Eliminate special cases, don't add them" - Linus
 *
 * Uses the RFC-compliant URI parser to provide unified addressing
 * across all transport protocols. No more special cases!
 */
#include "turbo_url.h"
#include "uri_parser.h"
#include "stb_sprintf.h"
#include "tlog.h"
#include <ctype.h>
#include <stc/cstr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>


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

  const transport_scheme_t schemes[] = {{"tcp", 80, TURBO_TCP},
                                        {"tls", 443, TURBO_TLS},
                                        {"ssl", 443, TURBO_TLS},
                                        {"https", 443, TURBO_TLS},
                                        {"http", 80, TURBO_TCP},
                                        {"ws", 80, TURBO_WEBSOCKET},
                                        {"wss", 443, TURBO_WEBSOCKET},
                                        {"udp", 53, TURBO_UDP},
                                        {"pipe", 0, TURBO_PIPE},
                                        {"kcp", 7000, TURBO_KCP},
                                        {"quic", 443, TURBO_QUIC},
                                        {"http3", 443, TURBO_QUIC},
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

static int parse_and_validate_url(const char *url, uri_t *parsed_url) {
  if (!uri_parse(url, parsed_url) || !parsed_url->valid) {
    TLOG_ERROR("Invalid URL format: {}", url);
    return TURBO_EINVAL_TRANSPORT;
  }

  if (parsed_url->port > 65535 || parsed_url->port < 0) {
    TLOG_ERROR("Invalid port number: {} (must be 0-65535)", parsed_url->port);
    return TURBO_EINVAL_TRANSPORT;
  }

  return 0;
}

static int get_transport_info(const char *scheme, int *default_port, turbo_transport_t *transport) {
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

static int build_address_path(turbo_address_t *addr, const uri_t *parsed_url) {
  addr->path[0] = '\0';
  const char *host = parsed_url->host;
  const char *path = parsed_url->path;
  const char *query = parsed_url->query;

  if (addr->transport == TURBO_PIPE) {
    if (host[0] && strcmp(host, ".") != 0) {
#ifdef _WIN32
      snprintf(addr->path, sizeof(addr->path), "\\\\.\\pipe\\%s", host);
#else
      snprintf(addr->path, sizeof(addr->path), "/tmp/%s", host);
#endif
    } else {
      TLOG_ERROR("Invalid pipe URL: {}. Expected pipe://service_name",
                 parsed_url->scheme);
      return TURBO_EINVAL_TRANSPORT;
    }
    addr->path[sizeof(addr->path) - 1] = '\0';
  } else {
    if (path[0] == '\0' && query[0] == '\0') {
      return 0;
    }
    if (query[0] == '\0') {
      strncpy(addr->path, path, sizeof(addr->path) - 1);
    } else {
      static const char FMT_PATH_QUERY[32] = "%s?%s";
      snprintf(addr->path, sizeof(addr->path), FMT_PATH_QUERY, path[0] ? path : "", query);
    }
    addr->path[sizeof(addr->path) - 1] = '\0';
  }

  return 0;
}

// Parse URL into transport and connection details
int parse_transport_url(const char *url, turbo_address_t *addr) {
  static uv_once_t once_guard = UV_ONCE_INIT;
  uv_once(&once_guard, initialize_scheme_map);

  if (!url || !addr) {
    return UV_EINVAL;
  }
  memset(addr, 0, sizeof(*addr));

  uri_t parsed_url;
  int err = parse_and_validate_url(url, &parsed_url);
  if (err) {
    return err;
  }

  int default_port;
  turbo_transport_t transport;
  err = get_transport_info(parsed_url.scheme, &default_port, &transport);
  if (err) {
    return err;
  }

  addr->transport = transport;
  addr->host_type = parsed_url.host_type;
  if (parsed_url.host[0]) {
    strncpy(addr->host, parsed_url.host, sizeof(addr->host) - 1);
    addr->host[sizeof(addr->host) - 1] = '\0';
  }
  addr->port = (parsed_url.port > 0) ? parsed_url.port : default_port;

  err = build_address_path(addr, &parsed_url);
  if (err) {
    return err;
  }

  addr->valid = true;
  return 0;
}

// Validates a transport URL format
int turbo_url_is_valid(const char *url) {
  if (!url || !url[0]) {
    return 0;
  }

  turbo_address_t addr;
  int result = parse_transport_url(url, &addr);
  return (result == 0 && addr.valid) ? 1 : 0;
}

// Extracts the scheme from a URL
int turbo_url_get_scheme(const char *url, char *scheme_buf, size_t buf_size) {
  if (!url || !scheme_buf || buf_size == 0) {
    return UV_EINVAL;
  }

  uri_t parsed_url;
  int err = parse_and_validate_url(url, &parsed_url);
  if (err) {
    return err;
  }

  if (parsed_url.scheme[0] == '\0') {
    return TURBO_EINVAL_TRANSPORT;
  }

  strncpy(scheme_buf, parsed_url.scheme, buf_size - 1);
  scheme_buf[buf_size - 1] = '\0';

  return 0;
}

// Builds a URL from components
int turbo_url_build(const char *scheme, const char *host, int port, const char *path, char *url_buf,
                    size_t buf_size) {
  if (!scheme || !url_buf || buf_size == 0) {
    return UV_EINVAL;
  }

  // Validate port range
  if (port < 0 || port > 65535) {
    return TURBO_EINVAL_TRANSPORT;
  }

  // Convert scheme to lowercase for consistency
  char lower_scheme[32];
  strncpy(lower_scheme, scheme, sizeof(lower_scheme) - 1);
  lower_scheme[sizeof(lower_scheme) - 1] = '\0';
  to_lower(lower_scheme);

  // Special handling for pipe URLs
  if (strcmp(lower_scheme, "pipe") == 0) {
    if (!path || !path[0]) {
      return TURBO_EINVAL_TRANSPORT;
    }
    snprintf(url_buf, buf_size, "pipe://%s", path);
    return 0;
  }

  // For other protocols, host is required
  if (!host || !host[0]) {
    return TURBO_EINVAL_TRANSPORT;
  }

  // Build URL based on whether we have a path
  if (path && path[0]) {
    // Ensure path starts with /
    const char *path_prefix = (path[0] == '/') ? "" : "/";
    if (port > 0) {
      snprintf(url_buf, buf_size, "%s://%s:%d%s%s", lower_scheme, host, port,
                     path_prefix, path);
    } else {
      snprintf(url_buf, buf_size, "%s://%s%s%s", lower_scheme, host, path_prefix, path);
    }
  } else {
    if (port > 0) {
      snprintf(url_buf, buf_size, "%s://%s:%d", lower_scheme, host, port);
    } else {
      snprintf(url_buf, buf_size, "%s://%s", lower_scheme, host);
    }
  }

  return 0;
}
