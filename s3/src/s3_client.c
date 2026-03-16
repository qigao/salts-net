#include "s3/s3_client.h"
#include "s3/s3_signer.h"
#include "s3/s3_response.h"
#include "s3/s3_xml_builder.h"
#include "s3_http.h"
#include "s3_client_internal.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <turbo_str.h>
#include <turbo_fs.h>
#include <fmt.h>
#include <turbo_coro.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>

#define S3_PART_SIZE (5 * 1024 * 1024) // 5MB
#define S3_PARALLEL_PARTS 10

static tstr_t s3_md5_hex(const char* data, size_t len);

// ── Client lifecycle ──

s3_client_t* s3_client_create(coro_context_t *ctx,
                                   const s3_base_url_t* base_url,
                                   s3_credential_provider_t* provider) {
    s3_client_t* client = calloc(1, sizeof(s3_client_t));
    if (!client) return NULL;
    client->coro_ctx = ctx;
    client->base_url.host = tstr_dup(base_url->host);
    client->base_url.port = base_url->port;
    client->base_url.is_https = base_url->is_https;
    client->base_url.region = tstr_dup(base_url->region);
    client->base_url.virtual_style = base_url->virtual_style;
    client->provider = provider;
    return client;
}

void s3_client_destroy(s3_client_t* client) {
    if (!client) return;
    tstr_free(client->base_url.host);
    tstr_free(client->base_url.region);
    free(client);
}

void s3_client_set_part_size(s3_client_t* client, size_t part_size) {
    if (client) client->part_size = part_size;
}

void s3_base_url_free(s3_base_url_t* url) {
    if (!url) return;
    tstr_free(url->host);
    tstr_free(url->region);
    url->host = NULL;
    url->region = NULL;
}

// ── Helper: check response and extract error ──

static s3_error_t check_response(s3_http_response_t* hres, int expected_status) {
    if (!s3_is_ok(hres->error)) {
        s3_error_t e = hres->error;
        hres->error = S3_OK;
        return e;
    }
    if (hres->status_code == expected_status) return S3_OK;
    if (hres->body && hres->body[0] != '\0') return s3_parse_error_xml(hres->body);
    return s3_error_make(hres->status_code, "Unexpected status code");
}

static s3_error_t check_response_2xx(s3_http_response_t* hres) {
    if (!s3_is_ok(hres->error)) {
        s3_error_t e = hres->error;
        hres->error = S3_OK;
        return e;
    }
    if (hres->status_code >= 200 && hres->status_code < 300) return S3_OK;
    if (hres->body && hres->body[0] != '\0') return s3_parse_error_xml(hres->body);
    return s3_error_make(hres->status_code, "Unexpected status code");
}

// ── Free helpers ──

void s3_stat_object_response_free(s3_stat_object_response_t* resp) {
    if (!resp) return;
    tstr_free(resp->etag);
    tstr_free(resp->content_type);
    s3_error_free(&resp->error);
    resp->etag = NULL;
    resp->content_type = NULL;
}

void s3_create_multipart_response_free(s3_create_multipart_response_t* resp) {
    if (!resp) return;
    tstr_free(resp->upload_id);
    s3_error_free(&resp->error);
    resp->upload_id = NULL;
}

void s3_upload_part_response_free(s3_upload_part_response_t* resp) {
    if (!resp) return;
    tstr_free(resp->etag);
    s3_error_free(&resp->error);
    resp->etag = NULL;
}

void s3_complete_multipart_response_free(s3_complete_multipart_response_t* resp) {
    if (!resp) return;
    tstr_free(resp->etag);
    tstr_free(resp->location);
    s3_error_free(&resp->error);
    resp->etag = NULL;
    resp->location = NULL;
}

void s3_remove_objects_response_free(s3_remove_objects_response_t* resp) {
    if (!resp) return;
    for (int i = 0; i < resp->error_count; i++) {
        tstr_free(resp->errors[i].key);
        tstr_free(resp->errors[i].version_id);
        tstr_free(resp->errors[i].error_code);
        tstr_free(resp->errors[i].error_message);
    }
    free(resp->errors);
    s3_error_free(&resp->error);
    resp->errors = NULL;
    resp->error_count = 0;
}

void s3_presigned_post_free(s3_presigned_post_t* p) {
    if (!p) return;
    tstr_free(p->url);
    S3Headers_drop(&p->form_data);
    s3_error_free(&p->error);
    p->url = NULL;
}

void s3_select_request_free(s3_select_request_t* req) {
    if (!req) return;
    tstr_free(req->expression);
    tstr_free(req->input_format);
    tstr_free(req->output_format);
    tstr_free(req->csv_delimiter);
    tstr_free(req->csv_quote_char);
    tstr_free(req->csv_header_info);
}

// ── Bucket operations ──

s3_list_buckets_response_t s3_list_buckets(s3_client_t* client) {
    s3_list_buckets_response_t res = {0};
    res.buckets = S3BucketVec_init();
    if (!client || !client->provider) {
        res.error = s3_error_make(-1, "Invalid client or provider");
        return res;
    }

    S3Headers qp = S3Headers_init();
    s3_http_response_t hres = s3_execute_signed(client, "GET", "/", NULL, &qp, NULL, 0);
    S3Headers_drop(&qp);

    res.error = check_response(&hres, 200);
    if (s3_is_ok(res.error)) {
        s3_list_buckets_parser_res_t pres = s3_parse_list_buckets_xml(hres.body);
        S3BucketVec_drop(&res.buckets);
        res.buckets = pres.buckets;
        res.error = pres.error;
    }
    s3_http_response_free(&hres);
    return res;
}

void s3_list_buckets_free(s3_list_buckets_response_t* resp) {
    if (!resp) return;
    S3BucketVec_drop(&resp->buckets);
    s3_error_free(&resp->error);
}

int s3_bucket_exists(s3_client_t* client, const char* bucket, s3_error_t* err) {
    if (!client || !bucket) return 0;

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    S3Headers qp = S3Headers_init();
    s3_http_response_t hres = s3_execute_signed(client, "HEAD", uri, NULL, &qp, NULL, 0);
    tstr_free(uri);
    S3Headers_drop(&qp);

    int exists = 0;
    if (!s3_is_ok(hres.error)) {
        if (err) { *err = hres.error; hres.error = S3_OK; }
    } else if (hres.status_code == 200) {
        exists = 1;
        if (err) *err = S3_OK;
    } else if (hres.status_code == 404) {
        if (err) *err = S3_OK;
    } else {
        if (err) {
            if (hres.body && hres.body[0] != '\0') *err = s3_parse_error_xml(hres.body);
            else if (hres.status_code == 403) *err = s3_error_make(-1, "Access Denied");
            else *err = s3_error_make(-1, "Bucket check failed");
        }
    }
    s3_http_response_free(&hres);
    return exists;
}

s3_error_t s3_make_bucket(s3_client_t* client, const char* bucket, const char* region) {
    (void)region;
    if (!client || !bucket) return s3_error_make(-1, "Invalid params");

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    S3Headers qp = S3Headers_init();
    s3_http_response_t hres = s3_execute_signed(client, "PUT", uri, NULL, &qp, NULL, 0);
    tstr_free(uri);
    S3Headers_drop(&qp);

    s3_error_t err = check_response(&hres, 200);
    s3_http_response_free(&hres);
    return err;
}

s3_error_t s3_remove_bucket(s3_client_t* client, const char* bucket) {
    if (!client || !bucket) return s3_error_make(-1, "Invalid params");

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    S3Headers qp = S3Headers_init();
    s3_http_response_t hres = s3_execute_signed(client, "DELETE", uri, NULL, &qp, NULL, 0);
    tstr_free(uri);
    S3Headers_drop(&qp);

    s3_error_t err = check_response_2xx(&hres);
    s3_http_response_free(&hres);
    return err;
}

// ── Object CRUD ──

s3_stat_object_response_t s3_stat_object(s3_client_t* client, const char* bucket, const char* object) {
    s3_stat_object_response_t res = {0};
    if (!client || !bucket || !object) {
        res.error = s3_error_make(-1, "Invalid params");
        return res;
    }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    S3Headers qp = S3Headers_init();
    s3_http_response_t hres = s3_execute_signed(client, "HEAD", uri, NULL, &qp, NULL, 0);
    tstr_free(uri);
    S3Headers_drop(&qp);

    if (!s3_is_ok(hres.error)) {
        res.error = hres.error; hres.error = S3_OK;
    } else if (hres.status_code == 200) {
        const char* cl = s3_headers_get(&hres.headers, "Content-Length");
        res.size = cl ? (size_t)atoll(cl) : 0;
        const char* et = s3_headers_get(&hres.headers, "ETag");
        res.etag = tstr_dup(et ? et : "");
        const char* ct = s3_headers_get(&hres.headers, "Content-Type");
        res.content_type = tstr_dup(ct ? ct : "");
        res.error = S3_OK;
    } else if (hres.status_code == 404) {
        res.error = s3_error_make(-1, "Object not found");
    } else {
        if (hres.body && hres.body[0] != '\0') res.error = s3_parse_error_xml(hres.body);
        else res.error = s3_error_make(hres.status_code, "Stat failed");
    }
    s3_http_response_free(&hres);
    return res;
}

// ── Parallel multipart upload (coroutine fan-out) ──

typedef struct {
    s3_client_t*    client;
    const char*     bucket;
    const char*     object;
    const char*     upload_id;
    int             part_number;
    const char*     data;       // in-memory mode
    turbo_file_t    fd;         // file mode (TURBO_INVALID_FILE if unused)
    int64_t         file_offset;
    size_t          len;
    char*           etag;
    s3_error_t      error;
    int*            abort_flag;
} s3_part_coro_task_t;

static void s3_upload_part_coro(coro_t* co, void* arg) {
    (void)co;
    s3_part_coro_task_t* t = (s3_part_coro_task_t*)arg;
    if (*t->abort_flag) return;

    const char* buf = t->data;
    char* alloc_buf = NULL;

    if (t->fd != TURBO_INVALID_FILE) {
        alloc_buf = malloc(t->len);
        if (!alloc_buf) {
            t->error = s3_error_make(-1, "Memory allocation failed");
            *t->abort_flag = 1;
            return;
        }
        int nread = turbo_fs_pread(t->fd, alloc_buf, t->len, t->file_offset);
        if (nread < 0 || (size_t)nread != t->len) {
            free(alloc_buf);
            t->error = s3_error_make(-1, "Failed to read file chunk");
            *t->abort_flag = 1;
            return;
        }
        buf = alloc_buf;
    }

    {
        s3_upload_part_response_t pr = s3_upload_part(
            t->client, t->bucket, t->object,
            t->upload_id, t->part_number, buf, t->len);

        free(alloc_buf);

        if (!s3_is_ok(pr.error)) {
            t->error = pr.error; pr.error = S3_OK;
            *t->abort_flag = 1;
        } else {
            t->etag = pr.etag; pr.etag = NULL;
        }
        s3_upload_part_response_free(&pr);
    }
}

// Shared multipart logic: dispatches parts from either memory or fd
static s3_error_t s3_multipart_upload_parts(s3_client_t* client,
                                            const char* bucket, const char* object,
                                            const char* content_type,
                                            const char* data, turbo_file_t fd,
                                            size_t total_len) {
    size_t part_size = client->part_size > 0 ? client->part_size : S3_PART_SIZE;
    s3_create_multipart_response_t cr = s3_create_multipart_upload(client, bucket, object, content_type, NULL);
    if (!s3_is_ok(cr.error)) {
        s3_error_t e = cr.error; cr.error = S3_OK;
        s3_create_multipart_response_free(&cr);
        return e;
    }

    // Pre-fetch credentials, create a static provider + temp client
    s3_credentials_t creds = {0};
    s3_error_t fetch_err = client->provider->fetch(client->provider->ctx, &creds);
    if (!s3_is_ok(fetch_err)) {
        s3_abort_multipart_upload(client, bucket, object, cr.upload_id);
        s3_create_multipart_response_free(&cr);
        return fetch_err;
    }

    s3_credential_provider_t* tmp_provider = s3_creds_static(
        creds.access_key, creds.secret_key, creds.session_token);
    s3_credentials_clear(&creds);

    s3_client_t* tmp_client = s3_client_create(client->coro_ctx, &client->base_url, tmp_provider);
    tmp_client->part_size = client->part_size;

    int part_count = (int)((total_len + part_size - 1) / part_size);
    int abort_flag = 0;
    s3_part_coro_task_t* tasks = calloc((size_t)part_count, sizeof(s3_part_coro_task_t));
    coro_task_t** ctasks = calloc((size_t)part_count, sizeof(coro_task_t*));

    for (int i = 0; i < part_count; i++) {
        size_t offset = (size_t)i * part_size;
        size_t chunk = (offset + part_size > total_len) ? total_len - offset : part_size;
        tasks[i] = (s3_part_coro_task_t){
            .client      = tmp_client,
            .bucket      = bucket,
            .object      = object,
            .upload_id   = cr.upload_id,
            .part_number = i + 1,
            .data        = data ? data + offset : NULL,
            .fd          = fd,
            .file_offset = (int64_t)offset,
            .len         = chunk,
            .abort_flag  = &abort_flag,
        };
        ctasks[i] = coro_task_create(client->coro_ctx, s3_upload_part_coro, &tasks[i]);
    }

    // Start tasks in parallel
    for (int i = 0; i < part_count; i++) {
        coro_task_start(ctasks[i]);
    }

    // Yield until all part tasks complete
    if (part_count > 0) {
        coro_when_all(client->coro_ctx, ctasks, part_count);
    }

    // Scan for first error
    s3_error_t err = S3_OK;
    for (int i = 0; i < part_count; i++) {
        if (!s3_is_ok(tasks[i].error)) {
            err = tasks[i].error; tasks[i].error = S3_OK;
            for (int j = i + 1; j < part_count; j++)
                s3_error_free(&tasks[j].error);
            break;
        }
    }

    if (s3_is_ok(err)) {
        for (int i = 0; i < part_count; i++) {
            if (tasks[i].etag && tasks[i].etag[0] != '\0') continue;
            if (tasks[i].data && tasks[i].len > 0) {
                tstr_t h = s3_md5_hex(tasks[i].data, tasks[i].len);
                tasks[i].etag = tstr_cat_fmt(tstr_new(), "\"%s\"", h);
                tstr_free(h);
            }
            if (!tasks[i].etag || tasks[i].etag[0] == '\0') {
                err = s3_error_make(-1, "Missing ETag for multipart part");
                break;
            }
        }
    }

    if (!s3_is_ok(err)) {
        s3_abort_multipart_upload(client, bucket, object, cr.upload_id);
    } else {
        const char** etags = calloc((size_t)part_count, sizeof(char*));
        for (int i = 0; i < part_count; i++)
            etags[i] = tasks[i].etag;

        s3_complete_multipart_response_t cmr = s3_complete_multipart_upload(
            client, bucket, object, cr.upload_id, etags, part_count);
        err = cmr.error; cmr.error = S3_OK;
        s3_complete_multipart_response_free(&cmr);
        free(etags);
    }

    for (int i = 0; i < part_count; i++) {
        tstr_free(tasks[i].etag);
        coro_task_destroy(ctasks[i]);
    }
    free(ctasks);
    free(tasks);
    s3_client_destroy(tmp_client);
    s3_credential_provider_destroy(tmp_provider);
    s3_create_multipart_response_free(&cr);
    return err;
}

static s3_error_t s3_put_object_multipart(s3_client_t* client,
                                          const char* bucket, const char* object,
                                          const char* data, size_t len,
                                          const char* content_type) {
    return s3_multipart_upload_parts(client, bucket, object, content_type,
                                    data, TURBO_INVALID_FILE, len);
}

static s3_error_t s3_put_object_multipart_fd(s3_client_t* client,
                                             const char* bucket, const char* object,
                                             turbo_file_t fd, size_t file_size,
                                             const char* content_type) {
    return s3_multipart_upload_parts(client, bucket, object, content_type,
                                    NULL, fd, file_size);
}

s3_error_t s3_put_object(s3_client_t* client,
                              const char* bucket, const char* object,
                              const char* data, size_t len,
                              const char* content_type) {
    if (!client || !bucket || !object) return s3_error_make(-1, "Invalid params");

    size_t part_size = client->part_size > 0 ? client->part_size : S3_PART_SIZE;
    if (len > part_size)
        return s3_put_object_multipart(client, bucket, object, data, len, content_type);

    // Simple PUT for small objects
    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    S3Headers hdrs = S3Headers_init();
    if (content_type) s3_headers_add(&hdrs, "Content-Type", content_type);
    S3Headers qp = S3Headers_init();

    s3_http_response_t hres = s3_execute_signed(client, "PUT", uri, &hdrs, &qp, data, len);
    tstr_free(uri);
    S3Headers_drop(&hdrs);
    S3Headers_drop(&qp);

    s3_error_t err = check_response(&hres, 200);
    s3_http_response_free(&hres);
    return err;
}

s3_error_t s3_get_object(s3_client_t* client,
                              const char* bucket, const char* object,
                              s3_data_callback_t callback, void* userdata) {
    if (!client || !bucket || !object || !callback) return s3_error_make(-1, "Invalid params");

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    S3Headers qp = S3Headers_init();
    s3_http_response_t hres = s3_execute_signed(client, "GET", uri, NULL, &qp, NULL, 0);
    tstr_free(uri);
    S3Headers_drop(&qp);

    s3_error_t err;
    if (hres.status_code == 200) {
        if (tstr_len(hres.body) > 0) callback(hres.body, tstr_len(hres.body), userdata);
        err = S3_OK;
    } else {
        err = check_response(&hres, 200);
    }
    s3_http_response_free(&hres);
    return err;
}

s3_error_t s3_remove_object(s3_client_t* client, const char* bucket, const char* object) {
    if (!client || !bucket || !object) return s3_error_make(-1, "Invalid params");

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    S3Headers qp = S3Headers_init();
    s3_http_response_t hres = s3_execute_signed(client, "DELETE", uri, NULL, &qp, NULL, 0);
    tstr_free(uri);
    S3Headers_drop(&qp);

    s3_error_t err = check_response_2xx(&hres);
    s3_http_response_free(&hres);
    return err;
}

s3_error_t s3_upload_object(s3_client_t* client,
                                 const char* bucket, const char* object,
                                 const char* filename) {
    if (!client || !bucket || !object || !filename)
        return s3_error_make(-1, "Invalid params");

    turbo_fs_stat_t st;
    if (turbo_fs_stat(filename, &st) != 0)
        return s3_error_make(-1, "Failed to stat file");

    size_t part_size = client->part_size > 0 ? client->part_size : S3_PART_SIZE;

    if (st.size <= part_size) {
        turbo_fs_buf_t buf;
        if (turbo_fs_read_file(filename, &buf) != 0)
            return s3_error_make(-1, "Failed to read file");
        s3_error_t err = s3_put_object(client, bucket, object, buf.base, buf.len, NULL);
        turbo_fs_buf_free(&buf);
        return err;
    }

    turbo_file_t fd = turbo_fs_open(filename, TURBO_FS_O_RDONLY, 0);
    if (fd == TURBO_INVALID_FILE)
        return s3_error_make(-1, "Failed to open file");

    s3_error_t err = s3_put_object_multipart_fd(client, bucket, object,
                                                 fd, (size_t)st.size, NULL);
    turbo_fs_close(fd);
    return err;
}

static int file_write_callback(const char* data, size_t len, void* userdata) {
    FILE* fp = (FILE*)userdata;
    return (int)fwrite(data, 1, len, fp);
}

s3_error_t s3_download_object(s3_client_t* client,
                                   const char* bucket, const char* object,
                                   const char* filename) {
    if (!filename) return s3_error_make(-1, "Null filename");
    FILE* fp = fopen(filename, "wb");
    if (!fp) return s3_error_make(-1, "Failed to open file for writing");
    s3_error_t err = s3_get_object(client, bucket, object, file_write_callback, fp);
    fclose(fp);
    if (!s3_is_ok(err)) remove(filename);
    return err;
}

// ── List objects (iterator) ──

struct s3_list_objects_iter_s {
    s3_client_t* client;
    tstr_t bucket;
    tstr_t prefix;
    int recursive;
    int is_truncated;
    tstr_t continuation_token;
    S3ItemVec items;
    size_t current_idx;
    s3_error_t error;
};

s3_list_objects_iter_t* s3_list_objects(s3_client_t* client,
                                            const char* bucket,
                                            const char* prefix,
                                            int recursive) {
    if (!client || !bucket) return NULL;
    s3_list_objects_iter_t* iter = calloc(1, sizeof(s3_list_objects_iter_t));
    if (!iter) return NULL;
    iter->client = client;
    iter->bucket = tstr_dup(bucket);
    iter->prefix = prefix ? tstr_dup(prefix) : tstr_new();
    iter->recursive = recursive;
    iter->items = S3ItemVec_init();
    iter->is_truncated = 1;
    iter->error = S3_OK;
    return iter;
}

static void s3_list_objects_fetch_next_page(s3_list_objects_iter_t* iter) {
    if (!iter->is_truncated) return;

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", iter->bucket);
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "list-type", "2");
    if (tstr_len(iter->prefix) > 0) s3_headers_add(&qp, "prefix", iter->prefix);
    if (!iter->recursive) s3_headers_add(&qp, "delimiter", "/");
    if (tstr_len(iter->continuation_token) > 0) s3_headers_add(&qp, "continuation-token", iter->continuation_token);

    s3_http_response_t hres = s3_execute_signed(iter->client, "GET", uri, NULL, &qp, NULL, 0);
    tstr_free(uri);
    S3Headers_drop(&qp);

    if (s3_is_ok(hres.error) && hres.status_code == 200) {
        s3_list_objects_parser_res_t pres = s3_parse_list_objects_xml(hres.body);
        S3ItemVec_drop(&iter->items);
        iter->items = pres.items;
        iter->is_truncated = pres.is_truncated;
        tstr_free(iter->continuation_token);
        iter->continuation_token = pres.next_continuation_token;
        iter->current_idx = 0;
        iter->error = pres.error;
    } else {
        if (!s3_is_ok(hres.error)) {
            iter->error = hres.error;
            hres.error = S3_OK;
        } else {
            iter->error = s3_parse_error_xml(hres.body);
        }
        iter->is_truncated = 0;
    }
    s3_http_response_free(&hres);
}

int s3_list_objects_next(s3_list_objects_iter_t* iter, s3_item_t* item) {
    if (!iter) return 0;
    while (iter->current_idx >= S3ItemVec_size(&iter->items)) {
        if (!iter->is_truncated) return 0;
        s3_list_objects_fetch_next_page(iter);
        if (!s3_is_ok(iter->error)) return 0;
        if (S3ItemVec_size(&iter->items) == 0 && !iter->is_truncated) return 0;
    }
    *item = s3_item_clone(*S3ItemVec_at(&iter->items, iter->current_idx));
    iter->current_idx++;
    return 1;
}

s3_error_t s3_list_objects_error(s3_list_objects_iter_t* iter) {
    if (!iter) return s3_error_make(-1, "Null iterator");
    return s3_error_clone(iter->error);
}

void s3_list_objects_free(s3_list_objects_iter_t* iter) {
    if (!iter) return;
    tstr_free(iter->bucket);
    tstr_free(iter->prefix);
    tstr_free(iter->continuation_token);
    S3ItemVec_drop(&iter->items);
    s3_error_free(&iter->error);
    free(iter);
}

// ── Multipart Upload ──

s3_create_multipart_response_t s3_create_multipart_upload(
    s3_client_t* client, const char* bucket, const char* object,
    const char* content_type, const s3_sse_t* sse) {
    s3_create_multipart_response_t res = {0};
    if (!client || !bucket || !object) {
        res.error = s3_error_make(-1, "Invalid params");
        return res;
    }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    S3Headers hdrs = S3Headers_init();
    if (content_type) s3_headers_add(&hdrs, "Content-Type", content_type);
    if (sse) s3_sse_apply_headers(sse, &hdrs);
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "uploads", "");

    s3_http_response_t hres = s3_execute_signed(client, "POST", uri, &hdrs, &qp, NULL, 0);
    tstr_free(uri);
    S3Headers_drop(&hdrs);
    S3Headers_drop(&qp);

    res.error = check_response(&hres, 200);
    if (s3_is_ok(res.error)) {
        res.upload_id = s3_parse_create_multipart_xml(hres.body, &res.error);
    }
    s3_http_response_free(&hres);
    return res;
}

s3_upload_part_response_t s3_upload_part(
    s3_client_t* client, const char* bucket, const char* object,
    const char* upload_id, int part_number,
    const char* data, size_t len) {
    s3_upload_part_response_t res = {0};
    if (!client || !bucket || !object || !upload_id) {
        res.error = s3_error_make(-1, "Invalid params");
        return res;
    }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    S3Headers qp = S3Headers_init();
    char pn[16]; fmt(pn, sizeof(pn), "{}", part_number);
    s3_headers_add(&qp, "partNumber", pn);
    s3_headers_add(&qp, "uploadId", upload_id);

    s3_http_response_t hres = s3_execute_signed(client, "PUT", uri, NULL, &qp, data, len);
    tstr_free(uri);
    S3Headers_drop(&qp);

    res.error = check_response(&hres, 200);
    if (s3_is_ok(res.error)) {
        const char* etag = s3_headers_get(&hres.headers, "ETag");
        if (etag && etag[0] != '\0') {
            res.etag = tstr_dup(etag);
        } else {
            // Fallback for servers/proxies that omit UploadPart ETag.
            // For standard multipart uploads, part ETag is MD5(part-bytes), and it must be quoted.
            tstr_t h = s3_md5_hex(data, len);
            res.etag = tstr_cat_fmt(tstr_new(), "\"%s\"", h);
            tstr_free(h);
        }
    }
    s3_http_response_free(&hres);
    return res;
}

s3_upload_part_response_t s3_upload_part_copy(
    s3_client_t* client, const char* bucket, const char* object,
    const char* upload_id, int part_number,
    const char* src_bucket, const char* src_object,
    size_t offset, size_t length) {
    s3_upload_part_response_t res = {0};
    if (!client || !bucket || !object || !upload_id || !src_bucket || !src_object) {
        res.error = s3_error_make(-1, "Invalid params");
        return res;
    }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    S3Headers hdrs = S3Headers_init();
    tstr_t copy_src = tstr_cat_fmt(tstr_new(), "/%s/%s", src_bucket, src_object);
    s3_headers_add(&hdrs, "x-amz-copy-source", copy_src);
    if (length > 0) {
        tstr_t range = tstr_cat_fmt(tstr_new(), "bytes=%zu-%zu", offset, offset + length - 1);
        s3_headers_add(&hdrs, "x-amz-copy-source-range", range);
        tstr_free(range);
    }
    tstr_free(copy_src);

    S3Headers qp = S3Headers_init();
    char pn[16]; fmt(pn, sizeof(pn), "{}", part_number);
    s3_headers_add(&qp, "partNumber", pn);
    s3_headers_add(&qp, "uploadId", upload_id);

    s3_http_response_t hres = s3_execute_signed(client, "PUT", uri, &hdrs, &qp, NULL, 0);
    tstr_free(uri);
    S3Headers_drop(&hdrs);
    S3Headers_drop(&qp);

    res.error = check_response(&hres, 200);
    if (s3_is_ok(res.error)) {
        res.etag = s3_parse_upload_part_copy_etag_xml(hres.body, &res.error);
    }
    s3_http_response_free(&hres);
    return res;
}

static tstr_t s3_multipart_fix_etag(const char* etag) {
    if (!etag) return tstr_new();

    const char* begin = etag;
    while (*begin && isspace((unsigned char)*begin)) begin++;

    const char* end = begin + strlen(begin);
    while (end > begin && isspace((unsigned char)*(end - 1))) end--;

    if ((end - begin) >= 2 && *begin == '"' && *(end - 1) == '"') {
        begin++;
        end--;
    }

    /* Standard S3 SDKs usually send the raw hex ETag in the XML element value.
     * Quoting it here and then escaping it in the XML builder leads to &quot; hex &quot;
     * which many S3 implementations (like MinIO) reject with 'entity tag may not match'. */
    return tstr_dup_len(begin, (size_t)(end - begin));
}

static tstr_t s3_md5_hex(const char* data, size_t len) {
    if (!data) return tstr_new();

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return tstr_new();

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    tstr_t out = tstr_new();

    if (EVP_DigestInit_ex(ctx, EVP_md5(), NULL) != 1 ||
        EVP_DigestUpdate(ctx, data, len) != 1 ||
        EVP_DigestFinal_ex(ctx, hash, &hash_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return out;
    }
    EVP_MD_CTX_free(ctx);

    for (unsigned int i = 0; i < hash_len; i++) {
        out = tstr_cat_fmt(out, "%02x", hash[i]);
    }
    return out;
}

s3_complete_multipart_response_t s3_complete_multipart_upload(
    s3_client_t* client, const char* bucket, const char* object,
    const char* upload_id, const char** etags, int part_count) {
    s3_complete_multipart_response_t res = {0};
    if (!client || !bucket || !object || !upload_id || !etags) {
        res.error = s3_error_make(-1, "Invalid params");
        return res;
    }
    if (part_count <= 0) {
        res.error = s3_error_make(-1, "Invalid multipart part count");
        return res;
    }
    for (int i = 0; i < part_count; i++) {
        if (!etags[i] || !etags[i][0]) {
            res.error = s3_error_make(-1, "Missing ETag for multipart part");
            return res;
        }
    }

    s3_xml_builder_t* xb = s3_xml_new();
    s3_xml_open_ns(xb, "CompleteMultipartUpload", "http://s3.amazonaws.com/doc/2006-03-01/");
    for (int i = 0; i < part_count; i++) {
        s3_xml_open(xb, "Part");
        s3_xml_elem_int(xb, "PartNumber", i + 1);
        
        /* ETags are expected exactly as returned by the server, but without 
         * surrounding quotes in the complete multipart XML body. */
        tstr_t fixed_etag = s3_multipart_fix_etag(etags[i]);
        s3_xml_elem(xb, "ETag", fixed_etag);
        tstr_free(fixed_etag);
        
        s3_xml_close(xb, "Part");
    }
    s3_xml_close(xb, "CompleteMultipartUpload");
    tstr_t body = s3_xml_finish(xb);

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    S3Headers extra_hdrs = S3Headers_init();
    s3_headers_add(&extra_hdrs, "Content-Type", "application/xml");
    
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "uploadId", upload_id);

    s3_http_response_t hres = s3_execute_signed(client, "POST", uri, &extra_hdrs, &qp, body, tstr_len(body));
    tstr_free(uri);
    tstr_free(body);
    S3Headers_drop(&extra_hdrs);
    S3Headers_drop(&qp);

    res.error = check_response(&hres, 200);
    if (s3_is_ok(res.error)) {
        res.etag = s3_parse_complete_multipart_etag_xml(hres.body, &res.location, &res.error);
    }
    s3_http_response_free(&hres);
    return res;
}

s3_error_t s3_abort_multipart_upload(
    s3_client_t* client, const char* bucket, const char* object,
    const char* upload_id) {
    if (!client || !bucket || !object || !upload_id) return s3_error_make(-1, "Invalid params");

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "uploadId", upload_id);

    s3_http_response_t hres = s3_execute_signed(client, "DELETE", uri, NULL, &qp, NULL, 0);
    tstr_free(uri);
    S3Headers_drop(&qp);

    s3_error_t err = check_response_2xx(&hres);
    s3_http_response_free(&hres);
    return err;
}

// ── Copy / Compose ──

s3_copy_object_response_t s3_copy_object(
    s3_client_t* client,
    const char* src_bucket, const char* src_object,
    const char* dst_bucket, const char* dst_object) {
    s3_copy_object_response_t res = {0};
    if (!client || !src_bucket || !src_object || !dst_bucket || !dst_object) {
        res.error = s3_error_make(-1, "Invalid params");
        return res;
    }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", dst_bucket, dst_object);
    S3Headers hdrs = S3Headers_init();
    tstr_t copy_src = tstr_cat_fmt(tstr_new(), "/%s/%s", src_bucket, src_object);
    s3_headers_add(&hdrs, "x-amz-copy-source", copy_src);
    tstr_free(copy_src);
    S3Headers qp = S3Headers_init();

    s3_http_response_t hres = s3_execute_signed(client, "PUT", uri, &hdrs, &qp, NULL, 0);
    tstr_free(uri);
    S3Headers_drop(&hdrs);
    S3Headers_drop(&qp);

    res.error = check_response(&hres, 200);
    if (s3_is_ok(res.error)) {
        s3_copy_object_parser_res_t pres = s3_parse_copy_object_xml(hres.body);
        res.etag = pres.etag;
        res.error = pres.error;
        tstr_free(pres.last_modified_str);
    }
    s3_http_response_free(&hres);
    return res;
}

s3_copy_object_response_t s3_compose_object(
    s3_client_t* client,
    const char* bucket, const char* object,
    const s3_compose_source_t* sources, int source_count) {
    s3_copy_object_response_t res = {0};
    if (!client || !bucket || !object || !sources || source_count < 1) {
        res.error = s3_error_make(-1, "Invalid params");
        return res;
    }

    // Single source without range: simple copy
    if (source_count == 1 && sources[0].offset == 0 && sources[0].length == 0) {
        return s3_copy_object(client, sources[0].bucket, sources[0].object, bucket, object);
    }

    // Multiple sources: multipart copy
    s3_create_multipart_response_t cr = s3_create_multipart_upload(client, bucket, object, NULL, NULL);
    if (!s3_is_ok(cr.error)) {
        res.error = cr.error;
        cr.error = S3_OK;
        s3_create_multipart_response_free(&cr);
        return res;
    }

    const char** etags = calloc((size_t)source_count, sizeof(char*));
    for (int i = 0; i < source_count; i++) {
        s3_upload_part_response_t pr = s3_upload_part_copy(
            client, bucket, object, cr.upload_id, i + 1,
            sources[i].bucket, sources[i].object,
            sources[i].offset, sources[i].length);
        if (!s3_is_ok(pr.error)) {
            res.error = pr.error;
            pr.error = S3_OK;
            s3_upload_part_response_free(&pr);
            s3_abort_multipart_upload(client, bucket, object, cr.upload_id);
            for (int j = 0; j < i; j++) free((void*)etags[j]);
            free(etags);
            s3_create_multipart_response_free(&cr);
            return res;
        }
        etags[i] = pr.etag;
        pr.etag = NULL;
        s3_upload_part_response_free(&pr);
    }

    s3_complete_multipart_response_t cmr = s3_complete_multipart_upload(
        client, bucket, object, cr.upload_id, etags, source_count);
    res.etag = cmr.etag;
    cmr.etag = NULL;
    res.error = cmr.error;
    cmr.error = S3_OK;
    s3_complete_multipart_response_free(&cmr);

    for (int i = 0; i < source_count; i++) free((void*)etags[i]);
    free(etags);
    s3_create_multipart_response_free(&cr);
    return res;
}

// ── Batch Delete ──

s3_remove_objects_response_t s3_remove_objects(
    s3_client_t* client, const char* bucket,
    const char** keys, int key_count) {
    s3_remove_objects_response_t res = {0};
    if (!client || !bucket || !keys || key_count < 1) {
        res.error = s3_error_make(-1, "Invalid params");
        return res;
    }

    s3_xml_builder_t* xb = s3_xml_new();
    s3_xml_open(xb, "Delete");
    s3_xml_elem(xb, "Quiet", "false");
    for (int i = 0; i < key_count; i++) {
        s3_xml_open(xb, "Object");
        s3_xml_elem(xb, "Key", keys[i]);
        s3_xml_close(xb, "Object");
    }
    s3_xml_close(xb, "Delete");
    tstr_t body = s3_xml_finish(xb);

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    S3Headers hdrs = S3Headers_init();
    s3_headers_add(&hdrs, "Content-Type", "application/xml");
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "delete", "");

    // S3 requires Content-MD5 for delete
    tstr_t content_md5 = s3_signer_sha256_hex(body, tstr_len(body));
    s3_headers_add(&hdrs, "x-amz-content-sha256", content_md5);
    tstr_free(content_md5);

    s3_http_response_t hres = s3_execute_signed(client, "POST", uri, &hdrs, &qp, body, tstr_len(body));
    tstr_free(uri);
    tstr_free(body);
    S3Headers_drop(&hdrs);
    S3Headers_drop(&qp);

    res.error = check_response(&hres, 200);
    if (s3_is_ok(res.error)) {
        s3_delete_objects_parser_res_t pres = s3_parse_delete_objects_xml(hres.body);
        res.errors = pres.errors;
        res.error_count = pres.error_count;
        res.error = pres.error;
    }
    s3_http_response_free(&hres);
    return res;
}

// ── Presigned URLs ──

tstr_t s3_get_presigned_object_url(
    s3_client_t* client, const char* method,
    const char* bucket, const char* object,
    int expires_secs) {
    if (!client || !method || !bucket || !object) return tstr_new();

    s3_credentials_t creds = {0};
    s3_error_t err = client->provider->fetch(client->provider->ctx, &creds);
    if (!s3_is_ok(err)) { s3_error_free(&err); return tstr_new(); }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    tstr_t url = s3_build_url(&client->base_url, uri);

    S3Headers qp = S3Headers_init();
    tstr_t result = s3_signer_presign_v4(method, url, client->base_url.region,
                                             &qp, creds.access_key, creds.secret_key,
                                             creds.session_token, s3_time_now(), expires_secs);

    s3_credentials_clear(&creds);
    tstr_free(uri);
    tstr_free(url);
    S3Headers_drop(&qp);
    return result;
}

s3_presigned_post_t s3_get_presigned_post_form_data(
    s3_client_t* client, const char* bucket, const char* object,
    int expires_secs) {
    s3_presigned_post_t res = {0};
    res.form_data = S3Headers_init();
    if (!client || !bucket || !object) {
        res.error = s3_error_make(-1, "Invalid params");
        return res;
    }

    s3_credentials_t creds = {0};
    res.error = client->provider->fetch(client->provider->ctx, &creds);
    if (!s3_is_ok(res.error)) return res;

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    res.url = s3_build_url(&client->base_url, uri);
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

    res.error = s3_signer_post_presign_v4(client->base_url.region,
                                              creds.access_key, creds.secret_key,
                                              creds.session_token, s3_time_now(),
                                              policy_b64, &res.form_data);

    s3_credentials_clear(&creds);
    tstr_free(policy);
    tstr_free(policy_b64);
    return res;
}

// ── Object Tags ──

s3_tag_set_t s3_get_object_tags(s3_client_t* client, const char* bucket, const char* object, s3_error_t* err) {
    s3_tag_set_t res = {0};
    if (!client || !bucket || !object) { if (err) *err = s3_error_make(-1, "Invalid params"); return res; }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "tagging", "");

    s3_http_response_t hres = s3_execute_signed(client, "GET", uri, NULL, &qp, NULL, 0);
    tstr_free(uri);
    S3Headers_drop(&qp);

    s3_error_t e = check_response(&hres, 200);
    if (s3_is_ok(e)) {
        res = s3_parse_tagging_xml(hres.body, &e);
    }
    if (err) *err = e; else s3_error_free(&e);
    s3_http_response_free(&hres);
    return res;
}

s3_error_t s3_set_object_tags(s3_client_t* client, const char* bucket, const char* object, const s3_tag_set_t* tags) {
    if (!client || !bucket || !object || !tags) return s3_error_make(-1, "Invalid params");

    s3_xml_builder_t* xb = s3_xml_new();
    s3_xml_open_ns(xb, "Tagging", "http://s3.amazonaws.com/doc/2006-03-01/");
    s3_xml_open(xb, "TagSet");
    for (int i = 0; i < tags->count; i++) {
        s3_xml_open(xb, "Tag");
        s3_xml_elem(xb, "Key", tags->tags[i].key);
        s3_xml_elem(xb, "Value", tags->tags[i].value);
        s3_xml_close(xb, "Tag");
    }
    s3_xml_close(xb, "TagSet");
    s3_xml_close(xb, "Tagging");
    tstr_t body = s3_xml_finish(xb);

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    S3Headers hdrs = S3Headers_init();
    s3_headers_add(&hdrs, "Content-Type", "application/xml");
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "tagging", "");

    s3_http_response_t hres = s3_execute_signed(client, "PUT", uri, &hdrs, &qp, body, tstr_len(body));
    tstr_free(uri);
    tstr_free(body);
    S3Headers_drop(&hdrs);
    S3Headers_drop(&qp);

    s3_error_t err = check_response(&hres, 200);
    s3_http_response_free(&hres);
    return err;
}

s3_error_t s3_delete_object_tags(s3_client_t* client, const char* bucket, const char* object) {
    if (!client || !bucket || !object) return s3_error_make(-1, "Invalid params");

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "tagging", "");

    s3_http_response_t hres = s3_execute_signed(client, "DELETE", uri, NULL, &qp, NULL, 0);
    tstr_free(uri);
    S3Headers_drop(&qp);

    s3_error_t err = check_response_2xx(&hres);
    s3_http_response_free(&hres);
    return err;
}

// ── Object Retention ──

s3_object_retention_t s3_get_object_retention(s3_client_t* client, const char* bucket, const char* object, s3_error_t* err) {
    s3_object_retention_t res = {0};
    if (!client || !bucket || !object) { if (err) *err = s3_error_make(-1, "Invalid params"); return res; }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "retention", "");

    s3_http_response_t hres = s3_execute_signed(client, "GET", uri, NULL, &qp, NULL, 0);
    tstr_free(uri);
    S3Headers_drop(&qp);

    s3_error_t e = check_response(&hres, 200);
    if (s3_is_ok(e)) {
        res = s3_parse_object_retention_xml(hres.body, &e);
    }
    if (err) *err = e; else s3_error_free(&e);
    s3_http_response_free(&hres);
    return res;
}

s3_error_t s3_set_object_retention(s3_client_t* client, const char* bucket, const char* object, const s3_object_retention_t* retention) {
    if (!client || !bucket || !object || !retention) return s3_error_make(-1, "Invalid params");

    s3_xml_builder_t* xb = s3_xml_new();
    s3_xml_open_ns(xb, "Retention", "http://s3.amazonaws.com/doc/2006-03-01/");
    s3_xml_elem(xb, "Mode", retention->mode == S3_RETENTION_COMPLIANCE ? "COMPLIANCE" : "GOVERNANCE");
    tstr_t date_str = s3_time_to_iso8601(retention->retain_until_date);
    s3_xml_elem(xb, "RetainUntilDate", date_str);
    tstr_free(date_str);
    s3_xml_close(xb, "Retention");
    tstr_t body = s3_xml_finish(xb);

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    S3Headers hdrs = S3Headers_init();
    s3_headers_add(&hdrs, "Content-Type", "application/xml");
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "retention", "");

    s3_http_response_t hres = s3_execute_signed(client, "PUT", uri, &hdrs, &qp, body, tstr_len(body));
    tstr_free(uri);
    tstr_free(body);
    S3Headers_drop(&hdrs);
    S3Headers_drop(&qp);

    s3_error_t err = check_response(&hres, 200);
    s3_http_response_free(&hres);
    return err;
}

// ── Legal Hold ──

int s3_is_object_legal_hold_enabled(s3_client_t* client, const char* bucket, const char* object, s3_error_t* err) {
    if (!client || !bucket || !object) { if (err) *err = s3_error_make(-1, "Invalid params"); return 0; }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "legal-hold", "");

    s3_http_response_t hres = s3_execute_signed(client, "GET", uri, NULL, &qp, NULL, 0);
    tstr_free(uri);
    S3Headers_drop(&qp);

    int enabled = 0;
    s3_error_t e = check_response(&hres, 200);
    if (s3_is_ok(e)) {
        enabled = s3_parse_legal_hold_xml(hres.body, &e);
    }
    if (err) *err = e; else s3_error_free(&e);
    s3_http_response_free(&hres);
    return enabled;
}

static s3_error_t set_legal_hold(s3_client_t* client, const char* bucket, const char* object, const char* status) {
    s3_xml_builder_t* xb = s3_xml_new();
    s3_xml_open_ns(xb, "LegalHold", "http://s3.amazonaws.com/doc/2006-03-01/");
    s3_xml_elem(xb, "Status", status);
    s3_xml_close(xb, "LegalHold");
    tstr_t body = s3_xml_finish(xb);

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    S3Headers hdrs = S3Headers_init();
    s3_headers_add(&hdrs, "Content-Type", "application/xml");
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "legal-hold", "");

    s3_http_response_t hres = s3_execute_signed(client, "PUT", uri, &hdrs, &qp, body, tstr_len(body));
    tstr_free(uri);
    tstr_free(body);
    S3Headers_drop(&hdrs);
    S3Headers_drop(&qp);

    s3_error_t err = check_response(&hres, 200);
    s3_http_response_free(&hres);
    return err;
}

s3_error_t s3_enable_object_legal_hold(s3_client_t* client, const char* bucket, const char* object) {
    if (!client || !bucket || !object) return s3_error_make(-1, "Invalid params");
    return set_legal_hold(client, bucket, object, "ON");
}

s3_error_t s3_disable_object_legal_hold(s3_client_t* client, const char* bucket, const char* object) {
    if (!client || !bucket || !object) return s3_error_make(-1, "Invalid params");
    return set_legal_hold(client, bucket, object, "OFF");
}

// ── SelectObjectContent ──

s3_error_t s3_select_object_content(
    s3_client_t* client, const char* bucket, const char* object,
    const s3_select_request_t* request,
    s3_data_callback_t callback, void* userdata) {
    if (!client || !bucket || !object || !request || !callback)
        return s3_error_make(-1, "Invalid params");

    // Build SelectObjectContentRequest XML
    s3_xml_builder_t* xb = s3_xml_new();
    s3_xml_open_ns(xb, "SelectObjectContentRequest", "http://s3.amazonaws.com/doc/2006-03-01/");
    s3_xml_elem(xb, "Expression", request->expression);
    s3_xml_elem(xb, "ExpressionType", "SQL");

    // InputSerialization
    s3_xml_open(xb, "InputSerialization");
    if (request->input_format && strcmp(request->input_format, "CSV") == 0) {
        s3_xml_open(xb, "CSV");
        if (request->csv_delimiter) s3_xml_elem(xb, "FieldDelimiter", request->csv_delimiter);
        if (request->csv_quote_char) s3_xml_elem(xb, "QuoteCharacter", request->csv_quote_char);
        if (request->csv_header_info) s3_xml_elem(xb, "FileHeaderInfo", request->csv_header_info);
        s3_xml_close(xb, "CSV");
    } else if (request->input_format && strcmp(request->input_format, "JSON") == 0) {
        s3_xml_open(xb, "JSON");
        s3_xml_elem(xb, "Type", "DOCUMENT");
        s3_xml_close(xb, "JSON");
    } else if (request->input_format && strcmp(request->input_format, "Parquet") == 0) {
        s3_xml_open(xb, "Parquet");
        s3_xml_close(xb, "Parquet");
    }
    s3_xml_close(xb, "InputSerialization");

    // OutputSerialization
    s3_xml_open(xb, "OutputSerialization");
    if (request->output_format && strcmp(request->output_format, "JSON") == 0) {
        s3_xml_open(xb, "JSON");
        s3_xml_close(xb, "JSON");
    } else {
        s3_xml_open(xb, "CSV");
        s3_xml_close(xb, "CSV");
    }
    s3_xml_close(xb, "OutputSerialization");

    s3_xml_close(xb, "SelectObjectContentRequest");
    tstr_t body = s3_xml_finish(xb);

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    S3Headers hdrs = S3Headers_init();
    s3_headers_add(&hdrs, "Content-Type", "application/xml");
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "select", "");
    s3_headers_add(&qp, "select-type", "2");

    s3_http_response_t hres = s3_execute_signed(client, "POST", uri, &hdrs, &qp, body, tstr_len(body));
    tstr_free(uri);
    tstr_free(body);
    S3Headers_drop(&hdrs);
    S3Headers_drop(&qp);

    s3_error_t err = check_response(&hres, 200);
    if (s3_is_ok(err) && tstr_len(hres.body) > 0) {
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
    s3_http_response_free(&hres);
    return err;
}

/* ── Streaming File Transfer ──────────────────────────────────────── */

s3_error_t s3_put_object_from_file(s3_client_t* client,
                                   const char* bucket, const char* object,
                                   const char* file_path,
                                   const char* content_type,
                                   http_progress_cb progress_cb,
                                   void* progress_ud) {
    if (!client || !bucket || !object || !file_path)
        return s3_error_make(-1, "Invalid params");

    /* Read file into memory */
    turbo_fs_buf_t buf = {0};
    if (turbo_fs_read_file(file_path, &buf) != 0) {
        return s3_error_make(-1, "Failed to read file");
    }

    /* Report initial progress */
    if (progress_cb) {
        progress_cb(0, buf.len, progress_ud);
    }

    /* Upload using standard put_object */
    s3_error_t err = s3_put_object(client, bucket, object, buf.base, buf.len, content_type);

    /* Report completion */
    if (progress_cb) {
        progress_cb(buf.len, buf.len, progress_ud);
    }

    turbo_fs_buf_free(&buf);
    return err;
}

s3_error_t s3_download_object_stream(s3_client_t* client,
                                     const char* bucket, const char* object,
                                     const char* output_path,
                                     http_progress_cb progress_cb,
                                     void* progress_ud) {
    if (!client || !bucket || !object || !output_path)
        return s3_error_make(-1, "Invalid params");

    /* Download to memory first */
    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s/%s", bucket, object);
    S3Headers qp = S3Headers_init();
    s3_http_response_t hres = s3_execute_signed(client, "GET", uri, NULL, &qp, NULL, 0);
    tstr_free(uri);
    S3Headers_drop(&qp);

    s3_error_t err = check_response(&hres, 200);
    if (!s3_is_ok(err)) {
        s3_http_response_free(&hres);
        return err;
    }

    /* Report progress */
    size_t total = tstr_len(hres.body);
    if (progress_cb) {
        progress_cb(0, total, progress_ud);
    }

    /* Write to file */
    turbo_fs_buf_t buf = {.base = (char*)hres.body, .len = total};
    if (turbo_fs_write_file(output_path, &buf) != 0) {
        s3_http_response_free(&hres);
        return s3_error_make(-1, "Failed to write file");
    }

    /* Report completion */
    if (progress_cb) {
        progress_cb(total, total, progress_ud);
    }

    s3_http_response_free(&hres);
    return S3_OK;
}

/* ── Batch Operations ─────────────────────────────────────────────── */

typedef struct {
    s3_client_t* client;
    const s3_batch_item_t* item;
    int index;
    int is_upload;  /* 1=upload, 0=download */
    s3_error_t result;  /* Store result here */
} s3_batch_task_t;

static void s3_batch_upload_coro(coro_t* co, void* arg) {
    UNUSED(co);
    s3_batch_task_t* task = (s3_batch_task_t*)arg;

    task->result = s3_put_object_from_file(
        task->client,
        task->item->bucket,
        task->item->key,
        task->item->file_path,
        task->item->content_type,
        NULL, NULL  /* No progress for batch items */
    );
}

static void s3_batch_download_coro(coro_t* co, void* arg) {
    UNUSED(co);
    s3_batch_task_t* task = (s3_batch_task_t*)arg;

    task->result = s3_download_object_stream(
        task->client,
        task->item->bucket,
        task->item->key,
        task->item->file_path,
        NULL, NULL  /* No progress for batch items */
    );
}

s3_batch_result_t* s3_put_objects_batch(s3_client_t* client,
                                        const s3_batch_item_t* items,
                                        int count,
                                        int concurrency) {
    return s3_put_objects_batch_progress(client, items, count, concurrency, NULL, NULL);
}

s3_batch_result_t* s3_put_objects_batch_progress(
    s3_client_t* client,
    const s3_batch_item_t* items,
    int count,
    int concurrency,
    s3_batch_progress_cb progress_cb,
    void* progress_ud) {

    if (!client || !items || count <= 0)
        return NULL;

    if (concurrency <= 0)
        concurrency = 10;

    /* Calculate total size */
    size_t total_size = 0;
    for (int i = 0; i < count; i++) {
        turbo_fs_stat_t stat;
        if (turbo_fs_stat(items[i].file_path, &stat) == 0) {
            total_size += stat.size;
        }
    }

    /* Allocate results */
    s3_batch_result_t* results = (s3_batch_result_t*)calloc(count, sizeof(s3_batch_result_t));
    if (!results)
        return NULL;

    /* Allocate tasks */
    s3_batch_task_t* tasks = (s3_batch_task_t*)calloc(count, sizeof(s3_batch_task_t));
    if (!tasks) {
        free(results);
        return NULL;
    }

    /* Initialize tasks */
    for (int i = 0; i < count; i++) {
        tasks[i].client = client;
        tasks[i].item = &items[i];
        tasks[i].index = i;
        tasks[i].is_upload = 1;
    }

    /* Create coroutine tasks */
    coro_task_t** coro_tasks = (coro_task_t**)calloc(count, sizeof(coro_task_t*));
    if (!coro_tasks) {
        free(tasks);
        free(results);
        return NULL;
    }

    for (int i = 0; i < count; i++) {
        coro_tasks[i] = coro_task_create(client->coro_ctx, s3_batch_upload_coro, &tasks[i]);
        if (coro_tasks[i])
            coro_task_start(coro_tasks[i]);
    }

    /* Progress tracking */
    s3_batch_progress_t progress = {
        .current_file = 0,
        .total_files = count,
        .current_file_uploaded = 0,
        .current_file_size = 0,
        .total_uploaded = 0,
        .total_size = total_size,
        .completed_files = 0,
        .failed_files = 0
    };

    /* Wait for all with concurrency limit */
    int completed = 0;
    while (completed < count) {
        int active = 0;
        for (int i = 0; i < count && active < concurrency; i++) {
            if (coro_tasks[i] && !coro_task_is_done(coro_tasks[i])) {
                active++;
            }
        }

        /* Run event loop */
        coro_context_run(client->coro_ctx, TURBO_RUN_NOWAIT);

        /* Check completed tasks */
        for (int i = 0; i < count; i++) {
            if (coro_tasks[i] && coro_task_is_done(coro_tasks[i])) {
                results[i].error = tasks[i].result;
                results[i].index = i;

                /* Update progress */
                if (s3_is_ok(tasks[i].result)) {
                    progress.completed_files++;
                } else {
                    progress.failed_files++;
                }

                /* Get file size */
                turbo_fs_stat_t stat;
                if (turbo_fs_stat(items[i].file_path, &stat) == 0) {
                    progress.total_uploaded += stat.size;
                }

                progress.current_file = i + 1;

                /* Call progress callback */
                if (progress_cb) {
                    progress_cb(&progress, progress_ud);
                }

                coro_task_destroy(coro_tasks[i]);
                coro_tasks[i] = NULL;
                completed++;
            }
        }
    }

    free(coro_tasks);
    free(tasks);
    return results;
}

s3_batch_result_t* s3_get_objects_batch(s3_client_t* client,
                                        const s3_batch_item_t* items,
                                        int count,
                                        int concurrency) {
    if (!client || !items || count <= 0)
        return NULL;

    if (concurrency <= 0)
        concurrency = 10;

    /* Allocate results */
    s3_batch_result_t* results = (s3_batch_result_t*)calloc(count, sizeof(s3_batch_result_t));
    if (!results)
        return NULL;

    /* Allocate tasks */
    s3_batch_task_t* tasks = (s3_batch_task_t*)calloc(count, sizeof(s3_batch_task_t));
    if (!tasks) {
        free(results);
        return NULL;
    }

    /* Initialize tasks */
    for (int i = 0; i < count; i++) {
        tasks[i].client = client;
        tasks[i].item = &items[i];
        tasks[i].index = i;
        tasks[i].is_upload = 0;
    }

    /* Create coroutine tasks */
    coro_task_t** coro_tasks = (coro_task_t**)calloc(count, sizeof(coro_task_t*));
    if (!coro_tasks) {
        free(tasks);
        free(results);
        return NULL;
    }

    for (int i = 0; i < count; i++) {
        coro_tasks[i] = coro_task_create(client->coro_ctx, s3_batch_download_coro, &tasks[i]);
        if (coro_tasks[i])
            coro_task_start(coro_tasks[i]);
    }

    /* Wait for all with concurrency limit */
    int completed = 0;
    while (completed < count) {
        int active = 0;
        for (int i = 0; i < count && active < concurrency; i++) {
            if (coro_tasks[i] && !coro_task_is_done(coro_tasks[i])) {
                active++;
            }
        }

        /* Run event loop */
        coro_context_run(client->coro_ctx, TURBO_RUN_NOWAIT);

        /* Check completed tasks */
        for (int i = 0; i < count; i++) {
            if (coro_tasks[i] && coro_task_is_done(coro_tasks[i])) {
                results[i].error = tasks[i].result;
                results[i].index = i;
                coro_task_destroy(coro_tasks[i]);
                coro_tasks[i] = NULL;
                completed++;
            }
        }
    }

    free(coro_tasks);
    free(tasks);
    return results;
}

void s3_batch_results_free(s3_batch_result_t* results, int count) {
    (void)count;
    if (results)
        free(results);
}

/* ── Multipart Upload for Large Files ─────────────────────────────── */

#define DEFAULT_PART_SIZE_MB 10
#define DEFAULT_CONCURRENCY 10
#define MIN_PART_SIZE_MB 5
#define MAX_PART_SIZE_MB 100

/* Resume state structure */
typedef struct {
    char upload_id[256];
    int total_parts;
    int completed_parts;
    char completed_etags[10000][256];  /* Max 10000 parts */
} s3_resume_state_t;

/* Save resume state to file */
static int save_resume_state(const char* resume_file, const s3_resume_state_t* state) {
    if (!resume_file || !state) return -1;

    FILE* fp = fopen(resume_file, "wb");
    if (!fp) return -1;

    size_t written = fwrite(state, sizeof(s3_resume_state_t), 1, fp);
    fclose(fp);

    return (written == 1) ? 0 : -1;
}

/* Load resume state from file */
static int load_resume_state(const char* resume_file, s3_resume_state_t* state) {
    if (!resume_file || !state) return -1;

    FILE* fp = fopen(resume_file, "rb");
    if (!fp) return -1;

    size_t read = fread(state, sizeof(s3_resume_state_t), 1, fp);
    fclose(fp);

    return (read == 1) ? 0 : -1;
}

typedef struct {
    s3_client_t* client;
    const char* bucket;
    const char* key;
    const char* upload_id;
    turbo_file_t fd;
    int part_number;
    size_t part_size;
    size_t file_offset;
    s3_upload_part_response_t result;
    const s3_multipart_options_t* options;
    t_atomic_uint64_t* total_uploaded;  /* Shared counter */
    size_t total_size;
    int total_parts;
} s3_part_upload_task_t;

static void s3_multipart_part_coro(coro_t* co, void* arg) {
    UNUSED(co);
    s3_part_upload_task_t* task = (s3_part_upload_task_t*)arg;

    /* Read part data */
    char* part_data = (char*)malloc(task->part_size);
    if (!part_data) {
        task->result.error = s3_error_make(-1, "Failed to allocate part buffer");
        return;
    }

    /* Seek and read */
    turbo_fs_seek(task->fd, task->file_offset, SEEK_SET);
    int nread = turbo_fs_read(task->fd, part_data, task->part_size);
    if (nread < 0) {
        free(part_data);
        task->result.error = s3_error_make(-1, "Failed to read part");
        return;
    }

    size_t actual_size = (size_t)nread;

    /* Upload part */
    task->result = s3_upload_part(
        task->client,
        task->bucket,
        task->key,
        task->upload_id,
        task->part_number,
        part_data,
        actual_size
    );

    free(part_data);

    /* Update progress */
    if (s3_is_ok(task->result.error)) {
        t_atomic_fetch_add_uint64(task->total_uploaded, actual_size);

        if (task->options && task->options->progress_cb) {
            size_t uploaded = (size_t)t_atomic_load_uint64(task->total_uploaded);
            task->options->progress_cb(
                task->part_number, task->total_parts,
                actual_size, actual_size,
                uploaded, task->total_size,
                task->options->progress_ud
            );
        }
    }
}

s3_error_t s3_put_object_multipart_file(
    s3_client_t* client,
    const char* bucket,
    const char* key,
    const char* file_path,
    const char* content_type,
    const s3_multipart_options_t* options) {

    if (!client || !bucket || !key || !file_path)
        return s3_error_make(-1, "Invalid params");

    /* Get file size */
    turbo_fs_stat_t stat;
    if (turbo_fs_stat(file_path, &stat) != 0 || !stat.is_file) {
        return s3_error_make(-1, "Failed to stat file");
    }

    size_t file_size = stat.size;
    if (file_size == 0) {
        return s3_error_make(-1, "File is empty");
    }

    /* Parse options */
    size_t part_size_mb = DEFAULT_PART_SIZE_MB;
    int concurrency = DEFAULT_CONCURRENCY;

    if (options) {
        if (options->part_size_mb > 0) {
            part_size_mb = options->part_size_mb;
            if (part_size_mb < MIN_PART_SIZE_MB) part_size_mb = MIN_PART_SIZE_MB;
            if (part_size_mb > MAX_PART_SIZE_MB) part_size_mb = MAX_PART_SIZE_MB;
        }
        if (options->concurrency > 0) {
            concurrency = options->concurrency;
        }
    }

    size_t part_size = part_size_mb * 1024 * 1024;
    int total_parts = (int)((file_size + part_size - 1) / part_size);

    if (total_parts > 10000) {
        return s3_error_make(-1, "File too large (>10000 parts)");
    }

    /* Open file */
    turbo_file_t fd = turbo_fs_open(file_path, TURBO_FS_O_RDONLY, 0);
    if (fd < 0) {
        return s3_error_make(-1, "Failed to open file");
    }

    /* Try to resume from previous upload */
    s3_resume_state_t resume_state = {0};
    int resuming = 0;
    const char* upload_id = NULL;

    if (options && options->resume_file) {
        if (load_resume_state(options->resume_file, &resume_state) == 0) {
            /* Validate resume state */
            if (resume_state.total_parts == total_parts) {
                resuming = 1;
                upload_id = resume_state.upload_id;
                printf("Resuming upload: %d/%d parts already completed\n",
                       resume_state.completed_parts, total_parts);
            }
        }
    }

    /* Create multipart upload if not resuming */
    s3_create_multipart_response_t create_resp = {0};
    if (!resuming) {
        create_resp = s3_create_multipart_upload(
            client, bucket, key, content_type, NULL
        );

        if (!s3_is_ok(create_resp.error)) {
            turbo_fs_close(fd);
            s3_error_t err = create_resp.error;
            tstr_free(create_resp.upload_id);
            return err;
        }

        upload_id = create_resp.upload_id;

        /* Initialize resume state */
        if (options && options->resume_file) {
            strncpy(resume_state.upload_id, upload_id, sizeof(resume_state.upload_id) - 1);
            resume_state.total_parts = total_parts;
            resume_state.completed_parts = 0;
        }
    }

    /* Allocate tasks */
    s3_part_upload_task_t* tasks = (s3_part_upload_task_t*)calloc(
        total_parts, sizeof(s3_part_upload_task_t)
    );
    if (!tasks) {
        s3_abort_multipart_upload(client, bucket, key, upload_id);
        tstr_free(create_resp.upload_id);
        turbo_fs_close(fd);
        return s3_error_make(-1, "Failed to allocate tasks");
    }

    /* Shared progress counter */
    t_atomic_uint64_t total_uploaded;
    t_atomic_store_uint64(&total_uploaded, 0);

    /* Initialize tasks */
    for (int i = 0; i < total_parts; i++) {
        /* Skip already completed parts when resuming */
        if (resuming && i < resume_state.completed_parts) {
            tasks[i].result.etag = resume_state.completed_etags[i];
            tasks[i].result.error = S3_OK;
            continue;
        }

        tasks[i].client = client;
        tasks[i].bucket = bucket;
        tasks[i].key = key;
        tasks[i].upload_id = upload_id;
        tasks[i].fd = fd;
        tasks[i].part_number = i + 1;
        tasks[i].file_offset = i * part_size;
        tasks[i].part_size = (i == total_parts - 1) ?
            (file_size - i * part_size) : part_size;
        tasks[i].options = options;
        tasks[i].total_uploaded = &total_uploaded;
        tasks[i].total_size = file_size;
        tasks[i].total_parts = total_parts;
    }

    /* Create coroutine tasks */
    coro_task_t** coro_tasks = (coro_task_t**)calloc(total_parts, sizeof(coro_task_t*));
    if (!coro_tasks) {
        free(tasks);
        s3_abort_multipart_upload(client, bucket, key, upload_id);
        tstr_free(create_resp.upload_id);
        turbo_fs_close(fd);
        return s3_error_make(-1, "Failed to allocate coro tasks");
    }

    /* Start uploads with concurrency control */
    int started = resuming ? resume_state.completed_parts : 0;
    int completed = resuming ? resume_state.completed_parts : 0;
    s3_error_t first_error = S3_OK;

    while (completed < total_parts) {
        /* Start new tasks up to concurrency limit */
        while (started < total_parts) {
            int active = 0;
            for (int i = resume_state.completed_parts; i < started; i++) {
                if (coro_tasks[i] && !coro_task_is_done(coro_tasks[i])) {
                    active++;
                }
            }

            if (active >= concurrency) break;

            coro_tasks[started] = coro_task_create(
                client->coro_ctx, s3_multipart_part_coro, &tasks[started]
            );
            if (coro_tasks[started]) {
                coro_task_start(coro_tasks[started]);
            }
            started++;
        }

        /* Run event loop */
        coro_context_run(client->coro_ctx, TURBO_RUN_NOWAIT);

        /* Check completed tasks */
        for (int i = resume_state.completed_parts; i < started; i++) {
            if (coro_tasks[i] && coro_task_is_done(coro_tasks[i])) {
                if (!s3_is_ok(tasks[i].result.error) && s3_is_ok(first_error)) {
                    first_error = tasks[i].result.error;
                } else if (s3_is_ok(tasks[i].result.error)) {
                    /* Save progress */
                    if (options && options->resume_file) {
                        strncpy(resume_state.completed_etags[i],
                                tasks[i].result.etag,
                                sizeof(resume_state.completed_etags[i]) - 1);
                        resume_state.completed_parts = i + 1;
                        save_resume_state(options->resume_file, &resume_state);
                    }
                }
                coro_task_destroy(coro_tasks[i]);
                coro_tasks[i] = NULL;
                completed++;
            }
        }
    }

    /* Check for errors */
    if (!s3_is_ok(first_error)) {
        /* Abort upload on error */
        s3_abort_multipart_upload(client, bucket, key, upload_id);

        /* Cleanup */
        for (int i = 0; i < total_parts; i++) {
            s3_upload_part_response_free(&tasks[i].result);
        }
        free(coro_tasks);
        free(tasks);
        tstr_free(create_resp.upload_id);
        turbo_fs_close(fd);
        return first_error;
    }

    /* Build parts list for completion */
    const char** etags = (const char**)calloc(total_parts, sizeof(char*));
    if (!etags) {
        s3_abort_multipart_upload(client, bucket, key, upload_id);
        for (int i = 0; i < total_parts; i++) {
            s3_upload_part_response_free(&tasks[i].result);
        }
        free(coro_tasks);
        free(tasks);
        tstr_free(create_resp.upload_id);
        turbo_fs_close(fd);
        return s3_error_make(-1, "Failed to allocate etags list");
    }

    for (int i = 0; i < total_parts; i++) {
        etags[i] = tasks[i].result.etag;
    }

    /* Complete multipart upload */
    s3_complete_multipart_response_t complete_resp = s3_complete_multipart_upload(
        client, bucket, key, upload_id, etags, total_parts
    );

    s3_error_t result = complete_resp.error;

    /* Delete resume file on success */
    if (s3_is_ok(result) && options && options->resume_file) {
        remove(options->resume_file);
    }

    /* Cleanup */
    free(etags);
    for (int i = 0; i < total_parts; i++) {
        s3_upload_part_response_free(&tasks[i].result);
    }
    free(coro_tasks);
    free(tasks);
    s3_complete_multipart_response_free(&complete_resp);
    tstr_free(create_resp.upload_id);
    turbo_fs_close(fd);

    return result;
}
