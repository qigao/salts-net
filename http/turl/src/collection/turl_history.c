#include "collection/turl_history.h"
#include "turl_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <platform.h>
#include <json_parser.h>

#define HISTORY_FILE ".turl_history.json"

void turl_history_log(const turl_http_config_t *config, const char *rendered_url,
                      char **rendered_headers, uint32_t rendered_header_count,
                      int status_code, const char *response_body, size_t response_len,
                      const char *response_headers) {
    
    json_value_t *entry = json_create_object();
    
    // Timestamp
    char timestamp[64];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", tm_info);
    json_object_add(entry, "timestamp", json_create_string(timestamp));

    // Request
    json_value_t *req = json_create_object();
    json_object_add(req, "method", json_create_string(config->method_str ? config->method_str : "GET"));
    json_object_add(req, "url", json_create_string(rendered_url));
    
    if (rendered_header_count > 0 && rendered_headers) {
        json_value_t *req_headers = json_create_object();
        for (uint32_t i = 0; i < rendered_header_count; i++) {
            char *h = rendered_headers[i];
            if (!h) continue;
            char *colon = strchr(h, ':');
            if (colon) {
                *colon = '\0';
                const char *val = colon + 1;
                while (*val == ' ') val++;
                json_object_add(req_headers, h, json_create_string(val));
                *colon = ':'; // restore
            }
        }
        json_object_add(req, "headers", req_headers);
    }
    
    if (config->body && config->body_len == 0) {
        json_object_add(req, "body", json_create_string(config->body));
    }
    
    json_object_add(entry, "request", req);

    // Response
    json_value_t *resp = json_create_object();
    json_object_add(resp, "status", json_create_number(status_code));
    
    if (response_headers) {
        json_object_add(resp, "headers", json_create_string(response_headers));
    }

    if (response_body && response_len > 0) {
        // Try to parse body as JSON for better history readability, otherwise store as string
        json_value_t *body_json = json_parse(response_body, response_len);
        if (body_json) {
            json_object_add(resp, "body", body_json);
        } else {
            json_object_add(resp, "body", json_create_string(response_body));
        }
    }
    
    json_object_add(entry, "response", resp);

    // Append to file (as JSON Lines for efficient appending)
    FILE *f = fopen(HISTORY_FILE, "a");
    if (f) {
        char *json_str = json_serialize(entry, NULL);
        if (json_str) {
            fprintf(f, "%s\n", json_str);
            json_serialize_free(json_str);
        }
        fclose(f);
    }
    
    json_free(entry);
}
