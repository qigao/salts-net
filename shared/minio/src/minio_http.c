#include "minio_http.h"
#include "minio_client_internal.h"
#include "minio/minio_signer.h"
#include "minio/minio_response.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

minio_http_response_t minio_http_execute(minio_http_request_t* req) {
    minio_http_response_t res = {0};
    res.headers = MinioHeaders_init();

    http_client_t* client = http_client_create();
    if (!client) {
        res.error = minio_error_make(-1, "Failed to create HTTP client");
        return res;
    }
    http_client_follow_redirects(client, 0);

    int hdr_count = 0;
    const char** hdrs = minio_headers_to_http_array(&req->headers, &hdr_count);

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

        // Parse response headers into MinioHeaders
        // http_response_t has headers_list (http_header_entry_t*)
        for (http_header_entry_t* p = hresp->headers_list; p; p = p->next) {
            minio_headers_add(&res.headers, p->name, p->value);
        }

        if (hresp->error) {
            res.error = minio_error_make(-1, hresp->error);
        } else {
            res.error = MINIO_OK;
        }
        http_response_free(hresp);
    } else {
        res.error = minio_error_make(-1, "HTTP request failed (no response)");
    }

    // Cleanup headers array
    if (hdrs) {
        for (int i = 0; i < hdr_count; i++) free((void*)hdrs[i]);
        free(hdrs);
    }

    http_client_destroy(client);
    return res;
}

void minio_http_response_free(minio_http_response_t* resp) {
    if (!resp) return;
    tstr_free(resp->body);
    MinioHeaders_drop(&resp->headers);
    minio_error_free(&resp->error);
}

tstr_t minio_build_url(const minio_base_url_t* base, const char* path) {
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

minio_http_response_t minio_execute_signed(minio_client_t* client,
                                            const char* method,
                                            const char* uri_path,
                                            MinioHeaders* extra_headers,
                                            MinioHeaders* query_params,
                                            const char* body, size_t body_len) {
    minio_http_response_t fail = {0};
    fail.headers = MinioHeaders_init();

    if (!client || !method || !uri_path) {
        fail.error = minio_error_make(-1, "Invalid params");
        return fail;
    }

    // Fetch credentials
    minio_credentials_t creds = {0};
    minio_error_t err = client->provider->fetch(client->provider->ctx, &creds);
    if (!minio_is_ok(err)) {
        fail.error = err;
        return fail;
    }

    // Build URL
    tstr_t url_s = minio_build_url(&client->base_url, uri_path);

    // Append query params to URL if any
    if (query_params && MinioHeaders_size(query_params) > 0) {
        const char* sep = "?";
        c_foreach(i, MinioHeaders, *query_params) {
            url_s = tstr_cat_fmt(url_s, "%s%s=%s", sep,
                                  cstr_str(&i.ref->first), cstr_str(&i.ref->second));
            sep = "&";
        }
    }

    // Build request
    minio_http_request_t req = {0};
    req.method = method;
    req.url = url_s;
    req.body = body;
    req.body_len = body_len;
    req.headers = MinioHeaders_init();
    minio_headers_add(&req.headers, "Host", client->base_url.host);

    // Merge extra headers
    if (extra_headers) {
        c_foreach(i, MinioHeaders, *extra_headers) {
            minio_headers_add(&req.headers, cstr_str(&i.ref->first), cstr_str(&i.ref->second));
        }
    }

    // Sign
    time_t now = minio_time_now();
    tstr_t content_sha256 = minio_signer_sha256_hex(body ? body : "", body_len);

    MinioHeaders sign_query = MinioHeaders_init();
    if (query_params) {
        c_foreach(i, MinioHeaders, *query_params) {
            minio_headers_add(&sign_query, cstr_str(&i.ref->first), cstr_str(&i.ref->second));
        }
    }

    err = minio_signer_sign_v4_s3(method, uri_path, client->base_url.region,
                                   &req.headers, &sign_query,
                                   creds.access_key, creds.secret_key, creds.session_token,
                                   content_sha256, now);

    minio_http_response_t result;
    if (minio_is_ok(err)) {
        result = minio_http_execute(&req);
    } else {
        result.status_code = 0;
        result.body = NULL;
        result.headers = MinioHeaders_init();
        result.error = err;
    }

    minio_credentials_clear(&creds);
    tstr_free(url_s);
    tstr_free(content_sha256);
    MinioHeaders_drop(&req.headers);
    MinioHeaders_drop(&sign_query);

    return result;
}
