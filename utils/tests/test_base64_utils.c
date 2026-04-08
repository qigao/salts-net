#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base64_utils.h"
#include "tinytest.h"

spec("base64_utils") {
  it("should encode known value") {
    const uint8_t input[] = "TurboUtils";
    char *encoded = NULL;

    int rc = tn_base64_encode(input, sizeof(input) - 1, &encoded);
    check_int_eq(rc, 0);
    check_not_null(encoded);
    check_str_eq(encoded, "VHVyYm9VdGlscw==");

    free(encoded);
  }

  it("should decode known value") {
    const char *encoded = "Zm9vYmFy";
    uint8_t *decoded = NULL;
    size_t decoded_len = 0;

    int rc = tn_base64_decode(encoded, &decoded, &decoded_len);
    check_int_eq(rc, 0);
    check_not_null(decoded);
    check_size_eq(decoded_len, 6);
    check_mem_eq(decoded, "foobar", decoded_len);

    free(decoded);
  }

  it("should return error for invalid input") {
    const char *encoded = "invalid*data";
    uint8_t *decoded = NULL;
    size_t decoded_len = 0;

    int rc = tn_base64_decode(encoded, &decoded, &decoded_len);
    check_int_eq(rc, -1);
    check_null(decoded);
  }

  it("should handle NULL parameters") {
    char *encoded = NULL;
    uint8_t *decoded = NULL;
    size_t decoded_len = 0;

    check_int_eq(tn_base64_encode(NULL, 4, &encoded), -1);
    check_int_eq(tn_base64_encode((const uint8_t *)"data", 4, NULL), -1);
    check_int_eq(tn_base64_decode(NULL, &decoded, &decoded_len), -1);
    check_int_eq(tn_base64_decode("Zg==", NULL, &decoded_len), -1);
    check_int_eq(tn_base64_decode("Zg==", &decoded, NULL), -1);
  }
}
