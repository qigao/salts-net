#include "minio/minio_lifecycle.h"
#include "minio/minio_xml_builder.h"
#include "minio/minio_response.h"
#include "minio_http.h"
#include "minio_client_internal.h"
#include "minio_xml_helpers.h"
#include <cxml/cxml.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void minio_lifecycle_config_free(minio_lifecycle_config_t* config) {
    if (!config) return;
    for (int i = 0; i < config->count; i++) {
        tstr_free(config->rules[i].id);
        tstr_free(config->rules[i].prefix);
        tstr_free(config->rules[i].transition_storage_class);
        tstr_free(config->rules[i].noncurrent_transition_storage_class);
    }
    free(config->rules);
    config->rules = NULL;
    config->count = 0;
}

tstr_t minio_lifecycle_config_to_xml(const minio_lifecycle_config_t* config) {
    if (!config) return tstr_new();

    minio_xml_builder_t* xb = minio_xml_new();
    minio_xml_open_ns(xb, "LifecycleConfiguration", "http://s3.amazonaws.com/doc/2006-03-01/");

    for (int i = 0; i < config->count; i++) {
        const minio_lifecycle_rule_t* r = &config->rules[i];
        minio_xml_open(xb, "Rule");
        if (r->id) minio_xml_elem(xb, "ID", r->id);

        minio_xml_open(xb, "Filter");
        if (r->prefix && r->prefix[0]) minio_xml_elem(xb, "Prefix", r->prefix);
        else minio_xml_elem(xb, "Prefix", "");
        minio_xml_close(xb, "Filter");

        minio_xml_elem(xb, "Status", r->enabled ? "Enabled" : "Disabled");

        if (r->expiration_days > 0) {
            minio_xml_open(xb, "Expiration");
            minio_xml_elem_int(xb, "Days", r->expiration_days);
            minio_xml_close(xb, "Expiration");
        }
        if (r->transition_days > 0 && r->transition_storage_class) {
            minio_xml_open(xb, "Transition");
            minio_xml_elem_int(xb, "Days", r->transition_days);
            minio_xml_elem(xb, "StorageClass", r->transition_storage_class);
            minio_xml_close(xb, "Transition");
        }
        if (r->noncurrent_expiration_days > 0) {
            minio_xml_open(xb, "NoncurrentVersionExpiration");
            minio_xml_elem_int(xb, "NoncurrentDays", r->noncurrent_expiration_days);
            minio_xml_close(xb, "NoncurrentVersionExpiration");
        }
        if (r->noncurrent_transition_days > 0 && r->noncurrent_transition_storage_class) {
            minio_xml_open(xb, "NoncurrentVersionTransition");
            minio_xml_elem_int(xb, "NoncurrentDays", r->noncurrent_transition_days);
            minio_xml_elem(xb, "StorageClass", r->noncurrent_transition_storage_class);
            minio_xml_close(xb, "NoncurrentVersionTransition");
        }
        if (r->abort_incomplete_days > 0) {
            minio_xml_open(xb, "AbortIncompleteMultipartUpload");
            minio_xml_elem_int(xb, "DaysAfterInitiation", r->abort_incomplete_days);
            minio_xml_close(xb, "AbortIncompleteMultipartUpload");
        }
        minio_xml_close(xb, "Rule");
    }
    minio_xml_close(xb, "LifecycleConfiguration");
    return minio_xml_finish(xb);
}

minio_lifecycle_config_t minio_lifecycle_config_from_xml(const char* xml, minio_error_t* err) {
    minio_lifecycle_config_t res = {0};
    if (!xml) { if (err) *err = minio_error_make(-1, "No XML data"); return res; }

    void* doc = cxml_load_string(xml);
    if (!doc) { if (err) *err = minio_error_make(-1, "Failed to parse XML"); return res; }

    cxml_elem_node* root = cxml_get_root_element(doc);
    cxml_list items;
    cxml_list_init(&items);
    cxml_find_all(root, "<Rule>/", &items);

    int count = 0;
    cxml_for(n1, &items) { (void)n1; count++; }

    if (count > 0) {
        res.rules = calloc((size_t)count, sizeof(minio_lifecycle_rule_t));
        res.count = count;
        int idx = 0;
        cxml_for(n2, &items) {
            cxml_elem_node* rnode = (cxml_elem_node*)n2;
            minio_lifecycle_rule_t* r = &res.rules[idx];
            r->id = mxml_child_text_dup(rnode, "ID");
            tstr_t status = mxml_child_text_dup(rnode, "Status");
            r->enabled = (status && strcmp(status, "Enabled") == 0);
            tstr_free(status);

            // Filter/Prefix
            cxml_elem_node* filter = cxml_find(rnode, "<Filter>/");
            if (filter) r->prefix = mxml_child_text_dup(filter, "Prefix");
            else r->prefix = mxml_child_text_dup(rnode, "Prefix");

            // Expiration
            cxml_elem_node* exp = cxml_find(rnode, "<Expiration>/");
            if (exp) {
                tstr_t days = mxml_child_text_dup(exp, "Days");
                if (tstr_len(days) > 0) r->expiration_days = atoi(days);
                tstr_free(days);
            }

            // Transition
            cxml_elem_node* trans = cxml_find(rnode, "<Transition>/");
            if (trans) {
                tstr_t days = mxml_child_text_dup(trans, "Days");
                if (tstr_len(days) > 0) r->transition_days = atoi(days);
                tstr_free(days);
                r->transition_storage_class = mxml_child_text_dup(trans, "StorageClass");
            }

            // NoncurrentVersionExpiration
            cxml_elem_node* nve = cxml_find(rnode, "<NoncurrentVersionExpiration>/");
            if (nve) {
                tstr_t days = mxml_child_text_dup(nve, "NoncurrentDays");
                if (tstr_len(days) > 0) r->noncurrent_expiration_days = atoi(days);
                tstr_free(days);
            }

            // NoncurrentVersionTransition
            cxml_elem_node* nvt = cxml_find(rnode, "<NoncurrentVersionTransition>/");
            if (nvt) {
                tstr_t days = mxml_child_text_dup(nvt, "NoncurrentDays");
                if (tstr_len(days) > 0) r->noncurrent_transition_days = atoi(days);
                tstr_free(days);
                r->noncurrent_transition_storage_class = mxml_child_text_dup(nvt, "StorageClass");
            }

            // AbortIncompleteMultipartUpload
            cxml_elem_node* aimu = cxml_find(rnode, "<AbortIncompleteMultipartUpload>/");
            if (aimu) {
                tstr_t days = mxml_child_text_dup(aimu, "DaysAfterInitiation");
                if (tstr_len(days) > 0) r->abort_incomplete_days = atoi(days);
                tstr_free(days);
            }
            idx++;
        }
    }
    cxml_list_free(&items);
    cxml_delete_document(doc);
    if (err) *err = MINIO_OK;
    return res;
}

minio_lifecycle_config_t minio_get_bucket_lifecycle(minio_client_t* client, const char* bucket, minio_error_t* err) {
    minio_lifecycle_config_t res = {0};
    if (!client || !bucket) { if (err) *err = minio_error_make(-1, "Invalid params"); return res; }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "lifecycle", "");

    minio_http_response_t hres = minio_execute_signed(client, "GET", uri, NULL, &qp, NULL, 0);
    tstr_free(uri); MinioHeaders_drop(&qp);

    minio_error_t e = MINIO_OK;
    if (!minio_is_ok(hres.error)) { e = hres.error; hres.error = MINIO_OK; }
    else if (hres.status_code == 200) { res = minio_lifecycle_config_from_xml(hres.body, &e); }
    else if (hres.status_code != 404) { e = minio_parse_error_xml(hres.body); }
    if (err) *err = e; else minio_error_free(&e);
    minio_http_response_free(&hres);
    return res;
}

minio_error_t minio_set_bucket_lifecycle(minio_client_t* client, const char* bucket, const minio_lifecycle_config_t* config) {
    if (!client || !bucket || !config) return minio_error_make(-1, "Invalid params");

    tstr_t body = minio_lifecycle_config_to_xml(config);
    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    MinioHeaders hdrs = MinioHeaders_init();
    minio_headers_add(&hdrs, "Content-Type", "application/xml");
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "lifecycle", "");

    minio_http_response_t hres = minio_execute_signed(client, "PUT", uri, &hdrs, &qp, body, tstr_len(body));
    tstr_free(uri); tstr_free(body);
    MinioHeaders_drop(&hdrs); MinioHeaders_drop(&qp);

    minio_error_t err = MINIO_OK;
    if (!minio_is_ok(hres.error)) { err = hres.error; hres.error = MINIO_OK; }
    else if (hres.status_code < 200 || hres.status_code >= 300) err = minio_parse_error_xml(hres.body);
    minio_http_response_free(&hres);
    return err;
}

minio_error_t minio_delete_bucket_lifecycle(minio_client_t* client, const char* bucket) {
    if (!client || !bucket) return minio_error_make(-1, "Invalid params");

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "lifecycle", "");

    minio_http_response_t hres = minio_execute_signed(client, "DELETE", uri, NULL, &qp, NULL, 0);
    tstr_free(uri); MinioHeaders_drop(&qp);

    minio_error_t err = MINIO_OK;
    if (!minio_is_ok(hres.error)) { err = hres.error; hres.error = MINIO_OK; }
    else if (hres.status_code < 200 || hres.status_code >= 300) err = minio_parse_error_xml(hres.body);
    minio_http_response_free(&hres);
    return err;
}
