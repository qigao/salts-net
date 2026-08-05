#include "CoroNet/turbo_tls.h"
#include "tinytest.h"
#include "turbo_error.h"

#include <openssl/ssl.h>

spec("TLS system CA integration") {
  it("rejects a null native context") {
    check_int_eq(turbo_tls_load_system_ca_certificates(NULL), TURBO_EINVAL);
  }

  it("loads trust into a caller-owned native context") {
    SSL_CTX *context = SSL_CTX_new(TLS_client_method());
    check_not_null(context);
    if (context) {
      check_int_eq(turbo_tls_load_system_ca_certificates(context), TURBO_OK);
      SSL_CTX_free(context);
    }
  }
}
