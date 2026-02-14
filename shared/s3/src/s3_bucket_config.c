#include "s3/s3_bucket_config.h"
#include "s3/s3_xml_builder.h"
#include "s3/s3_signer.h"
#include "s3_http.h"
#include "s3_client_internal.h"
#include <stdlib.h>
#include <string.h>

// ── Tags ──

s3_tag_set_t s3_get_bucket_tags(s3_client_t* client, const char* bucket, s3_error_t* err) {
    s3_tag_set_t res = {0};
    if (!client || !bucket) { if (err) *err = s3_error_make(-1, "Invalid params"); return res; }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "tagging", "");

    s3_http_response_t hres = s3_execute_signed(client, "GET", uri, NULL, &qp, NULL, 0);
    tstr_free(uri);
    S3Headers_drop(&qp);

    s3_error_t e = S3_OK;
    if (!s3_is_ok(hres.error)) {
        e = hres.error; hres.error = S3_OK;
    } else if (hres.status_code == 200) {
        res = s3_parse_tagging_xml(hres.body, &e);
    } else if (hres.status_code != 404) {
        e = s3_parse_error_xml(hres.body);
    }
    if (err) *err = e; else s3_error_free(&e);
    s3_http_response_free(&hres);
    return res;
}

s3_error_t s3_set_bucket_tags(s3_client_t* client, const char* bucket, const s3_tag_set_t* tags) {
    if (!client || !bucket || !tags) return s3_error_make(-1, "Invalid params");

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

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    S3Headers hdrs = S3Headers_init();
    s3_headers_add(&hdrs, "Content-Type", "application/xml");
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "tagging", "");

    s3_http_response_t hres = s3_execute_signed(client, "PUT", uri, &hdrs, &qp, body, tstr_len(body));
    tstr_free(uri); tstr_free(body);
    S3Headers_drop(&hdrs); S3Headers_drop(&qp);

    s3_error_t err = S3_OK;
    if (!s3_is_ok(hres.error)) { err = hres.error; hres.error = S3_OK; }
    else if (hres.status_code < 200 || hres.status_code >= 300) err = s3_parse_error_xml(hres.body);
    s3_http_response_free(&hres);
    return err;
}

s3_error_t s3_delete_bucket_tags(s3_client_t* client, const char* bucket) {
    if (!client || !bucket) return s3_error_make(-1, "Invalid params");

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "tagging", "");

    s3_http_response_t hres = s3_execute_signed(client, "DELETE", uri, NULL, &qp, NULL, 0);
    tstr_free(uri); S3Headers_drop(&qp);

    s3_error_t err = S3_OK;
    if (!s3_is_ok(hres.error)) { err = hres.error; hres.error = S3_OK; }
    else if (hres.status_code < 200 || hres.status_code >= 300) err = s3_parse_error_xml(hres.body);
    s3_http_response_free(&hres);
    return err;
}

// ── Policy ──

tstr_t s3_get_bucket_policy(s3_client_t* client, const char* bucket, s3_error_t* err) {
    if (!client || !bucket) { if (err) *err = s3_error_make(-1, "Invalid params"); return NULL; }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "policy", "");

    s3_http_response_t hres = s3_execute_signed(client, "GET", uri, NULL, &qp, NULL, 0);
    tstr_free(uri); S3Headers_drop(&qp);

    tstr_t result = NULL;
    s3_error_t e = S3_OK;
    if (!s3_is_ok(hres.error)) { e = hres.error; hres.error = S3_OK; }
    else if (hres.status_code == 200) { result = tstr_dup(hres.body); }
    else if (hres.status_code != 404) { e = s3_parse_error_xml(hres.body); }
    if (err) *err = e; else s3_error_free(&e);
    s3_http_response_free(&hres);
    return result;
}

s3_error_t s3_set_bucket_policy(s3_client_t* client, const char* bucket, const char* policy_json) {
    if (!client || !bucket || !policy_json) return s3_error_make(-1, "Invalid params");

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    S3Headers hdrs = S3Headers_init();
    s3_headers_add(&hdrs, "Content-Type", "application/json");
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "policy", "");

    s3_http_response_t hres = s3_execute_signed(client, "PUT", uri, &hdrs, &qp, policy_json, strlen(policy_json));
    tstr_free(uri); S3Headers_drop(&hdrs); S3Headers_drop(&qp);

    s3_error_t err = S3_OK;
    if (!s3_is_ok(hres.error)) { err = hres.error; hres.error = S3_OK; }
    else if (hres.status_code < 200 || hres.status_code >= 300) err = s3_parse_error_xml(hres.body);
    s3_http_response_free(&hres);
    return err;
}

s3_error_t s3_delete_bucket_policy(s3_client_t* client, const char* bucket) {
    if (!client || !bucket) return s3_error_make(-1, "Invalid params");

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "policy", "");

    s3_http_response_t hres = s3_execute_signed(client, "DELETE", uri, NULL, &qp, NULL, 0);
    tstr_free(uri); S3Headers_drop(&qp);

    s3_error_t err = S3_OK;
    if (!s3_is_ok(hres.error)) { err = hres.error; hres.error = S3_OK; }
    else if (hres.status_code < 200 || hres.status_code >= 300) err = s3_parse_error_xml(hres.body);
    s3_http_response_free(&hres);
    return err;
}

// ── Versioning ──

s3_versioning_status_t s3_get_bucket_versioning(s3_client_t* client, const char* bucket, s3_error_t* err) {
    if (!client || !bucket) { if (err) *err = s3_error_make(-1, "Invalid params"); return S3_VERSIONING_OFF; }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "versioning", "");

    s3_http_response_t hres = s3_execute_signed(client, "GET", uri, NULL, &qp, NULL, 0);
    tstr_free(uri); S3Headers_drop(&qp);

    s3_versioning_status_t status = S3_VERSIONING_OFF;
    s3_error_t e = S3_OK;
    if (!s3_is_ok(hres.error)) { e = hres.error; hres.error = S3_OK; }
    else if (hres.status_code == 200) {
        tstr_t s = s3_parse_versioning_xml(hres.body, &e);
        if (s && strcmp(s, "Enabled") == 0) status = S3_VERSIONING_ENABLED;
        else if (s && strcmp(s, "Suspended") == 0) status = S3_VERSIONING_SUSPENDED;
        tstr_free(s);
    } else {
        e = s3_parse_error_xml(hres.body);
    }
    if (err) *err = e; else s3_error_free(&e);
    s3_http_response_free(&hres);
    return status;
}

s3_error_t s3_set_bucket_versioning(s3_client_t* client, const char* bucket, s3_versioning_status_t status) {
    if (!client || !bucket) return s3_error_make(-1, "Invalid params");

    const char* status_str = (status == S3_VERSIONING_ENABLED) ? "Enabled" : "Suspended";
    s3_xml_builder_t* xb = s3_xml_new();
    s3_xml_open_ns(xb, "VersioningConfiguration", "http://s3.amazonaws.com/doc/2006-03-01/");
    s3_xml_elem(xb, "Status", status_str);
    s3_xml_close(xb, "VersioningConfiguration");
    tstr_t body = s3_xml_finish(xb);

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    S3Headers hdrs = S3Headers_init();
    s3_headers_add(&hdrs, "Content-Type", "application/xml");
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "versioning", "");

    s3_http_response_t hres = s3_execute_signed(client, "PUT", uri, &hdrs, &qp, body, tstr_len(body));
    tstr_free(uri); tstr_free(body);
    S3Headers_drop(&hdrs); S3Headers_drop(&qp);

    s3_error_t err = S3_OK;
    if (!s3_is_ok(hres.error)) { err = hres.error; hres.error = S3_OK; }
    else if (hres.status_code < 200 || hres.status_code >= 300) err = s3_parse_error_xml(hres.body);
    s3_http_response_free(&hres);
    return err;
}

// ── Encryption ──

s3_sse_config_t s3_get_bucket_encryption(s3_client_t* client, const char* bucket, s3_error_t* err) {
    s3_sse_config_t res = {0};
    if (!client || !bucket) { if (err) *err = s3_error_make(-1, "Invalid params"); return res; }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "encryption", "");

    s3_http_response_t hres = s3_execute_signed(client, "GET", uri, NULL, &qp, NULL, 0);
    tstr_free(uri); S3Headers_drop(&qp);

    s3_error_t e = S3_OK;
    if (!s3_is_ok(hres.error)) { e = hres.error; hres.error = S3_OK; }
    else if (hres.status_code == 200) { res = s3_parse_encryption_xml(hres.body, &e); }
    else if (hres.status_code != 404) { e = s3_parse_error_xml(hres.body); }
    if (err) *err = e; else s3_error_free(&e);
    s3_http_response_free(&hres);
    return res;
}

s3_error_t s3_set_bucket_encryption(s3_client_t* client, const char* bucket, const s3_sse_config_t* config) {
    if (!client || !bucket || !config) return s3_error_make(-1, "Invalid params");

    s3_xml_builder_t* xb = s3_xml_new();
    s3_xml_open_ns(xb, "ServerSideEncryptionConfiguration", "http://s3.amazonaws.com/doc/2006-03-01/");
    s3_xml_open(xb, "Rule");
    s3_xml_open(xb, "ApplyServerSideEncryptionByDefault");
    if (config->is_kms) {
        s3_xml_elem(xb, "SSEAlgorithm", "aws:kms");
        if (config->kms_master_key_id) s3_xml_elem(xb, "KMSMasterKeyID", config->kms_master_key_id);
    } else {
        s3_xml_elem(xb, "SSEAlgorithm", "AES256");
    }
    s3_xml_close(xb, "ApplyServerSideEncryptionByDefault");
    s3_xml_close(xb, "Rule");
    s3_xml_close(xb, "ServerSideEncryptionConfiguration");
    tstr_t body = s3_xml_finish(xb);

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    S3Headers hdrs = S3Headers_init();
    s3_headers_add(&hdrs, "Content-Type", "application/xml");
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "encryption", "");

    s3_http_response_t hres = s3_execute_signed(client, "PUT", uri, &hdrs, &qp, body, tstr_len(body));
    tstr_free(uri); tstr_free(body);
    S3Headers_drop(&hdrs); S3Headers_drop(&qp);

    s3_error_t err = S3_OK;
    if (!s3_is_ok(hres.error)) { err = hres.error; hres.error = S3_OK; }
    else if (hres.status_code < 200 || hres.status_code >= 300) err = s3_parse_error_xml(hres.body);
    s3_http_response_free(&hres);
    return err;
}

s3_error_t s3_delete_bucket_encryption(s3_client_t* client, const char* bucket) {
    if (!client || !bucket) return s3_error_make(-1, "Invalid params");

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "encryption", "");

    s3_http_response_t hres = s3_execute_signed(client, "DELETE", uri, NULL, &qp, NULL, 0);
    tstr_free(uri); S3Headers_drop(&qp);

    s3_error_t err = S3_OK;
    if (!s3_is_ok(hres.error)) { err = hres.error; hres.error = S3_OK; }
    else if (hres.status_code < 200 || hres.status_code >= 300) err = s3_parse_error_xml(hres.body);
    s3_http_response_free(&hres);
    return err;
}

// ── Object Lock ──

s3_object_lock_config_t s3_get_object_lock_config(s3_client_t* client, const char* bucket, s3_error_t* err) {
    s3_object_lock_config_t res = {0};
    if (!client || !bucket) { if (err) *err = s3_error_make(-1, "Invalid params"); return res; }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "object-lock", "");

    s3_http_response_t hres = s3_execute_signed(client, "GET", uri, NULL, &qp, NULL, 0);
    tstr_free(uri); S3Headers_drop(&qp);

    s3_error_t e = S3_OK;
    if (!s3_is_ok(hres.error)) { e = hres.error; hres.error = S3_OK; }
    else if (hres.status_code == 200) { res = s3_parse_object_lock_xml(hres.body, &e); }
    else { e = s3_parse_error_xml(hres.body); }
    if (err) *err = e; else s3_error_free(&e);
    s3_http_response_free(&hres);
    return res;
}

s3_error_t s3_set_object_lock_config(s3_client_t* client, const char* bucket, const s3_object_lock_config_t* config) {
    if (!client || !bucket || !config) return s3_error_make(-1, "Invalid params");

    s3_xml_builder_t* xb = s3_xml_new();
    s3_xml_open_ns(xb, "ObjectLockConfiguration", "http://s3.amazonaws.com/doc/2006-03-01/");
    s3_xml_elem(xb, "ObjectLockEnabled", "Enabled");
    s3_xml_open(xb, "Rule");
    s3_xml_open(xb, "DefaultRetention");
    s3_xml_elem(xb, "Mode", config->mode == S3_RETENTION_COMPLIANCE ? "COMPLIANCE" : "GOVERNANCE");
    if (config->days > 0) s3_xml_elem_int(xb, "Days", config->days);
    if (config->years > 0) s3_xml_elem_int(xb, "Years", config->years);
    s3_xml_close(xb, "DefaultRetention");
    s3_xml_close(xb, "Rule");
    s3_xml_close(xb, "ObjectLockConfiguration");
    tstr_t body = s3_xml_finish(xb);

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    S3Headers hdrs = S3Headers_init();
    s3_headers_add(&hdrs, "Content-Type", "application/xml");
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "object-lock", "");

    s3_http_response_t hres = s3_execute_signed(client, "PUT", uri, &hdrs, &qp, body, tstr_len(body));
    tstr_free(uri); tstr_free(body);
    S3Headers_drop(&hdrs); S3Headers_drop(&qp);

    s3_error_t err = S3_OK;
    if (!s3_is_ok(hres.error)) { err = hres.error; hres.error = S3_OK; }
    else if (hres.status_code < 200 || hres.status_code >= 300) err = s3_parse_error_xml(hres.body);
    s3_http_response_free(&hres);
    return err;
}

s3_error_t s3_delete_object_lock_config(s3_client_t* client, const char* bucket) {
    if (!client || !bucket) return s3_error_make(-1, "Invalid params");

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "object-lock", "");

    s3_http_response_t hres = s3_execute_signed(client, "DELETE", uri, NULL, &qp, NULL, 0);
    tstr_free(uri); S3Headers_drop(&qp);

    s3_error_t err = S3_OK;
    if (!s3_is_ok(hres.error)) { err = hres.error; hres.error = S3_OK; }
    else if (hres.status_code < 200 || hres.status_code >= 300) err = s3_parse_error_xml(hres.body);
    s3_http_response_free(&hres);
    return err;
}
