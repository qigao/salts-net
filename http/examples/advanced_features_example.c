#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  printf("=== Advanced Features Example ===\n\n");

  http_client_t *client = http_client_create();
  if (!client) {
    fprintf(stderr, "Failed to create HTTP client\n");
    return 1;
  }

  // ========================================================================
  // 1. Timeout Granularity
  // ========================================================================
  printf("1. Timeout Granularity\n");
  printf("   Setting different timeouts for different operations...\n");

  // Set specific timeouts
  http_client_set_connect_timeout(client, 3000); // 3 seconds to connect
  http_client_set_read_timeout(client, 10000);   // 10 seconds to read response

  printf("   - Connect timeout: 3 seconds\n");
  printf("   - Read timeout: 10 seconds\n");

  http_response_t *response = http_get(client, "https://httpbin.org/delay/2");

  if (response->error) {
    printf("   Error: %s\n", response->error);
  } else {
    printf("    Request succeeded with custom timeouts (status: %d)\n", response->status_code);
  }

  http_response_free(response);

  // ========================================================================
  // 2. Compression Support
  // ========================================================================
  printf("\n2. Compression Support\n");
  printf("   Enabling automatic compression...\n");

  http_client_enable_compression(client, 1);

  if (http_client_is_compression_enabled(client)) {
    printf("    Compression enabled (Accept-Encoding: gzip, deflate)\n");
  }

  response = http_get(client, "https://httpbin.org/gzip");

  if (!response->error) {
    printf("   Status: %d\n", response->status_code);

    // Check if response was compressed
    char *encoding = http_response_get_header(response, "Content-Encoding");
    if (encoding) {
      printf("   Content-Encoding: %s\n", encoding);
      printf("    Server sent compressed response\n");
      free(encoding);
    } else {
      printf("   Response was not compressed (or already decompressed)\n");
    }

    printf("   Response size: %zu bytes\n", response->body_len);
  }

  http_response_free(response);

  // Disable compression
  http_client_enable_compression(client, 0);
  printf("   Compression disabled\n");

  // ========================================================================
  // 3. Range Requests (Partial Content)
  // ========================================================================
  printf("\n3. Range Requests (Partial Content)\n");
  printf("   Requesting specific byte ranges...\n");

  // Request first 100 bytes
  printf("   Requesting bytes 0-99...\n");
  response = http_get_range(client, "https://httpbin.org/bytes/1000", 0, 99);

  if (!response->error) {
    printf("   Status: %d", response->status_code);

    if (response->status_code == 206) {
      printf(" (Partial Content)\n");
      printf("    Server supports range requests\n");
      printf("   Received: %zu bytes\n", response->body_len);

      char *content_range = http_response_get_header(response, "Content-Range");
      if (content_range) {
        printf("   Content-Range: %s\n", content_range);
        free(content_range);
      }
    } else {
      printf(" (Full Content)\n");
      printf("   Server doesn't support range requests\n");
    }
  }

  http_response_free(response);

  // Request from byte 500 to end
  printf("\n   Requesting bytes 500 to end...\n");
  response = http_get_range(client, "https://httpbin.org/bytes/1000", 500, 0);

  if (!response->error && response->status_code == 206) {
    printf("    Received partial content: %zu bytes\n", response->body_len);
  }

  http_response_free(response);

  // ========================================================================
  // 4. Combining Features
  // ========================================================================
  printf("\n4. Combining Multiple Features\n");
  printf("   Using compression + custom timeouts + range requests...\n");

  // Enable compression
  http_client_enable_compression(client, 1);

  // Set aggressive timeouts
  http_client_set_connect_timeout(client, 2000);
  http_client_set_read_timeout(client, 5000);

  // Request with range
  response = http_get_range(client, "https://httpbin.org/bytes/10000", 0, 999);

  if (!response->error) {
    printf("    Request succeeded\n");
    printf("   Status: %d\n", response->status_code);
    printf("   Size: %zu bytes\n", response->body_len);

    // Check headers
    char *encoding = http_response_get_header(response, "Content-Encoding");
    char *range = http_response_get_header(response, "Content-Range");

    if (encoding) {
      printf("   Compression: %s\n", encoding);
      free(encoding);
    }
    if (range) {
      printf("   Range: %s\n", range);
      free(range);
    }
  }

  http_response_free(response);

  // ========================================================================
  // 5. Resume Download Simulation
  // ========================================================================
  printf("\n5. Resume Download Simulation\n");
  printf("   Demonstrating how to resume a download...\n");

  // First, download first part
  printf("   Step 1: Download first 500 bytes...\n");
  response = http_get_range(client, "https://httpbin.org/bytes/2000", 0, 499);

  size_t downloaded = 0;
  if (!response->error && response->status_code == 206) {
    downloaded = response->body_len;
    printf("    Downloaded: %zu bytes\n", downloaded);
  }
  http_response_free(response);

  // Simulate interruption and resume
  printf("   Step 2: Resume from byte %zu...\n", downloaded);
  response = http_get_range(client, "https://httpbin.org/bytes/2000", downloaded, 0);

  if (!response->error && response->status_code == 206) {
    printf("    Resumed and downloaded: %zu more bytes\n", response->body_len);
    printf("   Total would be: %zu bytes\n", downloaded + response->body_len);
  }
  http_response_free(response);

  // ========================================================================
  // Summary
  // ========================================================================
  printf("\n=== Summary ===\n");
  printf(" Timeout Granularity: Separate connect/read timeouts\n");
  printf(" Compression: Automatic gzip/deflate support\n");
  printf(" Range Requests: Partial content and resume downloads\n");
  printf(" All features work together seamlessly\n");

  // Cleanup
  http_client_destroy(client);

  printf("\n Advanced features example complete!\n");
  return 0;
}
