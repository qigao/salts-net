#include "http_common.h"
#include "tinytest.h"
#include <string.h>

spec("http_enum") {
  describe("HTTP Error to String") {
    it("should convert HTTP_ERROR_NONE to 'OK'") {
      check_str_eq(http_error_to_str(HTTP_ERROR_NONE), "OK");
    }

    it("should convert HTTP_ERROR_TIMEOUT to 'Request timeout'") {
      check_str_eq(http_error_to_str(HTTP_ERROR_TIMEOUT), "Request timeout");
    }

    it("should handle unknown error codes gracefully") {
      // Cast to the enum type to test default case
      check_str_eq(http_error_to_str((http_error_code_t)999), "Unknown error");
    }
  }
}
