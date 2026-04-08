/**
 * @file json_example.c
 * @brief Example demonstrating mustache templating with JSON data
 */

#include "mustache_json.h"
#include "json_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    const char *template_str = 
        "Hello {{name}}!\n"
        "{{#users}}"
        "User: {{name}} ({{email}})\n"
        "{{/users}}"
        "{{#admin}}"
        "Admin privileges: {{level}}\n"
        "{{/admin}}"
        "{{^admin}}"
        "No admin privileges\n"
        "{{/admin}}";

    const char *json_str = 
        "{"
        "\"name\": \"World\","
        "\"users\": ["
        "  {\"name\": \"John\", \"email\": \"john@example.com\"},"
        "  {\"name\": \"Jane\", \"email\": \"jane@example.com\"}"
        "],"
        "\"admin\": {\"level\": \"super\"}"
        "}";

    /* Parse JSON data */
    json_value_t *json_data = json_parse(json_str, strlen(json_str));
    if (!json_data) {
        fprintf(stderr, "Failed to parse JSON: %s\n", json_get_error());
        return 1;
    }

    /* Compile mustache template */
    MUSTACHE_TEMPLATE *template = mustache_compile(template_str, strlen(template_str), NULL, NULL, 0);
    if (!template) {
        fprintf(stderr, "Failed to compile template\n");
        json_free(json_data);
        return 1;
    }

    /* Initialize string renderer */
    MUSTACHE_STRING_RENDERER renderer;
    if (mustache_string_renderer_init(&renderer) != 0) {
        fprintf(stderr, "Failed to initialize renderer\n");
        mustache_release(template);
        json_free(json_data);
        return 1;
    }

    /* Render template with JSON data */
    if (mustache_render_json(template, json_data, &renderer.base, &renderer, NULL, NULL) != 0) {
        fprintf(stderr, "Failed to render template\n");
        mustache_string_renderer_free(&renderer);
        mustache_release(template);
        json_free(json_data);
        return 1;
    }

    /* Get and print result */
    char *result = mustache_string_renderer_get(&renderer);
    if (result) {
        printf("Rendered output:\n%s", result);
        free(result);
    }

    /* Cleanup */
    mustache_string_renderer_free(&renderer);
    mustache_release(template);
    json_free(json_data);

    return 0;
}
