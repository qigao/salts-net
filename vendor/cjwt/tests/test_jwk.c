#include "unity.h"
#include "cjwt.h"
#include <string.h>
#include <stdlib.h>

void setUp(void) {}
void tearDown(void) {}

void test_jwk_parse_rsa(void)
{
    const char *jwk_json = "{"
        "\"kty\":\"RSA\","
        "\"kid\":\"test_kid\","
        "\"use\":\"sig\","
        "\"n\":\"o76m_D87p9B...example\","
        "\"e\":\"AQAB\""
    "}";
    
    cjwt_jwk_t *jwk = NULL;
    cjwt_code_t rv = cjwt_jwk_parse(jwk_json, &jwk);
    
    TEST_ASSERT_EQUAL(CJWTE_OK, rv);
    TEST_ASSERT_NOT_NULL(jwk);
    TEST_ASSERT_EQUAL(CJWT_KTY_RSA, jwk->kty);
    TEST_ASSERT_EQUAL_STRING("test_kid", jwk->kid);
    TEST_ASSERT_EQUAL_STRING("sig", jwk->use);
    
    cjwt_jwk_destroy(jwk);
}

void test_jwk_parse_ec(void)
{
    const char *jwk_json = "{"
        "\"kty\":\"EC\","
        "\"crv\":\"P-256\","
        "\"x\":\"f83OJ3D2x1Bg8vub9tLe1gHMzV76e8Tus9uPHvRVEUo\","
        "\"y\":\"x_FEzRu9m36HLN_tue659LNpXW6pCyStikYjKIWI5a0\""
    "}";
    
    cjwt_jwk_t *jwk = NULL;
    cjwt_code_t rv = cjwt_jwk_parse(jwk_json, &jwk);
    
    TEST_ASSERT_EQUAL(CJWTE_OK, rv);
    TEST_ASSERT_NOT_NULL(jwk);
    TEST_ASSERT_EQUAL(CJWT_KTY_EC, jwk->kty);
    
    cjwt_jwk_destroy(jwk);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_jwk_parse_rsa);
    RUN_TEST(test_jwk_parse_ec);
    return UNITY_END();
}
