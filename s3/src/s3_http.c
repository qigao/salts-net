#include "s3_http.h"
#include "s3_client_internal.h"
#include "s3/s3_signer.h"
#include "s3/s3_response.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Parse raw "Name: Value\r\n" header string into S3Headers
static void parse_raw_headers_into_s3headers(const char* raw, size_t raw_len, S3Headers* out) {
    if (!raw || raw_len == 0) return;
    const char* end = raw + raw_len;

    // Some servers may emit interim responses (e.g. 100 Continue) before
    // the final 2xx response. We want headers from the last HTTP block.
    const char* block_start = raw;
    const char* scan = raw;
    while (scan < end) {
        const char* http = strstr(scan, "HTTP/");
        if (!http || http >= end) break;
        if (http == raw || *(http - 1) == '\n') {
            block_start = http;
        }
        scan = http + 5;
    }

    const char* block_end = strstr(block_start, "\r\n\r\n");
    if (!block_end || block_end > end) block_end = end;

    const char* p = block_start;
    // Skip status line
    while (p < block_end && *p != '\r' && *p != '\n') p++;
    while (p < block_end && (*p == '\r' || *p == '\n')) p++;

    while (p < end) {
        if (p >= block_end) break;
        const char* line_end = p;
        while (line_end < block_end && *line_end != '\r' && *line_end != '\n') line_end++;
        if (line_end == p) { // empty line
            while (line_end < block_end && (*line_end == '\r' || *line_end == '\n')) line_end++;
            p = line_end;
            continue;
        }
        const char* colon = memchr(p, ':', (size_t)(line_end - p));
        if (colon) {
            // Name
            size_t name_len = (size_t)(colon - p);
            char* name = malloc(name_len + 1);
            memcpy(name, p, name_len);
            name[name_len] = '\0';
            // Value (skip ": " prefix)
            const char* val = colon + 1;
            while (val < line_end && *val == ' ') val++;
            size_t val_len = (size_t)(line_end - val);
            char* value = malloc(val_len + 1);
            memcpy(value, val, val_len);
            value[val_len] = '\0';
            s3_headers_add(out, name, value);
            free(name);
            free(value);
        }
        // Advance past \r\n
        p = line_end;
        while (p < block_end && (*p == '\r' || *p == '\n')) p++;
    }
}

s3_http_response_t s3_http_execute(coro_context_t *ctx, s3_http_request_t* req) {
    s3_http_response_t res = {0};
    res.headers = S3Headers_init();

    http_client_t *client = http_client_create(NULL);    if (!client) {
        res.error = s3_error_make(-1, "Failed to create HTTP coro client");
        return res;
    }
    http_client_follow_redirects(client, 0);

    http_retry_policy_t policy = {0};
    policy.max_retries = 3;
    policy.initial_delay_ms = 1000;
    policy.max_delay_ms = 30000;
    policy.exponential_backoff = 1;
    policy.retry_on_timeout = 0;
    policy.retry_on_connection_error = 1;
    policy.retry_on_5xx = 1;
    policy.jitter_factor = 0.1;
    http_client_set_retry_policy(client, &policy);

    // Base 30s + 30s per MB of body to accommodate large uploads on slow networks
    int timeout_ms = 30000;
    if (req->body_len > 0) {
        timeout_ms += (int)((req->body_len / (1024 * 1024)) + 1) * 30000;
    }
    http_client_set_timeout(client, timeout_ms);

    int hdr_count = 0;
    const char** hdrs = s3_headers_to_http_array(&req->headers, &hdr_count);

    // Map method string to http_method_t
    http_method_t method = HTTP_GET;
    if (strcmp(req->method, "POST") == 0) method = HTTP_POST;
    else if (strcmp(req->method, "PUT") == 0) method = HTTP_PUT;
    else if (strcmp(req->method, "DELETE") == 0) method = HTTP_DELETE;
    else if (strcmp(req->method, "HEAD") == 0) method = HTTP_HEAD;

    http_response_t *hresp = http_request(client, method, req->url,
                                                     hdrs, hdr_count,
                                                     req->body, req->body_len);

    if (hresp) {
        res.status_code = hresp->status_code;
        res.body = hresp->body ? tstr_dup(hresp->body) : tstr_new();

        // Parse raw headers string into S3Headers
        if (hresp->headers) {
            parse_raw_headers_into_s3headers(hresp->headers, strlen(hresp->headers), &res.headers);
        }

        if (hresp->error) {
            res.error = s3_error_make(-1, hresp->error);
        } else {
            res.error = S3_OK;
        }
        http_response_free(hresp);
    } else {
        res.error = s3_error_make(-1, "HTTP request failed (no response)");
    }

    // Cleanup headers array
    if (hdrs) {
        for (int i = 0; i < hdr_count; i++) free((void*)hdrs[i]);
        free(hdrs);
    }

    http_client_destroy(client);
    return res;
}

void s3_http_response_free(s3_http_response_t* resp) {
    if (!resp) return;
    tstr_free(resp->body);
    S3Headers_drop(&resp->headers);
    s3_error_free(&resp->error);
}

tstr_t s3_build_url(const s3_base_url_t* base, const char* path) {
    tstr_t url = tstr_new();
    url = tstr_cat(url, base->is_https ? "https://" : "http://");
    url = tstr_cat(url, base->host);
    if (!((base->is_https && base->port == 443) ||
          (!base->is_https && base->port == 80))) {
        url = tstr_cat_fmt(url, ":%d", base->port);
    }
    if (path) url = tstr_cat(url, path);
    return url;
}

s3_http_response_t s3_execute_signed(s3_client_t* client,
                                            const char* method,
                                            const char* uri_path,
                                            S3Headers* extra_headers,
                                            S3Headers* query_params,
                                            const char* body, size_t body_len) {
    s3_http_response_t fail = {0};
    fail.headers = S3Headers_init();

    if (!client || !method || !uri_path) {
        fail.error = s3_error_make(-1, "Invalid params");
        return fail;
    }

    // Fetch credentials
    s3_credentials_t creds = {0};
    s3_error_t err = client->provider->fetch(client->provider->ctx, &creds);
    if (!s3_is_ok(err)) {
        fail.error = err;
        return fail;
    }

    // Build URL
    tstr_t url_s = s3_build_url(&client->base_url, uri_path);

    // Append query params (URL-encoded) if any
    if (query_params && S3Headers_size(query_params) > 0) {
        tstr_t qs = s3_headers_to_query_string(query_params);
        if (qs && tstr_len(qs) > 0) {
            url_s = tstr_cat(url_s, "?");
            url_s = tstr_cat(url_s, qs);
        }
        tstr_free(qs);
    }

    // Build request
    s3_http_request_t req = {0};
    req.method = method;
    req.url = url_s;
    req.body = body;
    req.body_len = body_len;
    req.headers = S3Headers_init();
    s3_headers_add(&req.headers, "Host", client->base_url.host);

    // Merge extra headers
    if (extra_headers) {
        c_foreach(i, S3Headers, *extra_headers) {
            s3_headers_add(&req.headers, cstr_str(&i.ref->first), cstr_str(&i.ref->second));
        }
    }

    // Sign
    time_t now = s3_time_now();
    tstr_t content_sha256 = s3_signer_sha256_hex(body ? body : "", body_len);

    S3Headers sign_query = S3Headers_init();
    if (query_params) {
        c_foreach(i, S3Headers, *query_params) {
            s3_headers_add(&sign_query, cstr_str(&i.ref->first), cstr_str(&i.ref->second));
        }
    }

    err = s3_signer_sign_v4_s3(method, uri_path, client->base_url.region,
                                   &req.headers, &sign_query,
                                   creds.access_key, creds.secret_key, creds.session_token,
                                   content_sha256, now);

    s3_http_response_t result;
    if (s3_is_ok(err)) {
        result = s3_http_execute(client->coro_ctx, &req);
    } else {
        result.status_code = 0;
        result.body = NULL;
        result.headers = S3Headers_init();
        result.error = err;
    }

    s3_credentials_clear(&creds);
    tstr_free(url_s);
    tstr_free(content_sha256);
    S3Headers_drop(&req.headers);
    S3Headers_drop(&sign_query);

    return result;
}
