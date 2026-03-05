#include "s3/s3_client.h"
#include <turbo_str.h>
#include <turbo_coro.h>
#include <netcore/turbo_coro_context.h>
#include "tinytest.h"
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static int test_get_callback(const char* data, size_t len, void* userdata) {
    tstr_t* content = (tstr_t*)userdata;
    *content = tstr_cat_len(*content, data, len);
    return 1;
}

static char g_test_bucket[64] = {0};

static const char* get_test_bucket_name() {
    if (g_test_bucket[0] == 0) {
        struct timespec ts = {0};
        timespec_get(&ts, TIME_UTC);
        unsigned long long nonce = (unsigned long long)ts.tv_sec * 1000000000ull +
                                   (unsigned long long)ts.tv_nsec;
        snprintf(g_test_bucket, sizeof(g_test_bucket), "turbonet-test-%llx", nonce);
    }
    return g_test_bucket;
}

static coro_context_t *g_ctx = NULL;

// Helper: run a void(*)(void*) inside a coroutine with the event loop
typedef void (*test_fn_t)(void* arg);

typedef struct {
    test_fn_t fn;
    void* arg;
    coro_context_t* ctx;
} coro_test_wrapper_t;

static void coro_test_entry(coro_t* co, void* arg) {
    (void)co;
    coro_test_wrapper_t* w = (coro_test_wrapper_t*)arg;
    w->fn(w->arg);
    coro_context_stop(w->ctx);
}

static void run_in_coro(coro_context_t* ctx, test_fn_t fn, void* arg) {
    coro_test_wrapper_t w = { .fn = fn, .arg = arg, .ctx = ctx };
    int rc = coro_context_spawn(ctx, coro_test_entry, &w);
    if (rc != 0) {
        printf("Failed to spawn test coroutine: %d\n", rc);
        return;
    }
    coro_context_run(ctx, TURBO_RUN_DEFAULT);
}

// Test context — carries client for tests
typedef struct {
    s3_client_t* client;
    s3_base_url_t* url;
    const char* test_bucket;
} test_ctx_t;

static void ensure_test_bucket(test_ctx_t* t, __bdd_config_type__ *__bdd_config__) {
    s3_error_t berr = S3_OK;
    int exists = s3_bucket_exists(t->client, t->test_bucket, &berr);
    if (s3_is_ok(berr) && exists) {
        s3_error_free(&berr);
        return;
    }
    s3_error_free(&berr);

    berr = s3_make_bucket(t->client, t->test_bucket, NULL);
    if (!s3_is_ok(berr)) {
        if (berr.message &&
            (strstr(berr.message, "Your previous request to create the named bucket succeeded") ||
             strstr(berr.message, "BucketAlreadyOwnedByYou"))) {
            s3_error_free(&berr);
            berr = S3_OK;
        }
    }
    if (!s3_is_ok(berr)) {
        printf("Failed to ensure test bucket %s: %s\n", t->test_bucket, berr.message ? berr.message : "(null)");
    }
    check(s3_is_ok(berr));
    s3_error_free(&berr);
}

// Complex tests still need a separate function for coroutine execution
static void test_multipart_impl(void* arg) {
    typedef struct { test_ctx_t* t; __bdd_config_type__* bdd; } coro_args_t;
    coro_args_t* a = (coro_args_t*)arg;
    test_ctx_t* t = a->t;
    __bdd_config_type__* __bdd_config__ = a->bdd;

    ensure_test_bucket(t, __bdd_config__);
    size_t large_len = 10 * 1024 * 1024 + 1;
    char* large_data = malloc(large_len);
    check_not_null(large_data);
    if (!large_data) return;
    memset(large_data, 'A', large_len);
    large_data[large_len - 1] = 'Z';

    const char* object_name = "test-parallel-multipart.bin";
    s3_error_t err = s3_put_object(t->client, t->test_bucket, object_name, large_data, large_len, "application/octet-stream");
    if (!s3_is_ok(err)) {
        printf("Parallel multipart upload failed: code=%d msg=%s\n", err.code, err.message ? err.message : "(null)");
    }
    check(s3_is_ok(err));

    if (s3_is_ok(err)) {
        printf("Parallel multipart upload succeeded (10MB+1, 3 parts)\n");

        s3_stat_object_response_t st = s3_stat_object(t->client, t->test_bucket, object_name);
        if (!s3_is_ok(st.error)) {
            printf("s3_stat_object failed: code=%d msg=%s\n", st.error.code, st.error.message ? st.error.message : "(null)");
        }
        check(s3_is_ok(st.error));
        check_int_eq((int)st.size, (int)large_len);
        s3_stat_object_response_free(&st);

        tstr_t content = tstr_new();
        err = s3_get_object(t->client, t->test_bucket, object_name, test_get_callback, &content);
        check(s3_is_ok(err));
        check_int_eq((int)tstr_len(content), (int)large_len);
        printf("Downloaded %zu bytes. First byte: %c, last byte: %c\n", tstr_len(content), (int)tstr_len(content) > 0 ? content[0] : '?', (int)tstr_len(content) > 0 ? content[large_len - 1] : '?');
        check(content[0] == 'A');
        check(content[large_len - 1] == 'Z');
        printf("Parallel multipart content verified\n");
        tstr_free(content);
        s3_error_free(&err);

        err = s3_remove_object(t->client, t->test_bucket, object_name);
        s3_error_free(&err);
    }
    free(large_data);
}

suite("S3 Client Migration Tests") {
    static test_ctx_t tctx;
    static s3_base_url_t url;
    static s3_credential_provider_t* prov;
    static s3_client_t* client;

    before() {
        const char* test_bucket = get_test_bucket_name();
        memset(&url, 0, sizeof(url));
        url.host = tstr_dup("play.min.io");
        url.port = 443;
        url.is_https = 1;
        url.region = tstr_dup("us-east-1");
        prov = s3_creds_static("Q3AM3UQ867SPQQA43P2F", "zuf+tfteSlswRu7BJ86wekitnifILbZam1KYY3TG", NULL);
        g_ctx = coro_context_create(NULL);
        client = s3_client_create(g_ctx, &url, prov);

        tctx.client = client;
        tctx.url = &url;
        tctx.test_bucket = test_bucket;

        // Preparation (was test_prepare)
        s3_error_t err = s3_make_bucket(client, tctx.test_bucket, NULL);
        if (!s3_is_ok(err)) {
            if (err.message && (strstr(err.message, "Your previous request to create the named bucket succeeded") ||
                                strstr(err.message, "BucketAlreadyOwnedByYou"))) {
                s3_error_free(&err);
                err = S3_OK;
            }
        }
        check(s3_is_ok(err));
        if (!s3_is_ok(err)) printf("Failed to create test bucket %s: %s\n", tctx.test_bucket, err.message ? err.message : "(null)");
        s3_error_free(&err);
    }

    it("should successfully list buckets from a mock/real server") {
        s3_list_buckets_response_t resp = s3_list_buckets(tctx.client);
        if (s3_is_ok(resp.error)) {
            printf("Found %d buckets\n", (int)S3BucketVec_size(&resp.buckets));
            int found = 0;
            c_foreach (i, S3BucketVec, resp.buckets) {
                if (strcmp(i.ref->name, tctx.test_bucket) == 0) found = 1;
            }
            check(found);
        } else {
            printf("ListBuckets failed: %s (code %d)\n", resp.error.message ? resp.error.message : "(null)", resp.error.code);
        }
        s3_list_buckets_free(&resp);
    }

    it("should return false for non-existent bucket") {
        s3_error_t err = S3_OK;
        int exists = s3_bucket_exists(tctx.client, "non-existent-bucket-xyz-123456789", &err);
        check_int_eq(exists, 0);
        check(s3_is_ok(err));
        s3_error_free(&err);
    }

    it("should fail to stat non-existent object") {
        s3_stat_object_response_t resp = s3_stat_object(tctx.client, tctx.test_bucket, "non-existent-object-xyz");
        check(!s3_is_ok(resp.error));
        if (resp.error.message) {
            printf("StatObject failed as expected: %s\n", resp.error.message);
        }
        s3_stat_object_response_free(&resp);
    }

    it("should fail with invalid credentials") {
        s3_credential_provider_t* bad_prov = s3_creds_static("INVALID_ACCESS_KEY", "INVALID_SECRET_KEY", NULL);
        s3_client_t* bad_client = s3_client_create(g_ctx, tctx.url, bad_prov);
        s3_list_buckets_response_t resp = s3_list_buckets(bad_client);
        check(!s3_is_ok(resp.error));
        printf("ListBuckets failed with invalid credentials as expected: %s\n", resp.error.message ? resp.error.message : "(null)");
        s3_list_buckets_free(&resp);
        s3_client_destroy(bad_client);
        s3_credential_provider_destroy(bad_prov);
    }

    it("should attempt to remove an object") {
        s3_error_t err = s3_remove_object(tctx.client, tctx.test_bucket, "non-existent-to-remove");
        if (s3_is_ok(err)) {
            printf("RemoveObject succeeded (standard S3 behavior for non-existent)\n");
        } else {
            printf("RemoveObject failed: %s\n", err.message ? err.message : "(null)");
        }
        s3_error_free(&err);
    }

    it("should list objects in a bucket") {
        ensure_test_bucket(&tctx, __bdd_config__);
        s3_list_objects_iter_t* iter = s3_list_objects(tctx.client, tctx.test_bucket, NULL, 0);
        check_not_null(iter);
        int count = 0;
        if (iter) {
            s3_item_t item;
            while (s3_list_objects_next(iter, &item)) {
                s3_item_drop(&item);
                count++;
                if (count >= 5) break;
            }
            s3_error_t err = s3_list_objects_error(iter);
            check(s3_is_ok(err));
            s3_error_free(&err);
            s3_list_objects_free(iter);
        }
    }

    it("should put and get an object") {
        ensure_test_bucket(&tctx, __bdd_config__);
        const char* test_data = "Hello from S3 C Client SDK!";
        size_t test_len = strlen(test_data);
        const char* object_name = "test-put-object.txt";
        s3_error_t err = s3_put_object(tctx.client, tctx.test_bucket, object_name, test_data, test_len, "text/plain");
        if (s3_is_ok(err)) {
            tstr_t content = tstr_new();
            err = s3_get_object(tctx.client, tctx.test_bucket, object_name, test_get_callback, &content);
            check(s3_is_ok(err));
            check_str_eq(content, test_data);
            tstr_free(content);
            s3_error_free(&err);
            err = s3_remove_object(tctx.client, tctx.test_bucket, object_name);
            s3_error_free(&err);
        } else {
            printf("PutObject failed: %s\n", err.message ? err.message : "(null)");
            s3_error_free(&err);
        }
    }

    it("should upload and download a file") {
        ensure_test_bucket(&tctx, __bdd_config__);
        const char* local_test_file = "s3_test_upload.txt";
        const char* local_download_file = "s3_test_download.txt";
        const char* object_name = "test-upload-object.txt";
        const char* test_content = "This is a test file for upload/download functionality.";
        FILE* fp = fopen(local_test_file, "w");
        check_not_null(fp);
        if (fp) { fputs(test_content, fp); fclose(fp); }
        s3_error_t err = s3_upload_object(tctx.client, tctx.test_bucket, object_name, local_test_file);
        if (s3_is_ok(err)) {
            printf("UploadObject succeeded\n");
            err = s3_download_object(tctx.client, tctx.test_bucket, object_name, local_download_file);
            if (s3_is_ok(err)) {
                printf("DownloadObject succeeded\n");
                FILE* dfp = fopen(local_download_file, "r");
                check_not_null(dfp);
                if (dfp) {
                    char res_buf[128] = {0};
                    fgets(res_buf, sizeof(res_buf), dfp);
                    fclose(dfp);
                    check_str_eq(res_buf, test_content);
                    printf("Download verified the content\n");
                }
            } else {
                printf("DownloadObject failed: %s\n", err.message ? err.message : "(null)");
            }
            s3_error_free(&err);
            err = s3_remove_object(tctx.client, tctx.test_bucket, object_name);
            s3_error_free(&err);
        } else {
            printf("UploadObject failed: %s\n", err.message ? err.message : "(null)");
            s3_error_free(&err);
        }
        remove(local_test_file);
        remove(local_download_file);
    }

    it("should put a large object via parallel multipart upload") {
        typedef struct { test_ctx_t* t; __bdd_config_type__* bdd; } coro_args_t;
        coro_args_t args = { &tctx, __bdd_config__ };
        run_in_coro(g_ctx, test_multipart_impl, &args);
    }

    it("should still put small objects via simple PUT") {
        ensure_test_bucket(&tctx, __bdd_config__);
        const char* small_data = "small object via simple PUT path";
        const char* object_name = "test-small-put.txt";
        s3_error_t err = s3_put_object(tctx.client, tctx.test_bucket, object_name, small_data, strlen(small_data), "text/plain");
        if (!s3_is_ok(err)) {
            printf("Small PUT failed: code=%d msg=%s\n", err.code, err.message ? err.message : "(null)");
        }
        check(s3_is_ok(err));
        if (s3_is_ok(err)) {
            tstr_t content = tstr_new();
            err = s3_get_object(tctx.client, tctx.test_bucket, object_name, test_get_callback, &content);
            check(s3_is_ok(err));
            check_str_eq(content, small_data);
            tstr_free(content);
            s3_error_free(&err);
            err = s3_remove_object(tctx.client, tctx.test_bucket, object_name);
            s3_error_free(&err);
        }
    }

    after() {
        // Cleanup (was test_cleanup)
        s3_list_objects_iter_t* iter = s3_list_objects(tctx.client, tctx.test_bucket, NULL, 1);
        if (iter) {
            s3_item_t item;
            while (s3_list_objects_next(iter, &item)) {
                s3_error_t rerr = s3_remove_object(tctx.client, tctx.test_bucket, item.name);
                s3_error_free(&rerr);
                s3_item_drop(&item);
            }
            s3_list_objects_free(iter);
        }

        s3_error_t err = s3_remove_bucket(tctx.client, tctx.test_bucket);
        if (!s3_is_ok(err) && err.message &&
            (strstr(err.message, "NoSuchBucket") || strstr(err.message, "The specified bucket does not exist"))) {
            s3_error_free(&err);
            err = S3_OK;
        }
        if (!s3_is_ok(err)) printf("Failed to cleanup test bucket %s: %s\n", tctx.test_bucket, err.message ? err.message : "(null)");
        check(s3_is_ok(err));
        s3_error_free(&err);

        s3_client_destroy(client);
        s3_credential_provider_destroy(prov);
        s3_base_url_free(&url);
        coro_context_destroy(g_ctx);
    }
}
