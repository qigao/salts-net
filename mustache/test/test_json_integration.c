/**
 * @file test_json_integration.c
 * @brief Tests for mustache-JSON integration using TinyTest framework
 */

#include "tinytest.h"
#include "mustache_json.h"
#include "json_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

spec("mustache json integration") {

    describe("basic rendering") {
        it("should render simple JSON data") {
            const char *template_str = "Hello {{name}}!";
            const char *json_str = "{\"name\": \"World\"}";
            
            json_value_t *json_data = json_parse(json_str, strlen(json_str));
            check_not_null(json_data);
            
            if (json_data) {
                MUSTACHE_TEMPLATE *template = mustache_compile(template_str, strlen(template_str), NULL, NULL, 0);
                check_not_null(template);
                
                if (template) {
                    MUSTACHE_STRING_RENDERER renderer;
                    check_int_eq(mustache_string_renderer_init(&renderer), 0);
                    
                    check_int_eq(mustache_render_json(template, json_data, &renderer.base, &renderer, NULL, NULL), 0);
                    
                    char *result = mustache_string_renderer_get(&renderer);
                    check_not_null(result);
                    if (result) {
                        check_str_eq(result, "Hello World!");
                        free(result);
                    }
                    
                    mustache_string_renderer_free(&renderer);
                    mustache_release(template);
                }
                json_free(json_data);
            }
        }
    }

    describe("sections and arrays") {
        it("should iterate over JSON array") {
            const char *template_str = "{{#items}}{{name}} {{/items}}";
            const char *json_str = "{\"items\": [{\"name\": \"A\"}, {\"name\": \"B\"}]}";
            
            json_value_t *json_data = json_parse(json_str, strlen(json_str));
            check_not_null(json_data);
            
            if (json_data) {
                MUSTACHE_TEMPLATE *template = mustache_compile(template_str, strlen(template_str), NULL, NULL, 0);
                check_not_null(template);
                
                if (template) {
                    MUSTACHE_STRING_RENDERER renderer;
                    check_int_eq(mustache_string_renderer_init(&renderer), 0);
                    
                    check_int_eq(mustache_render_json(template, json_data, &renderer.base, &renderer, NULL, NULL), 0);
                    
                    char *result = mustache_string_renderer_get(&renderer);
                    check_not_null(result);
                    if (result) {
                        check_str_eq(result, "A B ");
                        free(result);
                    }
                    
                    mustache_string_renderer_free(&renderer);
                    mustache_release(template);
                }
                json_free(json_data);
            }
        }

        it("should handle inverted sections") {
            const char *template_str = "{{^missing}}Not found{{/missing}}{{#missing}}Found{{/missing}}";
            const char *json_str = "{}";
            
            json_value_t *json_data = json_parse(json_str, strlen(json_str));
            check_not_null(json_data);
            
            if (json_data) {
                MUSTACHE_TEMPLATE *template = mustache_compile(template_str, strlen(template_str), NULL, NULL, 0);
                check_not_null(template);
                
                if (template) {
                    MUSTACHE_STRING_RENDERER renderer;
                    check_int_eq(mustache_string_renderer_init(&renderer), 0);
                    
                    check_int_eq(mustache_render_json(template, json_data, &renderer.base, &renderer, NULL, NULL), 0);
                    
                    char *result = mustache_string_renderer_get(&renderer);
                    check_not_null(result);
                    if (result) {
                        check_str_eq(result, "Not found");
                        free(result);
                    }
                    
                    mustache_string_renderer_free(&renderer);
                    mustache_release(template);
                }
                json_free(json_data);
            }
        }
    }

    describe("security") {
        it("should escape HTML by default") {
            const char *template_str = "{{text}} vs {{{text}}}";
            const char *json_str = "{\"text\": \"<script>alert('xss')</script>\"}";
            
            json_value_t *json_data = json_parse(json_str, strlen(json_str));
            check_not_null(json_data);
            
            if (json_data) {
                MUSTACHE_TEMPLATE *template = mustache_compile(template_str, strlen(template_str), NULL, NULL, 0);
                check_not_null(template);
                
                if (template) {
                    MUSTACHE_STRING_RENDERER renderer;
                    check_int_eq(mustache_string_renderer_init(&renderer), 0);
                    
                    check_int_eq(mustache_render_json(template, json_data, &renderer.base, &renderer, NULL, NULL), 0);
                    
                    char *result = mustache_string_renderer_get(&renderer);
                    check_not_null(result);
                    if (result) {
                        /* Should contain escaped and unescaped versions */
                        check_str_contains(result, "&lt;script&gt;");
                        check_str_contains(result, "<script>alert('xss')</script>");
                        free(result);
                    }
                    
                    mustache_string_renderer_free(&renderer);
                    mustache_release(template);
                }
                json_free(json_data);
            }
        }
    }
}
