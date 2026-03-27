#include "collection/turl_history.h"
#include "turl_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <platform.h>
#include <json_parser.h>

#define HISTORY_FILE ".turl_history.json"

static int turl_history_add(json_value_t *obj, const char *key, json_value_t *value) {
    if (!obj || !key || !value) {
        return -1;
    }
    json_object_add(obj, key, value);
    return 0;
}

int turl_history_log(const turl_http_config_t *config, const char *rendered_url,
                     char **rendered_headers, uint32_t rendered_header_count,
                     int status_code, const char *response_body, size_t response_len,
                     const char *response_headers) {
    int rc = -1;
    FILE *f = NULL;
    char *json_str = NULL;
    json_value_t *entry = NULL;
    json_value_t *req = NULL;
    json_value_t *resp = NULL;
    json_value_t *req_headers = NULL;
    char timestamp[64];
    time_t now;
    struct tm *tm_info;

    if (!config || !rendered_url) {
        return -1;
    }

    entry = json_create_object();
    req = json_create_object();
    resp = json_create_object();
    if (!entry || !req || !resp) {
        goto cleanup;
    }

    now = time(NULL);
    tm_info = localtime(&now);
    if (!tm_info) {
        goto cleanup;
    }
    if (strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", tm_info) == 0) {
        goto cleanup;
    }

    if (turl_history_add(entry, "timestamp", json_create_string(timestamp)) != 0 ||
        turl_history_add(req, "method",
                         json_create_string(config->method_str ? config->method_str : "GET")) != 0 ||
        turl_history_add(req, "url", json_create_string(rendered_url)) != 0 ||
        turl_history_add(resp, "status", json_create_number(status_code)) != 0) {
        goto cleanup;
    }

    if (rendered_header_count > 0 && rendered_headers) {
        req_headers = json_create_object();
        if (!req_headers) {
            goto cleanup;
        }

        for (uint32_t i = 0; i < rendered_header_count; i++) {
            char *h = rendered_headers[i];
            char *colon;
            const char *val;

            if (!h) {
                goto cleanup;
            }

            colon = strchr(h, ':');
            if (!colon || colon == h) {
                goto cleanup;
            }

            *colon = '\0';
            val = colon + 1;
            while (*val == ' ') {
                val++;
            }

            if (turl_history_add(req_headers, h, json_create_string(val)) != 0) {
                *colon = ':';
                goto cleanup;
            }
            *colon = ':';
        }

        if (turl_history_add(req, "headers", req_headers) != 0) {
            goto cleanup;
        }
        req_headers = NULL;
    }

    if (config->body && config->body_len == 0) {
        if (turl_history_add(req, "body", json_create_string(config->body)) != 0) {
            goto cleanup;
        }
    }

    if (response_headers) {
        if (turl_history_add(resp, "headers", json_create_string(response_headers)) != 0) {
            goto cleanup;
        }
    }

    if (response_body && response_len > 0) {
        json_value_t *body_json = json_parse(response_body, response_len);
        if (!body_json) {
            body_json = json_create_string(response_body);
        }
        if (turl_history_add(resp, "body", body_json) != 0) {
            goto cleanup;
        }
    }

    if (turl_history_add(entry, "request", req) != 0 ||
        turl_history_add(entry, "response", resp) != 0) {
        goto cleanup;
    }
    req = NULL;
    resp = NULL;

    f = fopen(HISTORY_FILE, "a");
    if (!f) {
        goto cleanup;
    }

    json_str = json_serialize(entry, NULL);
    if (!json_str) {
        goto cleanup;
    }

    if (fprintf(f, "%s\n", json_str) < 0) {
        goto cleanup;
    }

    rc = 0;

cleanup:
    if (json_str) {
        json_serialize_free(json_str);
    }
    if (f) {
        fclose(f);
    }
    if (req_headers) {
        json_free(req_headers);
    }
    if (req) {
        json_free(req);
    }
    if (resp) {
        json_free(resp);
    }
    if (entry) {
        json_free(entry);
    }
    return rc;
}
