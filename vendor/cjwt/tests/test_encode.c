#include "unity.h"
#include "cjwt.h"
#include <string.h>
#include <stdlib.h>

void setUp(void) {}
void tearDown(void) {}

void test_encode_none(void)
{
    cjwt_t jwt = {0};
    jwt.header.alg = alg_none;
    jwt.iss = "test_issuer";
    jwt.sub = "test_subject";
    
    char *output = NULL;
    cjwt_code_t rv = cjwt_encode(&jwt, NULL, 0, &output);
    
    TEST_ASSERT_EQUAL(CJWTE_OK, rv);
    TEST_ASSERT_NOT_NULL(output);
    
    // Decode it back and check
    cjwt_t *decoded = NULL;
    rv = cjwt_decode(output, strlen(output), OPT_ALLOW_ALG_NONE, NULL, 0, 0, 0, &decoded);
    TEST_ASSERT_EQUAL(CJWTE_OK, rv);
    TEST_ASSERT_EQUAL_STRING("test_issuer", decoded->iss);
    TEST_ASSERT_EQUAL_STRING("test_subject", decoded->sub);
    
    cjwt_destroy(decoded);
    free(output);
}

void test_encode_hs256(void)
{
    int64_t iat = 123456789;
    cjwt_t jwt = {0};
    jwt.header.alg = alg_hs256;
    jwt.iss = "hs_issuer";
    jwt.iat = &iat;
    
    const char *key = "secret_key";
    char *output = NULL;
    cjwt_code_t rv = cjwt_encode(&jwt, (const uint8_t *)key, strlen(key), &output);
    
    TEST_ASSERT_EQUAL(CJWTE_OK, rv);
    TEST_ASSERT_NOT_NULL(output);
    
    // Decode it back and check
    cjwt_t *decoded = NULL;
    rv = cjwt_decode(output, strlen(output), OPT_ALLOW_ONLY_HS_ALG, (const uint8_t *)key, strlen(key), 0, 0, &decoded);
    TEST_ASSERT_EQUAL(CJWTE_OK, rv);
    TEST_ASSERT_EQUAL_STRING("hs_issuer", decoded->iss);
    TEST_ASSERT_EQUAL(123456789, *decoded->iat);
    
    cjwt_destroy(decoded);
    free(output);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_encode_none);
    RUN_TEST(test_encode_hs256);
    return UNITY_END();
}
