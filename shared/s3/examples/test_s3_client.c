#include "s3/s3_client.h"
#include <turbo_str.h>
#include <turbo_coro.h>
#include <netcore/turbo_coro_context.h>
#include "tinytest.h"
#include <stdio.h>
#include <time.h>
#include <string.h>

static int test_get_callback(const char* data, size_t len, void* userdata) {
    tstr_t* content = (tstr_t*)userdata;
    *content = tstr_cat_len(*content, data, len);
    return 1;
}

static char g_test_bucket[64] = {0};

static const char* get_test_bucket_name() {
    if (g_test_bucket[0] == 0) {
        sprintf(g_test_bucket, "turbonet-test-%u", (unsigned int)time(NULL) % 1000000);
    }
    return g_test_bucket;
}

static turbo_coro_context_t *g_ctx = NULL;

// Helper: run a void(*)(void*) inside a coroutine with the event loop
typedef void (*test_fn_t)(void* arg);

typedef struct {
    test_fn_t fn;
    void* arg;
    turbo_coro_context_t* ctx;
} coro_test_wrapper_t;

static void coro_test_entry(turbo_coro_t* co, void* arg) {
    (void)co;
    coro_test_wrapper_t* w = (coro_test_wrapper_t*)arg;
    w->fn(w->arg);
    turbo_coro_context_stop(w->ctx);
}

static void run_in_coro(turbo_coro_context_t* ctx, test_fn_t fn, void* arg) {
    coro_test_wrapper_t w = { .fn = fn, .arg = arg, .ctx = ctx };
    turbo_coro_t* co = turbo_coro_create(coro_test_entry, &w, NULL);
    turbo_coro_resume(co);
    turbo_coro_context_run(ctx);
    turbo_coro_destroy(co);
}

// Test context — carries client + bdd config for assertions
typedef struct {
    s3_client_t* client;
    s3_base_url_t* url;
    const char* test_bucket;
    __bdd_config_type__* bdd;
} test_ctx_t;

// Macro to bring __bdd_config__ into scope inside extracted test functions
#define BDD_CTX() __bdd_config_type__ *__bdd_config__ = ((test_ctx_t*)arg)->bdd

static void test_prepare(void* arg) {
    BDD_CTX();
    test_ctx_t* t = (test_ctx_t*)arg;
    s3_error_t err = s3_make_bucket(t->client, t->test_bucket, NULL);
    if (!s3_is_ok(err)) {
        if (err.message && (strstr(err.message, "Your previous request to create the named bucket succeeded") ||
                            strstr(err.message, "BucketAlreadyOwnedByYou"))) {
            s3_error_free(&err);
            err = S3_OK;
        }
    }
    check(s3_is_ok(err));
    if (!s3_is_ok(err)) printf("Failed to create test bucket %s: %s\n", t->test_bucket, err.message);
    s3_error_free(&err);
}

static void test_list_buckets(void* arg) {
    BDD_CTX();
    test_ctx_t* t = (test_ctx_t*)arg;
    s3_list_buckets_response_t resp = s3_list_buckets(t->client);
    if (s3_is_ok(resp.error)) {
        printf("Found %d buckets\n", (int)S3BucketVec_size(&resp.buckets));
        int found = 0;
        c_foreach (i, S3BucketVec, resp.buckets) {
            if (strcmp(i.ref->name, t->test_bucket) == 0) found = 1;
        }
        check(found);
    } else {
        printf("ListBuckets failed: %s (code %d)\n", resp.error.message, resp.error.code);
    }
    s3_list_buckets_free(&resp);
}

static void test_bucket_not_exists(void* arg) {
    BDD_CTX();
    test_ctx_t* t = (test_ctx_t*)arg;
    s3_error_t err = S3_OK;
    int exists = s3_bucket_exists(t->client, "non-existent-bucket-xyz-123456789", &err);
    check_int_eq(exists, 0);
    check(s3_is_ok(err));
    s3_error_free(&err);
}

static void test_stat_nonexistent(void* arg) {
    BDD_CTX();
    test_ctx_t* t = (test_ctx_t*)arg;
    s3_stat_object_response_t resp = s3_stat_object(t->client, t->test_bucket, "non-existent-object-xyz");
    check(!s3_is_ok(resp.error));
    if (resp.error.message) {
        printf("StatObject failed as expected: %s\n", resp.error.message);
    }
    s3_stat_object_response_free(&resp);
}

static void test_invalid_creds(void* arg) {
    BDD_CTX();
    test_ctx_t* t = (test_ctx_t*)arg;
    s3_credential_provider_t* bad_prov = s3_creds_static("INVALID_ACCESS_KEY", "INVALID_SECRET_KEY", NULL);
    s3_client_t* bad_client = s3_client_create(g_ctx, t->url, bad_prov);
    s3_list_buckets_response_t resp = s3_list_buckets(bad_client);
    check(!s3_is_ok(resp.error));
    printf("ListBuckets failed with invalid credentials as expected: %s\n", resp.error.message);
    s3_list_buckets_free(&resp);
    s3_client_destroy(bad_client);
    s3_credential_provider_destroy(bad_prov);
}

static void test_remove_nonexistent(void* arg) {
    BDD_CTX();
    test_ctx_t* t = (test_ctx_t*)arg;
    s3_error_t err = s3_remove_object(t->client, t->test_bucket, "non-existent-to-remove");
    if (s3_is_ok(err)) {
        printf("RemoveObject succeeded (standard S3 behavior for non-existent)\n");
    } else {
        printf("RemoveObject failed: %s\n", err.message);
    }
    s3_error_free(&err);
}

static void test_list_objects(void* arg) {
    BDD_CTX();
    test_ctx_t* t = (test_ctx_t*)arg;
    s3_list_objects_iter_t* iter = s3_list_objects(t->client, t->test_bucket, NULL, 0);
    check_not_null(iter);
    int count = 0;
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

static void test_put_get(void* arg) {
    BDD_CTX();
    test_ctx_t* t = (test_ctx_t*)arg;
    const char* test_data = "Hello from S3 C Client SDK!";
    size_t test_len = strlen(test_data);
    const char* object_name = "test-put-object.txt";
    s3_error_t err = s3_put_object(t->client, t->test_bucket, object_name, test_data, test_len, "text/plain");
    if (s3_is_ok(err)) {
        printf("PutObject succeeded\n");
        tstr_t content = tstr_new();
        err = s3_get_object(t->client, t->test_bucket, object_name, test_get_callback, &content);
        if (s3_is_ok(err)) {
            check_str_eq(content, test_data);
            printf("GetObject verified the content\n");
        } else {
            printf("GetObject failed after PutObject: %s\n", err.message);
        }
        tstr_free(content);
        s3_error_free(&err);
        err = s3_remove_object(t->client, t->test_bucket, object_name);
        s3_error_free(&err);
    } else {
        printf("PutObject failed: %s\n", err.message);
        s3_error_free(&err);
    }
}

static void test_upload_download(void* arg) {
    BDD_CTX();
    test_ctx_t* t = (test_ctx_t*)arg;
    const char* local_test_file = "s3_test_upload.txt";
    const char* local_download_file = "s3_test_download.txt";
    const char* object_name = "test-upload-object.txt";
    const char* test_content = "This is a test file for upload/download functionality.";
    FILE* fp = fopen(local_test_file, "w");
    check_not_null(fp);
    if (fp) { fputs(test_content, fp); fclose(fp); }
    s3_error_t err = s3_upload_object(t->client, t->test_bucket, object_name, local_test_file);
    if (s3_is_ok(err)) {
        printf("UploadObject succeeded\n");
        err = s3_download_object(t->client, t->test_bucket, object_name, local_download_file);
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
            printf("DownloadObject failed: %s\n", err.message);
        }
        s3_error_free(&err);
        err = s3_remove_object(t->client, t->test_bucket, object_name);
        s3_error_free(&err);
    } else {
        printf("UploadObject failed: %s\n", err.message);
        s3_error_free(&err);
    }
    remove(local_test_file);
    remove(local_download_file);
}

static void test_multipart(void* arg) {
    BDD_CTX();
    test_ctx_t* t = (test_ctx_t*)arg;
    size_t large_len = 10 * 1024 * 1024 + 1;
    char* large_data = malloc(large_len);
    check_not_null(large_data);
    memset(large_data, 'A', large_len);
    large_data[large_len - 1] = 'Z';

    const char* object_name = "test-parallel-multipart.bin";
    s3_error_t err = s3_put_object(t->client, t->test_bucket, object_name, large_data, large_len, "application/octet-stream");
    if (!s3_is_ok(err)) {
        printf("Parallel multipart upload failed: code=%d msg=%s\n", err.code, err.message ? err.message : "(null)");
    }
    check(s3_is_ok(err));

    printf("Parallel multipart upload succeeded (10MB+1, 3 parts)\n");

    s3_stat_object_response_t st = s3_stat_object(t->client, t->test_bucket, object_name);
    check(s3_is_ok(st.error));
    check_int_eq((int)st.size, (int)large_len);
    s3_stat_object_response_free(&st);

    tstr_t content = tstr_new();
    err = s3_get_object(t->client, t->test_bucket, object_name, test_get_callback, &content);
    check(s3_is_ok(err));
    check_int_eq((int)tstr_len(content), (int)large_len);
    check(content[large_len - 1] == 'Z');
    printf("Parallel multipart content verified\n");
    tstr_free(content);
    s3_error_free(&err);

    err = s3_remove_object(t->client, t->test_bucket, object_name);
    s3_error_free(&err);
    free(large_data);
}

static void test_small_put(void* arg) {
    BDD_CTX();
    test_ctx_t* t = (test_ctx_t*)arg;
    const char* small_data = "small object via simple PUT path";
    const char* object_name = "test-small-put.txt";
    s3_error_t err = s3_put_object(t->client, t->test_bucket, object_name, small_data, strlen(small_data), "text/plain");
    if (!s3_is_ok(err)) {
        printf("Small PUT failed: code=%d msg=%s\n", err.code, err.message ? err.message : "(null)");
    }
    check(s3_is_ok(err));
    if (s3_is_ok(err)) {
        tstr_t content = tstr_new();
        err = s3_get_object(t->client, t->test_bucket, object_name, test_get_callback, &content);
        check(s3_is_ok(err));
        check_str_eq(content, small_data);
        tstr_free(content);
        s3_error_free(&err);
        err = s3_remove_object(t->client, t->test_bucket, object_name);
    }
    s3_error_free(&err);
}

static void test_cleanup(void* arg) {
    BDD_CTX();
    test_ctx_t* t = (test_ctx_t*)arg;

    // Delete all objects in the bucket first
    s3_list_objects_iter_t* iter = s3_list_objects(t->client, t->test_bucket, NULL, 1);
    if (iter) {
        s3_item_t item;
        while (s3_list_objects_next(iter, &item)) {
            s3_error_t rerr = s3_remove_object(t->client, t->test_bucket, item.name);
            s3_error_free(&rerr);
            s3_item_drop(&item);
        }
        s3_list_objects_free(iter);
    }

    s3_error_t err = s3_remove_bucket(t->client, t->test_bucket);
    if (!s3_is_ok(err)) printf("Failed to cleanup test bucket %s: %s\n", t->test_bucket, err.message);
    check(s3_is_ok(err));
    s3_error_free(&err);
}

spec("S3 Client Migration Tests") {
    const char* test_bucket = get_test_bucket_name();
    s3_base_url_t url = {0};
    url.host = tstr_dup("play.min.io");
    url.port = 443;
    url.is_https = 1;
    url.region = tstr_dup("us-east-1");
    s3_credential_provider_t* prov = s3_creds_static("Q3AM3UQ867SPQQA43P2F", "zuf+tfteSlswRu7BJ86wekitnifILbZam1KYY3TG", NULL);
    g_ctx = turbo_coro_context_create();
    s3_client_t* client = s3_client_create(g_ctx, &url, prov);

    test_ctx_t tctx = {
        .client = client,
        .url = &url,
        .test_bucket = test_bucket,
        .bdd = __bdd_config__,
    };

    it("should prepare the test environment") {
        run_in_coro(g_ctx, test_prepare, &tctx);
    }

    it("should successfully list buckets from a mock/real server") {
        run_in_coro(g_ctx, test_list_buckets, &tctx);
    }

    it("should return false for non-existent bucket") {
        run_in_coro(g_ctx, test_bucket_not_exists, &tctx);
    }

    it("should fail to stat non-existent object") {
        run_in_coro(g_ctx, test_stat_nonexistent, &tctx);
    }

    it("should fail with invalid credentials") {
        run_in_coro(g_ctx, test_invalid_creds, &tctx);
    }

    it("should attempt to remove an object") {
        run_in_coro(g_ctx, test_remove_nonexistent, &tctx);
    }

    it("should list objects in a bucket") {
        run_in_coro(g_ctx, test_list_objects, &tctx);
    }

    it("should put and get an object") {
        run_in_coro(g_ctx, test_put_get, &tctx);
    }

    it("should upload and download a file") {
        run_in_coro(g_ctx, test_upload_download, &tctx);
    }

    it("should put a large object via parallel multipart upload") {
        run_in_coro(g_ctx, test_multipart, &tctx);
    }

    it("should still put small objects via simple PUT") {
        run_in_coro(g_ctx, test_small_put, &tctx);
    }

    it("should cleanup the test environment") {
        run_in_coro(g_ctx, test_cleanup, &tctx);
        s3_client_destroy(client);
        s3_credential_provider_destroy(prov);
        s3_base_url_free(&url);
        turbo_coro_context_destroy(g_ctx);
    }
}
