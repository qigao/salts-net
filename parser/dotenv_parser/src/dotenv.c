#include "dotenv.h"
#include "dotenv_lexer.h"
#include "turbo_str.h"
#include <fmt.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#if defined(_WIN32)
#if defined(_MSC_VER)
#ifndef strdup
#define strdup _strdup
#endif
#endif

static int setenv(const char *name, const char *value, int overwrite)
{
    int errcode = 0;
    if (!overwrite)
    {
        size_t envsize = 0;
        errcode = getenv_s(&envsize, NULL, 0, name);
        if (errcode || envsize)
            return errcode;
    }
    return _putenv_s(name, value);
}
#endif

static tstr_t concat(tstr_t buffer, const char *string)
{
    if (!string) return buffer;
    tstr_t updated = tstr_cat(buffer, string);
    return updated ? updated : buffer;
}

static tstr_t resolve_nested(const char *value)
{
    if (!value) return NULL;
    
    // Simple check for ${}
    if (!strstr(value, "${")) return tstr_dup(value);

    tstr_t result = NULL;
    const char *ptr = value;
    const char *start;

    while ((start = strstr(ptr, "${")) != NULL) {
        // Concat everything before ${
        if (start > ptr) {
            size_t len = start - ptr;
            tstr_t updated = tstr_cat_len(result, ptr, len);
            if (updated) {
                result = updated;
            }
        }

        const char *end = strstr(start, "}");
        if (!end) break; // Unterminated ${

        size_t name_len = end - (start + 2);
        tstr_t name = tstr_dup_len(start + 2, name_len);
        if (!name) break;

        const char *env_val = getenv(name);
        if (env_val) {
            result = concat(result, env_val);
        }
        tstr_free(name);
        ptr = end + 1;
    }

    if (*ptr) {
        result = concat(result, ptr);
    }

    return result ? result : tstr_new();
}

int dotenv_load(const char *path, bool overwrite)
{
    if (!path) return -1;

    char full_path[1024];
    FILE *file = fopen(path, "rb");

    if (!file) {
        // Try appending /.env if path is a directory (or just doesn't exist as is)
        fmt(full_path, sizeof(full_path), "{}/.env", path);
        file = fopen(full_path, "rb");
    }

    if (!file) return -1;

    // Read whole file into memory
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (size < 0) {
        fclose(file);
        return -1;
    }

    char *buffer = malloc(size + 1);
    if (!buffer) {
        fclose(file);
        return -1;
    }

    size_t read_len = fread(buffer, 1, size, file);
    buffer[read_len] = '\0';
    fclose(file);

    dotenv_lexer_t lexer;
    dotenv_lexer_init(&lexer, buffer, read_len);

    dotenv_token_t token;
    tstr_t current_key = NULL;

    while (dotenv_lexer_next(&lexer, &token) > 0) {
        if (token.type == DOTENV_TOKEN_KEY) {
            tstr_free(current_key);
            current_key = tstr_dup_len(token.value, token.length);
        } else if (token.type == DOTENV_TOKEN_VALUE) {
            if (current_key) {
                tstr_t raw_val = tstr_dup_len(token.value, token.length);
                if (!raw_val) {
                    tstr_free(current_key);
                    current_key = NULL;
                    continue;
                }

                tstr_t final_val = resolve_nested(raw_val);
                setenv(current_key, final_val, overwrite ? 1 : 0);

                tstr_free(raw_val);
                tstr_free(final_val);
                tstr_free(current_key);
                current_key = NULL;
            }
        } else if (token.type == DOTENV_TOKEN_EOF) {
            break;
        }
    }

    tstr_free(current_key);
    free(buffer);

    return 0;
}

int dotenv_load_default(bool overwrite)
{
    return dotenv_load(".env", overwrite);
}
