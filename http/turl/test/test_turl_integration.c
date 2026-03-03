#include <tinytest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "turl_http.h"
#include "turl_common.h"
#include "collection/turl_collection.h"

static int is_network_error_ret(int ret) {
  return ret != 0;
}

spec("turl_integration") {
    it("should perform GET request to httpbin") {
        turl_http_config_t config = {0};
        config.url = "http://httpbin.org/get";
        config.method_str = "GET";
        config.verbose = 1;
        check_int_eq(turl_execute_http_request(&config), 0);
    }

    it("should perform POST request to httpbin") {
        turl_http_config_t config = {0};
        config.url = "http://httpbin.org/post";
        config.method_str = "POST";
        config.body = "{\"greeting\":\"hello\"}";
        config.verbose = 1;
        check_int_eq(turl_execute_http_request(&config), 0);
    }

    it("should perform request with headers to httpbin") {
        turl_http_config_t config = {0};
        config.url = "http://httpbin.org/headers";
        config.method_str = "GET";
        char *headers[] = {"X-Test-Header: integration-test"};
        config.headers = headers;
        config.header_count = 1;
        config.verbose = 1;
        check_int_eq(turl_execute_http_request(&config), 0);
    }

    it("should perform request with basic auth to httpbin") {
        turl_http_config_t config = {0};
        config.url = "http://httpbin.org/basic-auth/user/pass";
        config.method_str = "GET";
        config.user_pass = "user:pass";
        config.verbose = 1;
        check_int_eq(turl_execute_http_request(&config), 0);
    }

    it("should perform request with JWT generation to httpbin") {
        turl_http_config_t config = {0};
        config.url = "http://httpbin.org/bearer";
        config.method_str = "GET";
        config.jwt_secret = "secret";
        config.jwt_claims = "{\"sub\":\"1234567890\",\"name\":\"John Doe\",\"admin\":true}";
        config.verbose = 1;
        check_int_eq(turl_execute_http_request(&config), 0);
    }

    it("should run collection integration test") {
        const char *collection_json =
            "{\"name\": \"httpbin_coll\", \"requests\": ["
            "  {\"name\": \"GET_REQ\", \"url\": \"http://httpbin.org/get\"},"
            "  {\"name\": \"POST_REQ\", \"url\": \"http://httpbin.org/post\", \"method\": \"POST\", \"body\": \"hello collection\"}"
            "]}";

        FILE *f = fopen("it_collection.json", "w");
        if (f) {
            fputs(collection_json, f);
            fclose(f);
        }

        turl_http_config_t global_cfg = {0};
        global_cfg.verbose = 1;
        int ret = turl_run_collection("it_collection.json", &global_cfg);
        remove("it_collection.json");
        check_int_eq(ret, 0);
    }
}
