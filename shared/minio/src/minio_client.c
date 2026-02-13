#include "minio/minio_client.h"
#include "minio/minio_signer.h"
#include "minio/minio_response.h"
#include "minio/minio_xml_builder.h"
#include "minio_http.h"
#include "minio_client_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <turbo_str.h>
#include <fmt.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>

#define MINIO_PART_SIZE (5 * 1024 * 1024) // 5MB

// ── Client lifecycle ──

minio_client_t* minio_client_create(const minio_base_url_t* base_url,
                                   minio_credential_provider_t* provider) {
    minio_client_t* client = calloc(1, sizeof(minio_client_t));
    if (!client) return NULL;
    client->base_url.host = tstr_dup(base_url->host);
    client->base_url.port = base_url->port;
    client->base_url.is_https = base_url->is_https;
    client->base_url.region = tstr_dup(base_url->region);
    client->base_url.virtual_style = base_url->virtual_style;
    client->provider = provider;
    return client;
}

void minio_client_destroy(minio_client_t* client) {
    if (!client) return;
    tstr_free(client->base_url.host);
    tstr_free(client->base_url.region);
    free(client);
}

void minio_base_url_free(minio_base_url_t* url) {
    if (!url) return;
    tstr_free(url->host);
    tstr_free(url->region);
    url->host = NULL;
    url->region = NULL;
}

// ── Helper: check response and extract error ──

static minio_error_t check_response(minio_http_response_t* hres, int expected_status) {
    if (!minio_is_ok(hres->error)) {
        minio_error_t e = hres->error;
        hres->error = MINIO_OK;
        return e;
    }
    if (hres->status_code == expected_status) return MINIO_OK;
    if (hres->body && hres->body[0] != '\0') return minio_parse_error_xml(hres->body);
    return minio_error_make(hres->status_code, "Unexpected status code");
}

static minio_error_t check_response_2xx(minio_http_response_t* hres) {
    if (!minio_is_ok(hres->error)) {
        minio_error_t e = hres->error;
        hres->error = MINIO_OK;
        return e;
    }
    if (hres->status_code >= 200 && hres->status_code < 300) return MINIO_OK;
    if (hres->body && hres->body[0] != '\0') return minio_parse_error_xml(hres->body);
    return minio_error_make(hres->status_code, "Unexpected status code");
}

// ── Free helpers ──

void minio_stat_object_response_free(minio_stat_object_response_t* resp) {
    if (!resp) return;
    tstr_free(resp->etag);
    tstr_free(resp->content_type);
    minio_error_free(&resp->error);
    resp->etag = NULL;
    resp->content_type = NULL;
}

void minio_create_multipart_response_free(minio_create_multipart_response_t* resp) {
    if (!resp) return;
    tstr_free(resp->upload_id);
    minio_error_free(&resp->error);
    resp->upload_id = NULL;
}

void minio_upload_part_response_free(minio_upload_part_response_t* resp) {
    if (!resp) return;
    tstr_free(resp->etag);
    minio_error_free(&resp->error);
    resp->etag = NULL;
}

void minio_complete_multipart_response_free(minio_complete_multipart_response_t* resp) {
    if (!resp) return;
    tstr_free(resp->etag);
    tstr_free(resp->location);
    minio_error_free(&resp->error);
    resp->etag = NULL;
    resp->location = NULL;
}

void minio_remove_objects_response_free(minio_remove_objects_response_t* resp) {
    if (!resp) return;
    for (int i = 0; i < resp->error_count; i++) {
        tstr_free(resp->errors[i].key);
        tstr_free(resp->errors[i].version_id);
        tstr_free(resp->errors[i].error_code);
        tstr_free(resp->errors[i].error_message);
    }
    free(resp->errors);
    minio_error_free(&resp->error);
    resp->errors = NULL;
    resp->error_count = 0;
}

void minio_presigned_post_free(minio_presigned_post_t* p) {
    if (!p) return;
    tstr_free(p->url);
    MinioHeaders_drop(&p->form_data);
    minio_error_free(&p->error);
    p->url = NULL;
}

void minio_select_request_free(minio_select_request_t* req) {
    if (!req) return;
    tstr_free(req->expression);
    tstr_free(req->input_format);
    tstr_free(req->output_format);
    tstr_free(req->csv_delimiter);
    tstr_free(req->csv_quote_char);
    tstr_free(req->csv_header_info);
}

// ── Bucket operations ──

minio_list_buckets_response_t minio_list_buckets(minio_client_t* client) {
    minio_list_buckets_response_t res = {0};
    res.buckets = MinioBucketVec_init();
    if (!client || !client->provider) {
        res.error = minio_error_make(-1, "Invalid client or provider");
        return res;
    }

    MinioHeaders qp = MinioHeaders_init();
    minio_http_response_t hres = minio_execute_signed(client, "GET", "/", NULL, &qp, NULL, 0);
    MinioHeaders_drop(&qp);

    res.error = check_response(&hres, 200);
    if (minio_is_ok(res.error)) {
        minio_list_buckets_parser_res_t pres = minio_parse_list_buckets_xml(hres.body);
        MinioBucketVec_drop(&res.buckets);
        res.buckets = pres.buckets;
        res.error = pres.error;
    }
    minio_http_response_free(&hres);
    return res;
}

void minio_list_buckets_free(minio_list_buckets_response_t* resp) {
    if (!resp) return;
    MinioBucketVec_drop(&resp->buckets);
    minio_error_free(&resp->error);
}

int minio_bucket_exists(minio_client_t* client, const char* bucket, minio_error_t* err) {
    if (!client || !bucket) return 0;

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    MinioHeaders qp = MinioHeaders_init();
    minio_http_response_t hres = minio_execute_signed(client, "HEAD", uri, NULL, &qp, NULL, 0);
    tstr_free(uri);
    MinioHeaders_drop(&qp);

    int exists = 0;
    if (hres.status_code == 200) {
        exists = 1;
        if (err) *err = MINIO_OK;
    } else if (hres.status_code == 404) {
        if (err) *err = MINIO_OK;
    } else {
        if (err) {
            if (hres.body && hres.body[0] != '\0') *err = minio_parse_error_xml(hres.body);
            else if (hres.status_code == 403) *err = minio_error_make(-1, "Access Denied");
            else *err = minio_error_make(-1, "Bucket check failed");
        }
    }
    minio_http_response_free(&hres);
    return exists;
}

minio_error_t minio_make_bucket(minio_client_t* client, const char* bucket, const char* region) {
    (void)region;
    if (!client || !bucket) return minio_error_make(-1, "Invalid params");

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    MinioHeaders qp = MinioHeaders_init();
    minio_http_response_t hres = minio_execute_signed(client, "PUT", uri, NULL, &qp, NULL, 0);
    tstr_free(uri);
    MinioHeaders_drop(&qp);

    minio_error_t err = check_response(&hres, 200);
    minio_http_response_free(&hres);
    return err;
}

minio_error_t minio_remove_bucket(minio_client_t* client, const char* bucket) {
    if (!client || !bucket) return minio_error_make(-1, "Invalid params");

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    MinioHeaders qp = MinioHeaders_init();
    minio_http_response_t hres = minio_execute_signed(client, "DELETE", uri, NULL, &qp, NULL, 0);
    tstr_free(uri);
    MinioHeaders_drop(&qp);

    minio_error_t err = check_response_2xx(&hres);
    minio_http_response_free(&hres);
    return err;
}

// ── Object CRUD ──

minio_stat_object_response_t minio_stat_object(minio_client_t* client, const char* bucket, const char* object) {
    minio_stat_object_response_t res = {0};
    if (!client || !bucket || !object) {
        res.error = minio_error_make(-1, "Invalid params");
        return res;
    }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    MinioHeaders qp = MinioHeaders_init();
    minio_http_response_t hres = minio_execute_signed(client, "HEAD", uri, NULL, &qp, NULL, 0);
    tstr_free(uri);
    MinioHeaders_drop(&qp);

    if (hres.status_code == 200) {
        const char* cl = minio_headers_get(&hres.headers, "Content-Length");
        res.size = cl ? (size_t)atoll(cl) : 0;
        const char* et = minio_headers_get(&hres.headers, "ETag");
        res.etag = tstr_dup(et ? et : "");
        const char* ct = minio_headers_get(&hres.headers, "Content-Type");
        res.content_type = tstr_dup(ct ? ct : "");
        res.error = MINIO_OK;
    } else if (hres.status_code == 404) {
        res.error = minio_error_make(-1, "Object not found");
    } else {
        if (hres.body && hres.body[0] != '\0') res.error = minio_parse_error_xml(hres.body);
        else res.error = minio_error_make(hres.status_code, "Stat failed");
    }
    minio_http_response_free(&hres);
    return res;
}

minio_error_t minio_put_object(minio_client_t* client,
                              const char* bucket, const char* object,
                              const char* data, size_t len,
                              const char* content_type) {
    if (!client || !bucket || !object) return minio_error_make(-1, "Invalid params");

    // Auto multipart for >5MB
    if (len > MINIO_PART_SIZE) {
        minio_create_multipart_response_t cr = minio_create_multipart_upload(client, bucket, object, content_type, NULL);
        if (!minio_is_ok(cr.error)) { minio_error_t e = cr.error; cr.error = MINIO_OK; minio_create_multipart_response_free(&cr); return e; }

        int part_count = (int)((len + MINIO_PART_SIZE - 1) / MINIO_PART_SIZE);
        const char** etags = calloc((size_t)part_count, sizeof(char*));

        minio_error_t err = MINIO_OK;
        for (int i = 0; i < part_count; i++) {
            size_t offset = (size_t)i * MINIO_PART_SIZE;
            size_t plen = (offset + MINIO_PART_SIZE > len) ? len - offset : MINIO_PART_SIZE;
            minio_upload_part_response_t pr = minio_upload_part(client, bucket, object, cr.upload_id, i + 1, data + offset, plen);
            if (!minio_is_ok(pr.error)) {
                err = pr.error;
                pr.error = MINIO_OK;
                minio_upload_part_response_free(&pr);
                // Abort on failure
                minio_abort_multipart_upload(client, bucket, object, cr.upload_id);
                for (int j = 0; j < i; j++) free((void*)etags[j]);
                free(etags);
                minio_create_multipart_response_free(&cr);
                return err;
            }
            etags[i] = pr.etag;
            pr.etag = NULL; // transfer ownership
            minio_upload_part_response_free(&pr);
        }

        minio_complete_multipart_response_t cmr = minio_complete_multipart_upload(client, bucket, object, cr.upload_id, etags, part_count);
        err = cmr.error;
        cmr.error = MINIO_OK;
        minio_complete_multipart_response_free(&cmr);

        for (int i = 0; i < part_count; i++) free((void*)etags[i]);
        free(etags);
        minio_create_multipart_response_free(&cr);
        return err;
    }

    // Simple PUT for small objects
    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    MinioHeaders hdrs = MinioHeaders_init();
    if (content_type) minio_headers_add(&hdrs, "Content-Type", content_type);
    MinioHeaders qp = MinioHeaders_init();

    minio_http_response_t hres = minio_execute_signed(client, "PUT", uri, &hdrs, &qp, data, len);
    tstr_free(uri);
    MinioHeaders_drop(&hdrs);
    MinioHeaders_drop(&qp);

    minio_error_t err = check_response(&hres, 200);
    minio_http_response_free(&hres);
    return err;
}

minio_error_t minio_get_object(minio_client_t* client,
                              const char* bucket, const char* object,
                              minio_data_callback_t callback, void* userdata) {
    if (!client || !bucket || !object || !callback) return minio_error_make(-1, "Invalid params");

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    MinioHeaders qp = MinioHeaders_init();
    minio_http_response_t hres = minio_execute_signed(client, "GET", uri, NULL, &qp, NULL, 0);
    tstr_free(uri);
    MinioHeaders_drop(&qp);

    minio_error_t err;
    if (hres.status_code == 200) {
        if (tstr_len(hres.body) > 0) callback(hres.body, tstr_len(hres.body), userdata);
        err = MINIO_OK;
    } else {
        err = check_response(&hres, 200);
    }
    minio_http_response_free(&hres);
    return err;
}

minio_error_t minio_remove_object(minio_client_t* client, const char* bucket, const char* object) {
    if (!client || !bucket || !object) return minio_error_make(-1, "Invalid params");

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    MinioHeaders qp = MinioHeaders_init();
    minio_http_response_t hres = minio_execute_signed(client, "DELETE", uri, NULL, &qp, NULL, 0);
    tstr_free(uri);
    MinioHeaders_drop(&qp);

    minio_error_t err = check_response_2xx(&hres);
    minio_http_response_free(&hres);
    return err;
}

minio_error_t minio_upload_object(minio_client_t* client,
                                 const char* bucket, const char* object,
                                 const char* filename) {
    if (!filename) return minio_error_make(-1, "Null filename");
    FILE* fp = fopen(filename, "rb");
    if (!fp) return minio_error_make(-1, "Failed to open file for reading");
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size < 0) { fclose(fp); return minio_error_make(-1, "Failed to get file size"); }
    char* buf = malloc(size > 0 ? (size_t)size : 1);
    if (!buf) { fclose(fp); return minio_error_make(-1, "Memory allocation failed"); }
    size_t read_bytes = size > 0 ? fread(buf, 1, (size_t)size, fp) : 0;
    fclose(fp);
    if (size > 0 && read_bytes != (size_t)size) { free(buf); return minio_error_make(-1, "Failed to read file"); }
    minio_error_t err = minio_put_object(client, bucket, object, buf, read_bytes, NULL);
    free(buf);
    return err;
}

static int file_write_callback(const char* data, size_t len, void* userdata) {
    FILE* fp = (FILE*)userdata;
    return (int)fwrite(data, 1, len, fp);
}

minio_error_t minio_download_object(minio_client_t* client,
                                   const char* bucket, const char* object,
                                   const char* filename) {
    if (!filename) return minio_error_make(-1, "Null filename");
    FILE* fp = fopen(filename, "wb");
    if (!fp) return minio_error_make(-1, "Failed to open file for writing");
    minio_error_t err = minio_get_object(client, bucket, object, file_write_callback, fp);
    fclose(fp);
    if (!minio_is_ok(err)) remove(filename);
    return err;
}

// ── List objects (iterator) ──

struct minio_list_objects_iter_s {
    minio_client_t* client;
    tstr_t bucket;
    tstr_t prefix;
    int recursive;
    int is_truncated;
    tstr_t continuation_token;
    MinioItemVec items;
    size_t current_idx;
    minio_error_t error;
};

minio_list_objects_iter_t* minio_list_objects(minio_client_t* client,
                                            const char* bucket,
                                            const char* prefix,
                                            int recursive) {
    if (!client || !bucket) return NULL;
    minio_list_objects_iter_t* iter = calloc(1, sizeof(minio_list_objects_iter_t));
    if (!iter) return NULL;
    iter->client = client;
    iter->bucket = tstr_dup(bucket);
    iter->prefix = prefix ? tstr_dup(prefix) : tstr_new();
    iter->recursive = recursive;
    iter->items = MinioItemVec_init();
    iter->is_truncated = 1;
    iter->error = MINIO_OK;
    return iter;
}

static void minio_list_objects_fetch_next_page(minio_list_objects_iter_t* iter) {
    if (!iter->is_truncated) return;

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", iter->bucket);
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "list-type", "2");
    if (tstr_len(iter->prefix) > 0) minio_headers_add(&qp, "prefix", iter->prefix);
    if (!iter->recursive) minio_headers_add(&qp, "delimiter", "/");
    if (tstr_len(iter->continuation_token) > 0) minio_headers_add(&qp, "continuation-token", iter->continuation_token);

    minio_http_response_t hres = minio_execute_signed(iter->client, "GET", uri, NULL, &qp, NULL, 0);
    tstr_free(uri);
    MinioHeaders_drop(&qp);

    if (minio_is_ok(hres.error) && hres.status_code == 200) {
        minio_list_objects_parser_res_t pres = minio_parse_list_objects_xml(hres.body);
        MinioItemVec_drop(&iter->items);
        iter->items = pres.items;
        iter->is_truncated = pres.is_truncated;
        tstr_free(iter->continuation_token);
        iter->continuation_token = pres.next_continuation_token;
        iter->current_idx = 0;
        iter->error = pres.error;
    } else {
        if (!minio_is_ok(hres.error)) {
            iter->error = hres.error;
            hres.error = MINIO_OK;
        } else {
            iter->error = minio_parse_error_xml(hres.body);
        }
        iter->is_truncated = 0;
    }
    minio_http_response_free(&hres);
}

int minio_list_objects_next(minio_list_objects_iter_t* iter, minio_item_t* item) {
    if (!iter) return 0;
    while (iter->current_idx >= MinioItemVec_size(&iter->items)) {
        if (!iter->is_truncated) return 0;
        minio_list_objects_fetch_next_page(iter);
        if (!minio_is_ok(iter->error)) return 0;
        if (MinioItemVec_size(&iter->items) == 0 && !iter->is_truncated) return 0;
    }
    *item = minio_item_clone(*MinioItemVec_at(&iter->items, iter->current_idx));
    iter->current_idx++;
    return 1;
}

minio_error_t minio_list_objects_error(minio_list_objects_iter_t* iter) {
    if (!iter) return minio_error_make(-1, "Null iterator");
    return minio_error_clone(iter->error);
}

void minio_list_objects_free(minio_list_objects_iter_t* iter) {
    if (!iter) return;
    tstr_free(iter->bucket);
    tstr_free(iter->prefix);
    tstr_free(iter->continuation_token);
    MinioItemVec_drop(&iter->items);
    minio_error_free(&iter->error);
    free(iter);
}

// ── Multipart Upload ──

minio_create_multipart_response_t minio_create_multipart_upload(
    minio_client_t* client, const char* bucket, const char* object,
    const char* content_type, const minio_sse_t* sse) {
    minio_create_multipart_response_t res = {0};
    if (!client || !bucket || !object) {
        res.error = minio_error_make(-1, "Invalid params");
        return res;
    }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    MinioHeaders hdrs = MinioHeaders_init();
    if (content_type) minio_headers_add(&hdrs, "Content-Type", content_type);
    if (sse) minio_sse_apply_headers(sse, &hdrs);
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "uploads", "");

    minio_http_response_t hres = minio_execute_signed(client, "POST", uri, &hdrs, &qp, NULL, 0);
    tstr_free(uri);
    MinioHeaders_drop(&hdrs);
    MinioHeaders_drop(&qp);

    res.error = check_response(&hres, 200);
    if (minio_is_ok(res.error)) {
        res.upload_id = minio_parse_create_multipart_xml(hres.body, &res.error);
    }
    minio_http_response_free(&hres);
    return res;
}

minio_upload_part_response_t minio_upload_part(
    minio_client_t* client, const char* bucket, const char* object,
    const char* upload_id, int part_number,
    const char* data, size_t len) {
    minio_upload_part_response_t res = {0};
    if (!client || !bucket || !object || !upload_id) {
        res.error = minio_error_make(-1, "Invalid params");
        return res;
    }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    MinioHeaders qp = MinioHeaders_init();
    char pn[16]; fmt(pn, sizeof(pn), "{}", part_number);
    minio_headers_add(&qp, "partNumber", pn);
    minio_headers_add(&qp, "uploadId", upload_id);

    minio_http_response_t hres = minio_execute_signed(client, "PUT", uri, NULL, &qp, data, len);
    tstr_free(uri);
    MinioHeaders_drop(&qp);

    res.error = check_response(&hres, 200);
    if (minio_is_ok(res.error)) {
        const char* etag = minio_headers_get(&hres.headers, "ETag");
        res.etag = tstr_dup(etag ? etag : "");
    }
    minio_http_response_free(&hres);
    return res;
}

minio_upload_part_response_t minio_upload_part_copy(
    minio_client_t* client, const char* bucket, const char* object,
    const char* upload_id, int part_number,
    const char* src_bucket, const char* src_object,
    size_t offset, size_t length) {
    minio_upload_part_response_t res = {0};
    if (!client || !bucket || !object || !upload_id || !src_bucket || !src_object) {
        res.error = minio_error_make(-1, "Invalid params");
        return res;
    }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    MinioHeaders hdrs = MinioHeaders_init();
    tstr_t copy_src = tstr_cat_fmt(tstr_new(), "/%s/%s", src_bucket, src_object);
    minio_headers_add(&hdrs, "x-amz-copy-source", copy_src);
    if (length > 0) {
        tstr_t range = tstr_cat_fmt(tstr_new(), "bytes=%zu-%zu", offset, offset + length - 1);
        minio_headers_add(&hdrs, "x-amz-copy-source-range", range);
        tstr_free(range);
    }
    tstr_free(copy_src);

    MinioHeaders qp = MinioHeaders_init();
    char pn[16]; fmt(pn, sizeof(pn), "{}", part_number);
    minio_headers_add(&qp, "partNumber", pn);
    minio_headers_add(&qp, "uploadId", upload_id);

    minio_http_response_t hres = minio_execute_signed(client, "PUT", uri, &hdrs, &qp, NULL, 0);
    tstr_free(uri);
    MinioHeaders_drop(&hdrs);
    MinioHeaders_drop(&qp);

    res.error = check_response(&hres, 200);
    if (minio_is_ok(res.error)) {
        res.etag = minio_parse_upload_part_copy_etag_xml(hres.body, &res.error);
    }
    minio_http_response_free(&hres);
    return res;
}

minio_complete_multipart_response_t minio_complete_multipart_upload(
    minio_client_t* client, const char* bucket, const char* object,
    const char* upload_id, const char** etags, int part_count) {
    minio_complete_multipart_response_t res = {0};
    if (!client || !bucket || !object || !upload_id || !etags) {
        res.error = minio_error_make(-1, "Invalid params");
        return res;
    }

    minio_xml_builder_t* xb = minio_xml_new();
    minio_xml_open(xb, "CompleteMultipartUpload");
    for (int i = 0; i < part_count; i++) {
        minio_xml_open(xb, "Part");
        minio_xml_elem_int(xb, "PartNumber", i + 1);
        minio_xml_elem(xb, "ETag", etags[i]);
        minio_xml_close(xb, "Part");
    }
    minio_xml_close(xb, "CompleteMultipartUpload");
    tstr_t body = minio_xml_finish(xb);

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "uploadId", upload_id);

    minio_http_response_t hres = minio_execute_signed(client, "POST", uri, NULL, &qp, body, tstr_len(body));
    tstr_free(uri);
    tstr_free(body);
    MinioHeaders_drop(&qp);

    res.error = check_response(&hres, 200);
    if (minio_is_ok(res.error)) {
        res.etag = minio_parse_complete_multipart_etag_xml(hres.body, &res.location, &res.error);
    }
    minio_http_response_free(&hres);
    return res;
}

minio_error_t minio_abort_multipart_upload(
    minio_client_t* client, const char* bucket, const char* object,
    const char* upload_id) {
    if (!client || !bucket || !object || !upload_id) return minio_error_make(-1, "Invalid params");

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "uploadId", upload_id);

    minio_http_response_t hres = minio_execute_signed(client, "DELETE", uri, NULL, &qp, NULL, 0);
    tstr_free(uri);
    MinioHeaders_drop(&qp);

    minio_error_t err = check_response_2xx(&hres);
    minio_http_response_free(&hres);
    return err;
}

// ── Copy / Compose ──

minio_copy_object_response_t minio_copy_object(
    minio_client_t* client,
    const char* src_bucket, const char* src_object,
    const char* dst_bucket, const char* dst_object) {
    minio_copy_object_response_t res = {0};
    if (!client || !src_bucket || !src_object || !dst_bucket || !dst_object) {
        res.error = minio_error_make(-1, "Invalid params");
        return res;
    }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", dst_bucket, dst_object);
    MinioHeaders hdrs = MinioHeaders_init();
    tstr_t copy_src = tstr_cat_fmt(tstr_new(), "/%s/%s", src_bucket, src_object);
    minio_headers_add(&hdrs, "x-amz-copy-source", copy_src);
    tstr_free(copy_src);
    MinioHeaders qp = MinioHeaders_init();

    minio_http_response_t hres = minio_execute_signed(client, "PUT", uri, &hdrs, &qp, NULL, 0);
    tstr_free(uri);
    MinioHeaders_drop(&hdrs);
    MinioHeaders_drop(&qp);

    res.error = check_response(&hres, 200);
    if (minio_is_ok(res.error)) {
        minio_copy_object_parser_res_t pres = minio_parse_copy_object_xml(hres.body);
        res.etag = pres.etag;
        res.error = pres.error;
        tstr_free(pres.last_modified_str);
    }
    minio_http_response_free(&hres);
    return res;
}

minio_copy_object_response_t minio_compose_object(
    minio_client_t* client,
    const char* bucket, const char* object,
    const minio_compose_source_t* sources, int source_count) {
    minio_copy_object_response_t res = {0};
    if (!client || !bucket || !object || !sources || source_count < 1) {
        res.error = minio_error_make(-1, "Invalid params");
        return res;
    }

    // Single source without range: simple copy
    if (source_count == 1 && sources[0].offset == 0 && sources[0].length == 0) {
        return minio_copy_object(client, sources[0].bucket, sources[0].object, bucket, object);
    }

    // Multiple sources: multipart copy
    minio_create_multipart_response_t cr = minio_create_multipart_upload(client, bucket, object, NULL, NULL);
    if (!minio_is_ok(cr.error)) {
        res.error = cr.error;
        cr.error = MINIO_OK;
        minio_create_multipart_response_free(&cr);
        return res;
    }

    const char** etags = calloc((size_t)source_count, sizeof(char*));
    for (int i = 0; i < source_count; i++) {
        minio_upload_part_response_t pr = minio_upload_part_copy(
            client, bucket, object, cr.upload_id, i + 1,
            sources[i].bucket, sources[i].object,
            sources[i].offset, sources[i].length);
        if (!minio_is_ok(pr.error)) {
            res.error = pr.error;
            pr.error = MINIO_OK;
            minio_upload_part_response_free(&pr);
            minio_abort_multipart_upload(client, bucket, object, cr.upload_id);
            for (int j = 0; j < i; j++) free((void*)etags[j]);
            free(etags);
            minio_create_multipart_response_free(&cr);
            return res;
        }
        etags[i] = pr.etag;
        pr.etag = NULL;
        minio_upload_part_response_free(&pr);
    }

    minio_complete_multipart_response_t cmr = minio_complete_multipart_upload(
        client, bucket, object, cr.upload_id, etags, source_count);
    res.etag = cmr.etag;
    cmr.etag = NULL;
    res.error = cmr.error;
    cmr.error = MINIO_OK;
    minio_complete_multipart_response_free(&cmr);

    for (int i = 0; i < source_count; i++) free((void*)etags[i]);
    free(etags);
    minio_create_multipart_response_free(&cr);
    return res;
}

// ── Batch Delete ──

minio_remove_objects_response_t minio_remove_objects(
    minio_client_t* client, const char* bucket,
    const char** keys, int key_count) {
    minio_remove_objects_response_t res = {0};
    if (!client || !bucket || !keys || key_count < 1) {
        res.error = minio_error_make(-1, "Invalid params");
        return res;
    }

    minio_xml_builder_t* xb = minio_xml_new();
    minio_xml_open(xb, "Delete");
    minio_xml_elem(xb, "Quiet", "false");
    for (int i = 0; i < key_count; i++) {
        minio_xml_open(xb, "Object");
        minio_xml_elem(xb, "Key", keys[i]);
        minio_xml_close(xb, "Object");
    }
    minio_xml_close(xb, "Delete");
    tstr_t body = minio_xml_finish(xb);

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    MinioHeaders hdrs = MinioHeaders_init();
    minio_headers_add(&hdrs, "Content-Type", "application/xml");
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "delete", "");

    // S3 requires Content-MD5 for delete
    tstr_t content_md5 = minio_signer_sha256_hex(body, tstr_len(body));
    minio_headers_add(&hdrs, "x-amz-content-sha256", content_md5);
    tstr_free(content_md5);

    minio_http_response_t hres = minio_execute_signed(client, "POST", uri, &hdrs, &qp, body, tstr_len(body));
    tstr_free(uri);
    tstr_free(body);
    MinioHeaders_drop(&hdrs);
    MinioHeaders_drop(&qp);

    res.error = check_response(&hres, 200);
    if (minio_is_ok(res.error)) {
        minio_delete_objects_parser_res_t pres = minio_parse_delete_objects_xml(hres.body);
        res.errors = pres.errors;
        res.error_count = pres.error_count;
        res.error = pres.error;
    }
    minio_http_response_free(&hres);
    return res;
}

// ── Presigned URLs ──

tstr_t minio_get_presigned_object_url(
    minio_client_t* client, const char* method,
    const char* bucket, const char* object,
    int expires_secs) {
    if (!client || !method || !bucket || !object) return tstr_new();

    minio_credentials_t creds = {0};
    minio_error_t err = client->provider->fetch(client->provider->ctx, &creds);
    if (!minio_is_ok(err)) { minio_error_free(&err); return tstr_new(); }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    tstr_t url = minio_build_url(&client->base_url, uri);

    MinioHeaders qp = MinioHeaders_init();
    tstr_t result = minio_signer_presign_v4(method, url, client->base_url.region,
                                             &qp, creds.access_key, creds.secret_key,
                                             creds.session_token, minio_time_now(), expires_secs);

    minio_credentials_clear(&creds);
    tstr_free(uri);
    tstr_free(url);
    MinioHeaders_drop(&qp);
    return result;
}

minio_presigned_post_t minio_get_presigned_post_form_data(
    minio_client_t* client, const char* bucket, const char* object,
    int expires_secs) {
    minio_presigned_post_t res = {0};
    res.form_data = MinioHeaders_init();
    if (!client || !bucket || !object) {
        res.error = minio_error_make(-1, "Invalid params");
        return res;
    }

    minio_credentials_t creds = {0};
    res.error = client->provider->fetch(client->provider->ctx, &creds);
    if (!minio_is_ok(res.error)) return res;

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    res.url = minio_build_url(&client->base_url, uri);
    tstr_free(uri);

    // Build a simple policy (base64 encoded JSON)
    // For now, a minimal policy placeholder
    tstr_t policy = tstr_cat_fmt(tstr_new(),
        "{\"expiration\":\"%s\",\"conditions\":[[\"eq\",\"$bucket\",\"%s\"],[\"eq\",\"$key\",\"%s\"]]}",
        "2099-12-31T23:59:59Z", bucket, object);
    (void)expires_secs; // TODO: compute proper expiration from expires_secs

    // Base64 encode the policy
    tstr_t policy_b64 = tstr_new();
    {
        BIO* b64 = BIO_new(BIO_f_base64());
        BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
        BIO* mem = BIO_new(BIO_s_mem());
        b64 = BIO_push(b64, mem);
        BIO_write(b64, policy, (int)tstr_len(policy));
        BIO_flush(b64);
        BUF_MEM* bptr;
        BIO_get_mem_ptr(b64, &bptr);
        policy_b64 = tstr_dup_len(bptr->data, bptr->length);
        BIO_free_all(b64);
    }

    res.error = minio_signer_post_presign_v4(client->base_url.region,
                                              creds.access_key, creds.secret_key,
                                              creds.session_token, minio_time_now(),
                                              policy_b64, &res.form_data);

    minio_credentials_clear(&creds);
    tstr_free(policy);
    tstr_free(policy_b64);
    return res;
}

// ── Object Tags ──

minio_tag_set_t minio_get_object_tags(minio_client_t* client, const char* bucket, const char* object, minio_error_t* err) {
    minio_tag_set_t res = {0};
    if (!client || !bucket || !object) { if (err) *err = minio_error_make(-1, "Invalid params"); return res; }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "tagging", "");

    minio_http_response_t hres = minio_execute_signed(client, "GET", uri, NULL, &qp, NULL, 0);
    tstr_free(uri);
    MinioHeaders_drop(&qp);

    minio_error_t e = check_response(&hres, 200);
    if (minio_is_ok(e)) {
        res = minio_parse_tagging_xml(hres.body, &e);
    }
    if (err) *err = e; else minio_error_free(&e);
    minio_http_response_free(&hres);
    return res;
}

minio_error_t minio_set_object_tags(minio_client_t* client, const char* bucket, const char* object, const minio_tag_set_t* tags) {
    if (!client || !bucket || !object || !tags) return minio_error_make(-1, "Invalid params");

    minio_xml_builder_t* xb = minio_xml_new();
    minio_xml_open_ns(xb, "Tagging", "http://s3.amazonaws.com/doc/2006-03-01/");
    minio_xml_open(xb, "TagSet");
    for (int i = 0; i < tags->count; i++) {
        minio_xml_open(xb, "Tag");
        minio_xml_elem(xb, "Key", tags->tags[i].key);
        minio_xml_elem(xb, "Value", tags->tags[i].value);
        minio_xml_close(xb, "Tag");
    }
    minio_xml_close(xb, "TagSet");
    minio_xml_close(xb, "Tagging");
    tstr_t body = minio_xml_finish(xb);

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    MinioHeaders hdrs = MinioHeaders_init();
    minio_headers_add(&hdrs, "Content-Type", "application/xml");
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "tagging", "");

    minio_http_response_t hres = minio_execute_signed(client, "PUT", uri, &hdrs, &qp, body, tstr_len(body));
    tstr_free(uri);
    tstr_free(body);
    MinioHeaders_drop(&hdrs);
    MinioHeaders_drop(&qp);

    minio_error_t err = check_response(&hres, 200);
    minio_http_response_free(&hres);
    return err;
}

minio_error_t minio_delete_object_tags(minio_client_t* client, const char* bucket, const char* object) {
    if (!client || !bucket || !object) return minio_error_make(-1, "Invalid params");

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "tagging", "");

    minio_http_response_t hres = minio_execute_signed(client, "DELETE", uri, NULL, &qp, NULL, 0);
    tstr_free(uri);
    MinioHeaders_drop(&qp);

    minio_error_t err = check_response_2xx(&hres);
    minio_http_response_free(&hres);
    return err;
}

// ── Object Retention ──

minio_object_retention_t minio_get_object_retention(minio_client_t* client, const char* bucket, const char* object, minio_error_t* err) {
    minio_object_retention_t res = {0};
    if (!client || !bucket || !object) { if (err) *err = minio_error_make(-1, "Invalid params"); return res; }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "retention", "");

    minio_http_response_t hres = minio_execute_signed(client, "GET", uri, NULL, &qp, NULL, 0);
    tstr_free(uri);
    MinioHeaders_drop(&qp);

    minio_error_t e = check_response(&hres, 200);
    if (minio_is_ok(e)) {
        res = minio_parse_object_retention_xml(hres.body, &e);
    }
    if (err) *err = e; else minio_error_free(&e);
    minio_http_response_free(&hres);
    return res;
}

minio_error_t minio_set_object_retention(minio_client_t* client, const char* bucket, const char* object, const minio_object_retention_t* retention) {
    if (!client || !bucket || !object || !retention) return minio_error_make(-1, "Invalid params");

    minio_xml_builder_t* xb = minio_xml_new();
    minio_xml_open_ns(xb, "Retention", "http://s3.amazonaws.com/doc/2006-03-01/");
    minio_xml_elem(xb, "Mode", retention->mode == MINIO_RETENTION_COMPLIANCE ? "COMPLIANCE" : "GOVERNANCE");
    tstr_t date_str = minio_time_to_iso8601(retention->retain_until_date);
    minio_xml_elem(xb, "RetainUntilDate", date_str);
    tstr_free(date_str);
    minio_xml_close(xb, "Retention");
    tstr_t body = minio_xml_finish(xb);

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    MinioHeaders hdrs = MinioHeaders_init();
    minio_headers_add(&hdrs, "Content-Type", "application/xml");
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "retention", "");

    minio_http_response_t hres = minio_execute_signed(client, "PUT", uri, &hdrs, &qp, body, tstr_len(body));
    tstr_free(uri);
    tstr_free(body);
    MinioHeaders_drop(&hdrs);
    MinioHeaders_drop(&qp);

    minio_error_t err = check_response(&hres, 200);
    minio_http_response_free(&hres);
    return err;
}

// ── Legal Hold ──

int minio_is_object_legal_hold_enabled(minio_client_t* client, const char* bucket, const char* object, minio_error_t* err) {
    if (!client || !bucket || !object) { if (err) *err = minio_error_make(-1, "Invalid params"); return 0; }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "legal-hold", "");

    minio_http_response_t hres = minio_execute_signed(client, "GET", uri, NULL, &qp, NULL, 0);
    tstr_free(uri);
    MinioHeaders_drop(&qp);

    int enabled = 0;
    minio_error_t e = check_response(&hres, 200);
    if (minio_is_ok(e)) {
        enabled = minio_parse_legal_hold_xml(hres.body, &e);
    }
    if (err) *err = e; else minio_error_free(&e);
    minio_http_response_free(&hres);
    return enabled;
}

static minio_error_t set_legal_hold(minio_client_t* client, const char* bucket, const char* object, const char* status) {
    minio_xml_builder_t* xb = minio_xml_new();
    minio_xml_open_ns(xb, "LegalHold", "http://s3.amazonaws.com/doc/2006-03-01/");
    minio_xml_elem(xb, "Status", status);
    minio_xml_close(xb, "LegalHold");
    tstr_t body = minio_xml_finish(xb);

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    MinioHeaders hdrs = MinioHeaders_init();
    minio_headers_add(&hdrs, "Content-Type", "application/xml");
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "legal-hold", "");

    minio_http_response_t hres = minio_execute_signed(client, "PUT", uri, &hdrs, &qp, body, tstr_len(body));
    tstr_free(uri);
    tstr_free(body);
    MinioHeaders_drop(&hdrs);
    MinioHeaders_drop(&qp);

    minio_error_t err = check_response(&hres, 200);
    minio_http_response_free(&hres);
    return err;
}

minio_error_t minio_enable_object_legal_hold(minio_client_t* client, const char* bucket, const char* object) {
    if (!client || !bucket || !object) return minio_error_make(-1, "Invalid params");
    return set_legal_hold(client, bucket, object, "ON");
}

minio_error_t minio_disable_object_legal_hold(minio_client_t* client, const char* bucket, const char* object) {
    if (!client || !bucket || !object) return minio_error_make(-1, "Invalid params");
    return set_legal_hold(client, bucket, object, "OFF");
}

// ── SelectObjectContent ──

minio_error_t minio_select_object_content(
    minio_client_t* client, const char* bucket, const char* object,
    const minio_select_request_t* request,
    minio_data_callback_t callback, void* userdata) {
    if (!client || !bucket || !object || !request || !callback)
        return minio_error_make(-1, "Invalid params");

    // Build SelectObjectContentRequest XML
    minio_xml_builder_t* xb = minio_xml_new();
    minio_xml_open_ns(xb, "SelectObjectContentRequest", "http://s3.amazonaws.com/doc/2006-03-01/");
    minio_xml_elem(xb, "Expression", request->expression);
    minio_xml_elem(xb, "ExpressionType", "SQL");

    // InputSerialization
    minio_xml_open(xb, "InputSerialization");
    if (request->input_format && strcmp(request->input_format, "CSV") == 0) {
        minio_xml_open(xb, "CSV");
        if (request->csv_delimiter) minio_xml_elem(xb, "FieldDelimiter", request->csv_delimiter);
        if (request->csv_quote_char) minio_xml_elem(xb, "QuoteCharacter", request->csv_quote_char);
        if (request->csv_header_info) minio_xml_elem(xb, "FileHeaderInfo", request->csv_header_info);
        minio_xml_close(xb, "CSV");
    } else if (request->input_format && strcmp(request->input_format, "JSON") == 0) {
        minio_xml_open(xb, "JSON");
        minio_xml_elem(xb, "Type", "DOCUMENT");
        minio_xml_close(xb, "JSON");
    } else if (request->input_format && strcmp(request->input_format, "Parquet") == 0) {
        minio_xml_open(xb, "Parquet");
        minio_xml_close(xb, "Parquet");
    }
    minio_xml_close(xb, "InputSerialization");

    // OutputSerialization
    minio_xml_open(xb, "OutputSerialization");
    if (request->output_format && strcmp(request->output_format, "JSON") == 0) {
        minio_xml_open(xb, "JSON");
        minio_xml_close(xb, "JSON");
    } else {
        minio_xml_open(xb, "CSV");
        minio_xml_close(xb, "CSV");
    }
    minio_xml_close(xb, "OutputSerialization");

    minio_xml_close(xb, "SelectObjectContentRequest");
    tstr_t body = minio_xml_finish(xb);

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    MinioHeaders hdrs = MinioHeaders_init();
    minio_headers_add(&hdrs, "Content-Type", "application/xml");
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "select", "");
    minio_headers_add(&qp, "select-type", "2");

    minio_http_response_t hres = minio_execute_signed(client, "POST", uri, &hdrs, &qp, body, tstr_len(body));
    tstr_free(uri);
    tstr_free(body);
    MinioHeaders_drop(&hdrs);
    MinioHeaders_drop(&qp);

    minio_error_t err = check_response(&hres, 200);
    if (minio_is_ok(err) && tstr_len(hres.body) > 0) {
        // SelectObjectContent returns binary frame format.
        // For simplicity, we parse the frames and extract data payloads.
        // Frame format: 4-byte total length, 4-byte headers length, 4-byte prelude CRC,
        //               headers, payload, 4-byte message CRC
        const char* p = hres.body;
        size_t remaining = tstr_len(hres.body);
        while (remaining >= 12) {
            uint32_t total_len = ((uint8_t)p[0] << 24) | ((uint8_t)p[1] << 16) | ((uint8_t)p[2] << 8) | (uint8_t)p[3];
            uint32_t headers_len = ((uint8_t)p[4] << 24) | ((uint8_t)p[5] << 16) | ((uint8_t)p[6] << 8) | (uint8_t)p[7];
            if (total_len > remaining || total_len < 16) break;
            // payload starts after prelude (12 bytes) + headers
            size_t payload_offset = 12 + headers_len;
            size_t payload_len = total_len - payload_offset - 4; // minus message CRC
            if (payload_offset + payload_len + 4 <= total_len && payload_len > 0) {
                callback(p + payload_offset, payload_len, userdata);
            }
            p += total_len;
            remaining -= total_len;
        }
    }
    minio_http_response_free(&hres);
    return err;
}
