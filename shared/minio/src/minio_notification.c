#include "minio/minio_notification.h"
#include "minio/minio_xml_builder.h"
#include "minio/minio_response.h"
#include "minio_http.h"
#include "minio_client_internal.h"
#include "minio_xml_helpers.h"
#include <cxml/cxml.h>
#include <stdlib.h>
#include <string.h>

static void free_notification_rules(minio_notification_rule_t* rules, int count) {
    if (!rules) return;
    for (int i = 0; i < count; i++) {
        tstr_free(rules[i].id);
        tstr_free(rules[i].arn);
        for (int j = 0; j < rules[i].event_count; j++) tstr_free(rules[i].events[j]);
        free(rules[i].events);
        tstr_free(rules[i].prefix_filter);
        tstr_free(rules[i].suffix_filter);
    }
    free(rules);
}

void minio_notification_config_free(minio_notification_config_t* config) {
    if (!config) return;
    free_notification_rules(config->queue_configs, config->queue_count);
    free_notification_rules(config->topic_configs, config->topic_count);
    free_notification_rules(config->cloud_func_configs, config->cloud_func_count);
    memset(config, 0, sizeof(minio_notification_config_t));
}

static int parse_notification_rules(cxml_elem_node* root, const char* config_tag, const char* arn_tag,
                                     minio_notification_rule_t** out_rules) {
    cxml_list items;
    cxml_list_init(&items);
    cxml_find_all(root, config_tag, &items);

    int count = 0;
    cxml_for(n1, &items) { (void)n1; count++; }
    if (count == 0) { cxml_list_free(&items); return 0; }

    *out_rules = calloc((size_t)count, sizeof(minio_notification_rule_t));
    int idx = 0;
    cxml_for(n2, &items) {
        cxml_elem_node* rnode = (cxml_elem_node*)n2;
        minio_notification_rule_t* r = &(*out_rules)[idx];
        r->id = mxml_child_text_dup(rnode, "Id");
        r->arn = mxml_child_text_dup(rnode, arn_tag);

        // Events
        cxml_list evts;
        cxml_list_init(&evts);
        cxml_find_all(rnode, "<Event>/", &evts);
        int ec = 0;
        cxml_for(e1, &evts) { (void)e1; ec++; }
        if (ec > 0) {
            r->events = calloc((size_t)ec, sizeof(tstr_t));
            r->event_count = ec;
            int ei = 0;
            cxml_for(e2, &evts) {
                char* t = cxml_text((cxml_elem_node*)e2, NULL);
                r->events[ei++] = t ? tstr_dup(t) : tstr_new();
                if (t) free(t);
            }
        }
        cxml_list_free(&evts);

        // Filter
        cxml_elem_node* filter = cxml_find(rnode, "<Filter>/");
        if (filter) {
            cxml_elem_node* s3key = cxml_find(filter, "<S3Key>/");
            if (s3key) {
                cxml_list frules;
                cxml_list_init(&frules);
                cxml_find_all(s3key, "<FilterRule>/", &frules);
                cxml_for(fn, &frules) {
                    cxml_elem_node* fnode = (cxml_elem_node*)fn;
                    tstr_t name = mxml_child_text_dup(fnode, "Name");
                    tstr_t value = mxml_child_text_dup(fnode, "Value");
                    if (name && strcmp(name, "prefix") == 0) { tstr_free(r->prefix_filter); r->prefix_filter = value; value = NULL; }
                    else if (name && strcmp(name, "suffix") == 0) { tstr_free(r->suffix_filter); r->suffix_filter = value; value = NULL; }
                    tstr_free(name);
                    tstr_free(value);
                }
                cxml_list_free(&frules);
            }
        }
        idx++;
    }
    cxml_list_free(&items);
    return count;
}

static void write_notification_rules_xml(minio_xml_builder_t* xb, const char* config_tag, const char* arn_tag,
                                          const minio_notification_rule_t* rules, int count) {
    for (int i = 0; i < count; i++) {
        const minio_notification_rule_t* r = &rules[i];
        minio_xml_open(xb, config_tag);
        if (r->id) minio_xml_elem(xb, "Id", r->id);
        if (r->arn) minio_xml_elem(xb, arn_tag, r->arn);
        for (int j = 0; j < r->event_count; j++) {
            minio_xml_elem(xb, "Event", r->events[j]);
        }
        if ((r->prefix_filter && r->prefix_filter[0]) || (r->suffix_filter && r->suffix_filter[0])) {
            minio_xml_open(xb, "Filter");
            minio_xml_open(xb, "S3Key");
            if (r->prefix_filter && r->prefix_filter[0]) {
                minio_xml_open(xb, "FilterRule");
                minio_xml_elem(xb, "Name", "prefix");
                minio_xml_elem(xb, "Value", r->prefix_filter);
                minio_xml_close(xb, "FilterRule");
            }
            if (r->suffix_filter && r->suffix_filter[0]) {
                minio_xml_open(xb, "FilterRule");
                minio_xml_elem(xb, "Name", "suffix");
                minio_xml_elem(xb, "Value", r->suffix_filter);
                minio_xml_close(xb, "FilterRule");
            }
            minio_xml_close(xb, "S3Key");
            minio_xml_close(xb, "Filter");
        }
        minio_xml_close(xb, config_tag);
    }
}

minio_notification_config_t minio_get_bucket_notification(minio_client_t* client, const char* bucket, minio_error_t* err) {
    minio_notification_config_t res = {0};
    if (!client || !bucket) { if (err) *err = minio_error_make(-1, "Invalid params"); return res; }

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "notification", "");

    minio_http_response_t hres = minio_execute_signed(client, "GET", uri, NULL, &qp, NULL, 0);
    tstr_free(uri); MinioHeaders_drop(&qp);

    minio_error_t e = MINIO_OK;
    if (!minio_is_ok(hres.error)) { e = hres.error; hres.error = MINIO_OK; }
    else if (hres.status_code == 200) {
        void* doc = cxml_load_string(hres.body);
        if (doc) {
            cxml_elem_node* root = cxml_get_root_element(doc);
            res.queue_count = parse_notification_rules(root, "<QueueConfiguration>/", "Queue", &res.queue_configs);
            res.topic_count = parse_notification_rules(root, "<TopicConfiguration>/", "Topic", &res.topic_configs);
            res.cloud_func_count = parse_notification_rules(root, "<CloudFunctionConfiguration>/", "CloudFunction", &res.cloud_func_configs);
            cxml_delete_document(doc);
        } else {
            e = minio_error_make(-1, "Failed to parse notification XML");
        }
    } else {
        e = minio_parse_error_xml(hres.body);
    }
    if (err) *err = e; else minio_error_free(&e);
    minio_http_response_free(&hres);
    return res;
}

minio_error_t minio_set_bucket_notification(minio_client_t* client, const char* bucket, const minio_notification_config_t* config) {
    if (!client || !bucket || !config) return minio_error_make(-1, "Invalid params");

    minio_xml_builder_t* xb = minio_xml_new();
    minio_xml_open_ns(xb, "NotificationConfiguration", "http://s3.amazonaws.com/doc/2006-03-01/");
    write_notification_rules_xml(xb, "QueueConfiguration", "Queue", config->queue_configs, config->queue_count);
    write_notification_rules_xml(xb, "TopicConfiguration", "Topic", config->topic_configs, config->topic_count);
    write_notification_rules_xml(xb, "CloudFunctionConfiguration", "CloudFunction", config->cloud_func_configs, config->cloud_func_count);
    minio_xml_close(xb, "NotificationConfiguration");
    tstr_t body = minio_xml_finish(xb);

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    MinioHeaders hdrs = MinioHeaders_init();
    minio_headers_add(&hdrs, "Content-Type", "application/xml");
    MinioHeaders qp = MinioHeaders_init();
    minio_headers_add(&qp, "notification", "");

    minio_http_response_t hres = minio_execute_signed(client, "PUT", uri, &hdrs, &qp, body, tstr_len(body));
    tstr_free(uri); tstr_free(body);
    MinioHeaders_drop(&hdrs); MinioHeaders_drop(&qp);

    minio_error_t err = MINIO_OK;
    if (!minio_is_ok(hres.error)) { err = hres.error; hres.error = MINIO_OK; }
    else if (hres.status_code < 200 || hres.status_code >= 300) err = minio_parse_error_xml(hres.body);
    minio_http_response_free(&hres);
    return err;
}

minio_error_t minio_delete_bucket_notification(minio_client_t* client, const char* bucket) {
    // S3 doesn't have a DELETE for notification; send empty config
    minio_notification_config_t empty = {0};
    return minio_set_bucket_notification(client, bucket, &empty);
}

minio_error_t minio_listen_bucket_notification(
    minio_client_t* client, const char* bucket,
    const char* prefix, const char* suffix,
    const char** events, int event_count,
    minio_notification_callback_t callback, void* userdata) {
    if (!client || !bucket || !callback) return minio_error_make(-1, "Invalid params");

    tstr_t uri = tstr_cat_fmt(tstr_new(), "/%s", bucket);
    MinioHeaders qp = MinioHeaders_init();
    if (prefix) minio_headers_add(&qp, "prefix", prefix);
    if (suffix) minio_headers_add(&qp, "suffix", suffix);
    for (int i = 0; i < event_count; i++) {
        minio_headers_add(&qp, "events", events[i]);
    }

    minio_http_response_t hres = minio_execute_signed(client, "GET", uri, NULL, &qp, NULL, 0);
    tstr_free(uri); MinioHeaders_drop(&qp);

    minio_error_t err = MINIO_OK;
    if (!minio_is_ok(hres.error)) {
        err = hres.error; hres.error = MINIO_OK;
    } else if (hres.status_code == 200 && hres.body) {
        // SSE response: each line is a JSON event
        const char* p = hres.body;
        while (*p) {
            const char* nl = strchr(p, '\n');
            if (!nl) nl = p + strlen(p);
            if (nl > p) {
                tstr_t line = tstr_dup_len(p, (size_t)(nl - p));
                if (tstr_len(line) > 0 && line[0] == '{') {
                    int cont = callback(line, userdata);
                    if (!cont) { tstr_free(line); break; }
                }
                tstr_free(line);
            }
            p = *nl ? nl + 1 : nl;
        }
    } else if (hres.status_code != 200) {
        err = minio_parse_error_xml(hres.body);
    }
    minio_http_response_free(&hres);
    return err;
}
