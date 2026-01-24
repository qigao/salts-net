#include <unity.h>
#include <json_parser.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "turl_common.h"
#include "history/turl_history.h"
#include "history/turl_collection.h"

void setUp(void) {
    // Delete potential residue
    remove(".turl_history.json");
    remove("test_collection.json");
}

void tearDown(void) {
    remove(".turl_history.json");
    remove("test_collection.json");
}

void test_turl_render_template(void) {
    json_value_t *ctx = json_create_object();
    json_object_set_string(ctx, "name", "turl");
    json_object_set_string(ctx, "version", "1.0");

    char *res = turl_render_template("Hello {{name}} v{{version}}", ctx);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_EQUAL_STRING("Hello turl v1.0", res);
    free(res);

    json_free(ctx);
}

void test_turl_history_logging(void) {
    turl_http_config_t config = {0};
    config.method_str = "POST";
    char *h1 = strdup("Content-Type: application/json");
    char *headers[] = {h1};
    config.headers = headers;
    config.header_count = 1;
    config.body = "{\"test\":true}";
    
    turl_history_log(&config, "http://example.com/api", 200, "{\"status\":\"ok\"}", 15, "Server: TurboNet\r\n");
    
    free(h1);

    // Check if file exists
    FILE *f = fopen(".turl_history.json", "r");
    TEST_ASSERT_NOT_NULL(f);
    
    char line[4096];
    TEST_ASSERT_NOT_NULL(fgets(line, sizeof(line), f));
    fclose(f);
    
    // Parse read line
    json_value_t *history = json_parse(line, strlen(line));
    TEST_ASSERT_NOT_NULL(history);
    
    json_value_t *req = json_object_get(history, "request");
    TEST_ASSERT_NOT_NULL(req);
    TEST_ASSERT_EQUAL_STRING("http://example.com/api", json_get_string(req, "url"));

    json_value_t *resp = json_object_get(history, "response");
    TEST_ASSERT_NOT_NULL(resp);
    TEST_ASSERT_EQUAL_INT(200, (int)json_get_double(resp, "status", 0.0));
    
    json_free(history);
}

void test_turl_collection_parsing(void) {
    const char *collection_json = 
        "{\"name\": \"test_coll\", \"requests\": ["
        "  {\"name\": \"req1\", \"url\": \"http://h.com/1\", \"method\": \"GET\"},"
        "  {\"name\": \"req2\", \"url\": \"http://h.com/2\", \"method\": \"POST\", \"body\": \"data\"}"
        "]}";
    
    FILE *f = fopen("test_collection.json", "w");
    fputs(collection_json, f);
    fclose(f);
    
    json_value_t *coll = json_parse_file("test_collection.json");
    TEST_ASSERT_NOT_NULL(coll);
    TEST_ASSERT_EQUAL_STRING("test_coll", json_get_string(coll, "name"));
    
    json_value_t *reqs = json_object_get(coll, "requests");
    TEST_ASSERT_NOT_NULL(reqs);
    TEST_ASSERT_EQUAL_INT(2, json_array_size(reqs));
    
    json_free(coll);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_turl_render_template);
    RUN_TEST(test_turl_history_logging);
    RUN_TEST(test_turl_collection_parsing);
    return UNITY_END();
}
