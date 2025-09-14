/**
 * test_tls_context_only.c - Test TLS context initialization only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "turbo_tls.h"
#include <turbo/asn1/x509_generate.h>

int main(void) {
    printf("TLS Context Test\n");
    printf("=================\n\n");
    fflush(stdout);

    // Generate certificate
    printf("1. Generating certificate...\n");
    fflush(stdout);

    char *cert_pem = NULL;
    char *key_pem = NULL;
    size_t cert_len = 0;
    size_t key_len = 0;

    int ret = x509_generate_tls_cert_pem("localhost", 365, &cert_pem, &cert_len, &key_pem, &key_len);
    if (ret != 0) {
        printf("   FAILED: ret=%d\n", ret);
        fflush(stdout);
        return 1;
    }
    printf("   OK: %zu + %zu bytes\n", cert_len, key_len);
    fflush(stdout);

    // Initialize TLS context
    printf("\n2. Initializing TLS context...\n");
    fflush(stdout);

    turbo_tls_context_t context;
    memset(&context, 0, sizeof(context));

    ret = turbo_tls_context_init(&context, 0);
    printf("   turbo_tls_context_init returned: %d\n", ret);
    fflush(stdout);

    if (ret != 0) {
        printf("   FAILED\n");
        fflush(stdout);
        free(cert_pem);
        memset(key_pem, 0, key_len);
        free(key_pem);
        return 1;
    }

    // Set certificate
    printf("\n3. Loading certificate...\n");
    fflush(stdout);

    ret = turbo_tls_context_set_cert(&context, cert_pem, cert_len);
    printf("   turbo_tls_context_set_cert returned: %d\n", ret);
    fflush(stdout);

    if (ret != 0) {
        printf("   FAILED\n");
        fflush(stdout);
        turbo_tls_context_destroy(&context);
        free(cert_pem);
        memset(key_pem, 0, key_len);
        free(key_pem);
        return 1;
    }

    // Set private key
    printf("\n4. Loading private key...\n");
    fflush(stdout);

    ret = turbo_tls_context_set_private_key(&context, key_pem, key_len);
    printf("   turbo_tls_context_set_private_key returned: %d\n", ret);
    fflush(stdout);

    if (ret != 0) {
        printf("   FAILED\n");
        fflush(stdout);
        turbo_tls_context_destroy(&context);
        free(cert_pem);
        memset(key_pem, 0, key_len);
        free(key_pem);
        return 1;
    }

    printf("\n5. Cleaning up...\n");
    fflush(stdout);

    turbo_tls_context_destroy(&context);
    free(cert_pem);
    memset(key_pem, 0, key_len);
    free(key_pem);

    printf("\nSUCCESS! TLS context works with Ed25519 certificates.\n");
    fflush(stdout);

    return 0;
}
