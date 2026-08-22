#include "CoroNet.h"
#include "tinytest.h"

#include "../src/turbo_kcp_fec_internal.h"
#include "../src/turbo_kcp_secure_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KCP_BENCH_DATA_SHARDS 10U
#define KCP_BENCH_PARITY_SHARDS 3U
#define KCP_BENCH_TOTAL_SHARDS (KCP_BENCH_DATA_SHARDS + KCP_BENCH_PARITY_SHARDS)
#define KCP_BENCH_GROUPS 300U
#define KCP_BENCH_PAYLOAD_SIZE 64U
#define KCP_BENCH_RECORD_SIZE (KCP_BENCH_PAYLOAD_SIZE + TURBO_KCP_SECURE_RECORD_OVERHEAD)

static const uint8_t KCP_BENCH_PSK[TURBO_KCP_PSK_SIZE] = {
    0x95, 0x17, 0xc2, 0x4a, 0xe1, 0x38, 0x6d, 0xb0, 0x27, 0xf4, 0x83,
    0x5c, 0x09, 0xda, 0x71, 0x3e, 0x68, 0xb5, 0x22, 0xcf, 0x44, 0x8a,
    0xf9, 0x10, 0x7b, 0x35, 0xd6, 0x0e, 0xa3, 0x59, 0x84, 0x2f};

typedef struct kcp_bench_receive_s {
  turbo_kcp_secure_state_t *secure;
  const char *expected;
  size_t expected_size;
  size_t delivered;
  size_t rejected;
} kcp_bench_receive_t;

typedef struct kcp_bench_result_s {
  uint32_t loss_percent;
  size_t sent_frames;
  size_t dropped_frames;
  size_t dropped_data;
  size_t delivered;
  size_t rejected;
  uint64_t elapsed_ns;
  uint64_t p50_ns;
  uint64_t p95_ns;
  uint64_t p99_ns;
} kcp_bench_result_t;

static int kcp_bench_u64_compare(const void *left, const void *right) {
  uint64_t a = *(const uint64_t *)left;
  uint64_t b = *(const uint64_t *)right;
  return (a > b) - (a < b);
}

static uint64_t kcp_bench_percentile(const uint64_t *sorted, size_t count,
                                     uint32_t percentile) {
  size_t rank;
  if (!sorted || count == 0U || percentile == 0U || percentile > 100U) return 0U;
  rank = ((size_t)percentile * count + 99U) / 100U;
  return sorted[rank > 0U ? rank - 1U : 0U];
}

static int kcp_bench_deliver(void *user, const char *record, size_t record_size) {
  kcp_bench_receive_t *receive = (kcp_bench_receive_t *)user;
  char plain[KCP_BENCH_PAYLOAD_SIZE];
  size_t plain_size = 0U;
  int rc;
  if (!receive || !record) return TURBO_EINVAL;
  rc = turbo_kcp_secure_open(receive->secure, record, record_size, plain, sizeof(plain),
                             &plain_size);
  if (rc != TURBO_OK || plain_size != receive->expected_size ||
      memcmp(plain, receive->expected, plain_size) != 0) {
    ++receive->rejected;
    return rc != TURBO_OK ? rc : TURBO_EPROTO;
  }
  ++receive->delivered;
  return TURBO_OK;
}

static int kcp_bench_handshake(turbo_kcp_secure_state_t *client,
                               turbo_kcp_secure_state_t *server) {
  uint8_t hello[TURBO_KCP_SECURE_HANDSHAKE_SIZE];
  uint8_t accept[TURBO_KCP_SECURE_HANDSHAKE_SIZE];
  int rc = turbo_kcp_secure_init(client, TURBO_KCP_SECURE_CLIENT, KCP_BENCH_PSK);
  if (rc == TURBO_OK)
    rc = turbo_kcp_secure_init(server, TURBO_KCP_SECURE_SERVER, KCP_BENCH_PSK);
  if (rc == TURBO_OK) rc = turbo_kcp_secure_build_client_hello(client, hello);
  if (rc == TURBO_OK)
    rc = turbo_kcp_secure_accept_client_hello(server, hello, sizeof(hello), accept);
  if (rc == TURBO_OK)
    rc = turbo_kcp_secure_accept_server_hello(client, accept, sizeof(accept));
  return rc;
}

static int kcp_bench_should_drop(uint32_t loss_percent, size_t frame_index) {
  if (loss_percent == 0U) return 0;
  return frame_index % (100U / loss_percent) == 0U;
}

static int kcp_bench_run_loss(uint32_t loss_percent, kcp_bench_result_t *result) {
  turbo_kcp_secure_state_t client;
  turbo_kcp_secure_state_t server;
  turbo_kcp_fec_config_t config;
  turbo_kcp_fec_state_t *fec = NULL;
  kcp_bench_receive_t receive;
  char payload[KCP_BENCH_PAYLOAD_SIZE];
  uint64_t samples[KCP_BENCH_GROUPS];
  size_t frame_index = 1U;
  int rc;

  if (!result || (loss_percent != 0U && loss_percent != 1U && loss_percent != 5U))
    return TURBO_EINVAL;
  memset(result, 0, sizeof(*result));
  memset(&client, 0, sizeof(client));
  memset(&server, 0, sizeof(server));
  memset(payload, 0x5a, sizeof(payload));
  memset(&receive, 0, sizeof(receive));
  result->loss_percent = loss_percent;

  rc = kcp_bench_handshake(&client, &server);
  if (rc != TURBO_OK) goto cleanup;
  turbo_kcp_fec_config_init(&config);
  config.backend = TURBO_KCP_FEC_BACKEND_REED_SOLOMON;
  config.data_shards = KCP_BENCH_DATA_SHARDS;
  config.parity_shards = KCP_BENCH_PARITY_SHARDS;
  config.max_payload_size = KCP_BENCH_RECORD_SIZE;
  config.receive_group_count = 64U;
  rc = turbo_kcp_fec_open(&config, &fec);
  if (rc == TURBO_OK)
    rc = turbo_kcp_fec_set_session(fec, client.session_epoch, client.fec_key);
  if (rc != TURBO_OK) goto cleanup;

  receive.secure = &server;
  receive.expected = payload;
  receive.expected_size = sizeof(payload);
  result->elapsed_ns = turbo_hrtime();
  for (uint32_t group = 0U; group < KCP_BENCH_GROUPS; ++group) {
    char records[KCP_BENCH_DATA_SHARDS][KCP_BENCH_RECORD_SIZE];
    const char *record_views[KCP_BENCH_DATA_SHARDS];
    size_t record_sizes[KCP_BENCH_DATA_SHARDS];
    mem_buffer_t *frames[KCP_BENCH_TOTAL_SHARDS] = {0};
    uint64_t started_ns = turbo_hrtime();

    for (uint16_t shard = 0U; shard < KCP_BENCH_DATA_SHARDS; ++shard) {
      record_views[shard] = records[shard];
      rc = turbo_kcp_secure_seal(&client, payload, sizeof(payload), records[shard],
                                 sizeof(records[shard]), &record_sizes[shard]);
      if (rc == TURBO_OK)
        rc = turbo_kcp_fec_build_data_frame_for_test(
            &config, client.session_epoch, client.fec_key, group + 1U, shard,
            records[shard], record_sizes[shard], &frames[shard]);
      if (rc != TURBO_OK) goto group_cleanup;
    }
    for (uint16_t parity = 0U; parity < KCP_BENCH_PARITY_SHARDS; ++parity) {
      rc = turbo_kcp_fec_build_reed_solomon_parity_frame_for_test(
          &config, client.session_epoch, client.fec_key, group + 1U, parity,
          record_views, record_sizes, KCP_BENCH_DATA_SHARDS,
          &frames[KCP_BENCH_DATA_SHARDS + parity]);
      if (rc != TURBO_OK) goto group_cleanup;
    }

    for (uint16_t order = 0U; order < KCP_BENCH_TOTAL_SHARDS; ++order) {
      uint16_t shard = order;
      mem_slice_t slice;
      if (kcp_bench_should_drop(loss_percent, frame_index++)) {
        ++result->dropped_frames;
        if (shard < KCP_BENCH_DATA_SHARDS) ++result->dropped_data;
        continue;
      }
      memset(&slice, 0, sizeof(slice));
      slice.data = frames[shard]->data;
      slice.length = frames[shard]->used;
      slice.buffer = frames[shard];
      rc = turbo_kcp_fec_receive_frame(fec, &slice, kcp_bench_deliver, &receive);
      if (rc == TURBO_EALREADY) rc = TURBO_OK;
      if (rc != TURBO_OK) goto group_cleanup;
      ++result->sent_frames;
    }

  group_cleanup:
    for (uint16_t shard = 0U; shard < KCP_BENCH_TOTAL_SHARDS; ++shard)
      mem_unref(frames[shard]);
    samples[group] = turbo_hrtime() - started_ns;
    if (rc != TURBO_OK) goto cleanup;
  }
  result->elapsed_ns = turbo_hrtime() - result->elapsed_ns;
  result->delivered = receive.delivered;
  result->rejected = receive.rejected;
  qsort(samples, KCP_BENCH_GROUPS, sizeof(samples[0]), kcp_bench_u64_compare);
  result->p50_ns = kcp_bench_percentile(samples, KCP_BENCH_GROUPS, 50U);
  result->p95_ns = kcp_bench_percentile(samples, KCP_BENCH_GROUPS, 95U);
  result->p99_ns = kcp_bench_percentile(samples, KCP_BENCH_GROUPS, 99U);
  if (result->delivered != KCP_BENCH_GROUPS * KCP_BENCH_DATA_SHARDS ||
      result->rejected != 0U)
    rc = TURBO_EPROTO;

cleanup:
  turbo_kcp_fec_close(fec);
  turbo_kcp_secure_wipe(&server);
  turbo_kcp_secure_wipe(&client);
  return rc;
}

spec("kcp secure fec loss benchmark") {
  it("reports authenticated 64-byte record recovery at 0 1 and 5 percent loss") {
    static const uint32_t losses[] = {0U, 1U, 5U};
    for (size_t i = 0U; i < sizeof(losses) / sizeof(losses[0]); ++i) {
      kcp_bench_result_t result;
      double messages_per_second;
      double mib_per_second;
      int rc = kcp_bench_run_loss(losses[i], &result);
      if (rc != TURBO_OK)
        fprintf(stderr,
                "KCP_BENCH_ERROR loss_percent=%u status=%d delivered=%zu "
                "dropped_frames=%zu recovered_data=%zu rejected=%zu\n",
                losses[i], rc, result.delivered, result.dropped_frames,
                result.dropped_data, result.rejected);
      check_equal(rc, TURBO_OK);
      messages_per_second =
          (double)result.delivered * 1000000000.0 / (double)result.elapsed_ns;
      mib_per_second = messages_per_second * KCP_BENCH_PAYLOAD_SIZE / (1024.0 * 1024.0);
      printf("KCP_BENCH_RESULT payload_bytes=%u data_shards=%u parity_shards=%u "
             "loss_percent=%u delivered=%zu dropped_frames=%zu recovered_data=%zu "
             "rejected=%zu throughput_msg_s=%.2f throughput_mib_s=%.2f "
             "p50_us=%.3f p95_us=%.3f p99_us=%.3f\n",
             KCP_BENCH_PAYLOAD_SIZE, KCP_BENCH_DATA_SHARDS,
             KCP_BENCH_PARITY_SHARDS, result.loss_percent, result.delivered,
             result.dropped_frames, result.dropped_data, result.rejected,
             messages_per_second, mib_per_second, (double)result.p50_ns / 1000.0,
             (double)result.p95_ns / 1000.0, (double)result.p99_ns / 1000.0);
    }
  }
}
