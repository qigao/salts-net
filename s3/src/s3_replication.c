#include "s3/s3_replication.h"
#include "s3/s3_xml_builder.h"
#include "s3/s3_response.h"
#include "s3_http.h"
#include "s3_client_internal.h"
#include "s3_xml_helpers.h"
#include <cxml/cxml.h>
#include <stdlib.h>
#include <string.h>

void s3_replication_config_free(s3_replication_config_t* config) {
    if (!config) return;
    tstr_free(config->role);
    for (int i = 0; i < config->count; i++) {
        tstr_free(config->rules[i].id);
        tstr_free(config->rules[i].prefix);
        tstr_free(config->rules[i].destination_bucket_arn);
        tstr_free(config->rules[i].destination_storage_class);
    }
    free(config->rules);
    memset(config, 0, sizeof(s3_replication_config_t));
}

s3_replication_config_t s3_get_bucket_replication(s3_client_t* client, const char* bucket, s3_error_t* err) {
    s3_replication_config_t res = {0};
    if (!client || !bucket) { if (err) *err = s3_error_make(-1, "Invalid params"); return res; }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "replication", "");

    s3_http_response_t hres = s3_execute_signed(client, "GET", uri, NULL, &qp, NULL, 0);
    tstr_free(uri); S3Headers_drop(&qp);

    s3_error_t e = S3_OK;
    if (!s3_is_ok(hres.error)) { e = hres.error; hres.error = S3_OK; }
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
                res.rules = calloc((size_t)count, sizeof(s3_replication_rule_t));
                res.count = count;
                int idx = 0;
                cxml_for(n2, &items) {
                    cxml_elem_node* rnode = (cxml_elem_node*)n2;
                    s3_replication_rule_t* r = &res.rules[idx];
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
            e = s3_error_make(-1, "Failed to parse replication XML");
        }
    } else if (hres.status_code != 404) {
        e = s3_parse_error_xml(hres.body);
    }
    if (err) *err = e; else s3_error_free(&e);
    s3_http_response_free(&hres);
    return res;
}

s3_error_t s3_set_bucket_replication(s3_client_t* client, const char* bucket, const s3_replication_config_t* config) {
    if (!client || !bucket || !config) return s3_error_make(-1, "Invalid params");

    s3_xml_builder_t* xb = s3_xml_new();
    s3_xml_open_ns(xb, "ReplicationConfiguration", "http://s3.amazonaws.com/doc/2006-03-01/");
    if (config->role) s3_xml_elem(xb, "Role", config->role);

    for (int i = 0; i < config->count; i++) {
        const s3_replication_rule_t* r = &config->rules[i];
        s3_xml_open(xb, "Rule");
        if (r->id) s3_xml_elem(xb, "ID", r->id);
        if (r->prefix) s3_xml_elem(xb, "Prefix", r->prefix);
        s3_xml_elem(xb, "Status", r->enabled ? "Enabled" : "Disabled");

        s3_xml_open(xb, "Destination");
        if (r->destination_bucket_arn) s3_xml_elem(xb, "Bucket", r->destination_bucket_arn);
        if (r->destination_storage_class) s3_xml_elem(xb, "StorageClass", r->destination_storage_class);
        s3_xml_close(xb, "Destination");

        if (r->replicate_delete_markers) {
            s3_xml_open(xb, "DeleteMarkerReplication");
            s3_xml_elem(xb, "Status", "Enabled");
            s3_xml_close(xb, "DeleteMarkerReplication");
        }
        s3_xml_close(xb, "Rule");
    }
    s3_xml_close(xb, "ReplicationConfiguration");
    tstr_t body = s3_xml_finish(xb);

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    S3Headers hdrs = S3Headers_init();
    s3_headers_add(&hdrs, "Content-Type", "application/xml");
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "replication", "");

    s3_http_response_t hres = s3_execute_signed(client, "PUT", uri, &hdrs, &qp, body, tstr_len(body));
    tstr_free(uri); tstr_free(body);
    S3Headers_drop(&hdrs); S3Headers_drop(&qp);

    s3_error_t err = S3_OK;
    if (!s3_is_ok(hres.error)) { err = hres.error; hres.error = S3_OK; }
    else if (hres.status_code < 200 || hres.status_code >= 300) err = s3_parse_error_xml(hres.body);
    s3_http_response_free(&hres);
    return err;
}

s3_error_t s3_delete_bucket_replication(s3_client_t* client, const char* bucket) {
    if (!client || !bucket) return s3_error_make(-1, "Invalid params");

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    S3Headers qp = S3Headers_init();
    s3_headers_add(&qp, "replication", "");

    s3_http_response_t hres = s3_execute_signed(client, "DELETE", uri, NULL, &qp, NULL, 0);
    tstr_free(uri); S3Headers_drop(&qp);

    s3_error_t err = S3_OK;
    if (!s3_is_ok(hres.error)) { err = hres.error; hres.error = S3_OK; }
    else if (hres.status_code < 200 || hres.status_code >= 300) err = s3_parse_error_xml(hres.body);
    s3_http_response_free(&hres);
    return err;
}
