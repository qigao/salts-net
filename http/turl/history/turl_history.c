#include "turl_history.h"
#include "../turl_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <platform.h>
#include <json_parser.h>

#define HISTORY_FILE ".turl_history.json"

void turl_history_log(const turl_http_config_t *config, const char *rendered_url,
                      int status_code, const char *response_body, size_t response_len,
                      const char *response_headers) {
    
    json_value_t *entry = json_create_object();
    if (!entry) return;
    
    // Timestamp
    char timestamp[64] = "unknown";
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    if (tm_info) {
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", tm_info);
    }
    json_object_set_string(entry, "timestamp", timestamp);

    // Request
    json_value_t *req = json_value_object_arena(entry->arena);
    if (req) {
        json_object_set_string(req, "method", config->method_str ? config->method_str : "GET");
        json_object_set_string(req, "url", rendered_url);
        
        if (config->header_count > 0) {
            json_value_t *req_headers = json_value_object_arena(entry->arena);
            if (req_headers) {
                for (uint32_t i = 0; i < config->header_count; i++) {
                    char *h = config->headers[i];
                    char *colon = strchr(h, ':');
                    if (colon) {
                        *colon = '\0';
                        const char *val = colon + 1;
                        while (*val == ' ') val++;
                        json_object_set_string(req_headers, h, val);
                        *colon = ':'; // restore
                    }
                }
                json_object_add(req, "headers", req_headers);
            }
        }
        
        if (config->body && config->body_len == 0) {
            json_object_set_string(req, "body", config->body);
        }
        
        json_object_add(entry, "request", req);
    }

    // Response
    json_value_t *resp = json_value_object_arena(entry->arena);
    if (resp) {
        json_object_set_number(resp, "status", (double)status_code);
        
        if (response_headers) {
            json_object_set_string(resp, "headers", response_headers);
        }

        if (response_body && response_len > 0) {
            // Try to parse body as JSON for better history readability
            // Note: json_parse creates its OWN arena. 
            // We'll use json_object_add which handles merging (now fixed/improved)
            json_value_t *body_json = json_parse(response_body, response_len);
            if (body_json) {
                json_object_add(resp, "body", body_json);
            } else {
                json_object_set_string(resp, "body", response_body);
            }
        }
        
        json_object_add(entry, "response", resp);
    }

    // Append to file
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
