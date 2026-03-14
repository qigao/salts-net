/**
 * @file turbo_coro_socket_udp.c
 * @brief UDP (connectionless) transport implementation.
 */

#include "CoroNet/turbo_coro_internal.h"
#include <stdlib.h>

/* ── UDP Server Send ──────────────────────────────────────── */

static int udp_server_send(coro_socket_t *s, const char *data, size_t len) {
  coro_socket_t *server = (coro_socket_t *)s->user_data;
  if (!server || server->transport != TURBO_UDP) return UV_EINVAL;
  
  return turbo_udp_send(
      &server->udp, 
      (const struct sockaddr *)&s->peer_addr, 
      data, 
      len
  );
}

/* ── UDP Server Recv ──────────────────────────────────────── */

static int udp_server_recv_start(coro_socket_t *s) {
  if (s->dgram_consumed) return UV_EOF;
  s->dgram_consumed = 1;
  return 0;
}

static void udp_server_recv_stop(coro_socket_t *s) {
  UNUSED(s);
}

/* ── UDP Server Close ─────────────────────────────────────── */

static void udp_server_close(coro_socket_t *s) {
  turbo_udp_server_stop(&s->udp);
}

/* ── UDP Server Transport Ops ─────────────────────────────── */

const coro_transport_ops_t udp_server_ops = {
    .connect = NULL,
    .bind = NULL,
    .listen = NULL,
    .accept = NULL,
    .send = udp_server_send,
    .recv_start = udp_server_recv_start,
    .recv_stop = udp_server_recv_stop,
    .get_local_addr = NULL,
    .close = udp_server_close
};
