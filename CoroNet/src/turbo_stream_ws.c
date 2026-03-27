#include <stdint.h>
#include <stddef.h>
#include "turbo_stream_internal.h"
#include "turbo_buffer.h"
#include "websocket_crypto.h"
#include "websocket_frame_parser.h"
#include "websocket_handshake_parser.h"
#include "base64_utils.h"
#include <fmt.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#endif

/* ── Handshake GUID (RFC 6455) ────────────────────────────── */

static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/* ── State machine ────────────────────────────────────────── */

typedef enum {
  WS_ST_INIT        = 0,
  WS_ST_CONNECTING  = 1,
  WS_ST_HANDSHAKING = 2,
  WS_ST_OPEN        = 3,
  WS_ST_CLOSING     = 4,
  WS_ST_CLOSED      = 5,
} ws_state_e;

/* ── accumulation buffer sizes ──────────────────────────────*/

#define WS_RX_BUF_CAP   (16 * 1024)
#define WS_HS_BUF_CAP   4096

typedef struct ws_state_s {
  turbo_stream_t *outer;  /* back-pointer to the outer handle */
  turbo_stream_t *tcp;    /* owned inner TCP stream */

  ws_state_e      state;
  char            key_b64[32];  /* Sec-WebSocket-Key (base64, always 24+1 chars) */
  char            path[256];    /* URI path, e.g. "/" */
  char            host[256];    /* Host header value */
  char            protocol[128];/* Optional Sec-WebSocket-Protocol value */

  /* Handshake accumulation buffer (on heap) */
  uint8_t        *hs_buf;
  size_t          hs_len;

  /* Receive ring: raw bytes from TCP not yet frame-parsed */
  uint8_t        *rx_buf;
  size_t          rx_len;
  size_t          rx_cap;

  /* Fragment reassembly */
  uint8_t        *frag_buf;
  size_t          frag_len;
  size_t          frag_cap;
  uint8_t         frag_opcode;
  int             expecting_cont;
} ws_state_t;

/* ── Forward declarations ─────────────────────────────────── */

static void ws_on_tcp_recv(void *handle, const mem_slice_t *slice, void *peer);
/* turbo_recv_cb signature: int(void*, const mem_slice_t*, void*) */
static int  ws_on_tcp_recv_cb(void *handle, const mem_slice_t *slice, void *peer);
static void ws_on_tcp_connect(void *handle, int status, void *peer);
static void ws_on_tcp_close(void *handle);

static int  ws_process_rx(ws_state_t *st);
static int  ws_process_handshake(ws_state_t *st);
static int  ws_process_frame(ws_state_t *st, const uint8_t *data, size_t len);
static int  ws_send_frame(ws_state_t *st, uint8_t opcode,
                          const uint8_t *payload, size_t len);
static int  ws_send_handshake(ws_state_t *st);

static int ws_fail(ws_state_t *st, int err);
static int ws_protocol_error(ws_state_t *st);

static void ws_drop_inner_tcp(ws_state_t *st) {
  turbo_stream_t *tcp;

  if (!st || !st->tcp) {
    return;
  }

  tcp = st->tcp;
  st->tcp = NULL;
  tcp->user_data = NULL;
  tcp->managed = 0;
  turbo_stream_destroy(tcp);
}

static void ws_free_state(ws_state_t *st) {
  if (!st) {
    return;
  }

  if (st->hs_buf) free(st->hs_buf);
  if (st->rx_buf) free(st->rx_buf);
  if (st->frag_buf) free(st->frag_buf);
  free(st);
}

static int ws_fail(ws_state_t *st, int err) {
  if (st && st->outer && !st->outer->closing) {
    turbo_stream_close(st->outer);
  }
  return err;
}

static int ws_protocol_error(ws_state_t *st) {
  uint8_t close_payload[2] = { 0x03, 0xEA }; /* 1002 Protocol Error */

  if (st && st->state == WS_ST_OPEN) {
    st->state = WS_ST_CLOSING;
    ws_send_frame(st, WS_OPCODE_CLOSE, close_payload, 2);
  }

  return ws_fail(st, TURBO_EPROTONOSUPPORT);
}

/* ── TCP callbacks (called on coro event-loop thread) ─────── */

static int ws_on_tcp_recv_cb(void *handle, const mem_slice_t *slice, void *peer) {
  (void)peer;
  turbo_stream_t *tcp = (turbo_stream_t *)handle;
  ws_state_t     *st  = (ws_state_t *)tcp->user_data;
  if (!st) return 0;

  if (!slice || !slice->data || slice->length == 0) {
    /* EOF from inner stream. Process any remaining buffered data, then close. */
    if (st->rx_len > 0) {
      ws_process_rx(st);
    }
    if (st->outer) turbo_stream_close(st->outer);
    return 0;
  }

  /* Append raw bytes into rx_buf, then drive state machine */
  size_t needed = st->rx_len + slice->length;
  if (needed > st->rx_cap) {
    size_t ncap = st->rx_cap * 2;
    while (ncap < needed) ncap *= 2;
    uint8_t *nb = (uint8_t *)realloc(st->rx_buf, ncap);
    if (!nb) return ws_fail(st, TURBO_ENOMEM);
    st->rx_buf = nb;
    st->rx_cap = ncap;
  }
  memcpy(st->rx_buf + st->rx_len, slice->data, slice->length);
  st->rx_len += slice->length;

  return ws_process_rx(st);
}

static void ws_on_tcp_connect(void *handle, int status, void *peer) {
  (void)peer;
  turbo_stream_t *tcp = (turbo_stream_t *)handle;
  ws_state_t     *st  = (ws_state_t *)tcp->user_data;
  if (!st) return;

  if (status != 0) {
    st->state = WS_ST_CLOSED;
    if (st->outer->on_connect)
      st->outer->on_connect(st->outer, status, NULL);
    return;
  }

  st->state = WS_ST_HANDSHAKING;
  {
    int rc = turbo_stream_recv_start(st->tcp, ws_on_tcp_recv_cb);
    if (rc != 0) {
      st->state = WS_ST_CLOSED;
      if (st->outer->on_connect) {
        st->outer->on_connect(st->outer, rc, NULL);
      }
      turbo_stream_close(st->tcp);
      return;
    }
  }

  {
    int rc = ws_send_handshake(st);
    if (rc != 0) {
      st->state = WS_ST_CLOSED;
      if (st->outer->on_connect) {
        st->outer->on_connect(st->outer, rc, NULL);
      }
      turbo_stream_close(st->tcp);
      return;
    }
  }
}

static void ws_on_tcp_close(void *handle) {
  turbo_stream_t *tcp = (turbo_stream_t *)handle;
  ws_state_t     *st  = (ws_state_t *)tcp->user_data;
  if (!st) return;

  turbo_stream_t *outer = st->outer;
  if (outer) {
    outer->backend_data = NULL;
  }
  tcp->user_data = NULL;
  ws_free_state(st);
  if (outer) {
    turbo_stream_finalize_close(outer);
  }
}

/* ── Handshake send ───────────────────────────────────────── */

static int ws_send_handshake(ws_state_t *st) {
  char buf[2048];
  int  len;

  if (st->protocol[0] != '\0') {
    len = fmt(
        buf, sizeof(buf),
        "GET {} HTTP/1.1\r\n"
        "Host: {}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: {}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: {}\r\n"
        "Origin: https://{}\r\n"
        "\r\n",
        st->path[0] ? st->path : "/",
        st->host,
        st->key_b64,
        st->protocol,
        st->host);
  } else {
    len = fmt(
        buf, sizeof(buf),
        "GET {} HTTP/1.1\r\n"
        "Host: {}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: {}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Origin: https://{}\r\n"
        "\r\n",
        st->path[0] ? st->path : "/",
        st->host,
        st->key_b64,
        st->host);
  }
  if (len <= 0) return TURBO_EINVAL;
  return turbo_stream_send(st->tcp, buf, (size_t)len);
}

/* ── Handshake response validation ───────────────────────── */

static int ws_validate_accept_key(const char *client_b64, const char *server_b64) {
  char combined[128];
  fmt(combined, sizeof(combined), "{}{}", client_b64, WS_GUID);

  sha1_context_t sha;
  uint8_t digest[20];
  sha1_init(&sha);
  sha1_update(&sha, (const uint8_t *)combined, strlen(combined));
  sha1_final(&sha, digest);

  char *expected = NULL;
  if (tn_base64_encode(digest, 20, &expected) != 0) return -1;
  int ok = (strcmp(expected, server_b64) == 0);
  free(expected);
  return ok ? 0 : -1;
}

static int ws_process_handshake(ws_state_t *st) {
  /* Need full HTTP response: look for \r\n\r\n */
  uint8_t *end = (uint8_t *)strstr((const char *)st->hs_buf, "\r\n\r\n");
  if (!end) return 0; /* need more */

  size_t   hdr_len = (size_t)(end + 4 - st->hs_buf);
  
  websocket_handshake_parser_t parser;
  websocket_handshake_parser_init(&parser, WEBSOCKET_HANDSHAKE_MODE_RESPONSE, (const char *)st->hs_buf, hdr_len);
  
  websocket_handshake_token_value_t val;
  int res;
  while ((res = websocket_handshake_parser_scan(&parser, &val)) > 0) {
    if (res == WEBSOCKET_HANDSHAKE_TOKEN_END) break;
  }
  
  int ok = 0;
  if (websocket_handshake_validate(&parser) == 0 &&
      websocket_handshake_is_websocket_request(&parser)) {
    
    char accept_val[96] = {0};
    if (websocket_handshake_get_key(&parser, accept_val, sizeof(accept_val)) == 0) {
      ok = (ws_validate_accept_key(st->key_b64, accept_val) == 0);
    }
  }
  
  websocket_handshake_parser_destroy(&parser);

  if (!ok) {
    st->state = WS_ST_CLOSED;
    if (st->outer->on_connect)
      st->outer->on_connect(st->outer, TURBO_EPROTONOSUPPORT, NULL);
    free(st->hs_buf);
    st->hs_buf = NULL;
    return -1;
  }

  /* Handshake OK. Process any payload bytes that followed in the same packet. */
  size_t leftover = st->hs_len - hdr_len;

  st->state = WS_ST_OPEN;
  st->outer->connected = 1;
  if (st->outer->on_connect)
    st->outer->on_connect(st->outer, 0, NULL);

  if (leftover > 0) {
    ws_process_frame(st, st->hs_buf + hdr_len, leftover);
  }

  free(st->hs_buf);
  st->hs_buf = NULL;
  st->hs_len = 0;
  return 0;
}

/* ── Frame dispatch ───────────────────────────────────────── */

static void ws_deliver(ws_state_t *st, const uint8_t *data, size_t len) {
  if (!st->outer->on_recv || len == 0) return;
  mem_slice_t sl = { .data = (char *)data, .length = len, .buffer = NULL };
  int close_req = st->outer->on_recv(st->outer, &sl, NULL);
  if (close_req) turbo_stream_close(st->outer);
}

static int ws_process_frame(ws_state_t *st, const uint8_t *data, size_t len) {
  size_t consumed = 0;

  while (consumed < len) {
    ws_frame_t        frame;
    ws_parse_result_t pr = ws_frame_parse(data + consumed, len - consumed, &frame);

    if (pr == WS_PARSE_NEED_MORE) {
      /* Shift unconsumed bytes to front of rx_buf */
      size_t rem = len - consumed;
      memmove(st->rx_buf, data + consumed, rem);
      st->rx_len = rem;
      return 0;
    }

    if (pr != WS_PARSE_OK) {
      /* Protocol error — close */
      return ws_protocol_error(st);
    }

    size_t frame_total = frame.header_len + (size_t)frame.payload_len;

    /* Unmask if server sends masked frames (uncommon but valid) */
    if (frame.masked)
      ws_frame_unmask((uint8_t *)frame.payload, (size_t)frame.payload_len,
                      frame.masking_key);

    switch (frame.opcode) {
      case WS_OPCODE_PING:
        ws_send_frame(st, WS_OPCODE_PONG, frame.payload,
                      (size_t)frame.payload_len);
        break;

      case WS_OPCODE_PONG:
        break; /* ignore */

      case WS_OPCODE_CLOSE:
        st->state = WS_ST_CLOSING;
        ws_send_frame(st, WS_OPCODE_CLOSE, frame.payload,
                      (size_t)frame.payload_len);
        turbo_stream_close(st->outer);
        st->rx_len = 0;
        return 0;

      case WS_OPCODE_TEXT:
      case WS_OPCODE_BINARY:
        if (frame.fin) {
          ws_deliver(st, frame.payload, (size_t)frame.payload_len);
        } else {
          /* First fragment */
          st->frag_opcode     = frame.opcode;
          st->expecting_cont  = 1;
          st->frag_len        = 0;
          /* Grow frag buf if needed */
          size_t need = (size_t)frame.payload_len;
          if (need > st->frag_cap) {
            uint8_t *nb = (uint8_t *)realloc(st->frag_buf, need * 2);
            if (!nb) { st->rx_len = 0; return ws_fail(st, TURBO_ENOMEM); }
            st->frag_buf = nb;
            st->frag_cap = need * 2;
          }
          memcpy(st->frag_buf, frame.payload, need);
          st->frag_len = need;
        }
        break;

      case WS_OPCODE_CONTINUATION:
        if (!st->expecting_cont) {
          st->rx_len = 0;
          return ws_protocol_error(st);
        }
        {
          size_t need = st->frag_len + (size_t)frame.payload_len;
          if (need > st->frag_cap) {
            uint8_t *nb = (uint8_t *)realloc(st->frag_buf, need * 2);
            if (!nb) { st->rx_len = 0; return ws_fail(st, TURBO_ENOMEM); }
            st->frag_buf = nb;
            st->frag_cap = need * 2;
          }
          memcpy(st->frag_buf + st->frag_len, frame.payload,
                 (size_t)frame.payload_len);
          st->frag_len += (size_t)frame.payload_len;

          if (frame.fin) {
            st->expecting_cont = 0;
            ws_deliver(st, st->frag_buf, st->frag_len);
            st->frag_len = 0;
          }
        }
        break;

      default:
        st->rx_len = 0;
        return ws_protocol_error(st);
    }

    consumed += frame_total;
  }

  st->rx_len = 0;
  return 0;
}

/**
 * @brief Drive the state machine with bytes accumulated in rx_buf.
 */
static int ws_process_rx(ws_state_t *st) {
  if (st->state == WS_ST_HANDSHAKING) {
    /* Accumulate into hs_buf */
    size_t need = st->hs_len + st->rx_len;
    if (!st->hs_buf) {
      st->hs_buf = (uint8_t *)malloc(WS_HS_BUF_CAP);
      if (!st->hs_buf) return ws_fail(st, TURBO_ENOMEM);
    }
    if (need >= WS_HS_BUF_CAP) {
      return ws_protocol_error(st);
    }
    memcpy(st->hs_buf + st->hs_len, st->rx_buf, st->rx_len);
    st->hs_len += st->rx_len;
    st->rx_len  = 0;
    st->hs_buf[st->hs_len] = '\0';
    return ws_process_handshake(st);
  }

  if (st->state == WS_ST_OPEN) {
    /* Take a snapshot: frame parser points into rx_buf */
    uint8_t *snap = st->rx_buf;
    size_t   slen = st->rx_len;
    st->rx_len = 0;
    return ws_process_frame(st, snap, slen);
  }

  return 0;
}

/* ── Frame send ───────────────────────────────────────────── */

static int ws_send_frame(ws_state_t *st, uint8_t opcode,
                          const uint8_t *payload, size_t len) {
  /* Generate 4-byte mask (client MUST mask, RFC 6455 §5.3) */
  uint8_t mask[4];
  secure_random(mask, 4);

  uint8_t hdr[14];
  size_t  hlen = ws_frame_build_header(hdr, opcode, len, /*fin=*/1, /*masked=*/1, mask);

  /* Build masked payload into a single arena buffer: header + payload */
  size_t total = hlen + len;
  mem_buffer_t *buf = mem_get_buffer(st->outer->arena, total);
  if (!buf) return TURBO_ENOMEM;

  memcpy(buf->data, hdr, hlen);
  if (len > 0) {
    uint8_t *dst = (uint8_t *)buf->data + hlen;
    memcpy(dst, payload, len);
    /* Mask in-place */
    ws_frame_unmask(dst, len, mask);
  }
  mem_set_used(buf, total);

  int rc = turbo_stream_send_buffer(st->tcp, buf, total);
  mem_unref(buf);
  return rc;
}

/* ── Backend vtable implementation ───────────────────────── */

static int ws_init(turbo_stream_t *s) {
  ws_state_t *st = (ws_state_t *)calloc(1, sizeof(ws_state_t));
  if (!st) return TURBO_ENOMEM;

  st->outer = s;
  st->state = WS_ST_INIT;

  /* Default path */
  st->path[0] = '/';
  st->path[1] = '\0';

  /* RX ring */
  st->rx_cap = WS_RX_BUF_CAP;
  st->rx_buf = (uint8_t *)malloc(st->rx_cap);
  if (!st->rx_buf) { free(st); return TURBO_ENOMEM; }

  /* Fragment buffer allocated on first use */
  st->frag_cap = 0;
  st->frag_buf = NULL;

  s->backend_data = st;
  return 0;
}

static int ws_connect(turbo_stream_t *s, const struct sockaddr *addr) {
  ws_state_t *st = (ws_state_t *)s->backend_data;
  if (!st || !addr) return TURBO_EINVAL;

  /* Create inner TCP stream — TCP4/TCP6 or TLS based on kind */
  turbo_stream_kind_t kind;
  if (s->kind == TURBO_STREAM_WSS) {
    kind = TURBO_STREAM_TLS;
  } else {
    kind = (addr->sa_family == AF_INET6) ? TURBO_STREAM_TCP6 : TURBO_STREAM_TCP4;
  }

  st->tcp = turbo_stream_create(s->ctx, kind);
  if (!st->tcp) return TURBO_ENOMEM;

  st->tcp->user_data = st;
  st->tcp->managed   = 1;   /* ws_state_t owns this stream */

  if (st->host[0] == '\0') {
    /* Resolve host string from addr for the Host header */
    if (addr->sa_family == AF_INET) {
      struct sockaddr_in *a4 = (struct sockaddr_in *)addr;
      inet_ntop(AF_INET, &a4->sin_addr, st->host, sizeof(st->host));
    } else {
      struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)addr;
      inet_ntop(AF_INET6, &a6->sin6_addr, st->host, sizeof(st->host));
    }
  }

  if (s->kind == TURBO_STREAM_WSS && st->host[0] != '\0') {
    extern void turbo_stream_tls_set_sni(turbo_stream_t *s, const char *hostname);
    turbo_stream_tls_set_sni(st->tcp, st->host);
  }

  /* Generate Sec-WebSocket-Key */
  uint8_t raw[16];
  if (secure_random(raw, 16) != 0) {
    ws_drop_inner_tcp(st);
    return TURBO_EINVAL;
  }
  char *b64 = NULL;
  if (tn_base64_encode(raw, 16, &b64) != 0) {
    ws_drop_inner_tcp(st);
    return TURBO_EINVAL;
  }
  size_t klen = strlen(b64);
  if (klen >= sizeof(st->key_b64)) {
    free(b64);
    ws_drop_inner_tcp(st);
    return TURBO_EINVAL;
  }
  memcpy(st->key_b64, b64, klen + 1);
  free(b64);

  st->state = WS_ST_CONNECTING;
  {
    int rc = turbo_stream_connect_addr(st->tcp, addr,
                                       ws_on_tcp_connect,
                                       ws_on_tcp_close);
    if (rc != 0) {
      ws_drop_inner_tcp(st);
    }
    return rc;
  }
}

static int ws_connect_pipe(turbo_stream_t *s, const char *name) {
  UNUSED(s); UNUSED(name);
  return TURBO_EINVAL; /* WS is not a pipe */
}

static int ws_send(turbo_stream_t *s, const char *data, size_t len) {
  ws_state_t *st = (ws_state_t *)s->backend_data;
  if (!st || st->state != WS_ST_OPEN) return TURBO_ENOTCONN;
  return ws_send_frame(st, WS_OPCODE_BINARY, (const uint8_t *)data, len);
}

static int ws_flush(turbo_stream_t *s) {
  ws_state_t *st = (ws_state_t *)s->backend_data;
  if (!st || !st->tcp) return 0;
  return turbo_stream_flush(st->tcp);
}

static int ws_recv_start(turbo_stream_t *s) {
  /* on_recv already stashed in s->on_recv by the dispatch layer;
     TCP recv was started during handshake — nothing more to do. */
  (void)s;
  return 0;
}

static void ws_recv_stop(turbo_stream_t *s) {
  ws_state_t *st = (ws_state_t *)s->backend_data;
  if (st && st->tcp) turbo_stream_recv_stop(st->tcp);
}

static void ws_close(turbo_stream_t *s) {
  /* s->closing already set by turbo_stream_close() */
  ws_state_t *st = (ws_state_t *)s->backend_data;
  if (!st) { turbo_stream_finalize_close(s); return; }

  if (st->state == WS_ST_OPEN) {
    st->state = WS_ST_CLOSING;
    uint8_t close_payload[2] = { 0x03, 0xE8 }; /* 1000 Normal Closure */
    ws_send_frame(st, WS_OPCODE_CLOSE, close_payload, 2);
  }

  if (st->tcp) {
    turbo_stream_close(st->tcp);   /* triggers ws_on_tcp_close async */
  } else {
    s->backend_data = NULL;
    ws_free_state(st);
    turbo_stream_finalize_close(s);
  }
}

static int ws_get_local(turbo_stream_t *s, struct sockaddr_storage *a) {
  ws_state_t *st = (ws_state_t *)s->backend_data;
  if (!st || !st->tcp) return TURBO_ENOTSUP;
  return turbo_stream_get_local_addr(st->tcp, a);
}

static int ws_get_peer(turbo_stream_t *s, struct sockaddr_storage *a) {
  ws_state_t *st = (ws_state_t *)s->backend_data;
  if (!st || !st->tcp) return TURBO_ENOTSUP;
  return turbo_stream_get_peer_addr(st->tcp, a);
}

/* Listener ops — Phase 2 */
static int  ws_bind(turbo_stream_listener_t *l, const struct sockaddr *a) {
  UNUSED(l); UNUSED(a); return TURBO_ENOTSUP;
}
static int  ws_bind_pipe(turbo_stream_listener_t *l, const char *n) {
  UNUSED(l); UNUSED(n); return TURBO_ENOTSUP;
}
static int  ws_listen(turbo_stream_listener_t *l, int b) {
  UNUSED(l); UNUSED(b); return TURBO_ENOTSUP;
}
static void ws_listener_close(turbo_stream_listener_t *l) {
  turbo_stream_listener_finalize_close(l);
}

/* ── Vtable ───────────────────────────────────────────────── */

const turbo_stream_backend_ops_t turbo_stream_ws_ops = {
  .init           = ws_init,
  .connect        = ws_connect,
  .connect_pipe   = ws_connect_pipe,
  .send           = ws_send,
  .flush          = ws_flush,
  .recv_start     = ws_recv_start,
  .recv_stop      = ws_recv_stop,
  .close          = ws_close,
  .get_local_addr = ws_get_local,
  .get_peer_addr  = ws_get_peer,
  .bind           = ws_bind,
  .bind_pipe      = ws_bind_pipe,
  .listen         = ws_listen,
  .listener_close = ws_listener_close,
};

CXX_C_API void turbo_stream_ws_set_path_host(turbo_stream_t *s, const char *path, const char *host) {
  turbo_stream_ws_set_path_host_protocol(s, path, host, NULL);
}

CXX_C_API void turbo_stream_ws_set_path_host_protocol(turbo_stream_t *s, const char *path,
                                                      const char *host, const char *protocol) {
  if (!s || (s->kind != TURBO_STREAM_WS && s->kind != TURBO_STREAM_WSS)) return;
  ws_state_t *st = (ws_state_t *)s->backend_data;
  if (!st) return;
  if (path) strncpy(st->path, path, sizeof(st->path) - 1);
  if (host) {
    strncpy(st->host, host, sizeof(st->host) - 1);
    st->host[sizeof(st->host) - 1] = '\0';
    if (s->kind == TURBO_STREAM_WSS && st->tcp) {
      turbo_stream_tls_set_sni(st->tcp, host);
    }
  }
  if (protocol) {
    strncpy(st->protocol, protocol, sizeof(st->protocol) - 1);
    st->protocol[sizeof(st->protocol) - 1] = '\0';
  } else {
    st->protocol[0] = '\0';
  }
}
