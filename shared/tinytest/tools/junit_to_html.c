#include <stdio.h>
#include <stdlib.h>
#include <platform.h>
#include <string.h>

#include "turbo_str.h"
#include "turbo_fs.h"
#include "cmd_arger.h"
#include "mustache_xml.h"
#include "xml/cxparser.h"
#include "core/cxdefs.h"



static const char* get_attr(cxml_elem_node *elem, const char *name) {
    if (!elem->attributes) return "";
    cxml_attr_node *attr = cxml_table_get(elem->attributes, name);
    if (attr) {
        return cxml_string_as_raw(&attr->value);
    }
    return "";
}

static void add_xml_attr(cxml_elem_node *elem, const char *name, const char *value) {
    if (!elem->attributes) {
        elem->attributes = new_alloc_cxml_table();
    }
    cxml_attr_node *attr = malloc(sizeof(cxml_attr_node));
    cxml_attr_node_init(attr);
    cxml_name_init(&attr->name);
    cxml_string_init(&attr->name.qname);
    cxml_string_raw_append(&attr->name.qname, name);
    cxml_string_init(&attr->value);
    cxml_string_raw_append(&attr->value, value);
    cxml_table_put(elem->attributes, name, attr);
}

static cxml_elem_node* find_child(cxml_elem_node* parent, const char* name) {
    if (!parent) return NULL;
    cxml_for_each(node, &parent->children) {
        if (_cxml_get_node_type(node) == CXML_ELEM_NODE) {
            cxml_elem_node *elem = (cxml_elem_node *)node;
            if (strcmp(cxml_string_as_raw(&elem->name.qname), name) == 0) {
                return elem;
            }
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *xml_file = NULL;
    const char *output_file = "test_dashboard.html";
    const char *template_path = "dashboard.html";
    CmdArgerBool use_colors = cmd_arger_true;
    CmdArgerBool no_color = cmd_arger_false;

    CmdArgerDesc optional_args[] = {
        cmd_arger_required(
            cmd_arger_desc_string_sh((char **)&xml_file, "input", "i", "JUnit XML input file")),
        cmd_arger_desc_string_sh((char **)&output_file, "output", "o", "Output HTML file"),
        cmd_arger_desc_string_sh((char **)&template_path, "template", "t", "Mustache template file"),
        cmd_arger_desc_flag(&no_color, "no-color", "Disable terminal colors"),
    };

    cmd_arger_parse(optional_args, sizeof(optional_args) / sizeof(*optional_args), NULL, 0, argc,
                    argv, "JUnit to HTML Dashboard Generator v1.2 (C refactor)", use_colors);

    if (no_color) use_colors = cmd_arger_false;

    if (!xml_file) {
        fprintf(stderr, "Error: XML input file is required.\n");
        return 1;
    }

    cxml_root_node *root_node = cxml_parse_xml_lazy(xml_file);
    if (!root_node) {
        fprintf(stderr, "Error: Could not parse XML: %s\n", xml_file);
        return 1;
    }

    /* root_node->root_element should be <testsuites> or <testsuite> */
    cxml_elem_node *root_elem = root_node->root_element;
    cxml_elem_node *suite = NULL;

    if (root_elem) {
        const char* root_name = cxml_string_as_raw(&root_elem->name.qname);
        if (tstr_casecmp(root_name, "testsuite") == 0) {
            suite = root_elem;
        } else {
            /* Look for testsuite children */
            suite = find_child(root_elem, "testsuite");
        }
    }

    if (!suite) {
        fprintf(stderr, "Error: No testsuite found in XML\n");
        cxml_root_node_free(root_node);
        return 1;
    }

    /* Decorate XML with computed values for the template */
    int total = atoi(get_attr(suite, "tests"));
    int failures = atoi(get_attr(suite, "failures"));
    int skipped = atoi(get_attr(suite, "skipped"));
    int passed = total - failures - skipped;
    double pass_percentage = (total > 0) ? (double)passed / total * 100.0 : 0.0;

    char buf[64];
    sprintf(buf, "%d", passed);
    add_xml_attr(suite, "passed_tests", buf);
    sprintf(buf, "%d", total);
    add_xml_attr(suite, "total_tests", buf);
    sprintf(buf, "%d", failures);
    add_xml_attr(suite, "failed_tests", buf);
    sprintf(buf, "%d", skipped);
    add_xml_attr(suite, "skipped_tests", buf);
    sprintf(buf, "%.1f", pass_percentage);
    add_xml_attr(suite, "pass_percentage", buf);

    /* Ensure name and timestamp are present for template */
    if (strlen(get_attr(suite, "name")) == 0) add_xml_attr(suite, "name", "Test Results");
    if (strlen(get_attr(suite, "timestamp")) == 0) add_xml_attr(suite, "timestamp", "Unknown");

    /* Decorate testcase nodes with status */
    int case_count = 0;
    cxml_for_each(node, &suite->children) {
        if (_cxml_get_node_type(node) != CXML_ELEM_NODE) continue;
        cxml_elem_node *testcase = (cxml_elem_node *)node;
        const char* tc_name = cxml_string_as_raw(&testcase->name.qname);
        if (tstr_casecmp(tc_name, "testcase") != 0) continue;

        case_count++;
        if (find_child(testcase, "failure")) {
            add_xml_attr(testcase, "status", "failed");
        } else if (find_child(testcase, "skipped")) {
            add_xml_attr(testcase, "status", "skipped");
        } else {
            add_xml_attr(testcase, "status", "passed");
        }
    }
    printf("Processing %d test cases...\n", case_count);

    /* Load template */
    turbo_fs_buf_t template_buf;
    if (turbo_fs_read_file(template_path, &template_buf) != 0) {
        fprintf(stderr, "Warning: Could not open template file at: %s. Trying relative to executable...\n", template_path);
        if (turbo_fs_read_file("dashboard.html", &template_buf) != 0) {
            fprintf(stderr, "Error: Could not find template file.\n");
            cxml_root_node_free(root_node);
            return 1;
        }
    }
    char *template_str = template_buf.base;
    size_t template_size = template_buf.len;

    /* Compile template */
    MUSTACHE_TEMPLATE *mustache_templ = mustache_compile(template_str, template_size, NULL, NULL, 0);
    free(template_str);

    if (!mustache_templ) {
        fprintf(stderr, "Error: Failed to compile mustache template\n");
        cxml_root_node_free(root_node);
        return 1;
    }

    /* Render */
    MUSTACHE_STRING_RENDERER renderer;
    if (mustache_string_renderer_init(&renderer) != 0) {
        fprintf(stderr, "Error: Failed to initialize mustache renderer\n");
        mustache_release(mustache_templ);
        cxml_root_node_free(root_node);
        return 1;
    }

    if (mustache_render_xml(mustache_templ, suite, &renderer.base, &renderer, NULL, NULL) != 0) {
        fprintf(stderr, "Error: Failed during mustache rendering\n");
        mustache_string_renderer_free(&renderer);
        mustache_release(mustache_templ);
        cxml_root_node_free(root_node);
        return 1;
    }

    char *output_str = mustache_string_renderer_get(&renderer);
    if (output_str) {
        FILE *out_f = fopen(output_file, "wb");
        if (out_f) {
            fwrite(output_str, 1, strlen(output_str), out_f);
            fclose(out_f);
            printf("Dashboard generated: %s\n", output_file);
        } else {
            fprintf(stderr, "Error: Could not write to output file: %s\n", output_file);
        }
        free(output_str);
    }

    /* Cleanup */
    mustache_string_renderer_free(&renderer);
    mustache_release(mustache_templ);
    cxml_root_node_free(root_node);

    return 0;
}
