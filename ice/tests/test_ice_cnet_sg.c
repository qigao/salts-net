#include "ice_cnet_datagram.h"
#include "ice/salts_stun.h"
#include "stun_binding_transaction.h"
#include "salts_ice_internal.h"
#include <cnet/destination_policy.h>
#include <cnet/sg_host.h>
#include <cmeta_buffer.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <tinytest.h>
#include <string.h>
#include <stdlib.h>

enum { SG_OWNERS = 4, SG_BATCH = 16, SG_TIMEOUT_MS = 5000, SG_PACKET = 512 };
static const char tcp_payload[] = "tcp-neighbor-after-udp-stop";
static const char tcp_during_stun[] = "tcp-neighbor-during-stun-timeout";

typedef struct udp_lane {
  native_io_sharded *runtime;
  size_t index;
  native_io_backend *backend;
  native_io_sharded_host_lease lease;
  ice_cnet_datagram_t udp[2];
  salts_ice_agent_t *cleanup_agent;
  ice_cnet_datagram_t *cleanup_udp; /* Routing alias; the Agent owns storage. */
  cnet_client tcp;
  cnet_listener listener;
  cnet_connection outbound, inbound;
  size_t connected, received, observes;
  const char *expected_tcp;
  size_t expected_tcp_size;
  int status;
  int failed_line;
  bool closing, drained;
} udp_lane;

static native_io_backend_kind sg_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static void sg_record(udp_lane *lane, int status) {
  if (lane->status == SALTS_OK && status != SALTS_OK) lane->status = status;
}

static bool sg_quiescent(void *user) {
  udp_lane *lane = (udp_lane *)user;
  return !lane->udp[0].initialized && !lane->udp[1].initialized &&
         !lane->tcp.impl && !lane->listener.impl && !lane->cleanup_agent;
}

static int sg_send(udp_lane *lane, cnet_connection connection, const void *data, size_t size) {
  mem_buffer_t *buffer = mem_get_buffer(mem_global(), size);
  int status;
  if (!buffer) return SALTS_ENOMEM;
  memcpy(mem_buffer_data(buffer), data, size);
  mem_set_used(buffer, size);
  status = cnet_send_buffer(&lane->tcp, connection, buffer);
  mem_buffer_release(buffer);
  return status;
}

static void sg_state(void *user, cnet_connection connection,
                      cnet_connection_state state, const cnet_error *error) {
  udp_lane *lane = (udp_lane *)user;
  if (native_io_sharded_current_shard(lane->runtime) != lane->index)
    sg_record(lane, SALTS_EPROTO);
  if (state == CNET_CONNECTION_CONNECTED) {
    ++lane->connected;
    sg_record(lane, cnet_receive(&lane->tcp, connection, 1u));
  } else if (state == CNET_CONNECTION_FAILED && !lane->closing) {
    sg_record(lane, error && error->status != SALTS_OK ? error->status : SALTS_EIO);
  }
}

static void sg_receive(void *user, cnet_connection connection,
                        const cnet_receive_view *view) {
  udp_lane *lane = (udp_lane *)user;
  if (native_io_sharded_current_shard(lane->runtime) != lane->index ||
      view->kind != CNET_MESSAGE_BYTES) {
    sg_record(lane, SALTS_EPROTO);
    return;
  }
  if (connection.slot == lane->inbound.slot &&
      connection.generation == lane->inbound.generation) {
    sg_record(lane, sg_send(lane, connection, view->data, view->size));
  } else {
    if (view->size > lane->expected_tcp_size - lane->received ||
        memcmp(view->data, lane->expected_tcp + lane->received, view->size) != 0) {
      sg_record(lane, SALTS_EPROTO);
      return;
    }
    lane->received += view->size;
  }
  sg_record(lane, cnet_receive(&lane->tcp, connection, 1u));
}

/* One observe per Owner turn; every borrowed datagram remains in the routing
 * set through its real terminal, including observed-but-unrouted cancellation. */
static int sg_pump(native_io_sharded_context *context, udp_lane *lane) {
  native_io_sharded_completion batch[SG_BATCH];
  cnet_client *clients[] = {&lane->tcp};
  cnet_datagram *datagrams[3];
  size_t count = 0u, udp_count = 0u, events, accepts, sharded;
  cnet_sg_host_routes routes = {sizeof(routes), CNET_SG_HOST_ROUTING_VERSION,
      lane->listener.impl ? &lane->listener : NULL, clients, lane->tcp.impl ? 1u : 0u};
  int status;
  for (size_t i = 0u; i < 2u; ++i) {
    if (!lane->udp[i].initialized) continue;
    datagrams[udp_count++] = &lane->udp[i].datagram;
    sg_record(lane, ice_cnet_datagram_advance_external(&lane->udp[i], &events));
  }
  if (lane->cleanup_udp && lane->cleanup_udp->initialized) {
    datagrams[udp_count++] = &lane->cleanup_udp->datagram;
    sg_record(lane, ice_cnet_datagram_advance_external(lane->cleanup_udp, &events));
  }
  if (lane->tcp.impl) sg_record(lane, cnet_client_advance_external(&lane->tcp, &events));
  status = native_io_sharded_context_observe_host(context, lane->lease, batch,
                                                  SG_BATCH, 1u, &count);
  ++lane->observes;
  if (status == SALTS_ETIMEDOUT) status = SALTS_OK;
  sg_record(lane, status);
  if (status == SALTS_OK)
    sg_record(lane, cnet_sg_host_route_batch_with_datagrams(batch, count, &routes,
                          datagrams, udp_count, &accepts, &sharded));
  return lane->status;
}

#define SG_OK(call) do { int rc_ = (call); if (rc_ != SALTS_OK) { \
  lane->failed_line = __LINE__; sg_record(lane, rc_); goto cleanup; } } while (0)
#define SG_REQUIRE(expr) do { if (!(expr)) { \
  lane->failed_line = __LINE__; sg_record(lane, SALTS_EPROTO); goto cleanup; } } while (0)
#define SG_UNTIL(expr) do { while (!(expr)) { \
  if (cmeta_monotonic_ms() >= deadline) { sg_record(lane, SALTS_ETIMEDOUT); goto cleanup; } \
  SG_OK(sg_pump(context, lane)); } } while (0)

static int sg_binding(native_io_sharded_context *context, udp_lane *lane,
                       const cnet_datagram_peer *destination, bool respond) {
  const uint64_t deadline = cmeta_monotonic_ms() + SG_TIMEOUT_MS;
  uint64_t now = cmeta_monotonic_ms(), tag = 0u;
  stun_binding_transaction binding = {0};
  stun_transaction_id_t first_id;
  cnet_datagram_peer actual;
  unsigned char bytes[SG_PACKET];
  size_t size = 0u;
  SG_OK(ice_cnet_datagram_receive_begin(&lane->udp[1]));
  SG_OK(stun_binding_transaction_start(&binding, &lane->udp[0], destination,
                                        1000u, 2u, now));
  SG_REQUIRE(stun_binding_transaction_start(&binding, &lane->udp[0], destination,
                                             1000u, 2u, now) == SALTS_EALREADY);
  SG_REQUIRE(ice_cnet_datagram_poll_until(&lane->udp[0], deadline) == SALTS_ENOTSUP);
  SG_UNTIL(lane->udp[1].receive_ready && !lane->udp[0].send_pending);
  SG_OK(ice_cnet_datagram_receive_take(&lane->udp[1], &actual, bytes, sizeof(bytes), &size));
  SG_REQUIRE(size == STUN_HEADER_SIZE && stun_is_stun_message(bytes, size));
  memcpy(first_id.id, bytes + 8u, sizeof(first_id.id));
  SG_REQUIRE(stun_binding_transaction_advance(&binding, now) == SALTS_EBUSY);

  /* Deliberately drop attempt one. A real TCP exchange must complete while its
   * STUN response is outstanding; the protocol clock advances deterministically. */
  lane->expected_tcp = tcp_during_stun;
  lane->expected_tcp_size = sizeof(tcp_during_stun);
  lane->received = 0u;
  SG_OK(sg_send(lane, lane->outbound, tcp_during_stun, sizeof(tcp_during_stun)));
  while (lane->received != sizeof(tcp_during_stun)) {
    SG_REQUIRE(cmeta_monotonic_ms() < deadline);
    SG_OK(sg_pump(context, lane));
    SG_REQUIRE(stun_binding_transaction_advance(&binding, now) == SALTS_EBUSY);
    SG_REQUIRE(binding.attempt == 1u);
  }
  now = binding.deadline_ms;
  SG_REQUIRE(stun_binding_transaction_advance(&binding, now) == SALTS_EBUSY);
  SG_REQUIRE(binding.attempt == 2u);
  SG_OK(ice_cnet_datagram_receive_begin(&lane->udp[1]));
  SG_UNTIL(lane->udp[1].receive_ready && !lane->udp[0].send_pending);
  SG_OK(ice_cnet_datagram_receive_take(&lane->udp[1], &actual, bytes, sizeof(bytes), &size));
  SG_REQUIRE(size == STUN_HEADER_SIZE && stun_is_stun_message(bytes, size));
  SG_REQUIRE(memcmp(bytes + 8u, first_id.id, sizeof(first_id.id)) != 0);
  SG_REQUIRE(memcmp(bytes + 8u, binding.id.id, sizeof(binding.id.id)) == 0);

  /* A late response to the expired attempt cannot settle the new attempt. */
  size = stun_build_binding_response(bytes, &first_id, "203.0.113.17", 45678u);
  SG_OK(ice_cnet_datagram_send_begin(&lane->udp[1], &actual, bytes, size, &tag));
  SG_UNTIL(lane->udp[0].receive_ready && !lane->udp[1].send_pending);
  SG_REQUIRE(stun_binding_transaction_advance(&binding, now) == SALTS_EBUSY);
  SG_REQUIRE(binding.phase == STUN_BINDING_WAITING && binding.mapped.port == 0u);
  SG_REQUIRE(binding.deadline_ms == now + 1000u);
  SG_OK(ice_cnet_datagram_send_begin(&lane->udp[1], &actual, "?", 1u, &tag));
  SG_UNTIL(lane->udp[0].receive_ready && !lane->udp[1].send_pending);
  SG_REQUIRE(stun_binding_transaction_advance(&binding, now) == SALTS_EBUSY);
  SG_REQUIRE(binding.deadline_ms == now + 1000u);

  if (!respond) {
    SG_REQUIRE(stun_binding_transaction_advance(&binding, binding.deadline_ms) == SALTS_ETIMEDOUT);
    SG_REQUIRE(binding.phase == STUN_BINDING_DONE && binding.attempt == 2u);
    SG_REQUIRE(binding.mapped.port == 0u && lane->udp[0].receive_armed);
    SG_REQUIRE(stun_binding_transaction_advance(&binding, UINT64_MAX) == SALTS_ETIMEDOUT);
    SG_REQUIRE(stun_binding_transaction_cancel(&binding) == SALTS_ETIMEDOUT);
    goto cleanup;
  }
  size = stun_build_binding_response(bytes, &binding.id, "203.0.113.17", 45678u);
  SG_OK(ice_cnet_datagram_send_begin(&lane->udp[1], &actual, bytes, size, &tag));
  SG_UNTIL(lane->udp[0].receive_ready && !lane->udp[1].send_pending);
  SG_OK(stun_binding_transaction_advance(&binding, now));
  SG_REQUIRE(binding.phase == STUN_BINDING_DONE && binding.mapped.port == 45678u);
  SG_REQUIRE(strcmp(binding.mapped.ip_str, "203.0.113.17") == 0);
  SG_OK(stun_binding_transaction_advance(&binding, UINT64_MAX));
  SG_OK(stun_binding_transaction_cancel(&binding));
cleanup:
  return lane->status;
}

static int sg_retained_cleanup(native_io_sharded_context *context, udp_lane *lane,
                                const cnet_datagram_peer *destination, bool unpublished) {
  const uint64_t deadline = cmeta_monotonic_ms() + SG_TIMEOUT_MS;
  ice_config_t config = ice_default_config();
  uint64_t tag;
  bool stopped = false;
  int status;
  lane->cleanup_agent = ice_agent_create(&config);
  SG_REQUIRE(lane->cleanup_agent != NULL);
  lane->cleanup_udp = (ice_cnet_datagram_t *)calloc(1u, sizeof(*lane->cleanup_udp));
  SG_REQUIRE(lane->cleanup_udp != NULL);
  if (unpublished) lane->cleanup_agent->gathering_socket = lane->cleanup_udp;
  else {
    lane->cleanup_agent->local_candidate_count = 2;
    lane->cleanup_agent->local_candidate_runtime[0].socket = lane->cleanup_udp;
    lane->cleanup_agent->local_candidate_runtime[1].socket = lane->cleanup_udp;
  }
  SG_OK(ice_cnet_datagram_init_external(lane->cleanup_udp, "127.0.0.1", 0u,
                                        SG_PACKET, lane->backend));
  SG_OK(ice_cnet_datagram_receive_begin(lane->cleanup_udp));
  SG_OK(ice_cnet_datagram_send_begin(lane->cleanup_udp, destination, "retained", 8u, &tag));
  SG_REQUIRE(ice_agent_destroy_checked(&lane->cleanup_agent, 0u) == SALTS_EBUSY);
  SG_REQUIRE(lane->cleanup_agent != NULL);
  SG_REQUIRE(ice_agent_get_state(lane->cleanup_agent) == ICE_STATE_CLOSED);
  SG_REQUIRE(lane->cleanup_udp->initialized && lane->cleanup_udp->send_pending);
  if (unpublished) SG_REQUIRE(lane->cleanup_agent->gathering_socket == lane->cleanup_udp);
  else {
    SG_REQUIRE(lane->cleanup_agent->local_candidate_runtime[0].socket == lane->cleanup_udp);
    SG_REQUIRE(lane->cleanup_agent->local_candidate_runtime[1].socket == lane->cleanup_udp);
  }
  while (!stopped) {
    SG_REQUIRE(cmeta_monotonic_ms() < deadline);
    SG_OK(sg_pump(context, lane));
    status = ice_cnet_datagram_stop_external(lane->cleanup_udp, &stopped);
    SG_REQUIRE(status == SALTS_OK || status == SALTS_EBUSY);
  }
  /* The successful retry consumes each shared socket exactly once. */
  SG_OK(ice_agent_destroy_checked(&lane->cleanup_agent, 0u));
  SG_REQUIRE(lane->cleanup_agent == NULL);
  lane->cleanup_udp = NULL;
cleanup:
  return lane->status;
}

static void sg_run(native_io_sharded_context *context, void *user) {
  udp_lane *lane = (udp_lane *)user;
  const uint64_t deadline = cmeta_monotonic_ms() + SG_TIMEOUT_MS;
  cnet_client_config config = {sg_backend(), 2u, 8u, 16u, SG_BATCH, 16u,
                                SG_PACKET, SG_PACKET, SG_TIMEOUT_MS, 0u, SG_TIMEOUT_MS};
  cnet_listener_config listener = {sg_backend(), "127.0.0.1", 0u, 2u};
  cnet_observer observer = {sg_state, sg_receive, lane, NULL};
  cnet_stream_peer tcp_peer = {0};
  native_io_request accept_request = {0};
  cnet_datagram_peer destination = {0}, actual = {0};
  cnet_destination_hint hint = {1u, 1u, 0u, true};
  cnet_destination_selection selection = {0};
  cnet_destination_result chosen = {0};
  stun_transaction_id_t transaction = {{0}};
  stun_mapped_address_t mapped = {0};
  stun_binding_transaction canceled = {0};
  unsigned char bytes[SG_PACKET], small[1];
  size_t size = 0u;
  uint64_t tag = 0u, rejected = 0u;
  uint16_t port = 0u;
  bool stopped = false;
  int status;

  SG_OK(native_io_sharded_context_acquire_host(context, sg_quiescent, lane,
                                               &lane->lease, &lane->backend));
  for (size_t i = 0u; i < 2u; ++i)
    SG_OK(ice_cnet_datagram_init_external(&lane->udp[i], "127.0.0.1", 0u,
                                          SG_PACKET, lane->backend));
  SG_OK(cnet_client_init_external(&lane->tcp, &config, lane->backend));
  SG_OK(cnet_listener_init(&lane->listener, &listener));
  SG_OK(cnet_listener_attach_external(&lane->listener, lane->backend));
  SG_OK(cnet_listener_port(&lane->listener, &tcp_peer.port));
  tcp_peer.family = CNET_DATAGRAM_ADDRESS_IPV4;
  tcp_peer.address[0] = 127u; tcp_peer.address[3] = 1u;
  SG_OK(cnet_listener_submit_external_accept(&lane->listener, &accept_request));
  SG_OK(cnet_connect_peer(&lane->tcp, &tcp_peer, NULL, &observer, &lane->outbound));

  SG_OK(ice_cnet_datagram_port(&lane->udp[1], &port));
  SG_OK(ice_cnet_datagram_peer_from_text("127.0.0.1", port, &destination));
  selection.size = sizeof(selection);
  selection.version = CNET_DESTINATION_POLICY_VERSION;
  selection.kind = CNET_DESTINATION_EXPLICIT;
  selection.endpoints = &hint; selection.endpoint_count = 1u;
  selection.snapshot_generation = 1u; selection.expires_at_ms = UINT64_MAX;
  selection.explicit_endpoint_id = hint.endpoint_id;
  SG_OK(cnet_destination_choose(&selection, &chosen));
  SG_REQUIRE(chosen.index == 0u && chosen.endpoint_id == 1u);
  /* Pin this peer for the whole transaction; no policy invocation per packet. */
  transaction.id[0] = (uint8_t)(lane->index + 1u);
  size = stun_build_binding_request(bytes, &transaction);
  SG_REQUIRE(size == STUN_HEADER_SIZE);
  SG_REQUIRE(ice_cnet_datagram_send(&lane->udp[0], &destination, bytes, size, 1u) == SALTS_ENOTSUP);
  SG_REQUIRE(!lane->udp[0].send_pending);
  SG_REQUIRE(ice_cnet_datagram_receive(&lane->udp[1], &actual, bytes, sizeof(bytes), &size, 1u) == SALTS_ENOTSUP);
  SG_REQUIRE(!lane->udp[1].receive_armed);
  SG_OK(ice_cnet_datagram_receive_begin(&lane->udp[1]));
  SG_OK(ice_cnet_datagram_receive_begin(&lane->udp[1]));
  SG_OK(ice_cnet_datagram_send_begin(&lane->udp[0], &destination, bytes, STUN_HEADER_SIZE, &tag));
  SG_REQUIRE(ice_cnet_datagram_send_begin(&lane->udp[0], &destination, bytes, STUN_HEADER_SIZE, &rejected) == SALTS_EBUSY);
  SG_REQUIRE(rejected == 0u);
  SG_REQUIRE(ice_cnet_datagram_send_result(&lane->udp[0], tag) == SALTS_EBUSY);
  SG_REQUIRE(ice_cnet_datagram_send_result(&lane->udp[0], tag + 1u) == SALTS_ENOENT);
  memset(bytes, 0xff, sizeof(bytes)); /* CNet owns the admitted copy. */
  SG_UNTIL(lane->udp[1].receive_ready && !lane->udp[0].send_pending);
  SG_OK(ice_cnet_datagram_send_result(&lane->udp[0], tag));
  SG_REQUIRE(ice_cnet_datagram_receive_take(&lane->udp[1], &actual, small, sizeof(small), &size) == SALTS_EMSGSIZE);
  SG_REQUIRE(size == 0u);
  SG_OK(ice_cnet_datagram_receive_take(&lane->udp[1], &actual, bytes, sizeof(bytes), &size));
  SG_REQUIRE(stun_is_stun_message(bytes, size) && size == STUN_HEADER_SIZE);
  SG_REQUIRE(memcmp(bytes + 8u, transaction.id, sizeof(transaction.id)) == 0);
  SG_REQUIRE(ice_cnet_datagram_receive_take(&lane->udp[1], &actual, bytes, sizeof(bytes), &size) == SALTS_EBUSY);
  size = stun_build_binding_response(bytes, &transaction, "203.0.113.17", 45678u);
  SG_REQUIRE(size > STUN_HEADER_SIZE);
  SG_OK(ice_cnet_datagram_receive_begin(&lane->udp[0]));
  SG_OK(ice_cnet_datagram_send_begin(&lane->udp[1], &actual, bytes, size, &tag));
  SG_UNTIL(lane->udp[0].receive_ready && !lane->udp[1].send_pending);
  SG_OK(ice_cnet_datagram_send_result(&lane->udp[1], tag));
  SG_OK(ice_cnet_datagram_receive_take(&lane->udp[0], &actual, bytes, sizeof(bytes), &size));
  SG_REQUIRE(actual.port == destination.port && actual.family == destination.family &&
             memcmp(actual.address, destination.address, 4u) == 0);
  transaction.id[1] ^= 1u;
  SG_REQUIRE(stun_parse_binding_response(bytes, size, &transaction, &mapped) != 0);
  transaction.id[1] ^= 1u;
  SG_REQUIRE(stun_parse_binding_response(bytes, size, &transaction, &mapped) == 0);
  SG_REQUIRE(mapped.port == 45678u && strcmp(mapped.ip_str, "203.0.113.17") == 0);

  do {
    status = cnet_listener_accept(&lane->listener, &lane->tcp, &observer, &lane->inbound);
    if (status == SALTS_OK) break;
    SG_REQUIRE(status == SALTS_ETIMEDOUT && cmeta_monotonic_ms() < deadline);
    SG_OK(sg_pump(context, lane));
  } while (true);
  SG_UNTIL(lane->connected == 2u);
  SG_OK(sg_binding(context, lane, &destination, true));
  SG_OK(ice_cnet_datagram_destroy(&lane->udp[0]));
  SG_OK(ice_cnet_datagram_init_external(&lane->udp[0], "127.0.0.1", 0u,
                                        SG_PACKET, lane->backend));
  SG_OK(sg_binding(context, lane, &destination, false));
  /* Protocol timeout leaves receive demand owned by the transport. */
  while (!stopped) {
    status = ice_cnet_datagram_stop_external(&lane->udp[0], &stopped);
    SG_REQUIRE(status == SALTS_OK || status == SALTS_EBUSY);
    if (stopped) break;
    SG_REQUIRE(cmeta_monotonic_ms() < deadline);
    SG_OK(sg_pump(context, lane));
  }
  stopped = false;

  /* A fresh transport episode owns the next single-use transaction. */
  SG_OK(ice_cnet_datagram_destroy(&lane->udp[0]));
  SG_OK(ice_cnet_datagram_init_external(&lane->udp[0], "127.0.0.1", 0u,
                                        SG_PACKET, lane->backend));
  SG_OK(stun_binding_transaction_start(&canceled, &lane->udp[0], &destination,
                                        1000u, 2u, cmeta_monotonic_ms()));
  tag = canceled.send_tag;
  SG_REQUIRE(stun_binding_transaction_cancel(&canceled) == SALTS_ECANCELED);
  SG_REQUIRE(canceled.phase == STUN_BINDING_DONE && lane->udp[0].send_pending);
  SG_REQUIRE(stun_binding_transaction_advance(&canceled, UINT64_MAX) == SALTS_ECANCELED);

  /* Close one UDP endpoint with both a send and receive awaiting real terminal.
   * The retained application buffer and tag must survive until host routing. */
  SG_OK(ice_cnet_datagram_receive_begin(&lane->udp[0]));
  SG_REQUIRE(ice_cnet_datagram_stop_external(&lane->udp[0], &stopped) == SALTS_EBUSY);
  SG_REQUIRE(!stopped && lane->udp[0].send_pending);
  SG_REQUIRE(ice_cnet_datagram_destroy(&lane->udp[0]) == SALTS_EBUSY);
  SG_REQUIRE(lane->udp[0].receive_storage != NULL);
  SG_REQUIRE(ice_cnet_datagram_receive_begin(&lane->udp[0]) == SALTS_ESHUTDOWN);
  SG_REQUIRE(ice_cnet_datagram_send_begin(&lane->udp[0], &destination, "x", 1u, &rejected) == SALTS_ESHUTDOWN);
  while (!stopped) {
    SG_REQUIRE(cmeta_monotonic_ms() < deadline);
    SG_OK(sg_pump(context, lane));
    status = ice_cnet_datagram_stop_external(&lane->udp[0], &stopped);
    SG_REQUIRE(status == SALTS_OK || status == SALTS_EBUSY);
  }
  SG_REQUIRE(!lane->udp[0].send_pending);
  status = ice_cnet_datagram_send_result(&lane->udp[0], tag);
  SG_REQUIRE(status == SALTS_OK || status == SALTS_ECANCELED);
  SG_OK(ice_cnet_datagram_destroy(&lane->udp[0]));

  /* The other UDP endpoint can still receive on the original host backend. */
  SG_OK(ice_cnet_datagram_port(&lane->udp[1], &port));
  SG_OK(ice_cnet_datagram_peer_from_text("127.0.0.1", port, &destination));
  SG_OK(ice_cnet_datagram_receive_begin(&lane->udp[1]));
  SG_OK(ice_cnet_datagram_send_begin(&lane->udp[1], &destination, "neighbor", 8u, &tag));
  /* A pre-stop packet may still be queued; consume it with explicit demand. */
  do {
    SG_UNTIL(lane->udp[1].receive_ready);
    SG_OK(ice_cnet_datagram_receive_take(&lane->udp[1], &actual, bytes, sizeof(bytes), &size));
    if (size == 8u && memcmp(bytes, "neighbor", 8u) == 0) break;
    SG_REQUIRE(size == STUN_HEADER_SIZE && stun_is_stun_message(bytes, size));
    SG_OK(ice_cnet_datagram_receive_begin(&lane->udp[1]));
  } while (true);

  lane->expected_tcp = tcp_payload;
  lane->expected_tcp_size = sizeof(tcp_payload);
  lane->received = 0u;
  SG_OK(sg_send(lane, lane->outbound, tcp_payload, sizeof(tcp_payload)));
  SG_UNTIL(lane->received == sizeof(tcp_payload));
  SG_OK(sg_retained_cleanup(context, lane, &destination, false));
  SG_OK(sg_retained_cleanup(context, lane, &destination, true));

cleanup:
  lane->closing = true;
  if (lane->tcp.impl) {
    if (lane->outbound.slot) (void)cnet_close(&lane->tcp, lane->outbound);
    if (lane->inbound.slot) (void)cnet_close(&lane->tcp, lane->inbound);
  }
  if (lane->lease.generation) {
    const uint64_t drain_deadline = cmeta_monotonic_ms() + SG_TIMEOUT_MS;
    while (!sg_quiescent(lane) && cmeta_monotonic_ms() < drain_deadline) {
      if (lane->cleanup_agent) {
        bool cleanup_stopped = true;
        if (lane->cleanup_udp && lane->cleanup_udp->initialized) {
          status = ice_cnet_datagram_stop_external(lane->cleanup_udp, &cleanup_stopped);
          if (status != SALTS_EBUSY) sg_record(lane, status);
        }
        if (cleanup_stopped) {
          ice_agent_destroy(lane->cleanup_agent);
          lane->cleanup_agent = NULL;
          lane->cleanup_udp = NULL;
        }
      }
      for (size_t i = 0u; i < 2u; ++i) {
        if (!lane->udp[i].initialized) continue;
        status = ice_cnet_datagram_destroy(&lane->udp[i]);
        if (status != SALTS_EBUSY) sg_record(lane, status);
      }
      if (lane->listener.impl) {
        status = cnet_listener_close(&lane->listener);
        if (status != SALTS_EALREADY) sg_record(lane, status);
        status = cnet_listener_destroy(&lane->listener);
        if (status != SALTS_EBUSY) sg_record(lane, status);
      }
      if (lane->tcp.impl) {
        status = cnet_client_stop_external(&lane->tcp);
        if (status == SALTS_OK) status = cnet_client_destroy(&lane->tcp);
        if (status != SALTS_EBUSY) sg_record(lane, status);
      }
      (void)sg_pump(context, lane);
    }
    lane->drained = sg_quiescent(lane);
    if (lane->drained)
      sg_record(lane, native_io_sharded_context_release_host(context, lane->lease));
    else sg_record(lane, SALTS_ETIMEDOUT);
  }
}

spec("ICE internal UDP on a mixed CNet SG host") {
  for (size_t owners = 1u; owners <= SG_OWNERS; owners *= 2u) {
    it("settles bounded STUN and preserves TCP/UDP neighbors on %zu Owners", owners) {
      static udp_lane lanes[SG_OWNERS];
      native_io_sharded *runtime = NULL;
      const native_io_sharded_config config = {owners, 8u,
          {sg_backend(), 8u, 32u, SG_BATCH}};
      memset(lanes, 0, sizeof(lanes));
      check_equal(native_io_sharded_create(&config, &runtime), SALTS_OK);
      for (size_t i = 0u; i < owners; ++i) {
        native_io_sharded_task task = {sg_run, NULL, NULL, &lanes[i]};
        lanes[i].runtime = runtime;
        lanes[i].index = i;
        check_equal(native_io_sharded_try_submit_to(runtime, i, &task), SALTS_OK);
      }
      check_equal(native_io_sharded_wait(runtime), SALTS_OK);
      check_equal(native_io_sharded_destroy(runtime), SALTS_OK);
      for (size_t i = 0u; i < owners; ++i) {
        capture(lanes[i].failed_line, "%d");
        check_equal(lanes[i].status, SALTS_OK);
        check(lanes[i].drained);
        check(lanes[i].observes > 0u);
        check_equal(lanes[i].received, sizeof(tcp_payload));
      }
    }
  }
}
