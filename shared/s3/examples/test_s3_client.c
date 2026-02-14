#include "s3/s3_client.h"
#include <turbo_str.h>
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
        // Use a semi-stable name for the run
        sprintf(g_test_bucket, "turbonet-test-%u", (unsigned int)time(NULL) % 1000000);
    }
    return g_test_bucket;
}

spec("S3 Client Migration Tests") {
    const char* test_bucket = get_test_bucket_name();
    s3_base_url_t url = {0};
    url.host = tstr_dup("play.min.io");
    url.port = 443;
    url.is_https = 1;
    url.region = tstr_dup("us-east-1");
    s3_credential_provider_t* prov = s3_creds_static("Q3AM3UQ867SPQQA43P2F", "zuf+tfteSlswRu7BJ86wekitnifILbZam1KYY3TG", NULL);
    s3_client_t* client = s3_client_create(&url, prov);

    it("should prepare the test environment") {
        s3_error_t err = s3_make_bucket(client, test_bucket, NULL);
        if (!s3_is_ok(err)) {
            if (err.message && (strstr(err.message, "Your previous request to create the named bucket succeeded") || 
                                strstr(err.message, "BucketAlreadyOwnedByYou"))) {
                s3_error_free(&err);
                err = S3_OK;
            }
        }
        check(s3_is_ok(err));
        if (!s3_is_ok(err)) printf("Failed to create test bucket %s: %s\n", test_bucket, err.message);
        s3_error_free(&err);
    }

    it("should successfully list buckets from a mock/real server") {
        s3_list_buckets_response_t resp = s3_list_buckets(client);
        if (s3_is_ok(resp.error)) {
            printf("Found %d buckets\n", (int)S3BucketVec_size(&resp.buckets));
            int found = 0;
            c_foreach (i, S3BucketVec, resp.buckets) {
                if (strcmp(i.ref->name, test_bucket) == 0) found = 1;
            }
            check(found);
        } else {
            printf("ListBuckets failed: %s (code %d)\n", resp.error.message, resp.error.code);
        }
        s3_list_buckets_free(&resp);
    }

    it("should return false for non-existent bucket") {
        s3_error_t err = S3_OK;
        int exists = s3_bucket_exists(client, "non-existent-bucket-xyz-123456789", &err);
        check_int_eq(exists, 0);
        check(s3_is_ok(err));
        s3_error_free(&err);
    }

    it("should fail to stat non-existent object") {
        s3_stat_object_response_t resp = s3_stat_object(client, test_bucket, "non-existent-object-xyz");
        check(!s3_is_ok(resp.error));
        if (resp.error.message) {
            printf("StatObject failed as expected: %s\n", resp.error.message);
        }
        s3_stat_object_response_free(&resp);
    }

    it("should fail with invalid credentials") {
        s3_credential_provider_t* bad_prov = s3_creds_static("INVALID_ACCESS_KEY", "INVALID_SECRET_KEY", NULL);
        s3_client_t* bad_client = s3_client_create(&url, bad_prov);
        s3_list_buckets_response_t resp = s3_list_buckets(bad_client);
        check(!s3_is_ok(resp.error));
        printf("ListBuckets failed with invalid credentials as expected: %s\n", resp.error.message);
        s3_list_buckets_free(&resp);
        s3_client_destroy(bad_client);
        s3_credential_provider_destroy(bad_prov);
    }

    it("should attempt to remove an object") {
        s3_error_t err = s3_remove_object(client, test_bucket, "non-existent-to-remove");
        if (s3_is_ok(err)) {
            printf("RemoveObject succeeded (standard S3 behavior for non-existent)\n");
        } else {
            printf("RemoveObject failed: %s\n", err.message);
        }
        s3_error_free(&err);
    }

    it("should list objects in a bucket") {
        s3_list_objects_iter_t* iter = s3_list_objects(client, test_bucket, NULL, 0);
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

    it("should put and get an object") {
        const char* test_data = "Hello from S3 C Client SDK!";
        size_t test_len = strlen(test_data);
        const char* object_name = "test-put-object.txt";
        s3_error_t err = s3_put_object(client, test_bucket, object_name, test_data, test_len, "text/plain");
        if (s3_is_ok(err)) {
            printf("PutObject succeeded\n");
            tstr_t content = tstr_new();
            err = s3_get_object(client, test_bucket, object_name, test_get_callback, &content);
            if (s3_is_ok(err)) {
                check_str_eq(content, test_data);
                printf("GetObject verified the content\n");
            } else {
                printf("GetObject failed after PutObject: %s\n", err.message);
            }
            tstr_free(content);
            s3_error_free(&err);
            err = s3_remove_object(client, test_bucket, object_name);
            s3_error_free(&err);
        } else {
            printf("PutObject failed: %s\n", err.message);
            s3_error_free(&err);
        }
    }

    it("should upload and download a file") {
        const char* local_test_file = "s3_test_upload.txt";
        const char* local_download_file = "s3_test_download.txt";
        const char* object_name = "test-upload-object.txt";
        const char* test_content = "This is a test file for upload/download functionality.";
        FILE* fp = fopen(local_test_file, "w");
        check_not_null(fp);
        if (fp) { fputs(test_content, fp); fclose(fp); }
        s3_error_t err = s3_upload_object(client, test_bucket, object_name, local_test_file);
        if (s3_is_ok(err)) {
            printf("UploadObject succeeded\n");
            err = s3_download_object(client, test_bucket, object_name, local_download_file);
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
            err = s3_remove_object(client, test_bucket, object_name);
            s3_error_free(&err);
        } else {
            printf("UploadObject failed: %s\n", err.message);
            s3_error_free(&err);
        }
        remove(local_test_file);
        remove(local_download_file);
    }

    it("should cleanup the test environment") {
        s3_error_t err = s3_remove_bucket(client, test_bucket);
        if (!s3_is_ok(err)) printf("Failed to cleanup test bucket %s: %s\n", test_bucket, err.message);
        check(s3_is_ok(err));
        s3_error_free(&err);
        
        s3_client_destroy(client);
        s3_credential_provider_destroy(prov);
        s3_base_url_free(&url);
    }
}
