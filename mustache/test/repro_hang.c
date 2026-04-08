#include "mustache.h"
#include "mustache_json.h"
#include "json_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct BUFFER {
    char data[8192];
    size_t n;
} BUFFER;

static int out(const char* output, size_t n, void* data) {
    BUFFER* buf = (BUFFER*) data;
    if (buf->n + n < sizeof(buf->data)) {
        memcpy(buf->data + buf->n, output, n);
        buf->n += n;
    }
    return 0;
}

static const MUSTACHE_RENDERER renderer = { out, out };

typedef struct PROVIDER_DATA {
    json_value_t* root;
    json_value_t* partials;
} PROVIDER_DATA;

static int dump_val(void* node, int (*out_fn)(const char*, size_t, void*), void* renderer_data, void* data) {
    json_value_t* value = (json_value_t*) node;
    if (json_type(value) == JSON_STRING) {
        return out_fn(json_string(value), json_string_len(value), renderer_data);
    }
    return 0;
}

static void* get_root_val(void* data) {
    return ((PROVIDER_DATA*)data)->root;
}

static int json_is_falsey(json_value_t* value) {
    if (!value) return 1;
    switch(json_type(value)) {
    case JSON_NULL: return 1;
    case JSON_BOOL: return json_bool(value) ? 0 : 1;
    case JSON_ARRAY: return json_array_size(value) == 0 ? 1 : 0;
    case JSON_STRING: return json_string_len(value) == 0 ? 1 : 0;
    default: return 0;
    }
}

static void* get_named_val(void* node, const char* name, size_t size, void* data) {
    char key[256];
    memcpy(key, name, size); key[size] = '\0';
    json_value_t* val = json_object_get((json_value_t*)node, key);
    if (!val) return NULL;
    return val;
}

static void* get_indexed_val(void* node, unsigned index, void* data) {
    json_value_t* value = (json_value_t*) node;
    if (json_is_falsey(value)) return NULL;
    if(json_type(value) == JSON_ARRAY) {
        if (index < json_array_size(value)) return json_array_get(value, index);
        return NULL;
    }
    return (index == 0) ? value : NULL;
}

static MUSTACHE_TEMPLATE* get_partial_val(const char* name, size_t size, void* data) {
    PROVIDER_DATA* pd = (PROVIDER_DATA*) data;
    char key[256];
    memcpy(key, name, size); key[size] = '\0';
    json_value_t* p_val = json_object_get(pd->partials, key);
    if (!p_val || json_type(p_val) != JSON_STRING) return NULL;
    printf("Compiling partial %s\n", key);
    return mustache_compile(json_string(p_val), json_string_len(p_val), NULL, NULL, 0);
}

static const MUSTACHE_DATAPROVIDER provider = {
    dump_val, get_root_val, get_named_val, get_indexed_val, get_partial_val, NULL, NULL
};

int main() {
    const char* template_str = "{{>node}}";
    const char* data_str = "{\"content\":\"X\",\"nodes\":[{\"content\":\"Y\",\"nodes\":[]}]}";
    const char* partials_str = "{\"node\":\"{{content}}<{{#nodes}}{{>node}}{{/nodes}}>\"}";

    json_value_t* data = json_parse(data_str, strlen(data_str));
    json_value_t* partials = json_parse(partials_str, strlen(partials_str));

    MUSTACHE_TEMPLATE* t = mustache_compile(template_str, strlen(template_str), NULL, NULL, 0);
    BUFFER buf = { 0 };
    PROVIDER_DATA pd = { data, partials };

    printf("Starting process...\n");
    mustache_process(t, &renderer, &buf, &provider, &pd);
    printf("Finished process.\n");

    buf.data[buf.n] = '\0';
    printf("Result: %s\n", buf.data);

    mustache_release(t);
    json_free(data);
    json_free(partials);

    return 0;
}
