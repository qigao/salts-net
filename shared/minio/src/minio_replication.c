#include "minio/minio_replication.h"
#include "minio/minio_xml_builder.h"
#include "minio/minio_response.h"
#include "minio_http.h"
#include "minio_client_internal.h"
#include "minio_xml_helpers.h"
#include <cxml/cxml.h>
#include <stdlib.h>
#include <string.h>

void minio_replication_config_free(minio_replication_config_t* config) {
    if (!config) return;
    tstr_free(config->role);
    for (int i = 0; i < config->count; i++) {
        tstr_free(config->rules[i].id);
        tstr_free(config->rules[i].prefix);
        tstr_free(config->rules[i].destination_bucket_arn);
        tstr_free(config->rules[i].destination_storage_class);
    }
    free(config->rules);
    memset(config, 0, sizeof(minio_replication_config_t));
}

minio_replication_config_t minio_get_bucket_replication(minio_client_t* client, const char* bucket, minio_error_t* err) {
    minio_replication_config_t res = {0};
    if (!client || !bucket) { if (err) *err = minio_error_make(-1, "Invalid params"); return res; }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "replication", "");

    minio_http_response_t hres = minio_execute_signed(client, "GET", uri, NULL, &qp, NULL, 0);
    tstr_free(uri); MinioHeaders_drop(&qp);

    minio_error_t e = MINIO_OK;
    if (!minio_is_ok(hres.error)) { e = hres.error; hres.error = MINIO_OK; }
    else if (hres.status_code == 200) {
        void* doc = cxml_load_string(hres.body);
        if (doc) {
            cxml_elem_node* root = cxml_get_root_element(doc);
            res.role = mxml_child_text_dup(root, "Role");

            cxml_list items;
            cxml_list_init(&items);
            cxml_find_all(root, "<Rule>/", &items);
            int count = 0;
            cxml_for(n1, &items) { (void)n1; count++; }

            if (count > 0) {
                res.rules = calloc((size_t)count, sizeof(minio_replication_rule_t));
                res.count = count;
                int idx = 0;
                cxml_for(n2, &items) {
                    cxml_elem_node* rnode = (cxml_elem_node*)n2;
                    minio_replication_rule_t* r = &res.rules[idx];
                    r->id = mxml_child_text_dup(rnode, "ID");
                    r->prefix = mxml_child_text_dup(rnode, "Prefix");
                    tstr_t status = mxml_child_text_dup(rnode, "Status");
                    r->enabled = (status && strcmp(status, "Enabled") == 0);
                    tstr_free(status);

                    cxml_elem_node* dest = cxml_find(rnode, "<Destination>/");
                    if (dest) {
                        r->destination_bucket_arn = mxml_child_text_dup(dest, "Bucket");
                        r->destination_storage_class = mxml_child_text_dup(dest, "StorageClass");
                    }

                    cxml_elem_node* dmc = cxml_find(rnode, "<DeleteMarkerReplication>/");
                    if (dmc) {
                        tstr_t s = mxml_child_text_dup(dmc, "Status");
                        r->replicate_delete_markers = (s && strcmp(s, "Enabled") == 0);
                        tstr_free(s);
                    }
                    idx++;
                }
            }
            cxml_list_free(&items);
            cxml_delete_document(doc);
        } else {
            e = minio_error_make(-1, "Failed to parse replication XML");
        }
    } else if (hres.status_code != 404) {
        e = minio_parse_error_xml(hres.body);
    }
    if (err) *err = e; else minio_error_free(&e);
    minio_http_response_free(&hres);
    return res;
}

minio_error_t minio_set_bucket_replication(minio_client_t* client, const char* bucket, const minio_replication_config_t* config) {
    if (!client || !bucket || !config) return minio_error_make(-1, "Invalid params");

    minio_xml_builder_t* xb = minio_xml_new();
    minio_xml_open_ns(xb, "ReplicationConfiguration", "http://s3.amazonaws.com/doc/2006-03-01/");
    if (config->role) minio_xml_elem(xb, "Role", config->role);

    for (int i = 0; i < config->count; i++) {
        const minio_replication_rule_t* r = &config->rules[i];
        minio_xml_open(xb, "Rule");
        if (r->id) minio_xml_elem(xb, "ID", r->id);
        if (r->prefix) minio_xml_elem(xb, "Prefix", r->prefix);
        minio_xml_elem(xb, "Status", r->enabled ? "Enabled" : "Disabled");

        minio_xml_open(xb, "Destination");
        if (r->destination_bucket_arn) minio_xml_elem(xb, "Bucket", r->destination_bucket_arn);
        if (r->destination_storage_class) minio_xml_elem(xb, "StorageClass", r->destination_storage_class);
        minio_xml_close(xb, "Destination");

        if (r->replicate_delete_markers) {
            minio_xml_open(xb, "DeleteMarkerReplication");
            minio_xml_elem(xb, "Status", "Enabled");
            minio_xml_close(xb, "DeleteMarkerReplication");
        }
        minio_xml_close(xb, "Rule");
    }
    minio_xml_close(xb, "ReplicationConfiguration");
    tstr_t body = minio_xml_finish(xb);

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    MinioHeaders hdrs = MinioHeaders_init();
    minio_headers_add(&hdrs, "Content-Type", "application/xml");
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "replication", "");

    minio_http_response_t hres = minio_execute_signed(client, "PUT", uri, &hdrs, &qp, body, tstr_len(body));
    tstr_free(uri); tstr_free(body);
    MinioHeaders_drop(&hdrs); MinioHeaders_drop(&qp);

    minio_error_t err = MINIO_OK;
    if (!minio_is_ok(hres.error)) { err = hres.error; hres.error = MINIO_OK; }
    else if (hres.status_code < 200 || hres.status_code >= 300) err = minio_parse_error_xml(hres.body);
    minio_http_response_free(&hres);
    return err;
}

minio_error_t minio_delete_bucket_replication(minio_client_t* client, const char* bucket) {
    if (!client || !bucket) return minio_error_make(-1, "Invalid params");

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "replication", "");

    minio_http_response_t hres = minio_execute_signed(client, "DELETE", uri, NULL, &qp, NULL, 0);
    tstr_free(uri); MinioHeaders_drop(&qp);

    minio_error_t err = MINIO_OK;
    if (!minio_is_ok(hres.error)) { err = hres.error; hres.error = MINIO_OK; }
    else if (hres.status_code < 200 || hres.status_code >= 300) err = minio_parse_error_xml(hres.body);
    minio_http_response_free(&hres);
    return err;
}
