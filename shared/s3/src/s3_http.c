#include "s3_http.h"
#include "s3_client_internal.h"
#include "s3/s3_signer.h"
#include "s3/s3_response.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

s3_http_response_t s3_http_execute(s3_http_request_t* req) {
    s3_http_response_t res = {0};
    res.headers = S3Headers_init();

    http_client_t* client = http_client_create();
    if (!client) {
        res.error = s3_error_make(-1, "Failed to create HTTP client");
        return res;
    }
    http_client_follow_redirects(client, 0);

    int hdr_count = 0;
    const char** hdrs = s3_headers_to_http_array(&req->headers, &hdr_count);

    // Map method string to http_method_t
    http_method_t method = HTTP_GET;
    if (strcmp(req->method, "POST") == 0) method = HTTP_POST;
    else if (strcmp(req->method, "PUT") == 0) method = HTTP_PUT;
    else if (strcmp(req->method, "DELETE") == 0) method = HTTP_DELETE;
    else if (strcmp(req->method, "HEAD") == 0) method = HTTP_HEAD;


    http_response_t* hresp = http_request(client, method, req->url, hdrs, hdr_count, req->body, req->body_len);

    if (hresp) {
        res.status_code = hresp->status_code;
        res.body = hresp->body ? tstr_dup(hresp->body) : tstr_new();

        // Parse response headers into S3Headers
        // http_response_t has headers_list (http_header_entry_t*)
        for (http_header_entry_t* p = hresp->headers_list; p; p = p->next) {
            s3_headers_add(&res.headers, p->name, p->value);
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

    // Append query params to URL if any
    if (query_params && S3Headers_size(query_params) > 0) {
        const char* sep = "?";
        c_foreach(i, S3Headers, *query_params) {
            url_s = tstr_cat_fmt(url_s, "%s%s=%s", sep,
                                  cstr_str(&i.ref->first), cstr_str(&i.ref->second));
            sep = "&";
        }
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
        result = s3_http_execute(&req);
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
