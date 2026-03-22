#include "s3/s3_url.h"
#include <uri_parser.h>
#include <turbo_str.h>
#include <fmt.h>
#include <string.h>
#include <stdlib.h>
#include <http_common.h>

s3_url_t s3_url_parse(const char* input) {
    s3_url_t url = {0};
    uri_t parsed = {0};
    if (uri_parse(input, &parsed) != 1 || !parsed.valid) {
        return url;
    }

    url.is_https = (strcmp(parsed.scheme, "https") == 0);
    url.host = tstr_dup(parsed.host);

    url.port = parsed.port;
    if (url.port == 0) {
        url.port = url.is_https ? 443 : 80;
    }

    url.path = tstr_dup(parsed.path);
    url.query_string = tstr_dup(parsed.query);

    return url;
}

tstr_t s3_url_to_string(const s3_url_t* url) {
    tstr_t s = tstr_new();
    s = tstr_cat(s, url->is_https ? "https://" : "http://");
    s = tstr_cat(s, url->host);
    if (!((url->is_https && url->port == 443) || (!url->is_https && url->port == 80))) {
        char port_buf[16];
        fmt(port_buf, sizeof(port_buf), ":{}", url->port);
        s = tstr_cat(s, port_buf);
    }
    if (tstr_len(url->path) > 0) {
        if (url->path[0] != '/') s = tstr_cat(s, "/");
        s = tstr_cat(s, url->path);
    }
    if (tstr_len(url->query_string) > 0) {
        s = tstr_cat(s, "?");
        s = tstr_cat(s, url->query_string);
    }
    return s;
}

tstr_t s3_url_encode(const char* input) {
    if (!input) return tstr_new();
    char* raw = turbo_url_encode(input);
    if (!raw) return tstr_new();
    tstr_t result = tstr_dup(raw);
    free(raw);
    return result;
}

tstr_t s3_url_decode(const char* input) {
    if (!input) return tstr_new();
    char* raw = turbo_url_decode(input);
    if (!raw) return tstr_new();
    tstr_t result = tstr_dup(raw);
    free(raw);
    return result;
}

void s3_url_free(s3_url_t* url) {
    tstr_free(url->host);
    tstr_free(url->path);
    tstr_free(url->query_string);
}
