#include "mime_smime.h"

#include "salts_simd_scan.h"

int mime_is_smime_encrypted(const char *content_type, size_t len) {
  if (!content_type || len < 20) return 0;
  return salts_scan_mem(content_type, len, "application/pkcs7-mime", 22) !=
             NULL &&
         salts_scan_mem(content_type, len, "smime-type=enveloped-data", 25) !=
             NULL;
}

int mime_is_smime_signed(const char *content_type, size_t len) {
  if (!content_type || len < 20) return 0;
  return (salts_scan_mem(content_type, len, "application/pkcs7-mime", 22) !=
              NULL &&
          salts_scan_mem(content_type, len, "smime-type=signed-data", 22) !=
              NULL) ||
         (salts_scan_mem(content_type, len, "multipart/signed", 16) != NULL &&
          salts_scan_mem(content_type, len,
                         "protocol=application/pkcs7-signature", 36) != NULL);
}
