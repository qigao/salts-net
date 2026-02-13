#include "minio/minio_xml_builder.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct minio_xml_builder_s {
    tstr_t buf;
};

minio_xml_builder_t* minio_xml_new(void) {
    minio_xml_builder_t* b = calloc(1, sizeof(minio_xml_builder_t));
    if (!b) return NULL;
    b->buf = tstr_dup("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    return b;
}

static void xml_escape_append(tstr_t* buf, const char* text) {
    for (const char* p = text; *p; p++) {
        switch (*p) {
            case '&':  *buf = tstr_cat(*buf, "&amp;");  break;
            case '<':  *buf = tstr_cat(*buf, "&lt;");   break;
            case '>':  *buf = tstr_cat(*buf, "&gt;");   break;
            case '"':  *buf = tstr_cat(*buf, "&quot;");  break;
            case '\'': *buf = tstr_cat(*buf, "&apos;");  break;
            default: {
                char c[2] = {*p, 0};
                *buf = tstr_cat(*buf, c);
                break;
            }
        }
    }
}

void minio_xml_open(minio_xml_builder_t* b, const char* tag) {
    if (!b || !tag) return;
    b->buf = tstr_cat_fmt(b->buf, "<%s>", tag);
}

void minio_xml_open_ns(minio_xml_builder_t* b, const char* tag, const char* ns) {
    if (!b || !tag || !ns) return;
    b->buf = tstr_cat_fmt(b->buf, "<%s xmlns=\"%s\">", tag, ns);
}

void minio_xml_attr(minio_xml_builder_t* b, const char* name, const char* value) {
    if (!b || !name || !value) return;
    // Replace the trailing '>' of the last open tag with the attribute
    size_t len = tstr_len(b->buf);
    if (len > 0 && b->buf[len - 1] == '>') {
        b->buf[len - 1] = '\0'; // remove '>'
        // tstr_len is now stale, but tstr_cat will handle it
        b->buf = tstr_cat_fmt(b->buf, " %s=\"", name);
        xml_escape_append(&b->buf, value);
        b->buf = tstr_cat(b->buf, "\">");
    }
}

void minio_xml_text(minio_xml_builder_t* b, const char* text) {
    if (!b || !text) return;
    xml_escape_append(&b->buf, text);
}

void minio_xml_close(minio_xml_builder_t* b, const char* tag) {
    if (!b || !tag) return;
    b->buf = tstr_cat_fmt(b->buf, "</%s>", tag);
}

void minio_xml_elem(minio_xml_builder_t* b, const char* tag, const char* text) {
    if (!b || !tag) return;
    if (!text || !*text) {
        b->buf = tstr_cat_fmt(b->buf, "<%s/>", tag);
        return;
    }
    b->buf = tstr_cat_fmt(b->buf, "<%s>", tag);
    xml_escape_append(&b->buf, text);
    b->buf = tstr_cat_fmt(b->buf, "</%s>", tag);
}

void minio_xml_elem_int(minio_xml_builder_t* b, const char* tag, int value) {
    if (!b || !tag) return;
    b->buf = tstr_cat_fmt(b->buf, "<%s>%d</%s>", tag, value, tag);
}

tstr_t minio_xml_finish(minio_xml_builder_t* b) {
    if (!b) return tstr_new();
    tstr_t result = b->buf;
    b->buf = NULL;
    free(b);
    return result;
}
