/**
 * @file ini_parser.c
 * @brief INI Parser Implementation using re2c/lemon
 */

#include "ini_parser.h"
#include "ini_lexer.h"
#include "ini_types.h"
#include "ini_grammar_gen.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

struct ini_t {
    ini_section_t *sections;
    size_t section_count;
};

/* Forward declarations for lemon parser */
void *IniParseAlloc(void *(*)(size_t));
void IniParseFree(void *, void (*)(void *));
void IniParse(void *, int, ini_token_t, ini_parse_ctx_t *);

static ini_section_t *ini_find_section(ini_t *ini, const char *name) {
    if (!name) name = "";
    for (ini_section_t *s = ini->sections; s; s = s->next) {
        if (strcmp(s->name, name) == 0) {
            return s;
        }
    }
    return NULL;
}

ini_t *ini_parse(const char *content, size_t len) {
    if (!content) return NULL;

    ini_lexer_t lexer;
    ini_token_t token;
    ini_parse_ctx_t ctx = {0};

    ini_lexer_init(&lexer, content, len);

    void *parser = IniParseAlloc(malloc);
    if (!parser) return NULL;

    int result;
    while ((result = ini_lexer_next(&lexer, &token)) != 0) {
        if (result < 0) {
            ctx.error = 1;
            break;
        }
        IniParse(parser, token.type, token, &ctx);
    }

    /* Send EOF (token type 0) */
    token.type = 0;
    IniParse(parser, 0, token, &ctx);

    IniParseFree(parser, free);

    if (ctx.error) {
        /* Free parsed data on error */
        ini_section_t *s = ctx.sections;
        while (s) {
            ini_section_t *next_s = s->next;
            ini_entry_t *e = s->entries;
            while (e) {
                ini_entry_t *next_e = e->next;
                free(e->key);
                free(e->value);
                free(e);
                e = next_e;
            }
            free(s->name);
            free(s);
            s = next_s;
        }
        return NULL;
    }

    ini_t *ini = calloc(1, sizeof(ini_t));
    if (!ini) return NULL;

    ini->sections = ctx.sections;

    /* Count sections */
    for (ini_section_t *s = ini->sections; s; s = s->next) {
        ini->section_count++;
    }

    return ini;
}

ini_t *ini_parse_file(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0) {
        fclose(f);
        return NULL;
    }

    char *content = malloc((size_t)size + 1);
    if (!content) {
        fclose(f);
        return NULL;
    }

    size_t read_size = fread(content, 1, (size_t)size, f);
    fclose(f);

    content[read_size] = '\0';
    ini_t *ini = ini_parse(content, read_size);
    free(content);
    return ini;
}

void ini_free(ini_t *ini) {
    if (!ini) return;

    ini_section_t *s = ini->sections;
    while (s) {
        ini_section_t *next_s = s->next;
        ini_entry_t *e = s->entries;
        while (e) {
            ini_entry_t *next_e = e->next;
            free(e->key);
            free(e->value);
            free(e);
            e = next_e;
        }
        free(s->name);
        free(s);
        s = next_s;
    }
    free(ini);
}

const char *ini_get(ini_t *ini, const char *section, const char *key) {
    if (!ini || !key) return NULL;

    ini_section_t *s = ini_find_section(ini, section);
    if (!s) return NULL;

    for (ini_entry_t *e = s->entries; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            return e->value;
        }
    }
    return NULL;
}

int ini_get_int(ini_t *ini, const char *section, const char *key, int default_val) {
    const char *val = ini_get(ini, section, key);
    if (!val) return default_val;

    char *end;
    long result = strtol(val, &end, 0);
    return (end != val) ? (int)result : default_val;
}

bool ini_get_bool(ini_t *ini, const char *section, const char *key, bool default_val) {
    const char *val = ini_get(ini, section, key);
    if (!val) return default_val;

    if (strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0 ||
        strcasecmp(val, "yes") == 0 || strcasecmp(val, "on") == 0) {
        return true;
    }
    if (strcmp(val, "0") == 0 || strcasecmp(val, "false") == 0 ||
        strcasecmp(val, "no") == 0 || strcasecmp(val, "off") == 0) {
        return false;
    }
    return default_val;
}

double ini_get_double(ini_t *ini, const char *section, const char *key, double default_val) {
    const char *val = ini_get(ini, section, key);
    if (!val) return default_val;

    char *end;
    double result = strtod(val, &end);
    return (end != val) ? result : default_val;
}

size_t ini_section_count(ini_t *ini) {
    return ini ? ini->section_count : 0;
}

const char *ini_section_name(ini_t *ini, size_t index) {
    if (!ini) return NULL;

    size_t i = 0;
    for (ini_section_t *s = ini->sections; s; s = s->next, i++) {
        if (i == index) return s->name;
    }
    return NULL;
}

size_t ini_key_count(ini_t *ini, const char *section) {
    if (!ini) return 0;

    ini_section_t *s = ini_find_section(ini, section);
    if (!s) return 0;

    size_t count = 0;
    for (ini_entry_t *e = s->entries; e; e = e->next) {
        count++;
    }
    return count;
}

const char *ini_key_name(ini_t *ini, const char *section, size_t index) {
    if (!ini) return NULL;

    ini_section_t *s = ini_find_section(ini, section);
    if (!s) return NULL;

    size_t i = 0;
    for (ini_entry_t *e = s->entries; e; e = e->next, i++) {
        if (i == index) return e->key;
    }
    return NULL;
}
