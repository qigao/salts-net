/**
 * @file test_spec_adapted.c
 * @brief Mustache specification tests adapted for TurboNet JSON parser
 * 
 * This file contains comprehensive tests based on the official mustache specification,
 * adapted to use TurboNet's json_parser instead of the original json.h library.
 */

#include "acutest.h"
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
    buf->n += sprintf(buf->data, "Error: %u:%u: %s\n", line, col, msg);
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

    memcpy(buf->data + buf->n, output, n);
    buf->n += n;
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
dump(void* node, int (*out_fn)(const char*, size_t, void*), void* renderer_data, void* data)
{
    json_value_t* value = (json_value_t*) node;

    switch(json_type(value)) {
    case JSON_NULL:
        /* no output. */
        return 0;

    case JSON_BOOL:
        if (json_bool(value)) {
            return out_fn("<<TRUE>>", strlen("<<TRUE>>"), renderer_data);
        } else {
            /* false - no output */
            return 0;
        }

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
        
        /* Check if it's an integer */
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

    /* Create null-terminated key string */
    char* key_buffer = malloc(size + 1);
    if (!key_buffer) return NULL;
    
    memcpy(key_buffer, name, size);
    key_buffer[size] = '\0';
    
    json_value_t* result = json_object_get(value, key_buffer);
    free(key_buffer);
    
    if (!result || json_is_null(result) || 
        (json_type(result) == JSON_BOOL && !json_bool(result))) {
        return NULL;
    }
    
    return result;
}

static void*
get_indexed(void* node, unsigned index, void* data)
{
    json_value_t* value = (json_value_t*) node;

    if (json_is_null(value) || 
        (json_type(value) == JSON_BOOL && !json_bool(value))) {
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
    get_partial
};

/*********************************
 *** Main body for test units. ***
 *********************************/

static void
run(const char* desc, const char* templ, const char* data, const char* partials, const char* expected)
{
    json_value_t* json_root;
    MUSTACHE_TEMPLATE* t;
    BUFFER buf = { 0 };

    json_root = json_parse(data, strlen(data));
    if(!TEST_CHECK(json_root != NULL))
        return;

    t = mustache_compile(templ, strlen(templ), &parser, (void*) &buf, 0);
    if(t != NULL) {
        PROVIDER_DATA provider_data = { 0 };
        int i;

        provider_data.root = json_root;

        if(partials != NULL) {
            json_value_t* json_partials;

            json_partials = json_parse(partials, strlen(partials));
            if(!TEST_CHECK(json_partials != NULL))
                return;
            if(!TEST_CHECK(json_type(json_partials) == JSON_OBJECT))
                return;

            for(i = 0; i < json_object_size(json_partials); i++) {
                const char* key = json_object_key(json_partials, i);
                json_value_t* val = json_object_value(json_partials, i);
                
                strcpy(provider_data.partial_dict[i].name, key);
                provider_data.partial_dict[i].templ = mustache_compile(
                            json_string(val),
                            json_string_len(val),
                            NULL, NULL, 0);
                TEST_CHECK(provider_data.partial_dict[i].templ != NULL);
            }
        }

        mustache_process(t, &renderer, (void*) &buf, &provider, &provider_data);

        for(i = 0; provider_data.partial_dict[i].templ != NULL; i++) {
            const PARTIAL_INFO* info = (const PARTIAL_INFO*) &provider_data.partial_dict[i];
            mustache_release(info->templ);
        }
    }

    if(!TEST_CHECK_(t != NULL &&
                    buf.n == strlen(expected) &&
                    memcmp(expected, buf.data, buf.n) == 0, "%s", desc))
    {
        TEST_MSG("Template:");
        TEST_MSG("---------");
        TEST_MSG("%s", templ);
        TEST_MSG("\nData:");
        TEST_MSG("---------");
        TEST_MSG("%s", data);
        if(partials != NULL) {
            TEST_MSG("\nPartials:");
            TEST_MSG("---------");
            TEST_MSG("%s", partials);
        }
        TEST_MSG("\nExpected:");
        TEST_MSG("---------");
        TEST_MSG("%s", expected);
        TEST_MSG("\nProduced:");
        TEST_MSG("---------");
        TEST_MSG("%.*s", (int)buf.n, buf.data);
    }

    json_free(json_root);
    mustache_release(t);
}

/***********************
 *** Sample test cases ***
 ***********************/

static void
test_comments_1(void)
{
    run(
        "comment blocks should be removed from the template",
        "12345{{! Comment Block! }}67890",
        "{}",
        NULL,
        "1234567890"
    );
}

static void
test_interpolation_1(void)
{
    run(
        "mustache-free templates should render as-is",
        "Hello from {Mustache}!\n",
        "{}",
        NULL,
        "Hello from {Mustache}!\n"
    );
}

static void
test_interpolation_2(void)
{
    run(
        "unadorned tags should interpolate content into the template",
        "Hello, {{subject}}!\n",
        "{\"subject\": \"world\"}",
        NULL,
        "Hello, world!\n"
    );
}

static void
test_sections_1(void)
{
    run(
        "truthy sections should have their contents rendered",
        "\"{{#boolean}}This should be rendered.{{/boolean}}\"",
        "{\"boolean\": true}",
        NULL,
        "\"This should be rendered.\""
    );
}

static void
test_sections_2(void)
{
    run(
        "falsey sections should have their contents omitted",
        "\"{{#boolean}}This should not be rendered.{{/boolean}}\"",
        "{\"boolean\": false}",
        NULL,
        "\"\""
    );
}

static void
test_inverted_1(void)
{
    run(
        "falsey sections should have their contents rendered",
        "\"{{^boolean}}This should be rendered.{{/boolean}}\"",
        "{\"boolean\": false}",
        NULL,
        "\"This should be rendered.\""
    );
}

static void
test_partials_1(void)
{
    run(
        "the greater-than operator should expand to the named partial",
        "\"{{>text}}\"",
        "{}",
        "{\"text\": \"from partial\"}",
        "\"from partial\""
    );
}

TEST_LIST = {
    { "comments-1", test_comments_1 },
    { "interpolation-1", test_interpolation_1 },
    { "interpolation-2", test_interpolation_2 },
    { "sections-1", test_sections_1 },
    { "sections-2", test_sections_2 },
    { "inverted-1", test_inverted_1 },
    { "partials-1", test_partials_1 },
    { 0 }
};