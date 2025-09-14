/**
 * @file test_json_integration.c
 * @brief Tests for mustache-JSON integration using acutest framework
 */

#include "acutest.h"
#include "mustache_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_basic_rendering(void)
{
    const char *template_str = "Hello {{name}}!";
    const char *json_str = "{\"name\": \"World\"}";
    
    json_value_t *json_data = json_parse(json_str, strlen(json_str));
    TEST_CHECK(json_data != NULL);
    
    MUSTACHE_TEMPLATE *template = mustache_compile(template_str, strlen(template_str), NULL, NULL, 0);
    TEST_CHECK(template != NULL);
    
    MUSTACHE_STRING_RENDERER renderer;
    TEST_CHECK(mustache_string_renderer_init(&renderer) == 0);
    
    TEST_CHECK(mustache_render_json(template, json_data, &renderer.base, &renderer, NULL, NULL) == 0);
    
    char *result = mustache_string_renderer_get(&renderer);
    TEST_CHECK(result != NULL);
    TEST_CHECK_(strcmp(result, "Hello World!") == 0, "Expected 'Hello World!', got '%s'", result);
    
    free(result);
    mustache_string_renderer_free(&renderer);
    mustache_release(template);
    json_free(json_data);
}

static void test_array_iteration(void)
{
    const char *template_str = "{{#items}}{{name}} {{/items}}";
    const char *json_str = "{\"items\": [{\"name\": \"A\"}, {\"name\": \"B\"}]}";
    
    json_value_t *json_data = json_parse(json_str, strlen(json_str));
    TEST_CHECK(json_data != NULL);
    
    MUSTACHE_TEMPLATE *template = mustache_compile(template_str, strlen(template_str), NULL, NULL, 0);
    TEST_CHECK(template != NULL);
    
    MUSTACHE_STRING_RENDERER renderer;
    TEST_CHECK(mustache_string_renderer_init(&renderer) == 0);
    
    TEST_CHECK(mustache_render_json(template, json_data, &renderer.base, &renderer, NULL, NULL) == 0);
    
    char *result = mustache_string_renderer_get(&renderer);
    TEST_CHECK(result != NULL);
    TEST_CHECK_(strcmp(result, "A B ") == 0, "Expected 'A B ', got '%s'", result);
    
    free(result);
    mustache_string_renderer_free(&renderer);
    mustache_release(template);
    json_free(json_data);
}

static void test_inverted_sections(void)
{
    const char *template_str = "{{^missing}}Not found{{/missing}}{{#missing}}Found{{/missing}}";
    const char *json_str = "{}";
    
    json_value_t *json_data = json_parse(json_str, strlen(json_str));
    TEST_CHECK(json_data != NULL);
    
    MUSTACHE_TEMPLATE *template = mustache_compile(template_str, strlen(template_str), NULL, NULL, 0);
    TEST_CHECK(template != NULL);
    
    MUSTACHE_STRING_RENDERER renderer;
    TEST_CHECK(mustache_string_renderer_init(&renderer) == 0);
    
    TEST_CHECK(mustache_render_json(template, json_data, &renderer.base, &renderer, NULL, NULL) == 0);
    
    char *result = mustache_string_renderer_get(&renderer);
    TEST_CHECK(result != NULL);
    TEST_CHECK_(strcmp(result, "Not found") == 0, "Expected 'Not found', got '%s'", result);
    
    free(result);
    mustache_string_renderer_free(&renderer);
    mustache_release(template);
    json_free(json_data);
}

static void test_html_escaping(void)
{
    const char *template_str = "{{text}} vs {{{text}}}";
    const char *json_str = "{\"text\": \"<script>alert('xss')</script>\"}";
    
    json_value_t *json_data = json_parse(json_str, strlen(json_str));
    TEST_CHECK(json_data != NULL);
    
    MUSTACHE_TEMPLATE *template = mustache_compile(template_str, strlen(template_str), NULL, NULL, 0);
    TEST_CHECK(template != NULL);
    
    MUSTACHE_STRING_RENDERER renderer;
    TEST_CHECK(mustache_string_renderer_init(&renderer) == 0);
    
    TEST_CHECK(mustache_render_json(template, json_data, &renderer.base, &renderer, NULL, NULL) == 0);
    
    char *result = mustache_string_renderer_get(&renderer);
    TEST_CHECK(result != NULL);
    
    /* Should contain escaped and unescaped versions */
    TEST_CHECK_(strstr(result, "&lt;script&gt;") != NULL, "Expected escaped HTML in result: %s", result);
    TEST_CHECK_(strstr(result, "<script>alert('xss')</script>") != NULL, "Expected unescaped HTML in result: %s", result);
    
    free(result);
    mustache_string_renderer_free(&renderer);
    mustache_release(template);
    json_free(json_data);
}

TEST_LIST = {
    { "basic_rendering", test_basic_rendering },
    { "array_iteration", test_array_iteration },
    { "inverted_sections", test_inverted_sections },
    { "html_escaping", test_html_escaping },
    { 0 }
};