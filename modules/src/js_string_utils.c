#include "js_string_utils.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

// Helper function to create a new string with modifications
static char* create_modified_string(const char* input, size_t len) {
    char* result = malloc(len + 1);
    if (!result) return NULL;
    result[len] = '\0';
    return result;
}

// slugify(text) - Convert to URL-friendly slug
static JSValue js_slugify(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "slugify requires 1 argument");
    }
    
    const char* input = JS_ToCString(ctx, argv[0]);
    if (!input) return JS_EXCEPTION;
    
    size_t len = strlen(input);
    char* result = create_modified_string(input, len);
    if (!result) {
        JS_FreeCString(ctx, input);
        return JS_ThrowOutOfMemory(ctx);
    }
    
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        char c = input[i];
        if (isalnum(c)) {
            result[j++] = tolower(c);
        } else if (c == ' ' || c == '-' || c == '_') {
            if (j > 0 && result[j-1] != '-') {
                result[j++] = '-';
            }
        }
    }
    
    // Remove trailing dash
    if (j > 0 && result[j-1] == '-') j--;
    result[j] = '\0';
    
    JSValue ret = JS_NewString(ctx, result);
    free(result);
    JS_FreeCString(ctx, input);
    return ret;
}

// capitalize(text) - Capitalize first letter of each word
static JSValue js_capitalize(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "capitalize requires 1 argument");
    }
    
    const char* input = JS_ToCString(ctx, argv[0]);
    if (!input) return JS_EXCEPTION;
    
    size_t len = strlen(input);
    char* result = create_modified_string(input, len);
    if (!result) {
        JS_FreeCString(ctx, input);
        return JS_ThrowOutOfMemory(ctx);
    }
    
    int capitalize_next = 1;
    for (size_t i = 0; i < len; i++) {
        if (isalpha(input[i])) {
            result[i] = capitalize_next ? toupper(input[i]) : tolower(input[i]);
            capitalize_next = 0;
        } else {
            result[i] = input[i];
            if (isspace(input[i])) {
                capitalize_next = 1;
            }
        }
    }
    
    JSValue ret = JS_NewString(ctx, result);
    free(result);
    JS_FreeCString(ctx, input);
    return ret;
}

// truncate(text, length) - Truncate text to specified length
static JSValue js_truncate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "truncate requires 2 arguments");
    }
    
    const char* input = JS_ToCString(ctx, argv[0]);
    if (!input) return JS_EXCEPTION;
    
    int32_t max_len;
    if (JS_ToInt32(ctx, &max_len, argv[1]) < 0) {
        JS_FreeCString(ctx, input);
        return JS_EXCEPTION;
    }
    
    if (max_len < 0) max_len = 0;
    
    size_t input_len = strlen(input);
    if (input_len <= (size_t)max_len) {
        JSValue ret = JS_NewString(ctx, input);
        JS_FreeCString(ctx, input);
        return ret;
    }
    
    char* result = malloc(max_len + 4); // +3 for "..." +1 for null
    if (!result) {
        JS_FreeCString(ctx, input);
        return JS_ThrowOutOfMemory(ctx);
    }
    
    if (max_len >= 3) {
        strncpy(result, input, max_len - 3);
        strcpy(result + max_len - 3, "...");
    } else {
        strncpy(result, input, max_len);
        result[max_len] = '\0';
    }
    
    JSValue ret = JS_NewString(ctx, result);
    free(result);
    JS_FreeCString(ctx, input);
    return ret;
}

// hash(text) - Generate simple hash of text
static JSValue js_hash(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "hash requires 1 argument");
    }
    
    const char* input = JS_ToCString(ctx, argv[0]);
    if (!input) return JS_EXCEPTION;
    
    // Simple djb2 hash algorithm
    unsigned long hash = 5381;
    const char* str = input;
    int c;
    
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    
    JS_FreeCString(ctx, input);
    return JS_NewUint32(ctx, (uint32_t)hash);
}

// template(template, vars) - Simple template substitution
static JSValue js_template(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "template requires 2 arguments");
    }
    
    const char* template_str = JS_ToCString(ctx, argv[0]);
    if (!template_str) return JS_EXCEPTION;
    
    if (!JS_IsObject(argv[1])) {
        JS_FreeCString(ctx, template_str);
        return JS_ThrowTypeError(ctx, "second argument must be an object");
    }
    
    // For simplicity, we'll just replace {{key}} patterns
    // In a real implementation, you'd want more sophisticated parsing
    size_t template_len = strlen(template_str);
    size_t result_capacity = template_len * 2; // Start with double capacity
    char* result = malloc(result_capacity);
    if (!result) {
        JS_FreeCString(ctx, template_str);
        return JS_ThrowOutOfMemory(ctx);
    }
    
    size_t result_len = 0;
    const char* pos = template_str;
    
    while (*pos) {
        if (pos[0] == '{' && pos[1] == '{') {
            // Find closing }}
            const char* end = strstr(pos + 2, "}}");
            if (end) {
                // Extract variable name
                size_t var_len = end - pos - 2;
                char* var_name = malloc(var_len + 1);
                if (var_name) {
                    strncpy(var_name, pos + 2, var_len);
                    var_name[var_len] = '\0';
                    
                    // Get property from object
                    JSValue prop = JS_GetPropertyStr(ctx, argv[1], var_name);
                    if (!JS_IsUndefined(prop)) {
                        const char* value = JS_ToCString(ctx, prop);
                        if (value) {
                            size_t value_len = strlen(value);
                            // Ensure capacity
                            while (result_len + value_len >= result_capacity) {
                                result_capacity *= 2;
                                result = realloc(result, result_capacity);
                                if (!result) {
                                    JS_FreeCString(ctx, value);
                                    JS_FreeValue(ctx, prop);
                                    free(var_name);
                                    JS_FreeCString(ctx, template_str);
                                    return JS_ThrowOutOfMemory(ctx);
                                }
                            }
                            strcpy(result + result_len, value);
                            result_len += value_len;
                            JS_FreeCString(ctx, value);
                        }
                    }
                    JS_FreeValue(ctx, prop);
                    free(var_name);
                }
                pos = end + 2;
            } else {
                // No closing }}, treat as literal
                result[result_len++] = *pos++;
            }
        } else {
            // Ensure capacity
            if (result_len >= result_capacity - 1) {
                result_capacity *= 2;
                result = realloc(result, result_capacity);
                if (!result) {
                    JS_FreeCString(ctx, template_str);
                    return JS_ThrowOutOfMemory(ctx);
                }
            }
            result[result_len++] = *pos++;
        }
    }
    
    result[result_len] = '\0';
    JSValue ret = JS_NewString(ctx, result);
    free(result);
    JS_FreeCString(ctx, template_str);
    return ret;
}

// Module initialization
int js_init_string_utils_module(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue strUtils = JS_NewObject(ctx);
    
    // Add functions to the strUtils object
    JS_SetPropertyStr(ctx, strUtils, "slugify", JS_NewCFunction(ctx, js_slugify, "slugify", 1));
    JS_SetPropertyStr(ctx, strUtils, "capitalize", JS_NewCFunction(ctx, js_capitalize, "capitalize", 1));
    JS_SetPropertyStr(ctx, strUtils, "truncate", JS_NewCFunction(ctx, js_truncate, "truncate", 2));
    JS_SetPropertyStr(ctx, strUtils, "hash", JS_NewCFunction(ctx, js_hash, "hash", 1));
    JS_SetPropertyStr(ctx, strUtils, "template", JS_NewCFunction(ctx, js_template, "template", 2));
    
    // Add strUtils to global scope
    JS_SetPropertyStr(ctx, global, "strUtils", strUtils);
    
    JS_FreeValue(ctx, global);
    return 0;
}