/**
 * @file scatter_gather_advanced.c
 * @brief Advanced scatter-gather examples with true zero-copy and vectored receive
 */

#include "turbo_sync_client.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stb_sprintf.h>

#define SERVER_HOST "127.0.0.1"
#define SERVER_PORT 8080

/**
 * Example 1: True zero-copy scatter-gather send
 */
static void example_zero_copy_send(sync_client_t *client) {
  printf("\n=== Example 1: True Zero-Copy Scatter-Gather ===\n");

  /* Prepare multiple buffers */
  const char *part1 = "Part1-";
  const char *part2 = "Part2-";
  const char *part3 = "Part3-";
  const char *part4 = "Part4-";
  const char *part5 = "Part5\n";

  sync_client_iovec_t iov[5] = {{part1, strlen(part1)},
                                {part2, strlen(part2)},
                                {part3, strlen(part3)},
                                {part4, strlen(part4)},
                                {part5, strlen(part5)}};

  printf("Sending 5 buffers via true zero-copy scatter-gather...\n");
  sync_client_status_t status = sync_client_sendv(client, iov, 5);

  if (status == SYNC_CLIENT_STATUS_OK) {
    printf(" Sent successfully using arena allocator + OS scatter-gather\n");
    printf("  - No extra allocations (arena pool)\n");
    printf("  - No concatenation needed\n");
    printf("  - Single writev() system call\n");
  } else {
    printf(" Send failed: %s\n", sync_client_last_message(client));
  }
}

/**
 * Example 2: Vectored receive into structured data
 */
static void example_vectored_receive(sync_client_t *client) {
  printf("\n=== Example 2: Vectored Receive (Structured Data) ===\n");

  /* Send a test message with structure: [4-byte length][1-byte type][payload] */
  uint32_t length = 13; /* "Hello, World!" */
  uint8_t msg_type = 0x01;
  const char *payload = "Hello, World!";

  sync_client_iovec_t send_iov[3] = {{(const char *)&length, sizeof(length)},
                                     {(const char *)&msg_type, sizeof(msg_type)},
                                     {payload, strlen(payload)}};

  printf("Sending structured message (length + type + payload)...\n");
  sync_client_sendv(client, send_iov, 3);

  /* Receive directly into structured fields */
  uint32_t recv_length;
  uint8_t recv_type;
  char recv_payload[64];

  sync_client_iovec_t recv_iov[3] = {{(char *)&recv_length, sizeof(recv_length)},
                                     {(char *)&recv_type, sizeof(recv_type)},
                                     {recv_payload, sizeof(recv_payload)}};

  size_t bytes_read;
  printf("Receiving via vectored receive (direct placement)...\n");
  sync_client_status_t status = sync_client_recvv(client, recv_iov, 3, &bytes_read);

  if (status == SYNC_CLIENT_STATUS_OK && bytes_read > 0) {
    printf(" Received %zu bytes directly into structured fields\n", bytes_read);
    printf("  Length: %u\n", recv_length);
    printf("  Type: 0x%02x\n", recv_type);
    if (bytes_read > sizeof(recv_length) + sizeof(recv_type)) {
      size_t payload_len = bytes_read - sizeof(recv_length) - sizeof(recv_type);
      recv_payload[payload_len] = '\0';
      printf("  Payload: '%s'\n", recv_payload);
    }
    printf("  No intermediate buffer needed!\n");
  } else {
    printf(" Receive failed or no data\n");
  }
}

/**
 * Example 3: High-performance batch processing
 */
static void example_batch_processing(sync_client_t *client) {
  printf("\n=== Example 3: High-Performance Batch Processing ===\n");

  const int BATCH_SIZE = 10;
  sync_client_iovec_t *iov = malloc(BATCH_SIZE * sizeof(sync_client_iovec_t));
  if (!iov)
    return;

  /* Prepare batch of messages */
  char **messages = malloc(BATCH_SIZE * sizeof(char *));
  for (int i = 0; i < BATCH_SIZE; i++) {
    messages[i] = malloc(32);
    stbsp_snprintf(messages[i], 32, "Batch message %d\n", i + 1);
    iov[i].data = messages[i];
    iov[i].len = strlen(messages[i]);
  }

  printf("Sending batch of %d messages via scatter-gather...\n", BATCH_SIZE);
  sync_client_status_t status = sync_client_sendv(client, iov, BATCH_SIZE);

  if (status == SYNC_CLIENT_STATUS_OK) {
    printf(" Batch sent atomically\n");
    printf("  - All messages sent together\n");
    printf("  - No interleaving with other sends\n");
    printf("  - Single system call\n");
  }

  /* Cleanup */
  for (int i = 0; i < BATCH_SIZE; i++) {
    free(messages[i]);
  }
  free(messages);
  free(iov);
}

/**
 * Example 4: Protocol framing with zero-copy
 */
static void example_protocol_framing(sync_client_t *client) {
  printf("\n=== Example 4: Protocol Framing (Zero-Copy) ===\n");

  /* Simulate a protocol with: [magic][version][length][type][payload][checksum] */
  uint32_t magic = 0xDEADBEEF;
  uint16_t version = 1;
  uint32_t length = 11; /* "Test Data!!" */
  uint8_t type = 0x42;
  const char *payload = "Test Data!!";
  uint32_t checksum = 0x12345678; /* Simplified */

  sync_client_iovec_t iov[6] = {{(const char *)&magic, sizeof(magic)},
                                {(const char *)&version, sizeof(version)},
                                {(const char *)&length, sizeof(length)},
                                {(const char *)&type, sizeof(type)},
                                {payload, strlen(payload)},
                                {(const char *)&checksum, sizeof(checksum)}};

  printf("Sending protocol frame (6 separate fields)...\n");
  sync_client_status_t status = sync_client_sendv(client, iov, 6);

  if (status == SYNC_CLIENT_STATUS_OK) {
    printf(" Protocol frame sent via zero-copy scatter-gather\n");
    printf("  - No struct packing needed\n");
    printf("  - No alignment issues\n");
    printf("  - Direct from variables\n");
  }
}

/**
 * Example 5: Statistics and performance monitoring
 */
static void example_statistics(sync_client_t *client) {
  printf("\n=== Example 5: Statistics & Performance ===\n");

  sync_client_stats_t stats;
  sync_client_get_stats(client, &stats);

  printf("Scatter-Gather Statistics:\n");
  printf("  Total messages sent: %llu\n", (unsigned long long)stats.messages_sent);
  printf("  Total bytes sent: %llu\n", (unsigned long long)stats.bytes_sent);
  printf("  Scatter-gather sends: %llu\n", (unsigned long long)stats.scatter_gather_sends);
  printf("  Total IOV buffers: %llu\n", (unsigned long long)stats.total_iov_buffers_sent);
  printf("  Scatter-gather receives: %llu\n", (unsigned long long)stats.scatter_gather_receives);

  if (stats.scatter_gather_sends > 0) {
    double avg_buffers = (double)stats.total_iov_buffers_sent / stats.scatter_gather_sends;
    printf("  Average buffers per sendv(): %.2f\n", avg_buffers);
  }

  printf("\nPerformance Benefits:\n");
  printf("   Zero extra allocations (arena pool)\n");
  printf("   True OS-level scatter-gather (writev)\n");
  printf("   Atomic send semantics\n");
  printf("   Reduced system call overhead\n");
}

int main(void) {
  printf("Advanced Scatter-Gather Examples\n");
  printf("=================================\n");

  /* Create client */
  sync_client_t *client = sync_client_create();
  if (!client) {
    fprintf(stderr, "Failed to create client\n");
    return 1;
  }

  /* Connect */
  printf("\nConnecting to %s:%d...\n", SERVER_HOST, SERVER_PORT);
  char url[128];
  snprintf(url, sizeof(url), "tcp://%s:%d", SERVER_HOST, SERVER_PORT);
  sync_client_status_t status = sync_client_connect(client, url);
  if (status != SYNC_CLIENT_STATUS_OK) {
    fprintf(stderr, "Connection failed: %s\n", sync_client_last_message(client));
    sync_client_destroy(client);
    return 1;
  }
  printf(" Connected\n");

  /* Run examples */
  example_zero_copy_send(client);
  example_vectored_receive(client);
  example_batch_processing(client);
  example_protocol_framing(client);
  example_statistics(client);

  /* Cleanup */
  printf("\nCleaning up...\n");
  sync_client_destroy(client);
  printf(" Done\n");

  return 0;
}
