#include <tinytest.h>
#include <json_parser.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "turl_common.h"
#include "collection/turl_history.h"
#include "collection/turl_collection.h"

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
        json_value_t *ctx = json_create_object();
        json_object_set_string(ctx, "name", "turl");
        json_object_set_string(ctx, "version", "1.0");

        char *res = turl_render_template("Hello {{name}} v{{version}}", ctx);
        check_not_null(res);
        check_str_eq(res, "Hello turl v1.0");
        free(res);

        json_free(ctx);
    }

    it("should render complex headers and URLs") {
        json_value_t *ctx = json_create_object();
        json_object_set_string(ctx, "base_url", "http://api.example.com");
        json_object_set_string(ctx, "token", "super-secret-token");
        json_object_set_string(ctx, "user_id", "42");

        char *url = turl_render_template("{{base_url}}/users/{{user_id}}", ctx);
        check_str_eq(url, "http://api.example.com/users/42");
        free(url);

        char *auth_header = turl_render_template("Authorization: Bearer {{token}}", ctx);
        check_str_eq(auth_header, "Authorization: Bearer super-secret-token");
        free(auth_header);

        json_free(ctx);
    }

    it("should render JWT claims correctly") {
        json_value_t *ctx = json_create_object();
        json_object_set_string(ctx, "sub", "user_123");
        json_object_set_string(ctx, "role", "admin");
        
        const char *claims_tmpl = "{\"sub\": \"{{sub}}\", \"role\": \"{{role}}\", \"iat\": 1600000000}";
        char *rendered = turl_render_template(claims_tmpl, ctx);
        
        check_not_null(rendered);
        // Note: mustache might change field order if it were a JSON re-serialization, 
        // but here it's just string substitution.
        check_str_eq(rendered, "{\"sub\": \"user_123\", \"role\": \"admin\", \"iat\": 1600000000}");
        
        free(rendered);
        json_free(ctx);
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
        
        turl_history_log(&config, "http://example.com/put", headers, 2, 201, "{\"ok\":true}", 11, "Server: TurboNet\r\n");
        
        free(h1);
        free(h2);

        FILE *f = fopen(".turl_history.json", "r");
        check_not_null(f);
        char line[4096];
        check_not_null(fgets(line, sizeof(line), f));
        fclose(f);
        
        json_value_t *history = json_parse(line, strlen(line));
        check_not_null(history);
        
        json_value_t *req = json_object_get(history, "request");
        json_value_t *req_headers = json_object_get(req, "headers");
        check_not_null(req_headers);
        
        // Check both headers exist in history
        check_not_null(json_object_get(req_headers, "Content-Type"));
        check_not_null(json_object_get(req_headers, "X-Custom-Header"));
        
        json_free(history);
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
        
        json_value_t *coll = json_parse_file("test_collection.json");
        check_not_null(coll);
        check_str_eq(json_get_string(coll, "name"), "test_coll");
        
        json_value_t *reqs = json_object_get(coll, "requests");
        check_not_null(reqs);
        check_int_eq(json_array_size(reqs), 2);
        
        json_free(coll);
    }
}
