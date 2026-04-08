/**
 * @file xml_example.c
 * @brief Example of using XML data provider with Mustache
 */
#include "mustache_xml.h"
#include "xml/cxparser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main() {
    const char *xml_data = 
        "<dashboard>"
        "  <project name=\"TurboUtils\">"
        "    <version>2.0</version>"
        "    <status>Active</status>"
        "  </project>"
        "  <stats>"
        "    <item>Tests: 150</item>"
        "    <item>Coverage: 85%</item>"
        "    <item>Uptime: 99.9%</item>"
        "  </stats>"
        "</dashboard>";

    const char *template_text = 
        "Project: {{project.name}}\n"
        "Version: {{project.version}}\n"
        "Status: {{project.status}}\n"
        "\n"
        "Stats:\n"
        "{{#stats.item}}"
        " - {{.}}\n"
        "{{/stats.item}}";

    /* Parse XML */
    cxml_root_node *root = cxml_parse_xml(xml_data);
    if (!root) {
        fprintf(stderr, "Failed to parse XML\n");
        return 1;
    }

    /* Compile Template */
    MUSTACHE_TEMPLATE *templ = mustache_compile(template_text, strlen(template_text), NULL, NULL, 0);
    if (!templ) {
        fprintf(stderr, "Failed to compile template\n");
        cxml_root_node_free(root);
        return 1;
    }

    /* Setup renderer */
    MUSTACHE_STRING_RENDERER renderer;
    mustache_string_renderer_init(&renderer);

    /* Render */
    if (mustache_render_xml(templ, root, &renderer.base, &renderer, NULL, NULL) != 0) {
        fprintf(stderr, "Failed to render template\n");
    } else {
        char *result = mustache_string_renderer_get(&renderer);
        printf("Rendered result:\n---\n%s\n---\n", result);
        free(result);
    }

    /* Cleanup */
    mustache_string_renderer_free(&renderer);
    mustache_release(templ);
    cxml_root_node_free(root);

    return 0;
}
