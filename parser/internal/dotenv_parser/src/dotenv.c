#include "dotenv.h"
#include "dotenv_lexer.h"
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

static char *concat(char *buffer, const char *string)
{
    if (!string) return buffer;
    if (!buffer) return strdup(string);

    size_t length = strlen(buffer) + strlen(string) + 1;
    char *new_buf = realloc(buffer, length);
    if (!new_buf) return buffer;
    strcat(new_buf, string);
    return new_buf;
}

static char *resolve_nested(const char *value)
{
    if (!value) return NULL;
    
    // Simple check for ${}
    if (!strstr(value, "${")) return strdup(value);

    char *result = NULL;
    const char *ptr = value;
    const char *start;

    while ((start = strstr(ptr, "${")) != NULL) {
        // Concat everything before ${
        if (start > ptr) {
            size_t len = start - ptr;
            char *tmp = malloc(len + 1);
            memcpy(tmp, ptr, len);
            tmp[len] = '\0';
            result = concat(result, tmp);
            free(tmp);
        }

        const char *end = strstr(start, "}");
        if (!end) break; // Unterminated ${

        size_t name_len = end - (start + 2);
        char *name = malloc(name_len + 1);
        memcpy(name, start + 2, name_len);
        name[name_len] = '\0';

        const char *env_val = getenv(name);
        if (env_val) {
            result = concat(result, env_val);
        }
        free(name);
        ptr = end + 1;
    }

    if (*ptr) {
        result = concat(result, ptr);
    }

    return result ? result : strdup("");
}

int dotenv_load(const char *path, bool overwrite)
{
    if (!path) return -1;

    char full_path[1024];
    FILE *file = fopen(path, "rb");

    if (!file) {
        // Try appending /.env if path is a directory (or just doesn't exist as is)
        snprintf(full_path, sizeof(full_path), "%s/.env", path);
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
    char *current_key = NULL;

    while (dotenv_lexer_next(&lexer, &token) > 0) {
        if (token.type == DOTENV_TOKEN_KEY) {
            current_key = malloc(token.length + 1);
            memcpy(current_key, token.value, token.length);
            current_key[token.length] = '\0';
        } else if (token.type == DOTENV_TOKEN_VALUE) {
            if (current_key) {
                char *raw_val = malloc(token.length + 1);
                memcpy(raw_val, token.value, token.length);
                raw_val[token.length] = '\0';

                char *final_val = resolve_nested(raw_val);
                setenv(current_key, final_val, overwrite ? 1 : 0);

                free(raw_val);
                free(final_val);
                free(current_key);
                current_key = NULL;
            }
        } else if (token.type == DOTENV_TOKEN_EOF) {
            break;
        }
    }

    if (current_key) free(current_key);
    free(buffer);

    return 0;
}

int dotenv_load_default(bool overwrite)
{
    return dotenv_load(".env", overwrite);
}
