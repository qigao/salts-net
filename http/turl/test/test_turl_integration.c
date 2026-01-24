#include <unity.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "turl_http.h"
#include "turl_common.h"

void setUp(void) {
}

void tearDown(void) {
}

void test_turl_httpbin_get(void) {
    turl_http_config_t config = {0};
    config.url = "http://httpbin.org/get";
    config.method_str = "GET";
    config.verbose = 1;
    
    int ret = turl_execute_http_request(&config);
    TEST_ASSERT_EQUAL_INT(0, ret);
}

void test_turl_httpbin_post(void) {
    turl_http_config_t config = {0};
    config.url = "http://httpbin.org/post";
    config.method_str = "POST";
    config.body = "{\"greeting\":\"hello\"}";
    config.verbose = 1;
    
    int ret = turl_execute_http_request(&config);
    TEST_ASSERT_EQUAL_INT(0, ret);
}

void test_turl_httpbin_headers(void) {
    turl_http_config_t config = {0};
    config.url = "http://httpbin.org/headers";
    config.method_str = "GET";
    char *headers[] = {"X-Test-Header: integration-test"};
    config.headers = headers;
    config.header_count = 1;
    config.verbose = 1;
    
    int ret = turl_execute_http_request(&config);
    TEST_ASSERT_EQUAL_INT(0, ret);
}

void test_turl_httpbin_basic_auth(void) {
    turl_http_config_t config = {0};
    config.url = "http://httpbin.org/basic-auth/user/pass";
    config.method_str = "GET";
    config.user_pass = "user:pass";
    config.verbose = 1;
    
    int ret = turl_execute_http_request(&config);
    TEST_ASSERT_EQUAL_INT(0, ret);
}

void test_turl_httpbin_jwt_generation(void) {
    turl_http_config_t config = {0};
    config.url = "http://httpbin.org/bearer";
    config.method_str = "GET";
    config.jwt_secret = "secret";
    config.jwt_claims = "{\"sub\":\"1234567890\",\"name\":\"John Doe\",\"admin\":true}";
    config.verbose = 1;
    
    int ret = turl_execute_http_request(&config);
    TEST_ASSERT_EQUAL_INT(0, ret);
}

void test_turl_run_collection_integration(void) {
    const char *collection_json = 
        "{\"name\": \"httpbin_coll\", \"requests\": ["
        "  {\"name\": \"GET_REQ\", \"url\": \"http://httpbin.org/get\"},"
        "  {\"name\": \"POST_REQ\", \"url\": \"http://httpbin.org/post\", \"method\": \"POST\", \"body\": \"hello collection\"}"
        "]}";
    
    FILE *f = fopen("it_collection.json", "w");
    fputs(collection_json, f);
    fclose(f);
    
    turl_http_config_t global_cfg = {0};
    global_cfg.verbose = 1;
    
    int ret = turl_run_collection("it_collection.json", &global_cfg);
    TEST_ASSERT_EQUAL_INT(0, ret);
    
    remove("it_collection.json");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_turl_httpbin_get);
    RUN_TEST(test_turl_httpbin_post);
    RUN_TEST(test_turl_httpbin_headers);
    RUN_TEST(test_turl_httpbin_basic_auth);
    RUN_TEST(test_turl_httpbin_jwt_generation);
    RUN_TEST(test_turl_run_collection_integration);
    return UNITY_END();
}
