// turbo_websocket_server.c - WebSocket server transport for netcore
// Full implementation replacing previous stubs

#include "turbo_websocket_server.h"
#include "base64_utils.h"
#include "stb_sprintf.h"
#include "turbo_tcp.h"
#include "turbo_tls.h"
#include "websocket_crypto.h"
#include "websocket_frame_parser.h"
#include "websocket_handshake_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// WebSocket Protocol Constants
// ============================================================================

/* Static array with padding to avoid ASan false positives from string literal redzones */
static const char WS_GUID[48] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
#define WS_GUID_LEN 36

// ============================================================================
// Forward declarations
// ============================================================================

// Transport callbacks
static void ws_server_on_connection(void *handle, int status, void *peer);
static int ws_server_on_recv(void *handle, const turbo_arena_slice_t *data, void *peer);
static void ws_server_on_close(void *handle);

// Connection management
static void ws_server_add_connection(turbo_websocket_server_t *server,
                                     turbo_websocket_connection_t *conn);
static void ws_server_remove_connection(turbo_websocket_server_t *server,
                                        turbo_websocket_connection_t *conn);
static turbo_websocket_connection_t *
ws_server_find_connection_by_peer(turbo_websocket_server_t *server, void *peer);
static void ws_server_close_connection_internal(turbo_websocket_connection_t *conn);

// Handshake processing
static int ws_server_parse_handshake(turbo_websocket_connection_t *conn, const char *data,
                                     size_t len);
static int ws_server_send_handshake_response(turbo_websocket_connection_t *conn,
                                             const char *sec_websocket_key);
static void ws_compute_accept_key(const char *client_key, char *accept_key);

// Frame processing
static void ws_server_handle_frame(turbo_websocket_connection_t *conn,
                                   const turbo_arena_slice_t *data);
static int ws_send_frame(turbo_websocket_connection_t *conn, websocket_opcode_t opcode,
                         const uint8_t *payload, size_t payload_len, int fin, int mask);
static void ws_handle_control_frame(turbo_websocket_connection_t *conn, websocket_opcode_t opcode,
                                    const uint8_t *payload, size_t payload_len);

// ============================================================================
// Server lifecycle
// ============================================================================

turbo_websocket_server_t *
turbo_websocket_server_create(uv_loop_t *loop, int use_tls,
                              const turbo_websocket_server_config_t *config) {

  if (!loop || !config)
    return NULL;

  turbo_websocket_server_t *server = calloc(1, sizeof(turbo_websocket_server_t));
  if (!server)
    return NULL;

  server->loop = loop;
  server->is_tls = use_tls;
  server->config = *config;
  server->next_connection_id = 1;

  // Allocate buffer pool for zero-copy
  server->buffer_pool = calloc(1, sizeof(turbo_arena_t));
  if (!server->buffer_pool) {
    free(server);
    return NULL;
  }

  if (turbo_arena_init(server->buffer_pool, 1024 * 1024) != 0) { // 1MB pool
    free(server->buffer_pool);
    free(server);
    return NULL;
  }

  return server;
}

int turbo_websocket_server_listen(turbo_websocket_server_t *server, const char *host, int port,
                                  int backlog) {
  if (!server)
    return -1;

  // Create and initialize underlying transport server
  if (server->is_tls) {
    // Allocate TLS server
    turbo_tls_server_t *tls_srv = calloc(1, sizeof(turbo_tls_server_t));
    if (!tls_srv)
      return -1;

    server->transport_server = tls_srv;

    // Initialize TLS server (requires TLS context)
    // Note: TLS context must be set via turbo_websocket_server_set_tls_config first
    turbo_tls_context_t *tls_ctx = server->tls_context;
    if (!tls_ctx) {
      free(tls_srv);
      server->transport_server = NULL;
      return -1;
    }

    int result = turbo_tls_server_init(tls_srv, server->loop, tls_ctx, host, (unsigned short)port);
    if (result != 0) {
      free(tls_srv);
      server->transport_server = NULL;
      return result;
    }

    tls_srv->user_data = server; // Store WebSocket server reference AFTER init

    // Start server with callbacks
    result = turbo_tls_server_start(tls_srv, ws_server_on_recv, ws_server_on_connection,
                                    ws_server_on_close);
    if (result == 0) {
      server->listening = 1;
    }
    return result;
  } else {
    // Allocate TCP server
    turbo_tcp_server_t *tcp_srv = calloc(1, sizeof(turbo_tcp_server_t));
    if (!tcp_srv)
      return -1;

    server->transport_server = tcp_srv;

    // Initialize TCP server (this does memset, so set user_data after)
    int result = turbo_tcp_server_init(tcp_srv, server->loop, host, (unsigned short)port);
    if (result != 0) {
      free(tcp_srv);
      server->transport_server = NULL;
      return result;
    }

    tcp_srv->user_data = server; // Store WebSocket server reference AFTER init

    // Start server with callbacks
    result = turbo_tcp_server_start(tcp_srv, ws_server_on_recv, ws_server_on_connection,
                                    ws_server_on_close);
    if (result == 0) {
      server->listening = 1;
    }
    return result;
  }
}

void turbo_websocket_server_stop(turbo_websocket_server_t *server) {
  if (!server)
    return;
  server->listening = 0;
}

int turbo_websocket_server_shutdown(turbo_websocket_server_t *server, int timeout_ms) {
  if (!server)
    return -1;

  turbo_websocket_server_stop(server);

  // Close all connections
  turbo_websocket_connection_t *conn = server->connections_head;
  while (conn) {
    turbo_websocket_connection_t *next = conn->next;
    turbo_websocket_server_close_connection(conn, 1001, "Server shutdown");
    conn = next;
  }

  return 0;
}

void turbo_websocket_server_destroy(turbo_websocket_server_t *server) {
  if (!server)
    return;

  if (server->buffer_pool) {
    turbo_arena_free(server->buffer_pool);
    free(server->buffer_pool);
  }

  free(server);
}

void turbo_websocket_server_set_callbacks(turbo_websocket_server_t *server,
                                          turbo_connect_cb on_connection, turbo_recv_cb on_recv,
                                          turbo_close_cb on_close) {
  if (!server)
    return;
  server->on_connection = on_connection;
  server->on_recv = on_recv;
  server->on_close = on_close;
}

int turbo_websocket_server_get_connection_count(turbo_websocket_server_t *server) {
  return server ? server->connection_count : 0;
}

// ============================================================================
// Connection management
// ============================================================================

static void ws_server_add_connection(turbo_websocket_server_t *server,
                                     turbo_websocket_connection_t *conn) {
  conn->next = NULL;
  conn->prev = server->connections_tail;

  if (server->connections_tail) {
    server->connections_tail->next = conn;
  } else {
    server->connections_head = conn;
  }

  server->connections_tail = conn;
  server->connection_count++;
}

static void ws_server_remove_connection(turbo_websocket_server_t *server,
                                        turbo_websocket_connection_t *conn) {
  if (conn->prev) {
    conn->prev->next = conn->next;
  } else {
    server->connections_head = conn->next;
  }

  if (conn->next) {
    conn->next->prev = conn->prev;
  } else {
    server->connections_tail = conn->prev;
  }

  server->connection_count--;
}

static turbo_websocket_connection_t *
ws_server_find_connection_by_peer(turbo_websocket_server_t *server, void *peer) {
  turbo_websocket_connection_t *conn = server->connections_head;
  while (conn) {
    if (conn->transport_client == peer) {
      return conn;
    }
    conn = conn->next;
  }
  return NULL;
}

// ============================================================================
// Transport callbacks
// ============================================================================

static void ws_server_on_connection(void *handle, int status, void *peer) {
  // handle = newly accepted TCP/TLS client
  // peer = TCP/TLS server (per turbo_tcp.c callback convention)

  // Retrieve WebSocket server from transport server's user_data
  turbo_tcp_server_t *tcp_srv = (turbo_tcp_server_t *)peer;
  turbo_websocket_server_t *server = (turbo_websocket_server_t *)tcp_srv->user_data;

  if (status != 0 || !server)
    return;

  // Check max connections
  if (server->config.max_connections > 0 &&
      server->connection_count >= server->config.max_connections) {
    // Reject connection
    if (server->is_tls) {
      turbo_tls_client_close((turbo_tls_client_t *)handle);
    } else {
      turbo_tcp_client_close((turbo_tcp_client_t *)handle);
    }
    return;
  }

  // Create WebSocket connection object
  turbo_websocket_connection_t *conn = calloc(1, sizeof(*conn));
  if (!conn)
    return;

  conn->server = server;
  conn->state = TURBO_WS_CONN_HANDSHAKING;
  conn->id = server->next_connection_id++;
  conn->transport_client = handle;

  // Allocate connection arena
  conn->conn_arena = calloc(1, sizeof(turbo_arena_t));
  if (!conn->conn_arena) {
    free(conn);
    return;
  }

  if (turbo_arena_init(conn->conn_arena, 16384) != 0) { // 16KB
    free(conn->conn_arena);
    free(conn);
    return;
  }

  // Allocate handshake buffer
  conn->handshake_recv_buffer = turbo_arena_get_buffer(conn->conn_arena, 4096);
  if (!conn->handshake_recv_buffer) {
    turbo_arena_free(conn->conn_arena);
    free(conn->conn_arena);
    free(conn);
    return;
  }

  conn->handshake_recv_used = 0;

  // Add to server's connection list
  ws_server_add_connection(server, conn);

  // Store connection reference in transport client
  if (server->is_tls) {
    ((turbo_tls_client_t *)handle)->user_data = conn;
  } else {
    ((turbo_tcp_client_t *)handle)->user_data = conn;
  }
}

static int ws_server_on_recv(void *handle, const turbo_arena_slice_t *data, void *peer) {
  // For recv callback: handle = TCP/TLS client, peer = NULL
  // Get WebSocket connection from client's user_data
  turbo_tcp_client_t *tcp_client = (turbo_tcp_client_t *)handle;
  turbo_websocket_connection_t *conn = (turbo_websocket_connection_t *)tcp_client->user_data;

  if (!conn || !conn->server)
    return 0;

  turbo_websocket_server_t *server = conn->server;

  if (conn->state == TURBO_WS_CONN_HANDSHAKING) {
    // Accumulate handshake data (leave room for null terminator)
    if (conn->handshake_recv_used + data->length + 1 > conn->handshake_recv_buffer->capacity) {
      // Handshake too large, reject
      ws_server_close_connection_internal(conn);
      return 0;
    }

    memcpy(conn->handshake_recv_buffer->data + conn->handshake_recv_used, data->data, data->length);
    conn->handshake_recv_used += data->length;

    // Null-terminate for strstr
    conn->handshake_recv_buffer->data[conn->handshake_recv_used] = '\0';

    // Check for complete handshake (\r\n\r\n)
    const char *end = strstr(conn->handshake_recv_buffer->data, "\r\n\r\n");
    if (!end)
      return 0; // Need more data

    // Parse handshake request
    int result = ws_server_parse_handshake(conn, conn->handshake_recv_buffer->data,
                                           conn->handshake_recv_used);
    if (result != 0) {
      ws_server_close_connection_internal(conn);
      return 0;
    }

    // Handshake successful, switch to OPEN state
    conn->state = TURBO_WS_CONN_OPEN;

    // Invoke user on_connection callback
    if (server->on_connection) {
      server->on_connection(handle, 0, conn);
    }
  } else if (conn->state == TURBO_WS_CONN_OPEN || conn->state == TURBO_WS_CONN_CLOSING) {
    // Process WebSocket frames
    ws_server_handle_frame(conn, data);
  }

  return 0;
}

static void ws_server_on_close(void *handle) {
  // handle = TCP/TLS client that was closed

  turbo_tcp_client_t *tcp_client = (turbo_tcp_client_t *)handle;
  turbo_websocket_connection_t *conn = (turbo_websocket_connection_t *)tcp_client->user_data;

  if (!conn)
    return;

  conn->state = TURBO_WS_CONN_CLOSED;

  // Invoke user on_close callback
  if (conn->server->on_close) {
    conn->server->on_close(conn);
  }

  // Remove from server's connection list
  ws_server_remove_connection(conn->server, conn);

  // Free resources
  if (conn->conn_arena) {
    turbo_arena_free(conn->conn_arena);
    free(conn->conn_arena);
  }

  free(conn);
}

static void ws_server_close_connection_internal(turbo_websocket_connection_t *conn) {
  if (!conn)
    return;

  // Close underlying transport
  if (conn->server->is_tls) {
    turbo_tls_client_close((turbo_tls_client_t *)conn->transport_client);
  } else {
    turbo_tcp_client_close((turbo_tcp_client_t *)conn->transport_client);
  }
}

// ============================================================================
// Handshake processing
// ============================================================================

static int ws_server_parse_handshake(turbo_websocket_connection_t *conn, const char *data,
                                     size_t len) {
  // Simple parsing for WebSocket handshake
  // Extract Sec-WebSocket-Key header

  const char *key_header = "Sec-WebSocket-Key: ";
  const char *key_start = strstr(data, key_header);
  if (!key_start)
    return -1;

  key_start += strlen(key_header);
  const char *key_end = strstr(key_start, "\r\n");
  if (!key_end)
    return -1;

  size_t key_len = key_end - key_start;
  if (key_len == 0 || key_len > 64)
    return -1;

  // Copy key
  char sec_websocket_key[65];
  memcpy(sec_websocket_key, key_start, key_len);
  sec_websocket_key[key_len] = '\0';

  // Extract request path (optional)
  const char *path_start = strstr(data, "GET ");
  if (path_start) {
    path_start += 4; // Skip "GET "
    const char *path_end = strstr(path_start, " HTTP");
    if (path_end) {
      size_t path_len = path_end - path_start;
      if (path_len > 0) {
        char *path_copy = (char *)turbo_arena_alloc(conn->conn_arena, path_len + 1);
        if (path_copy) {
          memcpy(path_copy, path_start, path_len);
          path_copy[path_len] = '\0';
          conn->request_path = path_copy;
        }
      }
    }
  }

  // Send handshake response
  return ws_server_send_handshake_response(conn, sec_websocket_key);
}

static void ws_compute_accept_key(const char *client_key, char *accept_key) {
  // Concatenate client_key + WS_GUID
  char combined[256];
  static const char FMT_COMBINED[32] = "%s%s";
  stbsp_snprintf(combined, (int)sizeof(combined), FMT_COMBINED, client_key, WS_GUID);

  // Compute SHA-1
  sha1_context_t sha1;
  uint8_t digest[20];
  sha1_init(&sha1);
  sha1_update(&sha1, (const uint8_t *)combined, strlen(combined));
  sha1_final(&sha1, digest);

  // Encode as base64
  char *b64 = NULL;
  if (tn_base64_encode(digest, 20, &b64) == 0) {
    strcpy(accept_key, b64);
    free(b64);
  }
}

static int ws_server_send_handshake_response(turbo_websocket_connection_t *conn,
                                             const char *sec_websocket_key) {
  // Compute Sec-WebSocket-Accept
  char accept_key[64];
  ws_compute_accept_key(sec_websocket_key, accept_key);

  // Build HTTP response
  char response[1024];
  static const char FMT_RESP[128] = "HTTP/1.1 101 Switching Protocols\r\n"
                                    "Upgrade: websocket\r\n"
                                    "Connection: Upgrade\r\n"
                                    "Sec-WebSocket-Accept: %s\r\n";
  int len = stbsp_snprintf(response, (int)sizeof(response), FMT_RESP, accept_key);

  // Add optional Sec-WebSocket-Protocol
  if (conn->selected_subprotocol) {
    static const char FMT_SUB[48] = "Sec-WebSocket-Protocol: %s\r\n";
    len += stbsp_snprintf(response + len, (int)(sizeof(response) - len),
                          FMT_SUB, conn->selected_subprotocol);
  }

  // End headers
  static const char FMT_END[8] = "\r\n";
  len += stbsp_snprintf(response + len, (int)(sizeof(response) - len), FMT_END);

  // Send response
  if (conn->server->is_tls) {
    return turbo_tls_send((turbo_tls_client_t *)conn->transport_client, response, len);
  } else {
    return turbo_tcp_send((turbo_tcp_client_t *)conn->transport_client, response, len);
  }
}

// ============================================================================
// Sending operations
// ============================================================================

int turbo_websocket_server_send(turbo_websocket_connection_t *conn, const char *data, size_t len) {

  if (!conn || !data)
    return -1;
  if (conn->state != TURBO_WS_CONN_OPEN)
    return -1;

  // Send as TEXT frame (FIN=1, no mask)
  return ws_send_frame(conn, WS_OPCODE_TEXT, (const uint8_t *)data, len, 1, 0);
}

int turbo_websocket_server_send_binary(turbo_websocket_connection_t *conn, const void *data,
                                       size_t len) {

  if (!conn || !data)
    return -1;
  if (conn->state != TURBO_WS_CONN_OPEN)
    return -1;

  // Send as BINARY frame (FIN=1, no mask)
  return ws_send_frame(conn, WS_OPCODE_BINARY, (const uint8_t *)data, len, 1, 0);
}

int turbo_websocket_server_send_ping(turbo_websocket_connection_t *conn, const uint8_t *payload,
                                     size_t len) {

  if (!conn)
    return -1;
  if (len > 125)
    return -1; // PING payload max 125 bytes
  if (conn->state != TURBO_WS_CONN_OPEN)
    return -1;

  return ws_send_frame(conn, WS_OPCODE_PING, payload, len, 1, 0);
}

int turbo_websocket_server_close_connection(turbo_websocket_connection_t *conn, uint16_t code,
                                            const char *reason) {

  if (!conn)
    return -1;
  if (conn->state != TURBO_WS_CONN_OPEN)
    return -1;

  // Build CLOSE frame payload
  uint8_t close_payload[125];
  size_t close_len = 2;
  close_payload[0] = (code >> 8) & 0xFF;
  close_payload[1] = code & 0xFF;

  if (reason) {
    size_t reason_len = strlen(reason);
    if (reason_len > 123)
      reason_len = 123;
    memcpy(close_payload + 2, reason, reason_len);
    close_len += reason_len;
  }

  conn->close_sent = 1;
  conn->state = TURBO_WS_CONN_CLOSING;

  return ws_send_frame(conn, WS_OPCODE_CLOSE, close_payload, close_len, 1, 0);
}

static int ws_send_frame(turbo_websocket_connection_t *conn, websocket_opcode_t opcode,
                         const uint8_t *payload, size_t payload_len, int fin, int mask) {

  // Build frame header
  uint8_t header[14]; // Max header size
  size_t header_len = 0;

  // Byte 0: FIN + RSV + Opcode
  header[0] = (fin ? 0x80 : 0x00) | (opcode & 0x0F);

  // Byte 1: MASK + Payload length
  header[1] = mask ? 0x80 : 0x00;

  if (payload_len <= 125) {
    header[1] |= payload_len;
    header_len = 2;
  } else if (payload_len <= 0xFFFF) {
    header[1] |= 126;
    header[2] = (payload_len >> 8) & 0xFF;
    header[3] = payload_len & 0xFF;
    header_len = 4;
  } else {
    header[1] |= 127;
    for (int i = 0; i < 8; i++) {
      header[2 + i] = (payload_len >> (56 - i * 8)) & 0xFF;
    }
    header_len = 10;
  }

  // Send using scatter-gather (header + payload)
  if (conn->server->is_tls) {
    turbo_tls_iovec_t iov[2];
    iov[0].data = (const char *)header;
    iov[0].len = header_len;
    iov[1].data = (const char *)payload;
    iov[1].len = payload_len;
    return turbo_tls_sendv((turbo_tls_client_t *)conn->transport_client, iov,
                           payload_len > 0 ? 2 : 1);
  } else {
    turbo_tcp_iovec_t iov[2];
    iov[0].data = (const char *)header;
    iov[0].len = header_len;
    iov[1].data = (const char *)payload;
    iov[1].len = payload_len;
    return turbo_tcp_sendv((turbo_tcp_client_t *)conn->transport_client, iov,
                           payload_len > 0 ? 2 : 1);
  }
}

/**
 * @brief Send multiple buffers as a single WebSocket message.
 *
 * Combines all iov buffers into a single TEXT frame with proper WebSocket framing.
 */
int turbo_websocket_server_sendv(turbo_websocket_connection_t *conn, const void *iov_ptr,
                                 int iovcnt) {
  if (!conn || !iov_ptr || iovcnt <= 0)
    return -1;
  if (conn->state != TURBO_WS_CONN_OPEN)
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

  // Byte 1: MASK=0 (server doesn't mask), payload length
  if (total_len <= 125) {
    header[1] = (uint8_t)total_len;
    header_len++;
  } else if (total_len <= 0xFFFF) {
    header[1] = 126;
    header[2] = (total_len >> 8) & 0xFF;
    header[3] = total_len & 0xFF;
    header_len += 3;
  } else {
    header[1] = 127;
    for (int i = 0; i < 8; i++) {
      header[2 + i] = (total_len >> (56 - i * 8)) & 0xFF;
    }
    header_len += 9;
  }

  // Build combined iov array: header + all payload buffers
  int total_iov = 1 + iovcnt;
  turbo_tcp_iovec_t *combined_iov = malloc(total_iov * sizeof(turbo_tcp_iovec_t));
  if (!combined_iov)
    return -1;

  combined_iov[0].data = (const char *)header;
  combined_iov[0].len = header_len;

  for (int i = 0; i < iovcnt; i++) {
    combined_iov[1 + i].data = iov[i].data;
    combined_iov[1 + i].len = iov[i].len;
  }

  // Send via transport
  int result;
  if (conn->server->is_tls) {
    result = turbo_tls_sendv((turbo_tls_client_t *)conn->transport_client,
                             (turbo_tls_iovec_t *)combined_iov, total_iov);
  } else {
    result = turbo_tcp_sendv((turbo_tcp_client_t *)conn->transport_client, combined_iov, total_iov);
  }

  free(combined_iov);
  return result;
}

// ============================================================================
// Frame processing
// ============================================================================

static void ws_server_handle_frame(turbo_websocket_connection_t *conn,
                                   const turbo_arena_slice_t *data) {
  // Parse WebSocket frame
  ws_frame_t frame;
  ws_parse_result_t result = ws_frame_parse((const uint8_t *)data->data, data->length, &frame);

  if (result == WS_PARSE_NEED_MORE) {
    // Buffer incomplete frame data for later
    if (!conn->frame_recv_buffer) {
      conn->frame_recv_buffer = turbo_arena_get_buffer(conn->conn_arena, 65536);
      if (!conn->frame_recv_buffer)
        return;
    }

    size_t space = conn->frame_recv_buffer->capacity - conn->frame_recv_used;
    if (data->length <= space) {
      memcpy(conn->frame_recv_buffer->data + conn->frame_recv_used, data->data, data->length);
      conn->frame_recv_used += data->length;
    }
    return;
  }

  if (result != WS_PARSE_OK) {
    ws_server_close_connection_internal(conn);
    return;
  }

  // Extract frame info
  int fin = frame.fin;
  uint8_t opcode = frame.opcode;
  uint64_t payload_len = frame.payload_len;
  int masked = frame.masked;

  // Get payload pointer
  const uint8_t *payload = frame.payload;

  // Unmask if needed
  uint8_t *decoded_payload = NULL;
  if (masked && payload_len > 0) {
    decoded_payload = malloc(payload_len);
    if (!decoded_payload)
      return;
    memcpy(decoded_payload, payload, payload_len);
    ws_frame_unmask(decoded_payload, payload_len, frame.masking_key);
    payload = decoded_payload;
  }

  // Handle control frames (can appear between fragmented data frames)
  if (opcode >= 0x08) {
    ws_handle_control_frame(conn, opcode, payload, payload_len);
    if (decoded_payload)
      free(decoded_payload);
    return;
  }

  // Handle fragmentation
  if (opcode == WS_OPCODE_CONTINUATION) {
    // Continuation frame - must be expecting one
    if (!conn->expecting_continuation) {
      // Protocol error: unexpected continuation
      ws_server_close_connection_internal(conn);
      if (decoded_payload)
        free(decoded_payload);
      return;
    }

    // Append to fragment buffer
    if (conn->fragment_buffer && payload_len > 0) {
      size_t space = conn->fragment_buffer->capacity - conn->fragment_buffer_used;
      if (payload_len <= space) {
        memcpy(conn->fragment_buffer->data + conn->fragment_buffer_used, payload, payload_len);
        conn->fragment_buffer_used += payload_len;
      } else {
        // Message too large
        ws_server_close_connection_internal(conn);
        if (decoded_payload)
          free(decoded_payload);
        return;
      }
    }

    if (fin) {
      // Final fragment - deliver complete message
      conn->expecting_continuation = 0;

      if (conn->server->on_recv && conn->fragment_buffer) {
        turbo_arena_slice_t slice = {.data = conn->fragment_buffer->data,
                                     .length = conn->fragment_buffer_used,
                                     .buffer = NULL};
        conn->server->on_recv(conn->transport_client, &slice, conn);
      }

      conn->messages_received++;
      conn->bytes_received += conn->fragment_buffer_used;
      conn->fragment_buffer_used = 0;
    }
  } else if (opcode == WS_OPCODE_TEXT || opcode == WS_OPCODE_BINARY) {
    if (fin) {
      // Complete unfragmented message
      if (conn->server->on_recv) {
        turbo_arena_slice_t slice = {
            .data = (char *)payload, .length = payload_len, .buffer = NULL};
        conn->server->on_recv(conn->transport_client, &slice, conn);
      }

      conn->messages_received++;
      conn->bytes_received += payload_len;
    } else {
      // First fragment of a fragmented message
      conn->expecting_continuation = 1;
      conn->fragment_opcode = opcode;

      // Allocate fragment buffer if needed
      if (!conn->fragment_buffer) {
        size_t buf_size = conn->server->config.max_message_size;
        if (buf_size == 0)
          buf_size = 1024 * 1024; // 1MB default
        conn->fragment_buffer = turbo_arena_get_buffer(conn->conn_arena, buf_size);
        if (!conn->fragment_buffer) {
          if (decoded_payload)
            free(decoded_payload);
          return;
        }
      }

      // Copy first fragment
      conn->fragment_buffer_used = 0;
      if (payload_len > 0 && payload_len <= conn->fragment_buffer->capacity) {
        memcpy(conn->fragment_buffer->data, payload, payload_len);
        conn->fragment_buffer_used = payload_len;
      }
    }
  }

  if (decoded_payload)
    free(decoded_payload);
}

static void ws_handle_control_frame(turbo_websocket_connection_t *conn, websocket_opcode_t opcode,
                                    const uint8_t *payload, size_t payload_len) {
  if (opcode == WS_OPCODE_CLOSE) {
    conn->close_received = 1;

    // Send CLOSE response if we haven't already
    if (!conn->close_sent) {
      uint16_t code = 1000; // Normal closure
      if (payload_len >= 2) {
        code = (payload[0] << 8) | payload[1];
      }
      turbo_websocket_server_close_connection(conn, code, NULL);
    }

    // Close transport
    ws_server_close_connection_internal(conn);
  } else if (opcode == WS_OPCODE_PING) {
    // Send PONG response
    ws_send_frame(conn, WS_OPCODE_PONG, payload, payload_len, 1, 0);
  } else if (opcode == WS_OPCODE_PONG) {
    // Ignore PONG
  }
}

// ============================================================================
// Utility functions
// ============================================================================

const char *turbo_websocket_connection_get_subprotocol(turbo_websocket_connection_t *conn) {
  return conn ? conn->selected_subprotocol : NULL;
}

const char *turbo_websocket_connection_get_path(turbo_websocket_connection_t *conn) {
  return conn ? conn->request_path : NULL;
}

// ============================================================================
// Configuration
// ============================================================================

int turbo_websocket_server_set_tls_context(turbo_websocket_server_t *server,
                                           turbo_tls_context_t *context) {
  if (!server || !context)
    return -1;
  if (!server->is_tls)
    return -1; // Only for TLS servers

  server->tls_context = context;
  return 0;
}
