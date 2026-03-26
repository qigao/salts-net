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

#include <stdint.h>
#include "compiler_core.h"
#include "cmd_arger.h"

int main(int argc, char **argv) {
    char    *schema_path   = NULL;
    char    *template_path = NULL;
    char    *output_path   = NULL;
    char    *dsl_output_path = NULL;
    int64_t  lang_enum     = 0;

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

    tbe_compiler_options_t options = {
        .schema_path = schema_path,
        .template_path = template_path,
        .output_path = output_path,
        .dsl_output_path = dsl_output_path,
        .lang_enum = lang_enum,
    };

    return tbe_compiler_run(&options);
}
