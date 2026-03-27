/**
 * @file test_rpc_error.c
 * @brief Unit tests for RPC error handling
 */

#include "../include/rpc_error.h"
#include "../include/rpc_client.h"
#include "tinytest.h"
#include <http_client.h>
#include <stdlib.h>
#include <string.h>

spec("rpc_error") {
  describe("Error Classification") {
    it("should return correct error names") {
      check_str_eq(rpc_error_name(RPC_OK), "RPC_OK");
      check_str_eq(rpc_error_name(RPC_ERROR_TIMEOUT), "RPC_ERROR_TIMEOUT");
      check_str_eq(rpc_error_name(RPC_ERROR_PARSE), "RPC_ERROR_PARSE");
      check_str_eq(rpc_error_name(RPC_ERROR_HTTP_500), "RPC_ERROR_HTTP_500");
      check_str_eq(rpc_error_name(99999), "RPC_ERROR_UNKNOWN");
    }

    it("should provide useful error descriptions") {
      const char *desc = rpc_error_description(RPC_ERROR_TIMEOUT);
      check_not_null(desc);
      check(strlen(desc) > 0);

      desc = rpc_error_description(RPC_ERROR_PARSE);
      check_not_null(desc);
      check(strstr(desc, "JSON") != NULL);
    }

    it("should identify correct severity levels") {
      check_int_eq(rpc_error_get_severity(RPC_ERROR_TIMEOUT), RPC_SEVERITY_ERROR);
      check_int_eq(rpc_error_get_severity(RPC_ERROR_OUT_OF_MEMORY), RPC_SEVERITY_FATAL);
      check_int_eq(rpc_error_get_severity(RPC_ERROR_CIRCUIT_OPEN), RPC_SEVERITY_WARNING);
    }
  }

  describe("Retry and Fatality") {
    it("should correctly identify retryable errors") {
      // Retryable errors
      check(rpc_error_is_retryable(RPC_ERROR_TIMEOUT));
      check(rpc_error_is_retryable(RPC_ERROR_NETWORK));
      check(rpc_error_is_retryable(RPC_ERROR_HTTP_503));
      check(rpc_error_is_retryable(RPC_ERROR_HTTP_502));

      // Non-retryable errors
      check(!rpc_error_is_retryable(RPC_ERROR_PARSE));
      check(!rpc_error_is_retryable(RPC_ERROR_INVALID_REQUEST));
      check(!rpc_error_is_retryable(RPC_ERROR_HTTP_400));
      check(!rpc_error_is_retryable(RPC_ERROR_HTTP_404));
    }

    it("should correctly identify fatal errors") {
      check(rpc_error_is_fatal(RPC_ERROR_OUT_OF_MEMORY));
      check(rpc_error_is_fatal(RPC_ERROR_BUFFER_OVERFLOW));
      check(!rpc_error_is_fatal(RPC_ERROR_TIMEOUT));
      check(!rpc_error_is_fatal(RPC_ERROR_NETWORK));
    }
  }

  describe("Error Creation") {
    it("should create error info with custom message and details") {
      rpc_error_info_t error = rpc_error_create(RPC_ERROR_TIMEOUT, "Request timeout", "Timeout after 5000ms");

      check_int_eq(error.code, RPC_ERROR_TIMEOUT);
      check_str_eq(error.message, "Request timeout");
      check_str_eq(error.details, "Timeout after 5000ms");
      check(error.timestamp > 0);
    }

    it("should create error info with default message when NULL is passed") {
      rpc_error_info_t error = rpc_error_create(RPC_ERROR_NETWORK, NULL, NULL);

      check_int_eq(error.code, RPC_ERROR_NETWORK);
      check_str_eq(error.message, "Network I/O error");
      check(error.timestamp > 0);
    }
  }

  describe("Formatting") {
    it("should format error info into a string correctly") {
      rpc_error_info_t error = rpc_error_create(RPC_ERROR_TIMEOUT, "Request timeout", "Server not responding");
      error.http_status = 503;
      error.retry_count = 3;

      char buffer[512];
      size_t len = rpc_error_format(&error, buffer, sizeof(buffer));

      check(len > 0);
      check(strstr(buffer, "RPC_ERROR_TIMEOUT") != NULL);
      check(strstr(buffer, "Request timeout") != NULL);
      check(strstr(buffer, "503") != NULL);
      check(strstr(buffer, "retries: 3") != NULL);
    }
  }

  describe("HTTP Mapping") {
    it("should map HTTP status codes to RPC errors correctly") {
      check_int_eq(rpc_error_from_http_status(400), RPC_ERROR_HTTP_400);
      check_int_eq(rpc_error_from_http_status(401), RPC_ERROR_HTTP_401);
      check_int_eq(rpc_error_from_http_status(404), RPC_ERROR_HTTP_404);
      check_int_eq(rpc_error_from_http_status(500), RPC_ERROR_HTTP_500);
      check_int_eq(rpc_error_from_http_status(503), RPC_ERROR_HTTP_503);
      check_int_eq(rpc_error_from_http_status(418), RPC_ERROR_HTTP_OTHER); // I'm a teapot
      check_int_eq(rpc_error_from_http_status(200), RPC_OK);
    }
  }

  describe("Client Validation") {
    it("should reject invalid params for calls before sending") {
      http_client_t *http = http_client_create(NULL);
      check_not_null(http);

      rpc_client_config_t config = {.url = "http://127.0.0.1:1/rpc", .http_client = http};
      rpc_client_t *client = rpc_client_create(&config);
      check_not_null(client);

      rpc_call_result_t result;
      memset(&result, 0, sizeof(result));
      check_int_eq(rpc_client_call(client, "math.add", "{", &result), -1);
      rpc_result_free(&result);

      rpc_client_destroy(client);
      http_client_destroy(http);
    }

    it("should reject invalid params for notifications before sending") {
      http_client_t *http = http_client_create(NULL);
      check_not_null(http);

      rpc_client_config_t config = {.url = "http://127.0.0.1:1/rpc", .http_client = http};
      rpc_client_t *client = rpc_client_create(&config);
      check_not_null(client);

      check_int_eq(rpc_client_notify(client, "math.add", "{"), -1);

      rpc_client_destroy(client);
      http_client_destroy(http);
    }

    it("should reject invalid params for batch calls before sending") {
      http_client_t *http = http_client_create(NULL);
      check_not_null(http);

      rpc_client_config_t config = {.url = "http://127.0.0.1:1/rpc", .http_client = http};
      rpc_client_t *client = rpc_client_create(&config);
      check_not_null(client);

      const char *methods[] = {"math.add"};
      const char *params[] = {"{"};
      rpc_call_result_t results[1];
      memset(results, 0, sizeof(results));

      check_int_eq(rpc_client_batch_call(client, methods, params, 1, results), -1);
      rpc_result_free(&results[0]);

      rpc_client_destroy(client);
      http_client_destroy(http);
    }

    it("should return failure for transport errors on calls") {
      http_client_t *http = http_client_create(NULL);
      check_not_null(http);

      rpc_client_config_t config = {.url = "http://127.0.0.1:1/rpc", .http_client = http};
      rpc_client_t *client = rpc_client_create(&config);
      check_not_null(client);

      rpc_call_result_t result;
      memset(&result, 0, sizeof(result));
      check_int_eq(rpc_client_call(client, "math.add", "{\"a\":1}", &result), -1);
      check(!result.success);
      rpc_result_free(&result);

      rpc_client_destroy(client);
      http_client_destroy(http);
    }

    it("should return failure for transport errors on batch calls") {
      http_client_t *http = http_client_create(NULL);
      check_not_null(http);

      rpc_client_config_t config = {.url = "http://127.0.0.1:1/rpc", .http_client = http};
      rpc_client_t *client = rpc_client_create(&config);
      check_not_null(client);

      const char *methods[] = {"math.add"};
      const char *params[] = {"{\"a\":1}"};
      rpc_call_result_t results[1];
      memset(results, 0, sizeof(results));

      check_int_eq(rpc_client_batch_call(client, methods, params, 1, results), -1);
      check(!results[0].success);
      rpc_result_free(&results[0]);

      rpc_client_destroy(client);
      http_client_destroy(http);
    }
  }
}
