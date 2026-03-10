/**
 * @file main.c
 * @brief tbe_compiler — code generator from schema definitions.
 *
 * Reads a .schema file, parses it, and renders output through a Mustache
 * template.  Supports multiple target languages by selecting different
 * template files (built-in or custom).
 *
 * CLI (via cmd_arger):
 *   tbe_compiler --schema <file> [--template <file>] [--lang c|python|rust]
 *              [--output <file>] [--dsl-output <file>]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <errno.h>
#include "node_tree.h"
#include "schema_parser_dsl.h"
#include "tbe_error.h"
#include "mustache_helpers.h"
#include "mustache.h"
#include "cmd_arger.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static char *read_file(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }

    long file_size = ftell(f);
    if (file_size < 0) {
        fclose(f);
        return NULL;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    size_t size = (size_t)file_size;
    char *dat = (char *)malloc(size + 1);
    if (!dat) {
        fclose(f);
        return NULL;
    }

    size_t bytes_read = fread(dat, 1, size, f);
    fclose(f);

    if (bytes_read != size) {
        free(dat);
        return NULL;
    }

    dat[size] = '\0';
    return dat;
}

/**
 * Resolve the template path.
 *   - If the user passed --template, use that directly.
 *   - Otherwise pick a built-in template based on --lang.
 */
static const char *resolve_template(const char *user_template,
                                    int64_t     lang_enum) {
    if (user_template) return user_template;

    /* Enum values: 0 = c (default), 1 = python, 2 = rust */
    switch (lang_enum) {
    default:
    case 0:  return "templates/c_structs.mustache";
    case 1:  return "templates/python_dataclass.mustache";
    case 2:  return "templates/rust_structs.mustache";
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    /* ---- argument descriptors ------------------------------------ */
    char    *schema_path   = NULL;
    char    *template_path = NULL;
    char    *output_path   = NULL;
    char    *dsl_output_path = NULL;
    int64_t  lang_enum     = 0;      /* default: c */

    CmdArgerEnumDesc lang_choices[] = {
        { "c",      "C/C++ header output",            0 },
        { "python", "Python dataclass output",         1 },
        { "rust",   "Rust struct output",              2 },
    };

    CmdArgerDesc optional_args[] = {
        cmd_arger_required(cmd_arger_desc_string_sh(&schema_path, "schema", "s",
                                 "Path to the .schema definition file")),
        cmd_arger_desc_string_sh(&template_path, "template", "t",
                                 "Path to a custom Mustache template file"),
        cmd_arger_desc_enum_sh(&lang_enum, "lang", "l",
                               "Target language (built-in template)",
                               lang_choices, 3),
        cmd_arger_desc_string_sh(&output_path, "output", "o",
                                 "Output file path (default: stdout)"),
        cmd_arger_desc_string_sh(&dsl_output_path, "dsl-output", "d",
                                 "Generate DSL type declarations (.rfl file)"),
    };

    cmd_arger_parse(
        optional_args, sizeof(optional_args) / sizeof(*optional_args),
        NULL, 0,
        argc, argv,
        "tbe_compiler - schema-based code generator",
        cmd_arger_false);

    /* ---- read schema --------------------------------------------- */
    char *schema_data = read_file(schema_path);
    if (!schema_data) {
        fprintf(stderr, "Failed to read schema file: %s\n", schema_path);
        return 1;
    }

    /* ---- parse schema -------------------------------------------- */
    Node *root = create_node_map(NULL);
    if (!root) {
        fprintf(stderr, "Failed to allocate root node\n");
        free(schema_data);
        return 1;
    }

    tbe_error_t parse_err;
    if (parse_schema(schema_data, strlen(schema_data), root, &parse_err) != 0) {
        if (parse_err.line >= 0) {
            fprintf(stderr, "Parse error at line %d: %s\n", parse_err.line, parse_err.message);
        } else {
            fprintf(stderr, "Parse error: %s\n", parse_err.message);
        }
        free(schema_data);
        node_free(root);
        return 1;
    }

    MUSTACHE_DATAPROVIDER provider = mustache_helpers_provider();
    MUSTACHE_RENDERER    renderer = mustache_helpers_renderer();
    int global_res = 0;

    /* ---- primary output generation ------------------------------- */
    const char *tmpl_path = resolve_template(template_path, lang_enum);
    char *templ_data = read_file(tmpl_path);
    if (!templ_data) {
        fprintf(stderr, "Failed to read template file: %s\n", tmpl_path);
        free(schema_data);
        node_free(root);
        return 1;
    }

    MUSTACHE_TEMPLATE *templ = mustache_compile(templ_data, strlen(templ_data), NULL, NULL, 0);
    if (!templ) {
        fprintf(stderr, "Failed to compile mustache template\n");
        free(templ_data);
        free(schema_data);
        node_free(root);
        return 1;
    }

    FILE *out_file = stdout;
    if (output_path) {
        out_file = fopen(output_path, "wb");
        if (!out_file) {
            fprintf(stderr, "Failed to open output file: %s\n", output_path);
            mustache_release(templ);
            free(templ_data);
            free(schema_data);
            node_free(root);
            return 1;
        }
    }

    int res = mustache_process(templ, &renderer, out_file, &provider, root);
    if (res != MUSTACHE_ERR_SUCCESS) {
        fprintf(stderr, "Failed to render mustache, error code: %d\n", res);
        global_res = 1;
    }

    if (out_file != stdout) fclose(out_file);
    mustache_release(templ);
    free(templ_data);

    /* ---- DSL Type generation ------------------------------------- */
    if (dsl_output_path) {
        char *dsl_types_data = read_file("templates/rfl_types.mustache");
        if (!dsl_types_data) {
            fprintf(stderr, "Failed to read DSL types template\n");
        } else {
            MUSTACHE_TEMPLATE *dsl_types_templ = mustache_compile(
                dsl_types_data, strlen(dsl_types_data), NULL, NULL, 0);
            if (!dsl_types_templ) {
                fprintf(stderr, "Failed to compile DSL types template\n");
            } else {
                FILE *dsl_types_file = fopen(dsl_output_path, "wb");
                if (!dsl_types_file) {
                    fprintf(stderr, "Failed to open DSL types output file: %s\n", dsl_output_path);
                } else {
                    mustache_process(dsl_types_templ, &renderer, dsl_types_file, &provider, root);
                    fclose(dsl_types_file);
                }
                mustache_release(dsl_types_templ);
            }
            free(dsl_types_data);
        }
    }

    /* ---- cleanup ------------------------------------------------- */
    free(schema_data);
    node_free(root);
    return global_res;
}
