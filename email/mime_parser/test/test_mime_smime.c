/**
 * @file test_mime_smime.c
 * @brief S/MIME content-type detection tests
 */

#include "mime_smime.h"
#include "tinytest.h"

#include <string.h>

spec("mime_smime") {
  describe("content type detection") {
    it("should detect encrypted S/MIME") {
      const char *ct =
          "application/pkcs7-mime; smime-type=enveloped-data; name=smime.p7m";
      check(mime_is_smime_encrypted(ct, strlen(ct)) == 1);
    }

    it("should detect signed S/MIME") {
      const char *ct =
          "application/pkcs7-mime; smime-type=signed-data; name=smime.p7m";
      check(mime_is_smime_signed(ct, strlen(ct)) == 1);
    }

    it("should detect multipart/signed") {
      const char *ct =
          "multipart/signed; protocol=application/pkcs7-signature; boundary=boundary";
      check(mime_is_smime_signed(ct, strlen(ct)) == 1);
    }

    it("should reject plain MIME and invalid inputs") {
      const char *ct = "text/plain";
      check(mime_is_smime_encrypted(ct, strlen(ct)) == 0);
      check(mime_is_smime_signed(ct, strlen(ct)) == 0);
      check(mime_is_smime_encrypted(NULL, 0) == 0);
      check(mime_is_smime_signed(NULL, 0) == 0);
    }
  }
}
