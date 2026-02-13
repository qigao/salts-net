#include "minio/minio_bucket_config.h"
#include "minio/minio_xml_builder.h"
#include "minio/minio_signer.h"
#include "minio_http.h"
#include "minio_client_internal.h"
#include <stdlib.h>
#include <string.h>

// ── Tags ──

minio_tag_set_t minio_get_bucket_tags(minio_client_t* client, const char* bucket, minio_error_t* err) {
    minio_tag_set_t res = {0};
    if (!client || !bucket) { if (err) *err = minio_error_make(-1, "Invalid params"); return res; }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "tagging", "");

    minio_http_response_t hres = minio_execute_signed(client, "GET", uri, NULL, &qp, NULL, 0);
    tstr_free(uri);
    MinioHeaders_drop(&qp);

    minio_error_t e = MINIO_OK;
    if (!minio_is_ok(hres.error)) {
        e = hres.error; hres.error = MINIO_OK;
    } else if (hres.status_code == 200) {
        res = minio_parse_tagging_xml(hres.body, &e);
    } else if (hres.status_code != 404) {
        e = minio_parse_error_xml(hres.body);
    }
    if (err) *err = e; else minio_error_free(&e);
    minio_http_response_free(&hres);
    return res;
}

minio_error_t minio_set_bucket_tags(minio_client_t* client, const char* bucket, const minio_tag_set_t* tags) {
    if (!client || !bucket || !tags) return minio_error_make(-1, "Invalid params");

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

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    MinioHeaders hdrs = MinioHeaders_init();
    minio_headers_add(&hdrs, "Content-Type", "application/xml");
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "tagging", "");

    minio_http_response_t hres = minio_execute_signed(client, "PUT", uri, &hdrs, &qp, body, tstr_len(body));
    tstr_free(uri); tstr_free(body);
    MinioHeaders_drop(&hdrs); MinioHeaders_drop(&qp);

    minio_error_t err = MINIO_OK;
    if (!minio_is_ok(hres.error)) { err = hres.error; hres.error = MINIO_OK; }
    else if (hres.status_code < 200 || hres.status_code >= 300) err = minio_parse_error_xml(hres.body);
    minio_http_response_free(&hres);
    return err;
}

minio_error_t minio_delete_bucket_tags(minio_client_t* client, const char* bucket) {
    if (!client || !bucket) return minio_error_make(-1, "Invalid params");

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "tagging", "");

    minio_http_response_t hres = minio_execute_signed(client, "DELETE", uri, NULL, &qp, NULL, 0);
    tstr_free(uri); MinioHeaders_drop(&qp);

    minio_error_t err = MINIO_OK;
    if (!minio_is_ok(hres.error)) { err = hres.error; hres.error = MINIO_OK; }
    else if (hres.status_code < 200 || hres.status_code >= 300) err = minio_parse_error_xml(hres.body);
    minio_http_response_free(&hres);
    return err;
}

// ── Policy ──

tstr_t minio_get_bucket_policy(minio_client_t* client, const char* bucket, minio_error_t* err) {
    if (!client || !bucket) { if (err) *err = minio_error_make(-1, "Invalid params"); return NULL; }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "policy", "");

    minio_http_response_t hres = minio_execute_signed(client, "GET", uri, NULL, &qp, NULL, 0);
    tstr_free(uri); MinioHeaders_drop(&qp);

    tstr_t result = NULL;
    minio_error_t e = MINIO_OK;
    if (!minio_is_ok(hres.error)) { e = hres.error; hres.error = MINIO_OK; }
    else if (hres.status_code == 200) { result = tstr_dup(hres.body); }
    else if (hres.status_code != 404) { e = minio_parse_error_xml(hres.body); }
    if (err) *err = e; else minio_error_free(&e);
    minio_http_response_free(&hres);
    return result;
}

minio_error_t minio_set_bucket_policy(minio_client_t* client, const char* bucket, const char* policy_json) {
    if (!client || !bucket || !policy_json) return minio_error_make(-1, "Invalid params");

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    MinioHeaders hdrs = MinioHeaders_init();
    minio_headers_add(&hdrs, "Content-Type", "application/json");
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "policy", "");

    minio_http_response_t hres = minio_execute_signed(client, "PUT", uri, &hdrs, &qp, policy_json, strlen(policy_json));
    tstr_free(uri); MinioHeaders_drop(&hdrs); MinioHeaders_drop(&qp);

    minio_error_t err = MINIO_OK;
    if (!minio_is_ok(hres.error)) { err = hres.error; hres.error = MINIO_OK; }
    else if (hres.status_code < 200 || hres.status_code >= 300) err = minio_parse_error_xml(hres.body);
    minio_http_response_free(&hres);
    return err;
}

minio_error_t minio_delete_bucket_policy(minio_client_t* client, const char* bucket) {
    if (!client || !bucket) return minio_error_make(-1, "Invalid params");

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "policy", "");

    minio_http_response_t hres = minio_execute_signed(client, "DELETE", uri, NULL, &qp, NULL, 0);
    tstr_free(uri); MinioHeaders_drop(&qp);

    minio_error_t err = MINIO_OK;
    if (!minio_is_ok(hres.error)) { err = hres.error; hres.error = MINIO_OK; }
    else if (hres.status_code < 200 || hres.status_code >= 300) err = minio_parse_error_xml(hres.body);
    minio_http_response_free(&hres);
    return err;
}

// ── Versioning ──

minio_versioning_status_t minio_get_bucket_versioning(minio_client_t* client, const char* bucket, minio_error_t* err) {
    if (!client || !bucket) { if (err) *err = minio_error_make(-1, "Invalid params"); return MINIO_VERSIONING_OFF; }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "versioning", "");

    minio_http_response_t hres = minio_execute_signed(client, "GET", uri, NULL, &qp, NULL, 0);
    tstr_free(uri); MinioHeaders_drop(&qp);

    minio_versioning_status_t status = MINIO_VERSIONING_OFF;
    minio_error_t e = MINIO_OK;
    if (!minio_is_ok(hres.error)) { e = hres.error; hres.error = MINIO_OK; }
    else if (hres.status_code == 200) {
        tstr_t s = minio_parse_versioning_xml(hres.body, &e);
        if (s && strcmp(s, "Enabled") == 0) status = MINIO_VERSIONING_ENABLED;
        else if (s && strcmp(s, "Suspended") == 0) status = MINIO_VERSIONING_SUSPENDED;
        tstr_free(s);
    } else {
        e = minio_parse_error_xml(hres.body);
    }
    if (err) *err = e; else minio_error_free(&e);
    minio_http_response_free(&hres);
    return status;
}

minio_error_t minio_set_bucket_versioning(minio_client_t* client, const char* bucket, minio_versioning_status_t status) {
    if (!client || !bucket) return minio_error_make(-1, "Invalid params");

    const char* status_str = (status == MINIO_VERSIONING_ENABLED) ? "Enabled" : "Suspended";
    minio_xml_builder_t* xb = minio_xml_new();
    minio_xml_open_ns(xb, "VersioningConfiguration", "http://s3.amazonaws.com/doc/2006-03-01/");
    minio_xml_elem(xb, "Status", status_str);
    minio_xml_close(xb, "VersioningConfiguration");
    tstr_t body = minio_xml_finish(xb);

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    MinioHeaders hdrs = MinioHeaders_init();
    minio_headers_add(&hdrs, "Content-Type", "application/xml");
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "versioning", "");

    minio_http_response_t hres = minio_execute_signed(client, "PUT", uri, &hdrs, &qp, body, tstr_len(body));
    tstr_free(uri); tstr_free(body);
    MinioHeaders_drop(&hdrs); MinioHeaders_drop(&qp);

    minio_error_t err = MINIO_OK;
    if (!minio_is_ok(hres.error)) { err = hres.error; hres.error = MINIO_OK; }
    else if (hres.status_code < 200 || hres.status_code >= 300) err = minio_parse_error_xml(hres.body);
    minio_http_response_free(&hres);
    return err;
}

// ── Encryption ──

minio_sse_config_t minio_get_bucket_encryption(minio_client_t* client, const char* bucket, minio_error_t* err) {
    minio_sse_config_t res = {0};
    if (!client || !bucket) { if (err) *err = minio_error_make(-1, "Invalid params"); return res; }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "encryption", "");

    minio_http_response_t hres = minio_execute_signed(client, "GET", uri, NULL, &qp, NULL, 0);
    tstr_free(uri); MinioHeaders_drop(&qp);

    minio_error_t e = MINIO_OK;
    if (!minio_is_ok(hres.error)) { e = hres.error; hres.error = MINIO_OK; }
    else if (hres.status_code == 200) { res = minio_parse_encryption_xml(hres.body, &e); }
    else if (hres.status_code != 404) { e = minio_parse_error_xml(hres.body); }
    if (err) *err = e; else minio_error_free(&e);
    minio_http_response_free(&hres);
    return res;
}

minio_error_t minio_set_bucket_encryption(minio_client_t* client, const char* bucket, const minio_sse_config_t* config) {
    if (!client || !bucket || !config) return minio_error_make(-1, "Invalid params");

    minio_xml_builder_t* xb = minio_xml_new();
    minio_xml_open_ns(xb, "ServerSideEncryptionConfiguration", "http://s3.amazonaws.com/doc/2006-03-01/");
    minio_xml_open(xb, "Rule");
    minio_xml_open(xb, "ApplyServerSideEncryptionByDefault");
    if (config->is_kms) {
        minio_xml_elem(xb, "SSEAlgorithm", "aws:kms");
        if (config->kms_master_key_id) minio_xml_elem(xb, "KMSMasterKeyID", config->kms_master_key_id);
    } else {
        minio_xml_elem(xb, "SSEAlgorithm", "AES256");
    }
    minio_xml_close(xb, "ApplyServerSideEncryptionByDefault");
    minio_xml_close(xb, "Rule");
    minio_xml_close(xb, "ServerSideEncryptionConfiguration");
    tstr_t body = minio_xml_finish(xb);

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    MinioHeaders hdrs = MinioHeaders_init();
    minio_headers_add(&hdrs, "Content-Type", "application/xml");
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "encryption", "");

    minio_http_response_t hres = minio_execute_signed(client, "PUT", uri, &hdrs, &qp, body, tstr_len(body));
    tstr_free(uri); tstr_free(body);
    MinioHeaders_drop(&hdrs); MinioHeaders_drop(&qp);

    minio_error_t err = MINIO_OK;
    if (!minio_is_ok(hres.error)) { err = hres.error; hres.error = MINIO_OK; }
    else if (hres.status_code < 200 || hres.status_code >= 300) err = minio_parse_error_xml(hres.body);
    minio_http_response_free(&hres);
    return err;
}

minio_error_t minio_delete_bucket_encryption(minio_client_t* client, const char* bucket) {
    if (!client || !bucket) return minio_error_make(-1, "Invalid params");

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "encryption", "");

    minio_http_response_t hres = minio_execute_signed(client, "DELETE", uri, NULL, &qp, NULL, 0);
    tstr_free(uri); MinioHeaders_drop(&qp);

    minio_error_t err = MINIO_OK;
    if (!minio_is_ok(hres.error)) { err = hres.error; hres.error = MINIO_OK; }
    else if (hres.status_code < 200 || hres.status_code >= 300) err = minio_parse_error_xml(hres.body);
    minio_http_response_free(&hres);
    return err;
}

// ── Object Lock ──

minio_object_lock_config_t minio_get_object_lock_config(minio_client_t* client, const char* bucket, minio_error_t* err) {
    minio_object_lock_config_t res = {0};
    if (!client || !bucket) { if (err) *err = minio_error_make(-1, "Invalid params"); return res; }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "object-lock", "");

    minio_http_response_t hres = minio_execute_signed(client, "GET", uri, NULL, &qp, NULL, 0);
    tstr_free(uri); MinioHeaders_drop(&qp);

    minio_error_t e = MINIO_OK;
    if (!minio_is_ok(hres.error)) { e = hres.error; hres.error = MINIO_OK; }
    else if (hres.status_code == 200) { res = minio_parse_object_lock_xml(hres.body, &e); }
    else { e = minio_parse_error_xml(hres.body); }
    if (err) *err = e; else minio_error_free(&e);
    minio_http_response_free(&hres);
    return res;
}

minio_error_t minio_set_object_lock_config(minio_client_t* client, const char* bucket, const minio_object_lock_config_t* config) {
    if (!client || !bucket || !config) return minio_error_make(-1, "Invalid params");

    minio_xml_builder_t* xb = minio_xml_new();
    minio_xml_open_ns(xb, "ObjectLockConfiguration", "http://s3.amazonaws.com/doc/2006-03-01/");
    minio_xml_elem(xb, "ObjectLockEnabled", "Enabled");
    minio_xml_open(xb, "Rule");
    minio_xml_open(xb, "DefaultRetention");
    minio_xml_elem(xb, "Mode", config->mode == MINIO_RETENTION_COMPLIANCE ? "COMPLIANCE" : "GOVERNANCE");
    if (config->days > 0) minio_xml_elem_int(xb, "Days", config->days);
    if (config->years > 0) minio_xml_elem_int(xb, "Years", config->years);
    minio_xml_close(xb, "DefaultRetention");
    minio_xml_close(xb, "Rule");
    minio_xml_close(xb, "ObjectLockConfiguration");
    tstr_t body = minio_xml_finish(xb);

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    MinioHeaders hdrs = MinioHeaders_init();
    minio_headers_add(&hdrs, "Content-Type", "application/xml");
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "object-lock", "");

    minio_http_response_t hres = minio_execute_signed(client, "PUT", uri, &hdrs, &qp, body, tstr_len(body));
    tstr_free(uri); tstr_free(body);
    MinioHeaders_drop(&hdrs); MinioHeaders_drop(&qp);

    minio_error_t err = MINIO_OK;
    if (!minio_is_ok(hres.error)) { err = hres.error; hres.error = MINIO_OK; }
    else if (hres.status_code < 200 || hres.status_code >= 300) err = minio_parse_error_xml(hres.body);
    minio_http_response_free(&hres);
    return err;
}

minio_error_t minio_delete_object_lock_config(minio_client_t* client, const char* bucket) {
    if (!client || !bucket) return minio_error_make(-1, "Invalid params");

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "object-lock", "");

    minio_http_response_t hres = minio_execute_signed(client, "DELETE", uri, NULL, &qp, NULL, 0);
    tstr_free(uri); MinioHeaders_drop(&qp);

    minio_error_t err = MINIO_OK;
    if (!minio_is_ok(hres.error)) { err = hres.error; hres.error = MINIO_OK; }
    else if (hres.status_code < 200 || hres.status_code >= 300) err = minio_parse_error_xml(hres.body);
    minio_http_response_free(&hres);
    return err;
}
