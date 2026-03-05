#include "s3/s3_multimap.h"
#include "s3/s3_url.h"
#include <stb_sprintf.h>
#include <stc/cstr.h>
#include <ctype.h>


void s3_headers_add(S3Headers* m, const char* key, const char* value) {
    if (!m || !key || !value) return;
    S3Headers_insert(m, cstr_from(key), cstr_from(value));
}

const char* s3_headers_get(S3Headers* m, const char* key) {
    if (!m || !key) return NULL;
    const S3Headers_value* v = S3Headers_get(m, key);
    if (v) return cstr_str(&v->second);

    // HTTP header field names are case-insensitive.
    c_foreach (i, S3Headers, *m) {
        if (tstr_casecmp(cstr_str(&i.ref->first), key) == 0) {
            return cstr_str(&i.ref->second);
        }
    }
    return NULL;
}

int s3_headers_contains(S3Headers* m, const char* key) {
    if (!m || !key) return 0;
    return S3Headers_get(m, key) != NULL;
}

tstr_t s3_headers_to_query_string(S3Headers* m) {
    tstr_t qs = tstr_new();
    int first = 1;
    c_foreach (i, S3Headers, *m) {
        if (!first) {
            qs = tstr_cat(qs, "&");
        }
        tstr_t ek = s3_url_encode(cstr_str(&i.ref->first));
        tstr_t ev = s3_url_encode(cstr_str(&i.ref->second));
        qs = tstr_cat(qs, ek);
        qs = tstr_cat(qs, "=");
        qs = tstr_cat(qs, ev);
        tstr_free(ek); tstr_free(ev);
        first = 0;
    }
    return qs;
}

const char** s3_headers_to_http_array(S3Headers* m, int* count) {
    if (!m || !count) return NULL;
    *count = (int)S3Headers_size(m);
    if (*count == 0) return NULL;

    const char** arr = malloc(sizeof(char*) * (*count));
    if (!arr) return NULL;

    int idx = 0;
    c_foreach (i, S3Headers, *m) {
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

void s3_headers_get_canonical(S3Headers* m, tstr_t* signed_headers, tstr_t* canonical_headers) {
    if (!m || !signed_headers || !canonical_headers) return;
    
    *signed_headers = tstr_new();
    *canonical_headers = tstr_new();

    SortedHeaders sorted = SortedHeaders_init();
    c_foreach (i, S3Headers, *m) {
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

tstr_t s3_headers_get_canonical_query(S3Headers* m) {
    if (!m) return tstr_new();

    SortedHeaders sorted = SortedHeaders_init();
    c_foreach (i, S3Headers, *m) {
        SortedHeaders_insert(&sorted, cstr_from(cstr_str(&i.ref->first)), cstr_from(cstr_str(&i.ref->second)));
    }

    tstr_t qs = tstr_new();
    int first = 1;
    c_foreach (i, SortedHeaders, sorted) {
        if (!first) {
            qs = tstr_cat(qs, "&");
        }
        // In S3, keys and values should be URL-encoded
        tstr_t ek = s3_url_encode(cstr_str(&i.ref->first));
        tstr_t ev = s3_url_encode(cstr_str(&i.ref->second));
        qs = tstr_cat(qs, ek);
        qs = tstr_cat(qs, "=");
        qs = tstr_cat(qs, ev);
        tstr_free(ek); tstr_free(ev);
        first = 0;
    }

    SortedHeaders_drop(&sorted);
    return qs;
}
