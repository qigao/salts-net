#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cmd_arger.h"
#include "turbo_script.h"

int main(int argc, char **argv) {
    CmdArgerBool use_jit = cmd_arger_false;
    char *eval_str = NULL;
    char *file_path = NULL;

    CmdArgerDesc optional_args[] = {
        cmd_arger_desc_flag_sh(&use_jit, "jit", "j", "Enable JIT execution via MIR"),
        cmd_arger_desc_string_sh(&eval_str, "eval", "e", "Evaluate a string of code and exit"),
        cmd_arger_desc_string_sh(&file_path, "file", "f", "Run the given script file and exit"),
    };

    cmd_arger_parse(optional_args, sizeof(optional_args) / sizeof(optional_args[0]),
                    NULL, 0, argc, argv, "TurboScript REPL", cmd_arger_true);

    turbo_script_ctx_t *ctx = turbo_script_init();
    if (!ctx) {
        fprintf(stderr, "Failed to initialize TurboScript context.\n");
        return 1;
    }

    if (eval_str) {
        int res;
        if (use_jit) {
            turbo_script_compile_mir(ctx, eval_str);
            res = turbo_script_exec_jit(ctx);    
        } else {
            res = turbo_script_run(ctx, eval_str);
        }
        turbo_script_free(ctx);
        return (res < 0) ? 1 : 0;
    }

    if (file_path) {
        int res = turbo_script_run_file(ctx, file_path);
        turbo_script_free(ctx);
        return (res < 0) ? 1 : 0;
    }

    printf("TurboScript REPL (v1.0)\n");
    printf("Type 'exit' or 'quit' to exit.\n");

    char buffer[4096];
    while (1) {
        printf("ts> ");
        if (!fgets(buffer, sizeof(buffer), stdin)) {
            break;
        }

        size_t len = strlen(buffer);
        while (len > 0 && (buffer[len - 1] == '\r' || buffer[len - 1] == '\n')) {
            buffer[--len] = '\0';
        }

        if (strcmp(buffer, "exit") == 0 || strcmp(buffer, "quit") == 0) {
            break;
        }

        if (len == 0) {
            continue;
        }

        if (use_jit) {
            if (turbo_script_compile_mir(ctx, buffer) == 0) {
                if (turbo_script_exec_jit(ctx) < 0) {
                    fprintf(stderr, "JIT Execution Error: %s\n", turbo_script_get_error(ctx));
                }
            } else {
                fprintf(stderr, "JIT Compile Error: %s\n", turbo_script_get_error(ctx));
            }
        } else {
            if (turbo_script_repl_run(ctx, buffer) < 0) {
                fprintf(stderr, "Error: %s\n", turbo_script_get_error(ctx));
            }
        }
    }

    turbo_script_free(ctx);
    return 0;
}
