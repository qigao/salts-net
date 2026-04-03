#ifndef S3_XML_HELPERS_H
#define S3_XML_HELPERS_H

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <turbo_parser.h>
#include <turbo_str.h>

typedef turbo_xml_doc_t s3_xml_doc_t;
typedef turbo_xml_node_t s3_xml_node_t;
typedef turbo_xml_list_node_t s3_xml_list_node_t;
typedef turbo_xml_list_t s3_xml_list_t;

#define s3_xml_for turbo_xml_for
#define s3_xml_free turbo_free_xml
#define s3_xml_root turbo_xml_root_element
#define s3_xml_list_init turbo_xml_list_init
#define s3_xml_list_free turbo_xml_list_free
#define s3_xml_find turbo_xml_find
#define s3_xml_find_all turbo_xml_find_all
#define s3_xml_text_dup turbo_xml_text_dup

static inline int s3_xml_parse(const char *xml, s3_xml_doc_t **out_doc) {
    return turbo_parse_xml((const uint8_t *)xml, xml ? strlen(xml) : 0, out_doc);
}

static inline tstr_t s3_xml_child_text_dup(s3_xml_node_t *parent, const char *name) {
    char *text = turbo_xml_child_text_dup(parent, name);
    tstr_t result = text ? tstr_dup(text) : tstr_new();
    free(text);
    return result;
}

#endif // S3_XML_HELPERS_H
