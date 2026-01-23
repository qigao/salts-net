#include "unity.h"
#include "cjwt.h"
#include <string.h>
#include <stdlib.h>

void setUp(void) {}
void tearDown(void) {}

void test_jwe_dir_a128gcm(void)
{
    cjwt_t jwt = {0};
    jwt.header.alg = alg_dir;
    jwt.header.enc = enc_a128gcm;
    jwt.iss = "jwe_issuer";
    
    const uint8_t key[16] = {0}; // 128-bit key for A128GCM
    char *output = NULL;
    cjwt_code_t rv = cjwt_encode(&jwt, key, sizeof(key), &output);
    
    TEST_ASSERT_EQUAL(CJWTE_OK, rv);
    TEST_ASSERT_NOT_NULL(output);
    
    /* Decode it back */
    cjwt_t *decoded = NULL;
    rv = cjwt_decode(output, strlen(output), 0, key, sizeof(key), 0, 0, &decoded);
    
    TEST_ASSERT_EQUAL(CJWTE_OK, rv);
    TEST_ASSERT_EQUAL_STRING("jwe_issuer", decoded->iss);
    TEST_ASSERT_EQUAL(alg_dir, decoded->header.alg);
    TEST_ASSERT_EQUAL(enc_a128gcm, decoded->header.enc);
    
    cjwt_destroy(decoded);
    free(output);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_jwe_dir_a128gcm);
    return UNITY_END();
}
