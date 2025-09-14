#include "../include/rpc_error.h"
#include <platform.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stb_sprintf.h>

// ============================================================================
// Error Name Table
// ============================================================================

const char *rpc_error_name(rpc_error_code_t code) {
  switch (code) {
  case RPC_OK:
    return "RPC_OK";

  // JSON-RPC 2.0 Standard Errors
  case RPC_ERROR_PARSE:
    return "RPC_ERROR_PARSE";
  case RPC_ERROR_INVALID_REQUEST:
    return "RPC_ERROR_INVALID_REQUEST";
  case RPC_ERROR_METHOD_NOT_FOUND:
    return "RPC_ERROR_METHOD_NOT_FOUND";
  case RPC_ERROR_INVALID_PARAMS:
    return "RPC_ERROR_INVALID_PARAMS";
  case RPC_ERROR_INTERNAL:
    return "RPC_ERROR_INTERNAL";

  // Transport Errors
  case RPC_ERROR_CONNECT_FAILED:
    return "RPC_ERROR_CONNECT_FAILED";
  case RPC_ERROR_CONNECTION_LOST:
    return "RPC_ERROR_CONNECTION_LOST";
  case RPC_ERROR_TIMEOUT:
    return "RPC_ERROR_TIMEOUT";
  case RPC_ERROR_NETWORK:
    return "RPC_ERROR_NETWORK";
  case RPC_ERROR_DNS:
    return "RPC_ERROR_DNS";
  case RPC_ERROR_TLS:
    return "RPC_ERROR_TLS";

  // HTTP Errors
  case RPC_ERROR_HTTP_400:
    return "RPC_ERROR_HTTP_400";
  case RPC_ERROR_HTTP_401:
    return "RPC_ERROR_HTTP_401";
  case RPC_ERROR_HTTP_403:
    return "RPC_ERROR_HTTP_403";
  case RPC_ERROR_HTTP_404:
    return "RPC_ERROR_HTTP_404";
  case RPC_ERROR_HTTP_500:
    return "RPC_ERROR_HTTP_500";
  case RPC_ERROR_HTTP_502:
    return "RPC_ERROR_HTTP_502";
  case RPC_ERROR_HTTP_503:
    return "RPC_ERROR_HTTP_503";
  case RPC_ERROR_HTTP_OTHER:
    return "RPC_ERROR_HTTP_OTHER";

  // Client Errors
  case RPC_ERROR_INVALID_CONFIG:
    return "RPC_ERROR_INVALID_CONFIG";
  case RPC_ERROR_NOT_CONNECTED:
    return "RPC_ERROR_NOT_CONNECTED";
  case RPC_ERROR_ALREADY_CONNECTED:
    return "RPC_ERROR_ALREADY_CONNECTED";
  case RPC_ERROR_INVALID_STATE:
    return "RPC_ERROR_INVALID_STATE";
  case RPC_ERROR_BUFFER_OVERFLOW:
    return "RPC_ERROR_BUFFER_OVERFLOW";
  case RPC_ERROR_OUT_OF_MEMORY:
    return "RPC_ERROR_OUT_OF_MEMORY";
  case RPC_ERROR_INVALID_RESPONSE:
    return "RPC_ERROR_INVALID_RESPONSE";
  case RPC_ERROR_POOL_EXHAUSTED:
    return "RPC_ERROR_POOL_EXHAUSTED";
  case RPC_ERROR_CIRCUIT_OPEN:
    return "RPC_ERROR_CIRCUIT_OPEN";

  // Retry Errors
  case RPC_ERROR_MAX_RETRIES:
    return "RPC_ERROR_MAX_RETRIES";
  case RPC_ERROR_RETRY_TIMEOUT:
    return "RPC_ERROR_RETRY_TIMEOUT";
  case RPC_ERROR_BACKOFF_LIMIT:
    return "RPC_ERROR_BACKOFF_LIMIT";

  // Resource Errors
  case RPC_ERROR_TOO_MANY_REQUESTS:
    return "RPC_ERROR_TOO_MANY_REQUESTS";
  case RPC_ERROR_REQUEST_TOO_LARGE:
    return "RPC_ERROR_REQUEST_TOO_LARGE";
  case RPC_ERROR_RESPONSE_TOO_LARGE:
    return "RPC_ERROR_RESPONSE_TOO_LARGE";
  case RPC_ERROR_QUEUE_FULL:
    return "RPC_ERROR_QUEUE_FULL";

  default:
    return "RPC_ERROR_UNKNOWN";
  }
}

// ============================================================================
// Error Description Table
// ============================================================================

const char *rpc_error_description(rpc_error_code_t code) {
  switch (code) {
  case RPC_OK:
    return "Success";

  // JSON-RPC 2.0 Standard Errors
  case RPC_ERROR_PARSE:
    return "Invalid JSON was received by the server";
  case RPC_ERROR_INVALID_REQUEST:
    return "The JSON sent is not a valid Request object";
  case RPC_ERROR_METHOD_NOT_FOUND:
    return "The method does not exist or is not available";
  case RPC_ERROR_INVALID_PARAMS:
    return "Invalid method parameter(s)";
  case RPC_ERROR_INTERNAL:
    return "Internal JSON-RPC error";

  // Transport Errors
  case RPC_ERROR_CONNECT_FAILED:
    return "Failed to connect to server";
  case RPC_ERROR_CONNECTION_LOST:
    return "Connection lost during request";
  case RPC_ERROR_TIMEOUT:
    return "Request timeout";
  case RPC_ERROR_NETWORK:
    return "Network I/O error";
  case RPC_ERROR_DNS:
    return "DNS resolution failed";
  case RPC_ERROR_TLS:
    return "TLS/SSL error";

  // HTTP Errors
  case RPC_ERROR_HTTP_400:
    return "Bad Request (HTTP 400)";
  case RPC_ERROR_HTTP_401:
    return "Unauthorized (HTTP 401)";
  case RPC_ERROR_HTTP_403:
    return "Forbidden (HTTP 403)";
  case RPC_ERROR_HTTP_404:
    return "Not Found (HTTP 404)";
  case RPC_ERROR_HTTP_500:
    return "Internal Server Error (HTTP 500)";
  case RPC_ERROR_HTTP_502:
    return "Bad Gateway (HTTP 502)";
  case RPC_ERROR_HTTP_503:
    return "Service Unavailable (HTTP 503)";
  case RPC_ERROR_HTTP_OTHER:
    return "HTTP error";

  // Client Errors
  case RPC_ERROR_INVALID_CONFIG:
    return "Invalid client configuration";
  case RPC_ERROR_NOT_CONNECTED:
    return "Client not connected";
  case RPC_ERROR_ALREADY_CONNECTED:
    return "Client already connected";
  case RPC_ERROR_INVALID_STATE:
    return "Invalid client state";
  case RPC_ERROR_BUFFER_OVERFLOW:
    return "Buffer overflow";
  case RPC_ERROR_OUT_OF_MEMORY:
    return "Memory allocation failed";
  case RPC_ERROR_INVALID_RESPONSE:
    return "Invalid RPC response format";
  case RPC_ERROR_POOL_EXHAUSTED:
    return "Connection pool exhausted";
  case RPC_ERROR_CIRCUIT_OPEN:
    return "Circuit breaker is open";

  // Retry Errors
  case RPC_ERROR_MAX_RETRIES:
    return "Maximum retries exceeded";
  case RPC_ERROR_RETRY_TIMEOUT:
    return "Retry timeout exceeded";
  case RPC_ERROR_BACKOFF_LIMIT:
    return "Backoff limit reached";

  // Resource Errors
  case RPC_ERROR_TOO_MANY_REQUESTS:
    return "Rate limit exceeded";
  case RPC_ERROR_REQUEST_TOO_LARGE:
    return "Request payload too large";
  case RPC_ERROR_RESPONSE_TOO_LARGE:
    return "Response payload too large";
  case RPC_ERROR_QUEUE_FULL:
    return "Request queue full";

  default:
    return "Unknown error";
  }
}

// ============================================================================
// Error Severity and Classification
// ============================================================================

rpc_error_severity_t rpc_error_get_severity(rpc_error_code_t code) {
  // Fatal errors
  if (code == RPC_ERROR_OUT_OF_MEMORY || code == RPC_ERROR_BUFFER_OVERFLOW ||
      code == RPC_ERROR_INVALID_CONFIG) {
    return RPC_SEVERITY_FATAL;
  }

  // Errors that may be recoverable
  if (code >= 100 && code < 200) {
    return RPC_SEVERITY_ERROR; // Transport errors
  }

  if (code >= 300 && code < 400) {
    return RPC_SEVERITY_ERROR; // Client errors
  }

  // Warnings
  if (code == RPC_ERROR_CIRCUIT_OPEN || code == RPC_ERROR_TOO_MANY_REQUESTS) {
    return RPC_SEVERITY_WARNING;
  }

  // Default to error
  return RPC_SEVERITY_ERROR;
}

int rpc_error_is_retryable(rpc_error_code_t code) {
  switch (code) {
  // Retryable transport errors
  case RPC_ERROR_TIMEOUT:
  case RPC_ERROR_CONNECTION_LOST:
  case RPC_ERROR_NETWORK:
  case RPC_ERROR_DNS:

  // Retryable HTTP errors
  case RPC_ERROR_HTTP_500:
  case RPC_ERROR_HTTP_502:
  case RPC_ERROR_HTTP_503:

  // Retryable resource errors
  case RPC_ERROR_TOO_MANY_REQUESTS:
  case RPC_ERROR_QUEUE_FULL:
    return 1;

  default:
    return 0;
  }
}

int rpc_error_is_fatal(rpc_error_code_t code) {
  return rpc_error_get_severity(code) == RPC_SEVERITY_FATAL;
}

// ============================================================================
// Error Info Creation
// ============================================================================

static uint64_t get_timestamp_us(void) {
#ifdef _WIN32
  LARGE_INTEGER frequency, counter;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&counter);
  return (uint64_t)((counter.QuadPart * 1000000ULL) / frequency.QuadPart);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)(ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000);
#endif
}

rpc_error_info_t rpc_error_create(rpc_error_code_t code, const char *message,
                                   const char *details) {
  rpc_error_info_t error;
  memset(&error, 0, sizeof(error));

  error.code = code;
  error.severity = rpc_error_get_severity(code);
  error.timestamp = get_timestamp_us();

  // Copy message or use default description
  if (message) {
    strncpy(error.message, message, sizeof(error.message) - 1);
  } else {
    strncpy(error.message, rpc_error_description(code), sizeof(error.message) - 1);
  }

  // Copy details
  if (details) {
    strncpy(error.details, details, sizeof(error.details) - 1);
  }

  return error;
}

rpc_error_info_t rpc_error_create_with_location(rpc_error_code_t code, const char *message,
                                                  const char *file, int line) {
  rpc_error_info_t error = rpc_error_create(code, message, NULL);
  error.file = file;
  error.line = line;
  return error;
}

// ============================================================================
// Error Formatting
// ============================================================================

size_t rpc_error_format(const rpc_error_info_t *error, char *buffer, size_t buffer_size) {
  if (!error || !buffer || buffer_size == 0)
    return 0;

  size_t written = 0;

  // Error code and name
  written += stbsp_snprintf(buffer + written, buffer_size - written, "[%s] %s",
                      rpc_error_name(error->code), error->message);

  // Details if available
  if (error->details[0] != '\0') {
    written += stbsp_snprintf(buffer + written, buffer_size - written, " - %s", error->details);
  }

  // HTTP status if applicable
  if (error->http_status > 0) {
    written += stbsp_snprintf(buffer + written, buffer_size - written, " (HTTP %d)",
                        error->http_status);
  }

  // Retry count if applicable
  if (error->retry_count > 0) {
    written += stbsp_snprintf(buffer + written, buffer_size - written, " [retries: %d]",
                        error->retry_count);
  }

  // Source location for debugging
  if (error->file) {
    written += stbsp_snprintf(buffer + written, buffer_size - written, " at %s:%d", error->file,
                        error->line);
  }

  return written;
}

// ============================================================================
// HTTP to RPC Error Mapping
// ============================================================================

rpc_error_code_t rpc_error_from_http_status(int http_status) {
  switch (http_status) {
  case 400:
    return RPC_ERROR_HTTP_400;
  case 401:
    return RPC_ERROR_HTTP_401;
  case 403:
    return RPC_ERROR_HTTP_403;
  case 404:
    return RPC_ERROR_HTTP_404;
  case 500:
    return RPC_ERROR_HTTP_500;
  case 502:
    return RPC_ERROR_HTTP_502;
  case 503:
    return RPC_ERROR_HTTP_503;
  default:
    if (http_status >= 400)
      return RPC_ERROR_HTTP_OTHER;
    return RPC_OK;
  }
}
