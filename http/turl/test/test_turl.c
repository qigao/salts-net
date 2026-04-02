#include <tinytest.h>
#include <turbo_parser.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "turl_common.h"
#include "turl_http.h"
#include "collection/turl_history.h"
#include "collection/turl_collection.h"

static json_value_t *turl_test_parse_json_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    long size;
    size_t read_size;
    char *buffer;
    json_value_t *root = NULL;

    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    size = ftell(fp);
    if (size < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }

    buffer = (char *)malloc((size_t)size + 1);
    if (!buffer) {
        fclose(fp);
        return NULL;
    }

    read_size = fread(buffer, 1, (size_t)size, fp);
    fclose(fp);
    if (read_size != (size_t)size) {
        free(buffer);
        return NULL;
    }

    buffer[size] = '\0';
    if (turbo_parse_json((const uint8_t *)buffer, (size_t)size, &root) != 0) {
        root = NULL;
    }
    free(buffer);
    return root;
}

spec("turl_unit") {
    before_each() {
        // Delete potential residue
        remove(".turl_history.json");
        remove("test_collection.json");
    }

    after_each() {
        remove(".turl_history.json");
        remove("test_collection.json");
    }

    it("should render template correctly") {
        json_value_t *ctx = turbo_json_create_object();
        turbo_json_object_set_string(ctx, "name", "turl");
        turbo_json_object_set_string(ctx, "version", "1.0");

        char *res = turl_render_template("Hello {{name}} v{{version}}", ctx);
        check_not_null(res);
        check_str_eq(res, "Hello turl v1.0");
        free(res);

        turbo_free_json(&ctx);
    }

    it("should render complex headers and URLs") {
        json_value_t *ctx = turbo_json_create_object();
        turbo_json_object_set_string(ctx, "base_url", "http://api.example.com");
        turbo_json_object_set_string(ctx, "token", "super-secret-token");
        turbo_json_object_set_string(ctx, "user_id", "42");

        char *url = turl_render_template("{{base_url}}/users/{{user_id}}", ctx);
        check_str_eq(url, "http://api.example.com/users/42");
        free(url);

        char *auth_header = turl_render_template("Authorization: Bearer {{token}}", ctx);
        check_str_eq(auth_header, "Authorization: Bearer super-secret-token");
        free(auth_header);

        turbo_free_json(&ctx);
    }

    it("should render JWT claims correctly") {
        json_value_t *ctx = turbo_json_create_object();
        turbo_json_object_set_string(ctx, "sub", "user_123");
        turbo_json_object_set_string(ctx, "role", "admin");
        
        const char *claims_tmpl = "{\"sub\": \"{{sub}}\", \"role\": \"{{role}}\", \"iat\": 1600000000}";
        char *rendered = turl_render_template(claims_tmpl, ctx);
        
        check_not_null(rendered);
        // Note: mustache might change field order if it were a JSON re-serialization, 
        // but here it's just string substitution.
        check_str_eq(rendered, "{\"sub\": \"user_123\", \"role\": \"admin\", \"iat\": 1600000000}");
        
        free(rendered);
        turbo_free_json(&ctx);
    }

    it("should fail request when JWT claims are invalid JSON") {
        turl_http_config_t config = {0};
        config.url = "http://127.0.0.1:1";
        config.method_str = "GET";
        config.jwt_secret = "secret";
        config.jwt_claims = "{\"sub\": ";

        check_int_eq(turl_execute_http_request(&config), 1);
    }

    it("should log collection with multiple headers correctly") {
        turl_http_config_t config = {0};
        config.method_str = "PUT";
        char *h1 = strdup("Content-Type: application/json");
        char *h2 = strdup("X-Custom-Header: {{val}}");
        char *headers[] = {h1, h2};
        config.headers = headers;
        config.header_count = 2;
        config.body = "{\"data\":\"test\"}";
        
        // We simulate that the header templates are ALREADY rendered before history_log
        // since history_log usually happens AFTER execution where rendering occurred.
        // Actually, turl_history_log uses config->headers directly.
        
        check_int_eq(turl_history_log(&config, "http://example.com/put", headers, 2, 201,
                                      "{\"ok\":true}", 11, "Server: TurboNet\r\n"),
                     0);
        
        free(h1);
        free(h2);

        FILE *f = fopen(".turl_history.json", "r");
        check_not_null(f);
        char line[4096];
        check_not_null(fgets(line, sizeof(line), f));
        fclose(f);
        
        json_value_t *history = NULL;
        check_int_eq(turbo_parse_json((const uint8_t *)line, strlen(line), &history), 0);
        check_not_null(history);
        
        json_value_t *req = turbo_json_object_get(history, "request");
        json_value_t *req_headers = turbo_json_object_get(req, "headers");
        check_not_null(req_headers);
        
        // Check both headers exist in history
        check_not_null(turbo_json_object_get(req_headers, "Content-Type"));
        check_not_null(turbo_json_object_get(req_headers, "X-Custom-Header"));
        
        turbo_free_json(&history);
    }

    it("should parse collection correctly") {
        const char *collection_json = 
            "{\"name\": \"test_coll\", \"requests\": ["
            "  {\"name\": \"req1\", \"url\": \"http://h.com/1\", \"method\": \"GET\"},"
            "  {\"name\": \"req2\", \"url\": \"http://h.com/2\", \"method\": \"POST\", \"body\": \"data\"}"
            "]}";
        
        FILE *f = fopen("test_collection.json", "w");
        if (f) {
            fputs(collection_json, f);
            fclose(f);
        }
        
        json_value_t *coll = turl_test_parse_json_file("test_collection.json");
        check_not_null(coll);
        check_str_eq(turbo_json_get_string(coll, "name"), "test_coll");
        
        json_value_t *reqs = turbo_json_object_get(coll, "requests");
        check_not_null(reqs);
        check_int_eq(turbo_json_array_size(reqs), 2);
        
        turbo_free_json(&coll);
    }

    it("should fail collection when a request is missing url") {
        const char *collection_json =
            "{\"name\": \"bad_coll\", \"requests\": ["
            "  {\"name\": \"broken\"}"
            "]}";

        FILE *f = fopen("test_collection.json", "w");
        check_not_null(f);
        fputs(collection_json, f);
        fclose(f);

        turl_http_config_t config = {0};
        check_int_eq(turl_run_collection("test_collection.json", &config), 1);
    }

    it("should fail history logging on malformed rendered header") {
        turl_http_config_t config = {0};
        config.method_str = "GET";
        char *headers[] = {"BrokenHeader"};

        check_int_eq(turl_history_log(&config, "http://example.com", headers, 1, 200,
                                      "ok", 2, "Server: TurboNet\r\n"),
                     -1);

        FILE *f = fopen(".turl_history.json", "r");
        check_null(f);
    }

    it("should fail collection on oversized header") {
        char long_value[1100];
        memset(long_value, 'x', sizeof(long_value) - 1);
        long_value[sizeof(long_value) - 1] = '\0';

        FILE *f = fopen("test_collection.json", "w");
        check_not_null(f);
        fprintf(f,
                "{\"name\":\"bad_headers\",\"requests\":[{\"url\":\"http://example.com\","
                "\"headers\":{\"X-Long\":\"%s\"}}]}",
                long_value);
        fclose(f);

        turl_http_config_t config = {0};
        check_int_eq(turl_run_collection("test_collection.json", &config), 1);
    }
}
