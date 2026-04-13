/**
 * @file test_spec_adapted.c
 * @brief Mustache specification tests adapted for TurboUtils JSON parser using TinyTest
 */

#include "tinytest.h"
#include "mustache.h"
#include "mustache_json.h"
#include "json_parser.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct BUFFER {
    char data[1024];
    size_t n;
} BUFFER;

/****************************************************
 *** Implementation of MUSTACHE_PARSER interface. ***
 ****************************************************/

static void
parse_error(int err_code, const char* msg, unsigned line, unsigned col, void* data)
{
    BUFFER* buf = (BUFFER*) data;
    buf->n += snprintf(buf->data + buf->n, sizeof(buf->data) - buf->n, "Error: %u:%u: %s\n", line, col, msg);
}

static const MUSTACHE_PARSER parser = {
    parse_error
};

/******************************************************
 *** Implementation of MUSTACHE_RENDERER interface. ***
 ******************************************************/

static int
out(const char* output, size_t n, void* data)
{
    BUFFER* buf = (BUFFER*) data;
    if (buf->n + n < sizeof(buf->data)) {
        memcpy(buf->data + buf->n, output, n);
        buf->n += n;
    }
    return 0;
}

static int
out_escaped(const char* output, size_t n, void* data)
{
    size_t i;

    for(i = 0; i < n; i++) {
        switch(output[i]) {
            case '&':   out("&amp;", 5, data); break;
            case '"':   out("&quot;", 6, data); break;
            case '<':   out("&lt;", 4, data); break;
            case '>':   out("&gt;", 4, data); break;
            default:    out(output + i, 1, data); break;
        }
    }

    return 0;
}

static const MUSTACHE_RENDERER renderer = {
    out,
    out_escaped,
};

/**********************************************************
 *** Implementation of MUSTACHE_DATAPROVIDER interface. ***
 **********************************************************/

typedef struct PARTIAL_INFO {
    char name[32];
    MUSTACHE_TEMPLATE* templ;
} PARTIAL_INFO;

typedef struct PROVIDER_DATA {
    json_value_t* root;
    PARTIAL_INFO partial_dict[8];
} PROVIDER_DATA;

static int
json_is_falsey(json_value_t* value)
{
    if (!value) return 1;

    switch(json_type(value)) {
    case JSON_NULL:
        return 1;
    case JSON_BOOL:
        return json_bool(value) ? 0 : 1;
    case JSON_STRING:
        return json_string_len(value) == 0 ? 1 : 0;
    case JSON_ARRAY:
        return json_array_size(value) == 0 ? 1 : 0;
    default:
        return 0;
    }
}

static int
dump(void* node, int (*out_fn)(const char*, size_t, void*), void* renderer_data, void* data)
{
    json_value_t* value = (json_value_t*) node;

    switch(json_type(value)) {
    case JSON_NULL:
        return 0;

    case JSON_BOOL:
        return json_bool(value) ? out_fn("<<TRUE>>", strlen("<<TRUE>>"), renderer_data) : 0;

    case JSON_ARRAY:
        return out_fn("<<ARRAY>>", strlen("<<ARRAY>>"), renderer_data);
    case JSON_OBJECT:
        return out_fn("<<OBJECT>>", strlen("<<OBJECT>>"), renderer_data);

    case JSON_STRING:
        return out_fn(json_string(value), json_string_len(value), renderer_data);
        
    case JSON_NUMBER: {
        char buffer[64];
        double num = json_number(value);
        int len;
        
        if (num == (long long)num) {
            len = snprintf(buffer, sizeof(buffer), "%lld", (long long)num);
        } else {
            len = snprintf(buffer, sizeof(buffer), "%.15g", num);
        }
        
        if (len > 0 && len < sizeof(buffer)) {
            return out_fn(buffer, len, renderer_data);
        }
        return -1;
    }
    }

    return 0;
}

static void*
get_root(void* data)
{
    PROVIDER_DATA* provider_data = (PROVIDER_DATA*) data;
    return provider_data->root;
}

static void*
get_named(void* node, const char* name, size_t size, void* data)
{
    json_value_t* value = (json_value_t*) node;

    if(json_type(value) != JSON_OBJECT)
        return NULL;

    char* key_buffer = malloc(size + 1);
    if (!key_buffer) return NULL;
    
    memcpy(key_buffer, name, size);
    key_buffer[size] = '\0';
    
    json_value_t* result = json_object_get(value, key_buffer);
    free(key_buffer);
    
    if (!result || json_is_falsey(result)) {
        return NULL;
    }
    
    return result;
}

static void*
get_indexed(void* node, unsigned index, void* data)
{
    json_value_t* value = (json_value_t*) node;

    if (json_is_falsey(value)) {
        return NULL;
    }

    if(json_type(value) == JSON_ARRAY && index < json_array_size(value)) {
        return json_array_get(value, index);
    } else if(json_type(value) != JSON_ARRAY && index == 0) {
        return value;
    }

    return NULL;
}

static MUSTACHE_TEMPLATE*
get_partial(const char* name, size_t size, void* data)
{
    PROVIDER_DATA* provider_data = (PROVIDER_DATA*) data;
    int i;

    for(i = 0; provider_data->partial_dict[i].templ != NULL; i++) {
        const PARTIAL_INFO* info = (const PARTIAL_INFO*) &provider_data->partial_dict[i];

        if(size == strlen(info->name) && strncmp(name, info->name, size) == 0)
            return info->templ;
    }
    return NULL;
}

static const MUSTACHE_DATAPROVIDER provider = {
    dump,
    get_root,
    get_named,
    get_indexed,
    get_partial,
    NULL,
    NULL
};

static void
run_case(__bdd_config_type__ *__bdd_config__, const char* desc, const char* templ, const char* data, const char* partials, const char* expected)
{
    json_value_t* json_root;
    json_value_t* json_partials = NULL;
    MUSTACHE_TEMPLATE* t;
    BUFFER buf = { 0 };

    json_root = json_parse(data, strlen(data));
    check_not_null(json_root);
    if (!json_root) return;

    t = mustache_compile(templ, strlen(templ), &parser, (void*) &buf, 0);
    check_not_null(t);
    
    if(t != NULL) {
        PROVIDER_DATA provider_data = { 0 };
        int i;

        provider_data.root = json_root;

        if(partials != NULL) {
            json_partials = json_parse(partials, strlen(partials));
            check_not_null(json_partials);
            
            if (json_partials) {
                check_int_eq(json_type(json_partials), JSON_OBJECT);
                if (json_type(json_partials) == JSON_OBJECT) {
                    for(i = 0; i < json_object_size(json_partials); i++) {
                        const char* key = json_object_key(json_partials, i);
                        json_value_t* val = json_object_value(json_partials, i);
                        
                        strncpy(provider_data.partial_dict[i].name, key, sizeof(provider_data.partial_dict[i].name)-1);
                        provider_data.partial_dict[i].templ = mustache_compile(
                                    json_string(val),
                                    json_string_len(val),
                                    NULL, NULL, 0);
                        check_not_null(provider_data.partial_dict[i].templ);
                    }
                }
            }
        }

        mustache_process(t, &renderer, (void*) &buf, &provider, &provider_data);

        for(i = 0; provider_data.partial_dict[i].templ != NULL; i++) {
            const PARTIAL_INFO* info = (const PARTIAL_INFO*) &provider_data.partial_dict[i];
            mustache_release(info->templ);
        }
    }

    buf.data[buf.n] = '\0';
    check_str_eq(buf.data, expected);

    if (json_partials) json_free(json_partials);
    if (t) mustache_release(t);
    if (json_root) json_free(json_root);
}

spec("mustache spec adapted") {

    describe("comments") {
        it("should remove comment blocks") {
            run_case(__bdd_config__,
                "comment blocks should be removed from the template",
                "12345{{! Comment Block! }}67890",
                "{}",
                NULL,
                "1234567890"
            );
        }
    }

    describe("interpolation") {
        it("should render mustache-free templates as-is") {
            run_case(__bdd_config__,
                "mustache-free templates should render as-is",
                "Hello from {Mustache}!\n",
                "{}",
                NULL,
                "Hello from {Mustache}!\n"
            );
        }

        it("should interpolate unadorned tags") {
            run_case(__bdd_config__,
                "unadorned tags should interpolate content into the template",
                "Hello, {{subject}}!\n",
                "{\"subject\": \"world\"}",
                NULL,
                "Hello, world!\n"
            );
        }
    }

    describe("sections") {
        it("should render truthy sections") {
            run_case(__bdd_config__,
                "truthy sections should have their contents rendered",
                "\"{{#boolean}}This should be rendered.{{/boolean}}\"",
                "{\"boolean\": true}",
                NULL,
                "\"This should be rendered.\""
            );
        }

        it("should omit falsey sections") {
            run_case(__bdd_config__,
                "falsey sections should have their contents omitted",
                "\"{{#boolean}}This should not be rendered.{{/boolean}}\"",
                "{\"boolean\": false}",
                NULL,
                "\"\""
            );
        }
    }

    describe("inverted sections") {
        it("should render falsey sections") {
            run_case(__bdd_config__,
                "falsey sections should have their contents rendered",
                "\"{{^boolean}}This should be rendered.{{/boolean}}\"",
                "{\"boolean\": false}",
                NULL,
                "\"This should be rendered.\""
            );
        }
    }

    describe("partials") {
        it("should expand to the named partial") {
            run_case(__bdd_config__,
                "the greater-than operator should expand to the named partial",
                "\"{{>text}}\"",
                "{}",
                "{\"text\": \"from partial\"}",
                "\"from partial\""
            );
        }
    }
}
