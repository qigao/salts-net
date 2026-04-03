#include "collection/turl_collection.h"
#include "turl_common.h"
#include <fmt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlog.h>

static json_value_t *turl_parse_json_file(const char *path) {
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

int turl_run_collection(const char *collection_file, const turl_http_config_t *global_config) {
    json_value_t *collection = turl_parse_json_file(collection_file);
    if (!collection) {
        TLOG_ERROR("Failed to parse collection file: {}", collection_file);
        return 1;
    }

    const char *collection_name = turbo_json_get_string(collection, "name");
    TLOG_INFO("Running Collection: {}", collection_name ? collection_name : collection_file);

    json_value_t *requests = turbo_json_object_get(collection, "requests");
    if (!requests || turbo_json_type(requests) != TURBO_JSON_ARRAY) {
        TLOG_ERROR("No 'requests' array found in collection");
        turbo_free_json(&collection);
        return 1;
    }

    size_t count = turbo_json_array_size(requests);
    int failures = 0;

    for (size_t i = 0; i < count; i++) {
        json_value_t *item = turbo_json_array_get(requests, i);
        const char *name = turbo_json_get_string(item, "name");
        const char *url = turbo_json_get_string(item, "url");
        const char *method = turbo_json_get_string(item, "method");
        const char *body = turbo_json_get_string(item, "body");
        char **dynamic_headers = NULL;
        uint32_t dynamic_count = 0;

        if (!url) {
            TLOG_ERROR("Request {} invalid: missing 'url'", i);
            failures++;
            continue;
        }

        TLOG_INFO("--------------------------------------------------");
        TLOG_INFO("Request {}/{}: {}", i + 1, count, name ? name : url);

        // Prepare local config: shallow copy global config, then override
        turl_http_config_t local_cfg = *global_config;
        local_cfg.url = url;
        local_cfg.method_str = method ? method : "GET";
        local_cfg.body = body;
        local_cfg.body_len = 0; // standard string data

        // Handle headers from item
        json_value_t *headers_obj = turbo_json_object_get(item, "headers");
        int header_error = 0;

        if (headers_obj && turbo_json_type(headers_obj) == TURBO_JSON_OBJECT) {
            size_t h_size = turbo_json_object_size(headers_obj);
            dynamic_headers = (char **)calloc(h_size, sizeof(char *));
            if (!dynamic_headers) {
                TLOG_ERROR("Failed to allocate collection headers");
                failures++;
                continue;
            }

            for (size_t j = 0; j < h_size; j++) {
                const char *key = turbo_json_object_key(headers_obj, j);
                const char *val = turbo_json_get_string(headers_obj, key);
                if (key && val) {
                    char buf[1024];
                    size_t required_len = strlen(key) + strlen(val) + 2;
                    if (required_len >= sizeof(buf)) {
                        TLOG_ERROR("Collection header too long: {}", key);
                        header_error = 1;
                        break;
                    }
                    int header_len = fmt(buf, sizeof(buf), "{}: {}", key, val);
                    if (header_len <= 0) {
                        TLOG_ERROR("Failed to format collection header: {}", key);
                        header_error = 1;
                        break;
                    }
                    dynamic_headers[dynamic_count] = strdup(buf);
                    if (!dynamic_headers[dynamic_count]) {
                        TLOG_ERROR("Failed to allocate collection header");
                        header_error = 1;
                        break;
                    }
                    dynamic_count++;
                }
            }
            if (!header_error) {
                local_cfg.headers = dynamic_headers;
                local_cfg.header_count = dynamic_count;
            }
        }

        if (header_error) {
            failures++;
            for (uint32_t j = 0; j < dynamic_count; j++) free(dynamic_headers[j]);
            free(dynamic_headers);
            continue;
        }

        if (turl_execute_http_request(&local_cfg) != 0) {
            failures++;
        }

        // Cleanup local headers
        for (uint32_t j = 0; j < dynamic_count; j++) free(dynamic_headers[j]);
        free(dynamic_headers);
    }

    TLOG_INFO("==================================================");
    TLOG_INFO("Collection finished: {} total, {} failures", (int)count, failures);

    turbo_free_json(&collection);
    return failures > 0 ? 1 : 0;
}
