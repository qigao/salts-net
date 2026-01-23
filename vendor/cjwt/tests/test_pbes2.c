#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "cjwt/cjwt.h"

void test_pbes2_encryption() {
    cjwt_t jwt = {0};
    jwt.header.alg = alg_pbes2_hs256_a128kw;
    jwt.header.enc = enc_a128gcm;
    jwt.iss = "password-derived-issuer";
    jwt.sub = "secret-subject";

    const uint8_t password[] = "my-secret-password";
    char *token = NULL;

    // Encrypt
    cjwt_code_t rv = cjwt_encode(&jwt, password, sizeof(password)-1, &token);
    assert(rv == CJWTE_OK);
    assert(token != NULL);
    printf("Generated JWE (PBES2): %s\n", token);

    // Decrypt
    cjwt_t *decrypted = NULL;
    rv = cjwt_decode(token, strlen(token), 0, password, sizeof(password)-1, 0, 0, &decrypted);
    
    if (rv != CJWTE_OK) {
        printf("Decryption failed with error: %d\n", rv);
        assert(0);
    }
    
    assert(decrypted != NULL);
    assert(strcmp(decrypted->iss, "password-derived-issuer") == 0);
    assert(decrypted->header.alg == alg_pbes2_hs256_a128kw);

    printf("PBES2 encryption/decryption test passed!\n");

    free(token);
    cjwt_destroy(decrypted);
}

int main() {
    test_pbes2_encryption();
    return 0;
}
