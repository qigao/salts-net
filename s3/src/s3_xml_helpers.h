#ifndef S3_XML_HELPERS_H
#define S3_XML_HELPERS_H

#include <cxml/cxml.h>
#include <turbo_str.h>
#include <string.h>
#include <stdlib.h>

static inline char* mxml_child_text(cxml_elem_node* parent, const char* name) {
    if (!parent || !name) return NULL;
    cxml_for (node, &parent->children) {
        if (cxml_get_node_type(node) == CXML_ELEM_NODE) {
            cxml_elem_node* elem = (cxml_elem_node*)node;
            // S3 responses often have a default namespace. 
            // Comparing the local name (lname) is more robust than qname.
            if (elem->name.lname && strcmp(elem->name.lname, name) == 0) {
                return cxml_text(elem, NULL);
            }
        }
    }
    return NULL;
}

static inline tstr_t mxml_child_text_dup(cxml_elem_node* parent, const char* name) {
    char* t = mxml_child_text(parent, name);
    if (!t) return tstr_new();
    tstr_t result = tstr_dup(t);
    free(t);
    return result;
}

#endif // S3_XML_HELPERS_H
