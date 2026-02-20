// turbo_websocket_client.c - WebSocket client transport for netcore
// Minimal implementation for architecture validation

#include "turbo_websocket_client.h"
#include "turbo_dns.h"
#include "base64_utils.h"
#include "client_common.h"
#include "stb_sprintf.h"
#include "tlog.h"
#include "turbo_tcp.h"
#include "turbo_tls.h"
#include "websocket_crypto.h"
#include "websocket_frame_parser.h"
#include "websocket_handshake_parser.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif


// ============================================================================
// Forward declarations
#include "tlog.h"

// ============================================================================
static int on_tcp_recv_internal(void *handle, const turbo_arena_slice_t *data, void *peer);
static void on_tcp_connect_internal(void *handle, int status, void *peer);
static void on_tcp_close_internal(void *handle);

static int on_tls_recv_internal(void *handle, const turbo_arena_slice_t *data, void *peer);
static void on_tls_connect_internal(void *handle, int status, void *peer);
static void on_tls_close_internal(void *handle);

static int send_websocket_handshake(turbo_websocket_client_t *client, const char *host, int port);
static int process_handshake_response(turbo_websocket_client_t *client, const char *data,
                                      size_t len);
static int process_websocket_frame(turbo_websocket_client_t *client, const uint8_t *data,
                                   size_t len);
static int send_websocket_frame(turbo_websocket_client_t *client, websocket_opcode_t opcode,
                                const uint8_t *payload, size_t len, int fin);

// ============================================================================
// Client lifecycle
// ============================================================================

turbo_websocket_client_t *turbo_websocket_client_create(uv_loop_t *loop, int use_tls,
                                                        const turbo_websocket_config_t *config) {

  if (!loop || !config)
    return NULL;

  turbo_websocket_client_t *client = calloc(1, sizeof(turbo_websocket_client_t));
  if (!client)
    return NULL;

  client->loop = loop;
  client->is_tls = use_tls;
  client->state = TURBO_WS_STATE_CONNECTING;

  // Allocate connection arena (8KB initial)
  client->conn_arena = calloc(1, sizeof(turbo_arena_t));
  if (!client->conn_arena) {
    free(client);
    return NULL;
  }

  if (turbo_arena_init(client->conn_arena, 8192) != 0) {
    free(client->conn_arena);
    free(client);
    return NULL;
  }

  // Copy config (allocate from arena)
  if (config->path) {
    client->config.path = turbo_arena_strdup(client->conn_arena, config->path);
  } else {
    client->config.path = turbo_arena_strdup(client->conn_arena, "/");
  }

  if (config->origin) {
    client->config.origin = turbo_arena_strdup(client->conn_arena, config->origin);
  }

  if (config->host) {
    client->config.host = turbo_arena_strdup(client->conn_arena, config->host);
  }

  // Copy subprotocols
  if (config->subprotocols && config->subprotocol_count > 0) {
    client->config.subprotocol_count = config->subprotocol_count;
    client->config.subprotocols = turbo_arena_alloc(client->conn_arena,
        sizeof(char *) * config->subprotocol_count);
    if (client->config.subprotocols) {
      for (int i = 0; i < config->subprotocol_count; i++) {
        ((char **)client->config.subprotocols)[i] =
            turbo_arena_strdup(client->conn_arena, config->subprotocols[i]);
      }
    }
  }

  // Allocate handshake buffer
  client->handshake_recv_buffer = turbo_arena_get_pooled_buffer(client->conn_arena, 4096);
  if (!client->handshake_recv_buffer) {
    turbo_arena_free(client->conn_arena);
    free(client->conn_arena);
    free(client);
    return NULL;
  }

  // Generate Sec-WebSocket-Key (16 random bytes, base64 encoded)
  uint8_t key_bytes[16];
  for (int i = 0; i < 16; i++) {
    key_bytes[i] = (uint8_t)(rand() % 256);
  }

  char *key_b64 = NULL;
  if (tn_base64_encode(key_bytes, 16, &key_b64) == 0) {
    client->sec_websocket_key = turbo_arena_strdup(client->conn_arena, key_b64);
    free(key_b64);
  }

  return client;
}

int turbo_websocket_client_connect(turbo_websocket_client_t *client, const char *host, int port) {
  TLOG_DEBUG("turbo_websocket_client_connect: start host={} port={}", host, port);

  if (!client || !host)
    return -1;

  // Store host and port for handshake
  client->connect_host = turbo_arena_strdup(client->conn_arena, host);
  client->connect_port = port;
  TLOG_DEBUG("turbo_websocket_client_connect: is_tls={}", client->is_tls);

  // Create underlying transport
  if (client->is_tls) {
    // Create TLS client - use stored TLS context
    if (!client->tls_context) {
      return -1; // TLS context must be set before connect
    }

    // Resolve hostname to IP address (TLS client requires IP)
    struct sockaddr_storage addr;
    int addr_len = 0;
    
    // Initialize DNS for this resolution
    int rc = turbo_dns_init();
    if (rc != 0) {
      return rc;
    }

    rc = turbo_dns_resolve(NULL, host, port, &addr, &addr_len);
    
    // Cleanup DNS (refcount decrement)
    turbo_dns_cleanup();

    if (rc != 0) {
      return rc;
    }

    // Convert resolved address back to IP string
    char ip_str[INET6_ADDRSTRLEN];
    if (addr.ss_family == AF_INET) {
      struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr;
      inet_ntop(AF_INET, &addr4->sin_addr, ip_str, sizeof(ip_str));
    } else if (addr.ss_family == AF_INET6) {
      struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&addr;
      inet_ntop(AF_INET6, &addr6->sin6_addr, ip_str, sizeof(ip_str));
    } else {
      return -1;
    }

    turbo_tls_client_t *tls =
        turbo_tls_client_create(client->loop, (turbo_tls_context_t *)client->tls_context);
    if (!tls) {
      return -1;
    }

    client->transport = tls;
    tls->user_data = client;

    // Set hostname for TLS SNI (Server Name Indication)
    turbo_tls_client_set_hostname(tls, host, strlen(host));

    // Connect with TLS-specific callbacks that use user_data
    int result = turbo_tls_client_connect(tls, ip_str, (unsigned short)port, on_tls_recv_internal,
                                          on_tls_connect_internal, on_tls_close_internal);
    if (result != 0) {
      tls->user_data = NULL; // Detach before close/free
      turbo_tls_client_close(tls);
      client->transport = NULL;
      return result;
    }

    // Send WebSocket handshake after TLS handshake (in callback)
    return 0;
  } else {
    TLOG_DEBUG("turbo_websocket_client_connect: creating TCP client");
    turbo_tcp_client_t *tcp = turbo_tcp_client_create(client->loop);
    if (!tcp) {
      TLOG_DEBUG("turbo_websocket_client_connect: turbo_tcp_client_create failed");
      return -1;
    }
    TLOG_DEBUG("turbo_websocket_client_connect: TCP client created, connecting...");

    client->transport = tcp;
    tcp->user_data = client;

    // Connect with TCP-specific callbacks
    // Connect with TCP-specific callbacks
    int result = turbo_tcp_client_connect(tcp, host, (unsigned short)port, on_tcp_recv_internal,
                                          on_tcp_connect_internal, on_tcp_close_internal);
    TLOG_DEBUG("turbo_websocket_client_connect: turbo_tcp_client_connect returned {}", result);
    if (result != 0) {
      tcp->user_data = NULL; // Detach before close/free
      turbo_tcp_client_close(tcp);
      client->transport = NULL;
      return result;
    }

    // Send WebSocket handshake after TCP connect (in callback)
    return 0;
  }
}

int turbo_websocket_client_send(turbo_websocket_client_t *client, const char *data, size_t len) {

  if (!client || !data || len == 0)
    return -1;
  if (client->state != TURBO_WS_STATE_OPEN)
    return -1;

  // Send BINARY frame (required for MQTT over WebSocket)
  return send_websocket_frame(client, WS_OPCODE_BINARY, (const uint8_t *)data, len, 1);
}

int turbo_websocket_client_send_ping(turbo_websocket_client_t *client, const uint8_t *payload,
                                     size_t len) {

  if (!client)
    return -1;
  if (len > 125)
    return -1; // PING payload max 125 bytes
  if (client->state != TURBO_WS_STATE_OPEN)
    return -1;

  return send_websocket_frame(client, WS_OPCODE_PING, payload, len, 1);
}

int turbo_websocket_client_close(turbo_websocket_client_t *client, uint16_t code,
                                 const char *reason) {

  if (!client)
    return -1;
  if (client->state != TURBO_WS_STATE_OPEN)
    return -1;

  // Build CLOSE frame payload
  uint8_t close_payload[125];
  size_t close_len = 0;

  close_payload[0] = (code >> 8) & 0xFF;
  close_payload[1] = code & 0xFF;
  close_len = 2;

  if (reason) {
    size_t reason_len = strlen(reason);
    if (reason_len > 123)
      reason_len = 123;
    memcpy(close_payload + 2, reason, reason_len);
    close_len += reason_len;
  }

  client->close_sent = 1;
  client->state = TURBO_WS_STATE_CLOSING;

  return send_websocket_frame(client, WS_OPCODE_CLOSE, close_payload, close_len, 1);
}

/* Internal cleanup - called from close callbacks when pending_destroy is set */
static void websocket_client_free(turbo_websocket_client_t *client) {
  if (!client)
    return;

  // Free arena
  if (client->conn_arena) {
    turbo_arena_free(client->conn_arena);
    free(client->conn_arena);
  }

  free(client);
}

void turbo_websocket_client_destroy(turbo_websocket_client_t *client) {
  if (!client)
    return;

  /* If already closed, free immediately, UNLESS we are in the close callback */
  if ((client->state == TURBO_WS_STATE_CLOSED || !client->transport) && !client->in_close_callback) {
    websocket_client_free(client);
    return;
  }

  /* Mark for deferred cleanup - will be freed in close callback */
  client->pending_destroy = 1;

  /* Initiate transport close - cleanup happens in on_tcp/tls_close_internal */
  if (client->is_tls) {
    turbo_tls_client_close((turbo_tls_client_t *)client->transport);
  } else {
    turbo_tcp_client_close((turbo_tcp_client_t *)client->transport);
  }
}

void turbo_websocket_client_set_callbacks(turbo_websocket_client_t *client, turbo_recv_cb on_recv,
                                          turbo_connect_cb on_connect, turbo_close_cb on_close) {

  if (!client)
    return;
  client->on_recv = on_recv;
  client->on_connect = on_connect;
  client->on_close = on_close;
}

const char *turbo_websocket_client_get_subprotocol(turbo_websocket_client_t *client) {
  return client ? client->selected_subprotocol : NULL;
}

const char *turbo_websocket_client_get_extensions(turbo_websocket_client_t *client) {
  return client ? client->negotiated_extensions : NULL;
}

turbo_websocket_state_t turbo_websocket_client_get_state(turbo_websocket_client_t *client) {
  return client ? client->state : TURBO_WS_STATE_CLOSED;
}

// ============================================================================
// Internal callbacks
// ============================================================================

static void on_tcp_connect_internal(void *handle, int status, void *peer) {
  (void)peer;
  TLOG_DEBUG("on_tcp_connect_internal: status={}", status);
  if (!handle) return;

  turbo_tcp_client_t *tcp = (turbo_tcp_client_t *)handle;
  turbo_websocket_client_t *client = (turbo_websocket_client_t *)tcp->user_data;

  if (!client) return;

  if (status != 0) {
    // TCP connection failed
    TLOG_DEBUG("on_tcp_connect_internal: TCP connection failed");
    if (client->on_connect) {
      client->on_connect(client, status, peer);
    }
    return;
  }

  // TCP connected, send WebSocket handshake
  TLOG_DEBUG("on_tcp_connect_internal: TCP connected, sending WS handshake");
  client->state = TURBO_WS_STATE_HANDSHAKING;
  send_websocket_handshake(client, client->connect_host, client->connect_port);
}

static int on_tcp_recv_internal(void *handle, const turbo_arena_slice_t *data, void *peer) {
  (void)peer;
  TLOG_DEBUG("on_tcp_recv_internal: handle={} data={}", handle, (void*)data);
  if (!handle) return 0;

  turbo_tcp_client_t *tcp = (turbo_tcp_client_t *)handle;
  turbo_websocket_client_t *client = (turbo_websocket_client_t *)tcp->user_data;

  if (!client || !data || !data->data || data->length == 0) {
    TLOG_DEBUG("on_tcp_recv_internal: invalid data or client");
    return 0;
  }
  TLOG_DEBUG("on_tcp_recv_internal: state={} data_len={}", (int)client->state, data->length);

  if (client->state == TURBO_WS_STATE_HANDSHAKING) {
    // Process handshake response
    TLOG_DEBUG("on_tcp_recv_internal: processing handshake response");
    process_handshake_response(client, data->data, data->length);
  } else if (client->state == TURBO_WS_STATE_OPEN || client->state == TURBO_WS_STATE_CLOSING) {
    // Process WebSocket frames
    process_websocket_frame(client, (const uint8_t *)data->data, data->length);
  }

  return 0;  // Don't close connection
}

static void on_tcp_close_internal(void *handle) {
  if (!handle) return;

  turbo_tcp_client_t *tcp = (turbo_tcp_client_t *)handle;
  turbo_websocket_client_t *client = (turbo_websocket_client_t *)tcp->user_data;

  if (!client) return;

  client->state = TURBO_WS_STATE_CLOSED;
  client->in_close_callback = 1;

  if (client->on_close) {
    client->on_close(client);
  }
  
  // Guard against client being freed in callback
  // Use a way to detect if client is still valid is impossible if freed.
  // BUT client->pending_destroy should only be processed if client is still alive.
  // If destroy was called during pending_destroy, destroy would have freed it if state was closed.
  // But wait, destroy sets pending_destroy=1 if we are 'in use'.
  // We need destroy to NOT free if in_close_callback is set. 
  
  // So proceed assuming destroy modified to respect in_close_callback.
  
  client->in_close_callback = 0;

  /* Deferred cleanup from turbo_websocket_client_destroy */
  if (client->pending_destroy) {
    websocket_client_free(client);
  }
}

// TLS-specific callbacks (use turbo_tls_client_t->user_data)
static void on_tls_connect_internal(void *handle, int status, void *peer) {
  (void)peer;
  if (!handle) return;

  turbo_tls_client_t *tls = (turbo_tls_client_t *)handle;
  turbo_websocket_client_t *client = (turbo_websocket_client_t *)tls->user_data;

  if (!client) return;

  if (status != 0) {
    if (client->on_connect) {
      client->on_connect(client, status, peer);
    }
    return;
  }

  client->state = TURBO_WS_STATE_HANDSHAKING;
  send_websocket_handshake(client, client->connect_host, client->connect_port);
}

static int on_tls_recv_internal(void *handle, const turbo_arena_slice_t *data, void *peer) {
  (void)peer;
  if (!handle) return 0;

  turbo_tls_client_t *tls = (turbo_tls_client_t *)handle;
  turbo_websocket_client_t *client = (turbo_websocket_client_t *)tls->user_data;

  if (!client || !data || !data->data || data->length == 0) {
    return 0;
  }

  if (client->state == TURBO_WS_STATE_HANDSHAKING) {
    process_handshake_response(client, data->data, data->length);
  } else if (client->state == TURBO_WS_STATE_OPEN || client->state == TURBO_WS_STATE_CLOSING) {
    process_websocket_frame(client, (const uint8_t *)data->data, data->length);
  }

  return 0;
}

static void on_tls_close_internal(void *handle) {
  if (!handle) return;

  turbo_tls_client_t *tls = (turbo_tls_client_t *)handle;
  turbo_websocket_client_t *client = (turbo_websocket_client_t *)tls->user_data;

  if (!client) return;

  client->state = TURBO_WS_STATE_CLOSED;
  client->in_close_callback = 1;

  if (client->on_close) {
    client->on_close(client);
  }
  
  client->in_close_callback = 0;

  /* Deferred cleanup from turbo_websocket_client_destroy */
  if (client->pending_destroy) {
    websocket_client_free(client);
  }
}

// ============================================================================
// WebSocket protocol implementation
// ============================================================================

static int send_websocket_handshake(turbo_websocket_client_t *client, const char *host, int port) {
  /* Explicit size with padding to avoid ASan false positives from stb_sprintf lookahead */
  static const char FMT_HANDSHAKE[128] =
      "GET %s HTTP/1.1\r\n"
      "Host: %s:%d\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: %s\r\n"
      "Sec-WebSocket-Version: 13\r\n";
  char handshake[2048];
  int len = stbsp_snprintf(handshake, (int)sizeof(handshake),
                           FMT_HANDSHAKE,
                           client->config.path ? client->config.path : "/", host, port,
                           client->sec_websocket_key ? client->sec_websocket_key
                                                     : "dGhlIHNhbXBsZSBub25jZQ==");

  if (client->config.origin) {
    static const char FMT_ORIGIN[32] = "Origin: %s\r\n";
    len += stbsp_snprintf(handshake + len, (int)(sizeof(handshake) - len), FMT_ORIGIN,
                          client->config.origin);
  }

  // Add subprotocol header if configured
  if (client->config.subprotocols && client->config.subprotocol_count > 0) {
    static const char FMT_SUB_HDR[32] = "Sec-WebSocket-Protocol: ";
    len += stbsp_snprintf(handshake + len, (int)(sizeof(handshake) - len),
                          FMT_SUB_HDR);
    for (int i = 0; i < client->config.subprotocol_count; i++) {
      if (i > 0) {
        static const char FMT_COMMA[32] = ", ";
        len += stbsp_snprintf(handshake + len, (int)(sizeof(handshake) - len), FMT_COMMA);
      }
      static const char FMT_STR[32] = "%s";
      len += stbsp_snprintf(handshake + len, (int)(sizeof(handshake) - len), FMT_STR,
                            client->config.subprotocols[i]);
    }
    static const char FMT_CRLF[32] = "\r\n";
    len += stbsp_snprintf(handshake + len, (int)(sizeof(handshake) - len), FMT_CRLF);
  }

  static const char FMT_FINAL_CRLF[32] = "\r\n";
  len += stbsp_snprintf(handshake + len, (int)(sizeof(handshake) - len), FMT_FINAL_CRLF);

  // Send via TCP or TLS
  if (client->is_tls) {
    return turbo_tls_send((turbo_tls_client_t *)client->transport, handshake, len);
  } else {
    return turbo_tcp_send((turbo_tcp_client_t *)client->transport, handshake, len);
  }
}

// Case-insensitive substring search
static const char *strcasestr_local(const char *haystack, const char *needle) {
  if (!haystack || !needle) return NULL;
  size_t needle_len = strlen(needle);
  if (needle_len == 0) return haystack;

  for (; *haystack; haystack++) {
    int match = 1;
    for (size_t i = 0; i < needle_len; i++) {
      char h = haystack[i];
      char n = needle[i];
      if (h >= 'A' && h <= 'Z') h += 32;
      if (n >= 'A' && n <= 'Z') n += 32;
      if (h != n) { match = 0; break; }
    }
    if (match) return haystack;
  }
  return NULL;
}

static int process_handshake_response(turbo_websocket_client_t *client, const char *data,
                                      size_t len) {
  TLOG_DEBUG("process_handshake_response: len={} client={}", len, (void*)client);
  TLOG_DEBUG("process_handshake_response: handshake_recv_buffer={}", (void*)client->handshake_recv_buffer);
  // Accumulate in handshake buffer (leave room for null terminator)
  if (client->handshake_recv_used + len >= client->handshake_recv_buffer->capacity) {
    TLOG_DEBUG("process_handshake_response: handshake too large");
    return -1; // Handshake too large
  }

  memcpy(client->handshake_recv_buffer->data + client->handshake_recv_used, data, len);
  client->handshake_recv_used += len;
  client->handshake_recv_buffer->data[client->handshake_recv_used] = '\0';

  // Check for complete handshake (\r\n\r\n)
  const char *end = strstr(client->handshake_recv_buffer->data, "\r\n\r\n");
  if (!end) {
    return 0; // Need more data
  }

  // Validate handshake response (case-insensitive for headers)
  // Check for 101 Switching Protocols (allow HTTP/1.0 or HTTP/1.1)
  if (strstr(client->handshake_recv_buffer->data, " 101 ") &&
      strcasestr_local(client->handshake_recv_buffer->data, "upgrade: websocket")) {

    // Handshake successful
    client->state = TURBO_WS_STATE_OPEN;

    if (client->on_connect) {
      client->on_connect(client, 0, NULL);
    }

    return 0;
  }

  // Handshake failed - call on_connect with error
  if (client->on_connect) {
    TLOG_DEBUG("WS Handshake failed. Response:\n{}", client->handshake_recv_buffer->data);
    client->on_connect(client, -1, NULL);
  }

  return -1;
}

static int process_websocket_frame(turbo_websocket_client_t *client, const uint8_t *data,
                                   size_t len) {
  TLOG_DEBUG("process_websocket_frame: len={}", len);
  // Parse WebSocket frame
  ws_frame_t frame;
  ws_parse_result_t result = ws_frame_parse(data, len, &frame);
  TLOG_DEBUG("process_websocket_frame: parse result={}", (int)result);

  if (result == WS_PARSE_NEED_MORE) {
    // Buffer incomplete frame data for later
    if (!client->frame_recv_buffer) {
      client->frame_recv_buffer = turbo_arena_get_buffer(client->conn_arena, 65536);
      if (!client->frame_recv_buffer)
        return -1;
    }

    size_t space = client->frame_recv_buffer->capacity - client->frame_recv_used;
    if (len <= space) {
      memcpy(client->frame_recv_buffer->data + client->frame_recv_used, data, len);
      client->frame_recv_used += len;
    }
    return 0; // Need more data
  }

  if (result != WS_PARSE_OK) {
    return -1; // Parse error
  }

  // Extract frame header
  int fin = frame.fin;
  uint8_t opcode = frame.opcode;
  uint64_t payload_len = frame.payload_len;
  size_t header_len = frame.header_len;
  (void)header_len;

  // Get payload
  const uint8_t *payload = frame.payload;

  // Handle control frames (can appear between fragmented data frames)
  if (opcode == WS_OPCODE_CLOSE) {
    client->close_received = 1;
    client->state = TURBO_WS_STATE_CLOSED;

    if (client->on_close) {
      client->on_close(client);
    }
    return -1;
  }

  if (opcode == WS_OPCODE_PING) {
    // Send PONG response
    send_websocket_frame(client, WS_OPCODE_PONG, payload, payload_len, 1);
    return 0;
  }

  if (opcode == WS_OPCODE_PONG) {
    return 0; // Ignore PONG
  }

  // Handle fragmentation
  if (opcode == WS_OPCODE_CONTINUATION) {
    // Continuation frame - must be expecting one
    if (!client->expecting_continuation) {
      // Protocol error: unexpected continuation
      return -1;
    }

    // Append to fragment buffer
    if (client->fragment_buffer && payload_len > 0) {
      size_t space = client->fragment_buffer->capacity - client->fragment_buffer_used;
      if (payload_len <= space) {
        memcpy(client->fragment_buffer->data + client->fragment_buffer_used, payload, payload_len);
        client->fragment_buffer_used += payload_len;
      } else {
        // Message too large
        return -1;
      }
    }

    if (fin) {
      // Final fragment - deliver complete message
      client->expecting_continuation = 0;

      if (client->on_recv && client->fragment_buffer) {
        turbo_arena_slice_t slice = {.data = client->fragment_buffer->data,
                                     .length = client->fragment_buffer_used,
                                     .buffer = NULL};
        client->on_recv(client, &slice, NULL);
      }

      client->fragment_buffer_used = 0;
    }
  } else if (opcode == WS_OPCODE_TEXT || opcode == WS_OPCODE_BINARY) {
    if (fin) {
      // Complete unfragmented message
      TLOG_DEBUG("process_websocket_frame: complete message opcode={} payload_len={} on_recv={}",
              (int)opcode, (unsigned long long)payload_len, (void*)client->on_recv);
      if (client->on_recv) {
        turbo_arena_slice_t slice = {
            .data = (char *)payload, .length = payload_len, .buffer = NULL};
        TLOG_DEBUG("process_websocket_frame: calling on_recv");
        client->on_recv(client, &slice, NULL);
        TLOG_DEBUG("process_websocket_frame: on_recv returned");
      }
    } else {
      // First fragment of a fragmented message
      client->expecting_continuation = 1;
      client->fragment_opcode = opcode;

      // Allocate fragment buffer if needed (1MB default)
      if (!client->fragment_buffer) {
        client->fragment_buffer = turbo_arena_get_buffer(client->conn_arena, 1024 * 1024);
        if (!client->fragment_buffer)
          return -1;
      }

      // Copy first fragment
      client->fragment_buffer_used = 0;
      if (payload_len > 0 && payload_len <= client->fragment_buffer->capacity) {
        memcpy(client->fragment_buffer->data, payload, payload_len);
        client->fragment_buffer_used = payload_len;
      }
    }
  }

  return 0;
}

static int send_websocket_frame(turbo_websocket_client_t *client, websocket_opcode_t opcode,
                                const uint8_t *payload, size_t payload_len, int fin) {

  // Build WebSocket frame header
  uint8_t header[14]; // Max header size
  size_t header_len = 0;

  // Byte 0: FIN + RSV + Opcode
  header[0] = (fin ? 0x80 : 0x00) | (opcode & 0x0F);
  header_len++;

  // Byte 1: MASK + Payload length
  header[1] = 0x80; // Client frames MUST be masked

  if (payload_len <= 125) {
    header[1] |= payload_len;
    header_len++;
  } else if (payload_len <= 0xFFFF) {
    header[1] |= 126;
    header[2] = (payload_len >> 8) & 0xFF;
    header[3] = payload_len & 0xFF;
    header_len += 3;
  } else {
    header[1] |= 127;
    for (int i = 0; i < 8; i++) {
      header[2 + i] = (payload_len >> (56 - i * 8)) & 0xFF;
    }
    header_len += 9;
  }

  // Masking key (4 random bytes)
  uint8_t masking_key[4];
  for (int i = 0; i < 4; i++) {
    masking_key[i] = rand() % 256;
    header[header_len++] = masking_key[i];
  }

  // Mask payload
  uint8_t *masked_payload = NULL;
  if (payload_len > 0) {
    masked_payload = malloc(payload_len);
    if (!masked_payload)
      return -1;

    memcpy(masked_payload, payload, payload_len);
    ws_frame_unmask(masked_payload, payload_len, masking_key);
  }

  // Send header + masked payload
  int result;
  size_t total_len = header_len + payload_len;
  char *combined = malloc(total_len);
  if (!combined) {
    if (masked_payload) free(masked_payload);
    return -1;
  }
  memcpy(combined, header, header_len);
  if (masked_payload && payload_len > 0) {
    memcpy(combined + header_len, masked_payload, payload_len);
  }

  if (client->is_tls) {
    result = turbo_tls_send((turbo_tls_client_t *)client->transport, combined, total_len);
  } else {
    result = turbo_tcp_send((turbo_tcp_client_t *)client->transport, combined, total_len);
  }

  free(combined);
  if (masked_payload) {
    free(masked_payload);
  }

  return result;
}

/**
 * @brief Send multiple buffers as a single WebSocket message.
 *
 * Combines all iov buffers into a single TEXT frame with proper WebSocket framing
 * and client-side masking.
 */
int turbo_websocket_client_sendv(turbo_websocket_client_t *client, const void *iov_ptr,
                                 int iovcnt) {
  if (!client || !iov_ptr || iovcnt <= 0)
    return -1;
  if (client->state != TURBO_WS_STATE_OPEN)
    return -1;

  const turbo_tcp_iovec_t *iov = (const turbo_tcp_iovec_t *)iov_ptr;

  // Calculate total payload length
  size_t total_len = 0;
  for (int i = 0; i < iovcnt; i++) {
    total_len += iov[i].len;
  }

  if (total_len == 0)
    return 0;

  // Build WebSocket frame header
  uint8_t header[14];
  size_t header_len = 0;

  // Byte 0: FIN=1, RSV=0, opcode=TEXT
  header[0] = 0x81; // FIN + TEXT
  header_len++;

  // Byte 1: MASK=1 (client must mask), payload length
  if (total_len <= 125) {
    header[1] = 0x80 | (uint8_t)total_len;
    header_len++;
  } else if (total_len <= 0xFFFF) {
    header[1] = 0x80 | 126;
    header[2] = (total_len >> 8) & 0xFF;
    header[3] = total_len & 0xFF;
    header_len += 3;
  } else {
    header[1] = 0x80 | 127;
    for (int i = 0; i < 8; i++) {
      header[2 + i] = (total_len >> (56 - i * 8)) & 0xFF;
    }
    header_len += 9;
  }

  // Generate masking key
  uint8_t masking_key[4];
  for (int i = 0; i < 4; i++) {
    masking_key[i] = rand() % 256;
    header[header_len++] = masking_key[i];
  }

  // Concatenate and mask all payload data
  uint8_t *masked_payload = malloc(total_len);
  if (!masked_payload)
    return -1;

  size_t offset = 0;
  for (int i = 0; i < iovcnt; i++) {
    memcpy(masked_payload + offset, iov[i].data, iov[i].len);
    offset += iov[i].len;
  }

  // Apply mask
  for (size_t i = 0; i < total_len; i++) {
    masked_payload[i] ^= masking_key[i % 4];
  }

  // Send header + masked payload
  int result;
  size_t combined_len = header_len + total_len;
  char *combined = malloc(combined_len);
  if (!combined) {
    free(masked_payload);
    return -1;
  }
  memcpy(combined, header, header_len);
  memcpy(combined + header_len, masked_payload, total_len);

  if (client->is_tls) {
    result = turbo_tls_send((turbo_tls_client_t *)client->transport, combined, combined_len);
  } else {
    result = turbo_tcp_send((turbo_tcp_client_t *)client->transport, combined, combined_len);
  }

  free(combined);
  free(masked_payload);
  return result;
}

// ============================================================================
// Configuration
// ============================================================================

int turbo_websocket_client_set_tls_context(turbo_websocket_client_t *client,
                                           turbo_tls_context_t *context) {
  if (!client || !context)
    return -1;
  if (!client->is_tls)
    return -1; // Only for TLS clients

  client->tls_context = context;
  return 0;
}
