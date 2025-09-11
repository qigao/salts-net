/**
 * Enhanced Error Handling Demo
 * "Good error messages save debugging time" - Clear feedback for developers
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "turbonet.h"
#include "turbonet_internal.h"  // Include internal constants

// Demo error callback to show how applications can handle non-fatal errors
static void app_error_callback(turbo_handle_t* handle,
                               const turbo_error_t* error)
{
  printf("\n🚨 [APP ERROR CALLBACK] Non-fatal error occurred:\n");

  char formatted_error[512];
  if (turbo_error_to_string(error, formatted_error, sizeof(formatted_error))
      == 0)
  {
    printf("   📋 %s\n", formatted_error);
  } else {
    printf("   📋 Error code: %d, Message: %s\n", error->code, error->message);
  }

  printf("   ⏰ Timestamp: %llu microseconds\n",
         (unsigned long long)error->timestamp);
  printf("   🔄 Application will continue...\n\n");
}

// Demo connection callback
static void demo_connect_cb(turbo_handle_t* handle, int status)
{
  if (status == 0) {
    printf("✅ [SUCCESS] Connected to %s:%d via %s\n",
           handle->remote_ip,
           handle->remote_port,
           turbo_transport_name(handle->transport));
  } else {
    printf("❌ [CONNECTION FAILED] Status: %d (%s)\n",
           status,
           turbo_strerror(status));

    // Demonstrate detailed error retrieval
    turbo_error_t detailed_error;
    int error_status = turbo_get_last_error(handle, &detailed_error);
    if (error_status != 0) {
      printf("📊 [DETAILED ERROR] Retrieved from handle:\n");

      char error_string[512];
      if (turbo_error_to_string(
              &detailed_error, error_string, sizeof(error_string))
          == 0)
      {
        printf("   📋 %s\n", error_string);
      }

      if (detailed_error.context[0]) {
        printf("   🔍 Context: %s\n", detailed_error.context);
      }
      if (detailed_error.details[0]) {
        printf("   🔧 Technical Details: %s\n", detailed_error.details);
      }
      printf("   ⏰ Error Timestamp: %llu μs\n",
             (unsigned long long)detailed_error.timestamp);
    }
  }
}

// Demo various error scenarios
static void demo_error_scenarios(void)
{
  printf("\n=== Enhanced Error Handling Demo ===\n");
  printf(
      "Testing various error conditions to show detailed error "
      "information...\n\n");

  turbo_handle_t handle;
  turbo_global_init(&handle);

  // Set up error callback
  turbo_set_error_callback(&handle, app_error_callback);

  printf("🧪 [TEST 1] Invalid URL format\n");
  turbo_transport_t transport;
  int err = turbo_validate_url("invalid-url-format", &transport);
  printf("   Result: %s\n", err == 0 ? "Valid" : turbo_strerror(err));

  printf("\n🧪 [TEST 2] Unsupported transport scheme\n");
  err = turbo_validate_url("ftp://example.com", &transport);
  printf("   Result: %s\n", err == 0 ? "Valid" : turbo_strerror(err));

  printf("\n🧪 [TEST 3] DNS resolution for non-existent domain\n");
  printf("   Attempting to connect to non-existent domain...\n");
  err = turbo_connect_url(&handle,
                          "tcp://this-domain-does-not-exist-12345.com:80",
                          demo_connect_cb);
  if (err == 0) {
    printf("   DNS resolution started (will fail asynchronously)\n");
    // Run briefly to let DNS fail
    turbo_global_init();
    for (int i = 0; i < 3; i++) {
      uv_run(turbo_get_global_loop(), UV_RUN_ONCE);
    }
  } else {
    printf("   Immediate error: %s\n", turbo_strerror(err));
  }

  printf("\n🧪 [TEST 4] Connection refused (valid IP, closed port)\n");
  turbo_clear_error(&handle);
  err = turbo_connect_url(&handle, "tcp://127.0.0.1:9999", demo_connect_cb);
  if (err == 0) {
    printf("   Connection attempt started (should be refused)\n");
    // Run briefly to let connection fail
    for (int i = 0; i < 3; i++) {
      uv_run(turbo_get_global_loop(), UV_RUN_ONCE);
    }
  } else {
    printf("   Immediate error: %s\n", turbo_strerror(err));
  }

  printf(
      "\n🧪 [TEST 5] TLS connection without certificates (should show detailed "
      "SSL errors)\n");
  turbo_close(&handle, NULL);  // Clean up previous handle
  turbo_global_init(&handle);
  turbo_set_error_callback(&handle, app_error_callback);

  err = turbo_connect_url(&handle, "tls://127.0.0.1:8443", demo_connect_cb);
  if (err == 0) {
    printf("   TLS connection attempt started (will likely fail)\n");
    // Run briefly to let TLS handshake fail
    for (int i = 0; i < 3; i++) {
      uv_run(turbo_get_global_loop(), UV_RUN_ONCE);
    }
  } else {
    printf("   Immediate error: %s\n", turbo_strerror(err));
  }

  printf("\n🧪 [TEST 6] Error callback demonstration\n");
  printf("   Triggering a non-fatal error to show callback functionality...\n");
  // Manually trigger an error to demonstrate callback
  turbo_set_error(&handle,
                  TURBO_ETLS_CERT,
                  "Manual test error",
                  "This is a demonstration of the error callback system");

  turbo_close(&handle, NULL);

  printf("\n📊 [SUMMARY] Error Handling Features Demonstrated:\n");
  printf("   ✓ Detailed error messages with context\n");
  printf("   ✓ Technical details for debugging\n");
  printf("   ✓ Timestamp tracking for error analysis\n");
  printf("   ✓ Non-fatal error callback system\n");
  printf("   ✓ Error retrieval and formatting\n");
  printf("   ✓ Integration with existing libuv error codes\n");
  printf("\n");
}

// Demo error formatting utilities
static void demo_error_formatting(void)
{
  printf("\n=== Error Formatting Demo ===\n");

  // Create some sample errors
  turbo_error_t sample_errors[] = {
      {.code = UV_ECONNREFUSED,
       .message = "Connection refused",
       .context = "tcp://127.0.0.1:8080",
       .details = "Target server is not accepting connections on port 8080",
       .timestamp = 1234567890123456ULL},
      {.code = UV_EAI_NONAME,
       .message = "Name or service not known",
       .context = "DNS resolution for 'nonexistent.domain'",
       .details = "DNS query failed: no such domain",
       .timestamp = 1234567890987654ULL},
      {.code = TURBO_ETLS_HANDSHAKE,
       .message = "TLS handshake failed",
       .context = "TLS handshake (SSL error 1)",
       .details = "SSL error: 0x14094410 (SSL routines:ssl3_read_bytes:sslv3 "
                  "alert handshake failure)",
       .timestamp = 1234567891111111ULL}};

  for (int i = 0; i < 3; i++) {
    printf("\n📋 [SAMPLE ERROR %d]\n", i + 1);

    char formatted[512];
    int result =
        turbo_error_to_string(&sample_errors[i], formatted, sizeof(formatted));
    if (result == 0) {
      printf("   Formatted: %s\n", formatted);
    } else {
      printf("   Formatting failed: %s\n", turbo_strerror(result));
    }

    // Show individual components
    printf("   Code: %d\n", sample_errors[i].code);
    printf("   Message: %s\n", sample_errors[i].message);
    if (sample_errors[i].context[0]) {
      printf("   Context: %s\n", sample_errors[i].context);
    }
    if (sample_errors[i].details[0]) {
      printf("   Details: %s\n", sample_errors[i].details);
    }
    printf("   Timestamp: %llu μs\n",
           (unsigned long long)sample_errors[i].timestamp);
  }

  printf("\n✨ Error formatting provides structured information for:\n");
  printf("   🔍 Debugging and log analysis\n");
  printf("   📱 User-friendly error displays\n");
  printf("   📊 Error tracking and metrics\n");
  printf("   🔧 Automated error handling\n");
}

int main(int argc, char* argv[])
{
  // Initialize TurboNet
  log_set_level(LOG_INFO);  // Reduce log noise for demo

  printf("🚀 TurboNet Enhanced Error Handling Demo\n");
  printf("==========================================\n");
  printf(
      "This demo shows the improved error handling with detailed context,\n");
  printf("technical details, and application-friendly error callbacks.\n");

  // Demo error formatting first (doesn't require network)
  demo_error_formatting();

  // Demo various error scenarios
  demo_error_scenarios();

  printf("✅ Demo completed successfully!\n");
  printf("\nKey improvements in error handling:\n");
  printf("• Rich error context (URL, operation, etc.)\n");
  printf("• Technical details for debugging\n");
  printf("• Timestamps for error analysis\n");
  printf("• Non-fatal error callbacks\n");
  printf("• Backward compatibility with existing code\n");

  return 0;
}
