#include <tinytest.h>
#include <json_parser.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "turl_common.h"
#include "history/turl_history.h"
#include "history/turl_collection.h"

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

    it("should log history correctly") {
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
        check_not_null(f);
        
        char line[4096];
        check_not_null(fgets(line, sizeof(line), f));
        fclose(f);
        
        // Parse read line
        json_value_t *history = json_parse(line, strlen(line));
        check_not_null(history);
        
        json_value_t *req = json_object_get(history, "request");
        check_not_null(req);
        check_str_eq(json_get_string(req, "url"), "http://example.com/api");

        json_value_t *resp = json_object_get(history, "response");
        check_not_null(resp);
        check_int_eq((int)json_get_double(resp, "status", 0.0), 200);
        
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
