#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "cjwt/cjwt.h"

void test_jwks_parsing() {
    const char *jwks_json = "{"
        "\"keys\": ["
            "{"
                "\"kty\": \"RSA\","
                "\"kid\": \"rsa-key-1\","
                "\"use\": \"sig\","
                "\"n\": \"0vx7agoebGcQSuuPiLJXZptN9nndrQmbXEps2aiAFbWhM78LhWx4cbbfAAtVT86zwu1RK7aPFFxuhDR1L6tSoc_BJECPebWKRXjBZCiFV4n3oknjhMstn64tZ_2W-5JsGY4Hc5n9yBXArwl93lqt7_RN5w6Cf0h4QyQ5v-65YGjQR0_FDW2QvzqY368QQMicAtaSqzs8KJZgnYb9c7d0zgdAZHzu6qMQvRL5hajrn1n91CbOpbISD08qNLyrdkt-bFTWhAI4vMQFh6WeZu0fM4lFd2NcRwr3XPksINHaQ-G_xBniIqbw0Ls1jF44-csFCur-kEgU8awapJzKnqDKgw\","
                "\"e\": \"AQAB\""
            "},"
            "{"
                "\"kty\": \"EC\","
                "\"kid\": \"ec-key-1\","
                "\"crv\": \"P-256\","
                "\"x\": \"f83OJ3D2x1Bg8vub9tLe1gHMzV76e8Tus9uPHvRVEUo\","
                "\"y\":\"x_FEzRu9m36HLN_tue659LNpXW6pCyStikYjKIWI5a0\""
            "}"
        "]"
    "}";

    cjwt_jwks_t *jwks = NULL;
    cjwt_code_t rv = cjwt_jwks_parse(jwks_json, &jwks);
    assert(rv == CJWTE_OK);
    assert(jwks != NULL);
    assert(jwks->count == 2);
    
    assert(jwks->keys[0]->kty == CJWT_KTY_RSA);
    assert(strcmp(jwks->keys[0]->kid, "rsa-key-1") == 0);
    
    assert(jwks->keys[1]->kty == CJWT_KTY_EC);
    assert(strcmp(jwks->keys[1]->kid, "ec-key-1") == 0);

    printf("JWKS Parsing test passed!\n");
    cjwt_jwks_destroy(jwks);
}

int main() {
    test_jwks_parsing();
    return 0;
}
