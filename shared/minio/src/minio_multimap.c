#include "minio/minio_multimap.h"
#include <stb_sprintf.h>
#include <stc/cstr.h>
#include <ctype.h>

static tstr_t url_encode(const char* s) {
    if (!s) return tstr_new();
    tstr_t res = tstr_new();
    for (; *s; s++) {
        unsigned char c = *s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            res = tstr_cat_fmt(res, "%c", c);
        } else {
            res = tstr_cat_fmt(res, "%%%02X", c);
        }
    }
    return res;
}

void minio_headers_add(MinioHeaders* m, const char* key, const char* value) {
    if (!m || !key || !value) return;
    MinioHeaders_insert(m, cstr_from(key), cstr_from(value));
}

const char* minio_headers_get(MinioHeaders* m, const char* key) {
    if (!m || !key) return NULL;
    const MinioHeaders_value* v = MinioHeaders_get(m, key);
    return v ? cstr_str(&v->second) : NULL;
}

int minio_headers_contains(MinioHeaders* m, const char* key) {
    if (!m || !key) return 0;
    return MinioHeaders_get(m, key) != NULL;
}

tstr_t minio_headers_to_query_string(MinioHeaders* m) {
    tstr_t qs = tstr_new();
    int first = 1;
    c_foreach (i, MinioHeaders, *m) {
        if (!first) {
            qs = tstr_cat(qs, "&");
        }
        tstr_t ek = url_encode(cstr_str(&i.ref->first));
        tstr_t ev = url_encode(cstr_str(&i.ref->second));
        qs = tstr_cat(qs, ek);
        qs = tstr_cat(qs, "=");
        qs = tstr_cat(qs, ev);
        tstr_free(ek); tstr_free(ev);
        first = 0;
    }
    return qs;
}

const char** minio_headers_to_http_array(MinioHeaders* m, int* count) {
    if (!m || !count) return NULL;
    *count = (int)MinioHeaders_size(m);
    if (*count == 0) return NULL;

    const char** arr = malloc(sizeof(char*) * (*count));
    if (!arr) return NULL;

    int idx = 0;
    c_foreach (i, MinioHeaders, *m) {
        const char* key = cstr_str(&i.ref->first);
        const char* val = cstr_str(&i.ref->second);

        // Skip Host header because TurboNet http_client adds it automatically.
        // Duplicate Host headers cause 400 Bad Request on many S3 servers.
        if (tstr_casecmp(key, "Host") == 0) continue;

        size_t len = strlen(key) + strlen(val) + 5; // "Key: Value\0"
        char* header_str = malloc(len);
        if (header_str) {
            stbsp_snprintf(header_str, (int)len, "%s: %s", key, val);
            arr[idx++] = header_str;
        }
    }
    *count = idx;
    return arr;
}

#define i_static
#define i_type SortedHeaders
#define i_key_str
#define i_val_str
#include <stc/smap.h>

void minio_headers_get_canonical(MinioHeaders* m, tstr_t* signed_headers, tstr_t* canonical_headers) {
    if (!m || !signed_headers || !canonical_headers) return;
    
    *signed_headers = tstr_new();
    *canonical_headers = tstr_new();

    SortedHeaders sorted = SortedHeaders_init();
    c_foreach (i, MinioHeaders, *m) {
        tstr_t key = tstr_dup(cstr_str(&i.ref->first));
        // Lowercase key as per S3 spec
        for (size_t j = 0; j < tstr_len(key); j++) {
            if (key[j] >= 'A' && key[j] <= 'Z') key[j] += ('a' - 'A');
        }
        SortedHeaders_insert(&sorted, cstr_from(key), cstr_from(cstr_str(&i.ref->second)));
        tstr_free(key);
    }

    int first = 1;
    c_foreach (i, SortedHeaders, sorted) {
        if (!first) {
            *signed_headers = tstr_cat(*signed_headers, ";");
        }
        *signed_headers = tstr_cat(*signed_headers, cstr_str(&i.ref->first));
        
        *canonical_headers = tstr_cat(*canonical_headers, cstr_str(&i.ref->first));
        *canonical_headers = tstr_cat(*canonical_headers, ":");
        *canonical_headers = tstr_cat(*canonical_headers, cstr_str(&i.ref->second));
        *canonical_headers = tstr_cat(*canonical_headers, "\n");
        first = 0;
    }

    SortedHeaders_drop(&sorted);
}

tstr_t minio_headers_get_canonical_query(MinioHeaders* m) {
    if (!m) return tstr_new();

    SortedHeaders sorted = SortedHeaders_init();
    c_foreach (i, MinioHeaders, *m) {
        SortedHeaders_insert(&sorted, cstr_from(cstr_str(&i.ref->first)), cstr_from(cstr_str(&i.ref->second)));
    }

    tstr_t qs = tstr_new();
    int first = 1;
    c_foreach (i, SortedHeaders, sorted) {
        if (!first) {
            qs = tstr_cat(qs, "&");
        }
        // In S3, keys and values should be URL-encoded
        tstr_t ek = url_encode(cstr_str(&i.ref->first));
        tstr_t ev = url_encode(cstr_str(&i.ref->second));
        qs = tstr_cat(qs, ek);
        qs = tstr_cat(qs, "=");
        qs = tstr_cat(qs, ev);
        tstr_free(ek); tstr_free(ev);
        first = 0;
    }

    SortedHeaders_drop(&sorted);
    return qs;
}
