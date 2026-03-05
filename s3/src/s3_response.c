#include "s3/s3_response.h"
#include "s3/s3_time.h"
#include "s3_xml_helpers.h"
#include <turbo_str.h>
#include <stdlib.h>
#include <string.h>

s3_error_t s3_parse_error_xml(const char* xml_data) {
    if (!xml_data || xml_data[0] == '\0') return s3_error_make(-1, "No XML data");

    // Skip leading whitespace
    const char* p = xml_data;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    // If it doesn't start with '<', it's not XML
    if (*p != '<') return s3_error_make(-1, xml_data);

    // Quick check: S3 error XML contains <Error> or <Code>. If not present, don't bother parsing.
    if (!strstr(xml_data, "<Error") && !strstr(xml_data, "<Code>")) {
        // Likely HTML or other non-S3 XML. Truncate for error message.
        size_t len = strlen(xml_data);
        if (len > 200) {
            tstr_t truncated = tstr_dup_len(xml_data, 200);
            s3_error_t err = s3_error_make(-1, truncated);
            tstr_free(truncated);
            return err;
        }
        return s3_error_make(-1, xml_data);
    }

    void* doc = cxml_load_string(xml_data);
    if (!doc) return s3_error_make(-1, xml_data);

    cxml_elem_node* root = cxml_get_root_element(doc);
    if (!root) {
        cxml_delete_document(doc);
        return s3_error_make(-1, "No root element in XML");
    }

    tstr_t code = mxml_child_text_dup(root, "Code");
    tstr_t message = mxml_child_text_dup(root, "Message");

    s3_error_t err = s3_error_make(-1, tstr_len(message) > 0 ? message : code);

    tstr_free(code);
    tstr_free(message);
    cxml_delete_document(doc);

    return err;
}

s3_list_buckets_parser_res_t s3_parse_list_buckets_xml(const char* xml_data) {
    s3_list_buckets_parser_res_t res = {0};
    res.buckets = S3BucketVec_init();
    
    void* doc = cxml_load_string(xml_data);
    if (!doc) {
        res.error = s3_error_make(-1, "Failed to parse XML");
        return res;
    }

    cxml_elem_node* root = cxml_get_root_element(doc);
    cxml_elem_node* buckets_node = cxml_find(root, "<Buckets>/");
    if (buckets_node) {
        cxml_list items;
        cxml_list_init(&items);
        cxml_find_all(buckets_node, "<Bucket>/", &items);
        
        cxml_for (node, &items) {
            cxml_elem_node* btnode = (cxml_elem_node*)node;
            s3_bucket_t b;
            b.name = mxml_child_text_dup(btnode, "Name");
            tstr_t date_str = mxml_child_text_dup(btnode, "CreationDate");
            // Placeholder: convert date_str to time_t
            b.creation_date = 0; 
            S3BucketVec_push(&res.buckets, b);
            tstr_free(date_str);
        }
        cxml_list_free(&items);
    }

    cxml_delete_document(doc);
    res.error = S3_OK;
    return res;
}

s3_list_objects_parser_res_t s3_parse_list_objects_xml(const char* xml_data) {
    s3_list_objects_parser_res_t res = {0};
    res.items = S3ItemVec_init();
    
    void* doc = cxml_load_string(xml_data);
    if (!doc) {
        res.error = s3_error_make(-1, "Failed to parse XML");
        return res;
    }

    cxml_elem_node* root = cxml_get_root_element(doc);
    if (!root) {
        cxml_delete_document(doc);
        res.error = s3_error_make(-1, "No root element in XML");
        return res;
    }

    tstr_t is_truncated_str = mxml_child_text_dup(root, "IsTruncated");
    res.is_truncated = (tstr_len(is_truncated_str) > 0 && strcmp(is_truncated_str, "true") == 0);
    tstr_free(is_truncated_str);

    res.next_continuation_token = mxml_child_text_dup(root, "NextContinuationToken");

    // Parse Contents
    cxml_list items;
    cxml_list_init(&items);
    cxml_find_all(root, "<Contents>/", &items);
    cxml_for(node, &items) {
        cxml_elem_node* inode = (cxml_elem_node*)node;
        s3_item_t item = {0};
        item.name = mxml_child_text_dup(inode, "Key");
        tstr_t size_str = mxml_child_text_dup(inode, "Size");
        item.size = (size_t)atoll(size_str);
        tstr_free(size_str);
        
        item.etag = mxml_child_text_dup(inode, "ETag");
        tstr_t lm_str = mxml_child_text_dup(inode, "LastModified");
        // Convert to time_t if needed, for now just placeholder
        item.last_modified = 0;
        tstr_free(lm_str);

        item.storage_class = mxml_child_text_dup(inode, "StorageClass");
        item.is_prefix = 0;

        S3ItemVec_push(&res.items, item);
    }
    cxml_list_free(&items);

    // Parse CommonPrefixes
    cxml_list_init(&items);
    cxml_find_all(root, "<CommonPrefixes>/", &items);
    cxml_for(pnode_loop, &items) {
        cxml_elem_node* pnode = (cxml_elem_node*)pnode_loop;
        s3_item_t item = {0};
        item.name = mxml_child_text_dup(pnode, "Prefix");
        item.is_prefix = 1;
        S3ItemVec_push(&res.items, item);
    }
    cxml_list_free(&items);

    cxml_delete_document(doc);
    res.error = S3_OK;
    return res;
}

void s3_list_objects_parser_res_free(s3_list_objects_parser_res_t* res) {
    if (!res) return;
    S3ItemVec_drop(&res->items);
    tstr_free(res->next_continuation_token);
    s3_error_free(&res->error);
    res->next_continuation_token = NULL;
}

tstr_t s3_parse_create_multipart_xml(const char* xml_data, s3_error_t* err) {
    if (!xml_data) { if (err) *err = s3_error_make(-1, "No XML data"); return NULL; }
    void* doc = cxml_load_string(xml_data);
    if (!doc) { if (err) *err = s3_error_make(-1, "Failed to parse XML"); return NULL; }
    cxml_elem_node* root = cxml_get_root_element(doc);
    tstr_t upload_id = mxml_child_text_dup(root, "UploadId");
    cxml_delete_document(doc);
    if (tstr_len(upload_id) == 0) {
        tstr_free(upload_id);
        if (err) *err = s3_error_make(-1, "UploadId not found in response");
        return NULL;
    }
    if (err) *err = S3_OK;
    return upload_id;
}

tstr_t s3_parse_complete_multipart_etag_xml(const char* xml_data, tstr_t* location, s3_error_t* err) {
    if (!xml_data) { if (err) *err = s3_error_make(-1, "No XML data"); return NULL; }
    
    /* S3 can return 200 OK but then an error in the body for CompleteMultipartUpload. */
    if (strstr(xml_data, "<Error")) {
        if (err) *err = s3_parse_error_xml(xml_data);
        return NULL;
    }

    void* doc = cxml_load_string(xml_data);
    if (!doc) { if (err) *err = s3_error_make(-1, "Failed to parse XML"); return NULL; }
    cxml_elem_node* root = cxml_get_root_element(doc);
    
    tstr_t etag = mxml_child_text_dup(root, "ETag");
    if (location) *location = mxml_child_text_dup(root, "Location");
    
    if (tstr_len(etag) == 0) {
        tstr_free(etag);
        etag = NULL;
        if (err) *err = s3_error_make(-1, "ETag not found in CompleteMultipartUpload response");
    } else {
        if (err) *err = S3_OK;
    }
    
    cxml_delete_document(doc);
    return etag;
}

tstr_t s3_parse_upload_part_copy_etag_xml(const char* xml_data, s3_error_t* err) {
    if (!xml_data) { if (err) *err = s3_error_make(-1, "No XML data"); return NULL; }
    void* doc = cxml_load_string(xml_data);
    if (!doc) { if (err) *err = s3_error_make(-1, "Failed to parse XML"); return NULL; }
    cxml_elem_node* root = cxml_get_root_element(doc);
    tstr_t etag = mxml_child_text_dup(root, "ETag");
    cxml_delete_document(doc);
    if (err) *err = S3_OK;
    return etag;
}

s3_delete_objects_parser_res_t s3_parse_delete_objects_xml(const char* xml_data) {
    s3_delete_objects_parser_res_t res = {0};
    if (!xml_data) { res.error = s3_error_make(-1, "No XML data"); return res; }
    void* doc = cxml_load_string(xml_data);
    if (!doc) { res.error = s3_error_make(-1, "Failed to parse XML"); return res; }

    cxml_elem_node* root = cxml_get_root_element(doc);
    cxml_list items;
    cxml_list_init(&items);
    cxml_find_all(root, "<Error>/", &items);

    int count = 0;
    cxml_for(n1, &items) { (void)n1; count++; }

    if (count > 0) {
        res.errors = calloc((size_t)count, sizeof(s3_delete_error_t));
        res.error_count = count;
        int idx = 0;
        cxml_for(n2, &items) {
            cxml_elem_node* enode = (cxml_elem_node*)n2;
            res.errors[idx].key = mxml_child_text_dup(enode, "Key");
            res.errors[idx].version_id = mxml_child_text_dup(enode, "VersionId");
            res.errors[idx].error_code = mxml_child_text_dup(enode, "Code");
            res.errors[idx].error_message = mxml_child_text_dup(enode, "Message");
            idx++;
        }
    }
    cxml_list_free(&items);
    cxml_delete_document(doc);
    res.error = S3_OK;
    return res;
}

void s3_delete_objects_parser_res_free(s3_delete_objects_parser_res_t* res) {
    if (!res) return;
    for (int i = 0; i < res->error_count; i++) {
        tstr_free(res->errors[i].key);
        tstr_free(res->errors[i].version_id);
        tstr_free(res->errors[i].error_code);
        tstr_free(res->errors[i].error_message);
    }
    free(res->errors);
    s3_error_free(&res->error);
    res->errors = NULL;
    res->error_count = 0;
}

s3_copy_object_parser_res_t s3_parse_copy_object_xml(const char* xml_data) {
    s3_copy_object_parser_res_t res = {0};
    if (!xml_data) { res.error = s3_error_make(-1, "No XML data"); return res; }
    void* doc = cxml_load_string(xml_data);
    if (!doc) { res.error = s3_error_make(-1, "Failed to parse XML"); return res; }
    cxml_elem_node* root = cxml_get_root_element(doc);
    res.etag = mxml_child_text_dup(root, "ETag");
    res.last_modified_str = mxml_child_text_dup(root, "LastModified");
    cxml_delete_document(doc);
    res.error = S3_OK;
    return res;
}

s3_tag_set_t s3_parse_tagging_xml(const char* xml_data, s3_error_t* err) {
    s3_tag_set_t res = {0};
    if (!xml_data) { if (err) *err = s3_error_make(-1, "No XML data"); return res; }
    void* doc = cxml_load_string(xml_data);
    if (!doc) { if (err) *err = s3_error_make(-1, "Failed to parse XML"); return res; }

    cxml_elem_node* root = cxml_get_root_element(doc);
    cxml_elem_node* tag_set = cxml_find(root, "<TagSet>/");
    if (tag_set) {
        cxml_list items;
        cxml_list_init(&items);
        cxml_find_all(tag_set, "<Tag>/", &items);
        int count = 0;
        cxml_for(t1, &items) { (void)t1; count++; }
        if (count > 0) {
            res.tags = calloc((size_t)count, sizeof(s3_tag_t));
            res.count = count;
            int idx = 0;
            cxml_for(t2, &items) {
                cxml_elem_node* tnode = (cxml_elem_node*)t2;
                res.tags[idx].key = mxml_child_text_dup(tnode, "Key");
                res.tags[idx].value = mxml_child_text_dup(tnode, "Value");
                idx++;
            }
        }
        cxml_list_free(&items);
    }
    cxml_delete_document(doc);
    if (err) *err = S3_OK;
    return res;
}

void s3_tag_set_free(s3_tag_set_t* tags) {
    if (!tags) return;
    for (int i = 0; i < tags->count; i++) {
        tstr_free(tags->tags[i].key);
        tstr_free(tags->tags[i].value);
    }
    free(tags->tags);
    tags->tags = NULL;
    tags->count = 0;
}

tstr_t s3_parse_versioning_xml(const char* xml_data, s3_error_t* err) {
    if (!xml_data) { if (err) *err = s3_error_make(-1, "No XML data"); return NULL; }
    void* doc = cxml_load_string(xml_data);
    if (!doc) { if (err) *err = s3_error_make(-1, "Failed to parse XML"); return NULL; }
    cxml_elem_node* root = cxml_get_root_element(doc);
    tstr_t status = mxml_child_text_dup(root, "Status");
    cxml_delete_document(doc);
    if (err) *err = S3_OK;
    return status;
}

s3_sse_config_t s3_parse_encryption_xml(const char* xml_data, s3_error_t* err) {
    s3_sse_config_t res = {0};
    if (!xml_data) { if (err) *err = s3_error_make(-1, "No XML data"); return res; }
    void* doc = cxml_load_string(xml_data);
    if (!doc) { if (err) *err = s3_error_make(-1, "Failed to parse XML"); return res; }
    cxml_elem_node* root = cxml_get_root_element(doc);
    cxml_elem_node* rule = cxml_find(root, "<Rule>/");
    if (rule) {
        cxml_elem_node* sse = cxml_find(rule, "<ApplyServerSideEncryptionByDefault>/");
        if (sse) {
            tstr_t algo = mxml_child_text_dup(sse, "SSEAlgorithm");
            if (algo && strcmp(algo, "aws:kms") == 0) {
                res.is_kms = 1;
                res.kms_master_key_id = mxml_child_text_dup(sse, "KMSMasterKeyID");
            }
            tstr_free(algo);
        }
    }
    cxml_delete_document(doc);
    if (err) *err = S3_OK;
    return res;
}

void s3_sse_config_free(s3_sse_config_t* config) {
    if (!config) return;
    tstr_free(config->kms_master_key_id);
    config->kms_master_key_id = NULL;
}

s3_object_lock_config_t s3_parse_object_lock_xml(const char* xml_data, s3_error_t* err) {
    s3_object_lock_config_t res = {0};
    if (!xml_data) { if (err) *err = s3_error_make(-1, "No XML data"); return res; }
    void* doc = cxml_load_string(xml_data);
    if (!doc) { if (err) *err = s3_error_make(-1, "Failed to parse XML"); return res; }
    cxml_elem_node* root = cxml_get_root_element(doc);

    tstr_t oe = mxml_child_text_dup(root, "ObjectLockEnabled");
    res.enabled = (oe && strcmp(oe, "Enabled") == 0);
    tstr_free(oe);

    cxml_elem_node* rule = cxml_find(root, "<Rule>/");
    if (rule) {
        cxml_elem_node* dr = cxml_find(rule, "<DefaultRetention>/");
        if (dr) {
            tstr_t mode = mxml_child_text_dup(dr, "Mode");
            if (mode && strcmp(mode, "COMPLIANCE") == 0) res.mode = S3_RETENTION_COMPLIANCE;
            else res.mode = S3_RETENTION_GOVERNANCE;
            tstr_free(mode);

            tstr_t days = mxml_child_text_dup(dr, "Days");
            if (tstr_len(days) > 0) res.days = atoi(days);
            tstr_free(days);

            tstr_t years = mxml_child_text_dup(dr, "Years");
            if (tstr_len(years) > 0) res.years = atoi(years);
            tstr_free(years);
        }
    }
    cxml_delete_document(doc);
    if (err) *err = S3_OK;
    return res;
}

s3_object_retention_t s3_parse_object_retention_xml(const char* xml_data, s3_error_t* err) {
    s3_object_retention_t res = {0};
    if (!xml_data) { if (err) *err = s3_error_make(-1, "No XML data"); return res; }
    void* doc = cxml_load_string(xml_data);
    if (!doc) { if (err) *err = s3_error_make(-1, "Failed to parse XML"); return res; }
    cxml_elem_node* root = cxml_get_root_element(doc);

    tstr_t mode = mxml_child_text_dup(root, "Mode");
    if (mode && strcmp(mode, "COMPLIANCE") == 0) res.mode = S3_RETENTION_COMPLIANCE;
    else res.mode = S3_RETENTION_GOVERNANCE;
    tstr_free(mode);

    tstr_t date_str = mxml_child_text_dup(root, "RetainUntilDate");
    if (tstr_len(date_str) > 0) res.retain_until_date = s3_time_from_iso8601(date_str);
    tstr_free(date_str);

    cxml_delete_document(doc);
    if (err) *err = S3_OK;
    return res;
}

int s3_parse_legal_hold_xml(const char* xml_data, s3_error_t* err) {
    if (!xml_data) { if (err) *err = s3_error_make(-1, "No XML data"); return 0; }
    void* doc = cxml_load_string(xml_data);
    if (!doc) { if (err) *err = s3_error_make(-1, "Failed to parse XML"); return 0; }
    cxml_elem_node* root = cxml_get_root_element(doc);
    tstr_t status = mxml_child_text_dup(root, "Status");
    int enabled = (status && strcmp(status, "ON") == 0);
    tstr_free(status);
    cxml_delete_document(doc);
    if (err) *err = S3_OK;
    return enabled;
}