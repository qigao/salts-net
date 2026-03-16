#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cmd_arger.h"
#include "turbo_script.h"

int main(int argc, char **argv) {
    char *eval_str = NULL;
    char *file_path = NULL;

    CmdArgerDesc optional_args[] = {
        cmd_arger_desc_string_sh(&eval_str, "eval", "e", "Evaluate a string of code and exit"),
        cmd_arger_desc_string_sh(&file_path, "file", "f", "Run the given script file and exit"),
    };

    cmd_arger_parse(optional_args, sizeof(optional_args) / sizeof(optional_args[0]),
                    NULL, 0, argc, argv, "TurboScript REPL", cmd_arger_true);

    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
    if (!ctx) {
        fprintf(stderr, "Failed to initialize TurboScript context.\n");
        return 1;
    }

    if (eval_str) {
        int res = turbo_script_run(ctx, eval_str);
        turbo_script_free(ctx);
        return (res < 0) ? 1 : 0;
    }

    if (file_path) {
        int res = turbo_script_run_file(ctx, file_path);
        turbo_script_free(ctx);
        return (res < 0) ? 1 : 0;
    }

#ifdef DEBUG
    printf("TurboScript REPL (v1.0) [DEBUG MODE - Interpreter]\n");
#else
    printf("TurboScript REPL (v1.0) [JIT Mode]\n");
#endif
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

        if (turbo_script_run_and_print(ctx, buffer) < 0) {
            fprintf(stderr, "Error: %s\n", turbo_script_get_error(ctx));
        }
    }

    turbo_script_free(ctx);
    return 0;
}
