#include "s3/s3_xml_builder.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct s3_xml_builder_s {
    tstr_t buf;
};

s3_xml_builder_t* s3_xml_new(void) {
    s3_xml_builder_t* b = calloc(1, sizeof(s3_xml_builder_t));
    if (!b) return NULL;
    b->buf = tstr_dup("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    return b;
}

static void xml_escape_append(tstr_t* buf, const char* text, int escape_quotes) {
    for (const char* p = text; *p; p++) {
        switch (*p) {
            case '&':  *buf = tstr_cat(*buf, "&amp;");  break;
            case '<':  *buf = tstr_cat(*buf, "&lt;");   break;
            case '>':  *buf = tstr_cat(*buf, "&gt;");   break;
            case '"':  
                if (escape_quotes) *buf = tstr_cat(*buf, "&quot;");
                else { char c[2] = {*p, 0}; *buf = tstr_cat(*buf, c); }
                break;
            case '\'': 
                if (escape_quotes) *buf = tstr_cat(*buf, "&apos;");
                else { char c[2] = {*p, 0}; *buf = tstr_cat(*buf, c); }
                break;
            default: {
                char c[2] = {*p, 0};
                *buf = tstr_cat(*buf, c);
                break;
            }
        }
    }
}

void s3_xml_open(s3_xml_builder_t* b, const char* tag) {
    if (!b || !tag) return;
    b->buf = tstr_cat_fmt(b->buf, "<%s>", tag);
}

void s3_xml_open_ns(s3_xml_builder_t* b, const char* tag, const char* ns) {
    if (!b || !tag || !ns) return;
    b->buf = tstr_cat_fmt(b->buf, "<%s xmlns=\"%s\">", tag, ns);
}

void s3_xml_attr(s3_xml_builder_t* b, const char* name, const char* value) {
    if (!b || !name || !value) return;
    // Replace the trailing '>' of the last open tag with the attribute
    size_t len = tstr_len(b->buf);
    if (len > 0 && b->buf[len - 1] == '>') {
        b->buf[len - 1] = '\0'; // remove '>'
        // tstr_len is now stale, but tstr_cat will handle it
        b->buf = tstr_cat_fmt(b->buf, " %s=\"", name);
        xml_escape_append(&b->buf, value, 1);
        b->buf = tstr_cat(b->buf, "\">");
    }
}

void s3_xml_text(s3_xml_builder_t* b, const char* text) {
    if (!b || !text) return;
    xml_escape_append(&b->buf, text, 0);
}

void s3_xml_close(s3_xml_builder_t* b, const char* tag) {
    if (!b || !tag) return;
    b->buf = tstr_cat_fmt(b->buf, "</%s>", tag);
}

void s3_xml_elem(s3_xml_builder_t* b, const char* tag, const char* text) {
    if (!b || !tag) return;
    if (!text || !*text) {
        b->buf = tstr_cat_fmt(b->buf, "<%s/>", tag);
        return;
    }
    b->buf = tstr_cat_fmt(b->buf, "<%s>", tag);
    xml_escape_append(&b->buf, text, 0);
    b->buf = tstr_cat_fmt(b->buf, "</%s>", tag);
}

void s3_xml_elem_int(s3_xml_builder_t* b, const char* tag, int value) {
    if (!b || !tag) return;
    b->buf = tstr_cat_fmt(b->buf, "<%s>%d</%s>", tag, value, tag);
}

tstr_t s3_xml_finish(s3_xml_builder_t* b) {
    if (!b) return tstr_new();
    tstr_t result = b->buf;
    b->buf = NULL;
    free(b);
    return result;
}
