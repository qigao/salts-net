#include "platform.h"
#include "http_common.h"
#include "tinytest.h"
#include <stdlib.h>
#include <string.h>


spec("URL Encode/Decode") {

  describe("turbo_url_encode") {
    it("should pass through unreserved characters") {
      char *r = turbo_url_encode("abcXYZ019-_.~");
      check_str_eq(r, "abcXYZ019-_.~");
      free(r);
    }

    it("should encode spaces as +") {
      char *r = turbo_url_encode("hello world");
      check_str_eq(r, "hello+world");
      free(r);
    }

    it("should encode special characters as %XX") {
      char *r = turbo_url_encode("/path?q=1&v=2");
      check_str_eq(r, "%2Fpath%3Fq%3D1%26v%3D2");
      free(r);
    }

    it("should return NULL for NULL input") { check(turbo_url_encode(NULL) == NULL); }

    it("should handle empty string") {
      char *r = turbo_url_encode("");
      check_str_eq(r, "");
      free(r);
    }
  }

  describe("turbo_url_decode") {
    it("should decode %XX sequences") {
      char *r = turbo_url_decode("%2Fpath%3Fq%3D1%26v%3D2");
      check_str_eq(r, "/path?q=1&v=2");
      free(r);
    }

    it("should decode + as space") {
      char *r = turbo_url_decode("hello+world");
      check_str_eq(r, "hello world");
      free(r);
    }

    it("should pass through plain text") {
      char *r = turbo_url_decode("abc123");
      check_str_eq(r, "abc123");
      free(r);
    }

    it("should handle lowercase hex") {
      char *r = turbo_url_decode("%2f%3a");
      check_str_eq(r, "/:");
      free(r);
    }

    it("should leave incomplete % sequences as-is") {
      char *r = turbo_url_decode("100%");
      check_str_eq(r, "100%");
      free(r);
    }

    it("should leave invalid hex digits as-is") {
      char *r = turbo_url_decode("%GG");
      check_str_eq(r, "%GG");
      free(r);
    }

    it("should return NULL for NULL input") { check(turbo_url_decode(NULL) == NULL); }

    it("should handle empty string") {
      char *r = turbo_url_decode("");
      check_str_eq(r, "");
      free(r);
    }
  }

  describe("roundtrip") {
    it("should survive encode then decode") {
      const char *original = "hello world/foo?bar=baz&x=1";
      char *encoded = turbo_url_encode(original);
      char *decoded = turbo_url_decode(encoded);
      check_str_eq(decoded, original);
      free(encoded);
      free(decoded);
    }

    it("should roundtrip UTF-8 / CJK") {
      const char *original = "\xe4\xbd\xa0\xe5\xa5\xbd"; /* 你好 */
      char *encoded = turbo_url_encode(original);
      check_str_eq(encoded, "%E4%BD%A0%E5%A5%BD");
      char *decoded = turbo_url_decode(encoded);
      check_str_eq(decoded, original);
      free(encoded);
      free(decoded);
    }
  }
}
