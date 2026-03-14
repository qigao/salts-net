/**
 * test_ed25519_minimal.c - Minimal test for Ed25519 certificate generation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <asn1/x509_generate.h>

int main(void) {
    printf("Minimal Ed25519 Certificate Test\n");
    printf("==================================\n\n");
    fflush(stdout);

    printf("1. Testing certificate generation...\n");
    fflush(stdout);

    char *cert_pem = NULL;
    char *key_pem = NULL;
    size_t cert_len = 0;
    size_t key_len = 0;

    int ret = x509_generate_tls_cert_pem(
        "Test",
        365,
        &cert_pem, &cert_len,
        &key_pem, &key_len
    );

    printf("   Return code: %d\n", ret);
    printf("   cert_pem: %p\n", (void*)cert_pem);
    printf("   key_pem: %p\n", (void*)key_pem);
    printf("   cert_len: %zu\n", cert_len);
    printf("   key_len: %zu\n", key_len);
    fflush(stdout);

    if (ret != 0) {
        printf("   FAILED to generate certificate\n");
        fflush(stdout);
        return 1;
    }

    if (!cert_pem || !key_pem) {
        printf("   FAILED: NULL pointers\n");
        fflush(stdout);
        return 1;
    }

    printf("\n2. Certificate PEM:\n");
    printf("%s\n", cert_pem);
    fflush(stdout);

    printf("\n3. Private Key PEM:\n");
    printf("%s\n", key_pem);
    fflush(stdout);

    printf("\n4. Cleaning up...\n");
    fflush(stdout);

    memset(key_pem, 0, key_len);
    free(key_pem);
    free(cert_pem);

    printf("\nSUCCESS!\n");
    fflush(stdout);

    return 0;
}
