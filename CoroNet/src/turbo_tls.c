#include "CoroNet/turbo_tls.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "turbo_error.h"

#ifdef _WIN32
  #include <wincrypt.h>
  #include <windows.h>

static int turbo_tls_load_windows_cert_store(SSL_CTX *context, const char *store_name) {
  HCERTSTORE store;
  PCCERT_CONTEXT certificate = NULL;
  X509_STORE *x509_store;
  int loaded = 0;

  store = CertOpenSystemStoreA(0, store_name);
  if (!store) return 0;

  x509_store = SSL_CTX_get_cert_store(context);
  if (!x509_store) {
    CertCloseStore(store, 0);
    return 0;
  }

  while ((certificate = CertEnumCertificatesInStore(store, certificate)) != NULL) {
    const unsigned char *encoded = certificate->pbCertEncoded;
    X509 *x509 = d2i_X509(NULL, &encoded, certificate->cbCertEncoded);
    if (!x509) {
      ERR_clear_error();
      continue;
    }

    if (X509_STORE_add_cert(x509_store, x509) == 1) {
      loaded = 1;
    } else {
      unsigned long error = ERR_peek_last_error();
      if (ERR_GET_LIB(error) == ERR_LIB_X509 &&
          ERR_GET_REASON(error) == X509_R_CERT_ALREADY_IN_HASH_TABLE) {
        loaded = 1;
      }
      ERR_clear_error();
    }
    X509_free(x509);
  }

  CertCloseStore(store, 0);
  return loaded;
}
#endif

int turbo_tls_load_system_ca_certificates(SSL_CTX *context) {
  int loaded = 0;

  if (!context) return TURBO_EINVAL;

  if (SSL_CTX_set_default_verify_paths(context) == 1) {
    loaded = 1;
  } else {
    ERR_clear_error();
  }

#ifdef _WIN32
  if (turbo_tls_load_windows_cert_store(context, "ROOT")) loaded = 1;
  if (turbo_tls_load_windows_cert_store(context, "CA")) loaded = 1;
#endif

  return loaded ? TURBO_OK : TURBO_EIO;
}
