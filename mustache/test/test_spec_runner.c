/**
 * @file test_spec_runner.c
 * @brief Comprehensive mustache specification test runner using TinyTest
 * 
 * Loads and runs all tests from the official mustache specification JSON files.
 */

#include "tinytest.h"
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

/****************************************************
 *** Implementation of MUSTACHE_PARSER interface. ***
 ****************************************************/

static void
parse_error(int err_code, const char* msg, unsigned line, unsigned col, void* data)
{
    BUFFER* buf = (BUFFER*) data;
    buf->n += snprintf(buf->data + buf->n, sizeof(buf->data) - buf->n, 
                      "Error: %u:%u: %s\n", line, col, msg);
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
            case '\'':  out("&#39;", 5, data); break;
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

typedef struct PROVIDER_DATA {
    json_value_t* root;
    json_value_t* partials;
    int lambda_calls;
    struct {
        char name[256];
        MUSTACHE_TEMPLATE* templ;
    } partial_dict[64];
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
is_lambda_node(json_value_t* value)
{
    if (!value || json_type(value) != JSON_OBJECT) {
        return 0;
    }
    json_value_t* tag = json_object_get(value, "__tag__");
    if (!tag || json_type(tag) != JSON_STRING) {
        return 0;
    }
    return strcmp(json_string(tag), "code") == 0;
}

static const char*
get_pwsh_code(json_value_t* value)
{
    json_value_t* pwsh = json_object_get(value, "pwsh");
    if (!pwsh || json_type(pwsh) != JSON_STRING) {
        return NULL;
    }
    return json_string(pwsh);
}

static char*
dup_subst_args(const char* text, const char* section)
{
    size_t text_len = strlen(text);
    size_t section_len = strlen(section);
    size_t count = 0;
    const char* p = text;
    const char* needle = "$($args[0])";
    size_t needle_len = strlen(needle);

    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += needle_len;
    }

    size_t out_len = text_len + (section_len > needle_len ? (section_len - needle_len) * count : 0);
    char* out = (char*)malloc(out_len + 1);
    if (!out) return NULL;

    const char* src = text;
    char* dst = out;
    while ((p = strstr(src, needle)) != NULL) {
        size_t n = (size_t)(p - src);
        memcpy(dst, src, n);
        dst += n;
        memcpy(dst, section, section_len);
        dst += section_len;
        src = p + needle_len;
    }
    strcpy(dst, src);
    return out;
}

static int
dump_val(void* node, int (*out_fn)(const char*, size_t, void*), void* renderer_data, void* data)
{
    json_value_t* value = (json_value_t*) node;

    switch(json_type(value)) {
    case JSON_NULL:
        return 0;

    case JSON_BOOL:
        return json_bool(value) ? out_fn("true", 4, renderer_data) : 0;

    case JSON_ARRAY:
        return out_fn("<<ARRAY>>", 9, renderer_data);
    case JSON_OBJECT:
        return out_fn("<<OBJECT>>", 10, renderer_data);

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
get_root_val(void* data)
{
    PROVIDER_DATA* provider_data = (PROVIDER_DATA*) data;
    return provider_data->root;
}

static void*
get_named_val(void* node, const char* name, size_t size, void* data)
{
    json_value_t* value = (json_value_t*) node;

    if(json_type(value) != JSON_OBJECT)
        return NULL;

    char key_buffer[256];
    if (size >= sizeof(key_buffer)) return NULL;
    memcpy(key_buffer, name, size);
    key_buffer[size] = '\0';
    
    json_value_t* result = json_object_get(value, key_buffer);

    if (!result) {
        return NULL;
    }
    return result;
}

static void*
get_indexed_val(void* node, unsigned index, void* data)
{
    json_value_t* value = (json_value_t*) node;

    if (json_is_falsey(value) && !is_lambda_node(value)) {
        return NULL;
    }

    if(json_type(value) == JSON_ARRAY && index < json_array_size(value)) {
        return json_array_get(value, index);
    } else if(json_type(value) != JSON_ARRAY && index == 0) {
        // For non-arrays, truthy values are treated as a list of one element
        return value;
    }

    return NULL;
}

static MUSTACHE_TEMPLATE*
get_partial_val(const char* name, size_t size, void* data)
{
    PROVIDER_DATA* provider_data = (PROVIDER_DATA*) data;
    int i;

    for (i = 0; provider_data->partial_dict[i].templ != NULL; i++) {
        if (size == strlen(provider_data->partial_dict[i].name) &&
            strncmp(name, provider_data->partial_dict[i].name, size) == 0) {
            return provider_data->partial_dict[i].templ;
        }
    }
    return NULL;
}

static int
is_lambda(void* node, void* data)
{
    return is_lambda_node((json_value_t*)node);
}

static int
call_lambda(void* node, const char* text, size_t text_len, char** out_text, size_t* out_len, void* data)
{
    PROVIDER_DATA* provider_data = (PROVIDER_DATA*)data;
    json_value_t* value = (json_value_t*)node;
    const char* code = get_pwsh_code(value);
    const char* section = text ? text : "";

    if (!code) {
        return -1;
    }

    if (strstr(code, "$script:calls") != NULL) {
        provider_data->lambda_calls += 1;
        char buf[16];
        int n = snprintf(buf, sizeof(buf), "%d", provider_data->lambda_calls);
        *out_text = (char*)malloc((size_t)n + 1);
        if (!*out_text) return -1;
        memcpy(*out_text, buf, (size_t)n + 1);
        *out_len = (size_t)n;
        return 0;
    }

    if (strcmp(code, "$false") == 0) {
        *out_text = (char*)malloc(1);
        if (!*out_text) return -1;
        (*out_text)[0] = '\0';
        *out_len = 0;
        return 0;
    }

    if (strstr(code, "$args[0] -eq \"{{x}}\"") != NULL) {
        const char* ret = (text_len == strlen("{{x}}") && strncmp(section, "{{x}}", 5) == 0)
                              ? "yes"
                              : "no";
        *out_text = (char*)malloc(strlen(ret) + 1);
        if (!*out_text) return -1;
        strcpy(*out_text, ret);
        *out_len = strlen(ret);
        return 0;
    }

    if (code[0] == '"' && code[strlen(code) - 1] == '"') {
        size_t inner_len = strlen(code) - 2;
        char* inner = (char*)malloc(inner_len + 1);
        if (!inner) return -1;
        memcpy(inner, code + 1, inner_len);
        inner[inner_len] = '\0';

        char* substituted = dup_subst_args(inner, section);
        free(inner);
        if (!substituted) return -1;
        *out_text = substituted;
        *out_len = strlen(substituted);
        return 0;
    }

    *out_text = (char*)malloc(strlen(code) + 1);
    if (!*out_text) return -1;
    strcpy(*out_text, code);
    *out_len = strlen(code);
    return 0;
}

static const MUSTACHE_DATAPROVIDER provider = {
    dump_val,
    get_root_val,
    get_named_val,
    get_indexed_val,
    get_partial_val,
    is_lambda,
    call_lambda
};

static void run_spec_test_case(__bdd_config_type__ *__bdd_config__, const char* test_name, const char* template_str, 
                               json_value_t* data, json_value_t* partials, const char* expected)
{
    MUSTACHE_TEMPLATE* t = NULL;
    PROVIDER_DATA provider_data = { data, partials, 0 };
    BUFFER buf = { 0 };
    int i;

    // Compile template
    t = mustache_compile(template_str, strlen(template_str), &parser, (void*) &buf, 0);
    check_not_null(t);
    
    if (!t) {
        // info("Failed to compile template for test '%s'", test_name); // info macro might also need config
        printf("Failed to compile template for test '%s'\n", test_name);
        return;
    }

    if (partials && json_type(partials) == JSON_OBJECT) {
        int partial_count = (int)json_object_size(partials);
        if (partial_count > (int)(sizeof(provider_data.partial_dict) / sizeof(provider_data.partial_dict[0])) - 1) {
            partial_count = (int)(sizeof(provider_data.partial_dict) / sizeof(provider_data.partial_dict[0])) - 1;
        }
        for (i = 0; i < partial_count; i++) {
            const char* key = json_object_key(partials, i);
            json_value_t* val = json_object_value(partials, i);

            if (!key || !val || json_type(val) != JSON_STRING) {
                continue;
            }

            strncpy(provider_data.partial_dict[i].name, key,
                    sizeof(provider_data.partial_dict[i].name) - 1);
            provider_data.partial_dict[i].name[sizeof(provider_data.partial_dict[i].name) - 1] = '\0';
            provider_data.partial_dict[i].templ =
                mustache_compile(json_string(val), json_string_len(val), NULL, NULL, 0);
            check_not_null(provider_data.partial_dict[i].templ);
        }
    }

    (void)mustache_process(t, &renderer, (void*) &buf, &provider, &provider_data);

    // Check result
    buf.data[buf.n] = '\0';
    if (strcmp(expected, buf.data) != 0) {
        printf("Test '%s' failed\n", test_name);
        printf("Template: %s\n", template_str);
        printf("Expected: '%s'\n", expected);
        printf("Got:      '%s'\n", buf.data);
    }
    check_str_eq(buf.data, expected);

    // Cleanup
    for (i = 0; provider_data.partial_dict[i].templ != NULL; i++) {
        mustache_release(provider_data.partial_dict[i].templ);
    }
    mustache_release(t);
}

static void run_spec_file(__bdd_config_type__ *__bdd_config__, const char* filename)
{
    char filepath[512];
#ifdef SPEC_JSON_DIR
    snprintf(filepath, sizeof(filepath), "%s/%s", SPEC_JSON_DIR, filename);
#else
    snprintf(filepath, sizeof(filepath), "spec/%s", filename);
#endif
    
    json_value_t* spec = json_parse_file(filepath);
    if (!spec) {
        printf("Could not load/parse spec file: %s\n", filepath);
        return;
    }

    describe(filename) {
        json_value_t* tests = json_object_get(spec, "tests");
        if (tests && json_type(tests) == JSON_ARRAY) {
            size_t test_count = json_array_size(tests);
            for (size_t i = 0; i < test_count; i++) {
                json_value_t* test = json_array_get(tests, i);
                const char* name = json_get_string(test, "name");
                const char* templ = json_get_string(test, "template");
                const char* expected = json_get_string(test, "expected");
                json_value_t* data = json_object_get(test, "data");
                json_value_t* partials = json_object_get(test, "partials");

                if (name && templ && expected) {
                    it(name) {
                        run_spec_test_case(__bdd_config__, name, templ, data, partials, expected);
                    }
                }
            }
        }
    }

    json_free(spec);
}

spec("mustache spec runner") {
  bdd_invoke(run_spec_file, "partials.json");
    bdd_invoke(run_spec_file, "comments.json");
    bdd_invoke(run_spec_file, "interpolation.json");
    bdd_invoke(run_spec_file, "sections.json");
    bdd_invoke(run_spec_file, "inverted.json");
    bdd_invoke(run_spec_file, "delimiters.json");
    bdd_invoke(run_spec_file, "lambdas.json");   
  
}
