#include "toonc.h"
#include "toon_lexer.h"
#include "toon_grammar_gen.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <stdint.h>
#include <stddef.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>

/* -----------------------------------------------------------------------------
 * Compiler-specific optimization macros
 * -------------------------------------------------------------------------- */

#if defined(_MSC_VER)
    #define FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
    #define FORCE_INLINE static inline __attribute__((always_inline))
#else
    #define FORCE_INLINE inline
#endif

#if defined(__GNUC__) || defined(__clang__)
    #define LIKELY(x)   __builtin_expect(!!(x), 1)
    #define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define LIKELY(x)   (x)
    #define UNLIKELY(x) (x)
#endif

/* Lemon forward declarations */
void *ToonParseAlloc(void *(*mallocProc)(size_t));
void ToonParse(void *yyp, int yymajor, toon_token_t yyminor, toon_parse_ctx_t *ctx);
void ToonParseFree(void *p, void (*freeProc)(void*));
void ToonParseTrace(FILE *TraceFILE, char *zTracePrompt);

/* -----------------------------------------------------------------------------
 * Memory allocation wrappers
 * -------------------------------------------------------------------------- */

static int safe_mul_size(size_t nmemb, size_t size, size_t *result) {
    if (size != 0 && nmemb > SIZE_MAX / size) return 0;
    *result = nmemb * size;
    return 1;
}

void *tmalloc(size_t size) {
    if (UNLIKELY(size == 0)) size = 1;
    void *ptr = malloc(size);
    if (UNLIKELY(ptr == NULL)) {
        fprintf(stderr, "TOONC: memory allocation failed.\n");
        exit(EXIT_FAILURE);
    }
    return ptr;
}

void *tcalloc(size_t nmemb, size_t size) {
    size_t total;
    if (!safe_mul_size(nmemb, size, &total)) {
        fprintf(stderr, "TOONC: memory allocation overflow.\n");
        exit(EXIT_FAILURE);
    }
    void *ptr = calloc(1, total);
    if (UNLIKELY(ptr == NULL)) {
        fprintf(stderr, "TOONC: memory allocation failed.\n");
        exit(EXIT_FAILURE);
    }
    return ptr;
}

void *trealloc(void *ptr, size_t size) {
    if (UNLIKELY(size == 0)) size = 1;
    void *new_ptr = realloc(ptr, size);
    if (UNLIKELY(new_ptr == NULL)) {
        fprintf(stderr, "TOONC: memory reallocation failed.\n");
        exit(EXIT_FAILURE);
    }
    return new_ptr;
}

void tfree(void *ptr) {
    if (ptr) free(ptr);
}

#define ARENA_MANAGED_SENTINEL ((void *)(uintptr_t)1)

/* -----------------------------------------------------------------------------
 * Object creation and manipulation (Arena-aware)
 * -------------------------------------------------------------------------- */

toonObject *TOONc_newObjectArena(void *arena, int kvtype) {
    toonObject *o = arena ? mem_alloc((mem_pool_t *)arena, sizeof(toonObject)) : tmalloc(sizeof(toonObject));
    o->kvtype = kvtype;
    o->indent = 0;
    o->key = NULL;
    o->child = NULL;
    o->next = NULL;
    o->arena = arena ? ARENA_MANAGED_SENTINEL : NULL;
    o->array.items = NULL;
    o->array.len = 0;
    o->array.capacity = 0;
    return o;
}

toonObject *TOONc_newStringObjArena(void *arena, char *s, size_t len) {
    toonObject *o = TOONc_newObjectArena(arena, KV_STRING);
    o->str.ptr = arena ? mem_alloc((mem_pool_t *)arena, len + 1) : tmalloc(len + 1);
    o->str.len = len;
    memcpy(o->str.ptr, s, len);
    o->str.ptr[len] = '\0';
    return o;
}

toonObject *TOONc_newIntObjArena(void *arena, int value) {
    toonObject *o = TOONc_newObjectArena(arena, KV_INT);
    o->i = value;
    return o;
}

toonObject *TOONc_newDoubleObjArena(void *arena, double value) {
    toonObject *o = TOONc_newObjectArena(arena, KV_DOUBLE);
    o->d = value;
    return o;
}

toonObject *TOONc_newBoolObjArena(void *arena, int value) {
    toonObject *o = TOONc_newObjectArena(arena, KV_BOOL);
    o->boolean = !!value;
    return o;
}

toonObject *TOONc_newNullObjArena(void *arena) {
    return TOONc_newObjectArena(arena, KV_NULL);
}

toonObject *TOONc_newListObjArena(void *arena, size_t initial_capacity) {
    toonObject *o = TOONc_newObjectArena(arena, KV_LIST);
    if (initial_capacity > 0) {
        TOONc_listReserveArena(arena, o, initial_capacity);
    }
    return o;
}

void TOONc_listPushArena(void *arena, toonObject *list, toonObject *item) {
    if (list->kvtype != KV_LIST) return;
    if (UNLIKELY(list->array.len >= list->array.capacity)) {
        size_t new_cap = list->array.capacity == 0 ? 4 : list->array.capacity * 2;
        TOONc_listReserveArena(arena, list, new_cap);
    }
    list->array.items[list->array.len++] = item;
}

void TOONc_listReserveArena(void *arena, toonObject *list, size_t capacity) {
    if (list->kvtype != KV_LIST) return;
    if (capacity <= list->array.capacity) return;
    
    if (arena) {
        toonObject **new_items = mem_alloc((mem_pool_t *)arena, sizeof(toonObject *) * capacity);
        if (list->array.items) {
            memcpy(new_items, list->array.items, sizeof(toonObject *) * list->array.len);
        }
        list->array.items = new_items;
    } else {
        list->array.items = trealloc(list->array.items, sizeof(toonObject *) * capacity);
    }
    list->array.capacity = capacity;
}

/* Public versions (heap-based or legacy) */
toonObject *TOONc_newObject(int kvtype) { return TOONc_newObjectArena(NULL, kvtype); }
toonObject *TOONc_newStringObj(char *s, size_t len) { return TOONc_newStringObjArena(NULL, s, len); }
toonObject *TOONc_newIntObj(int value) { return TOONc_newIntObjArena(NULL, value); }
toonObject *TOONc_newDoubleObj(double value) { return TOONc_newDoubleObjArena(NULL, value); }
toonObject *TOONc_newBoolObj(int value) { return TOONc_newBoolObjArena(NULL, value); }
toonObject *TOONc_newNullObj(void) { return TOONc_newNullObjArena(NULL); }
toonObject *TOONc_newListObj(void) { return TOONc_newListObjArena(NULL, 0); }
void TOONc_listPush(toonObject *list, toonObject *item) { TOONc_listPushArena(NULL, list, item); }

void TOONc_free(toonObject *obj) {
    if (obj == NULL) return;
    
    // If this is a root object with an arena, free the whole arena
    if (obj->arena != NULL && obj->arena != ARENA_MANAGED_SENTINEL) {
        mem_pool_t *arena = (mem_pool_t *)obj->arena;
        mem_destroy(arena);
        tfree(arena);
        return;
    }

    // If this is a child managed by an arena, we don't free it individually
    if (obj->arena == ARENA_MANAGED_SENTINEL) {
        return;
    }

    if (obj->next) TOONc_free(obj->next);
    if (obj->child) TOONc_free(obj->child);
    if (obj->key) tfree(obj->key);
    if (obj->kvtype == KV_STRING && obj->str.ptr) tfree(obj->str.ptr);
    if (obj->kvtype == KV_LIST && obj->array.items) {
        for (size_t i = 0; i < obj->array.len; i++) {
            if (obj->array.items[i]) TOONc_free(obj->array.items[i]);
        }
        tfree(obj->array.items);
    }
    tfree(obj);
}

/* -----------------------------------------------------------------------------
 * Main parsing logic (re2c + Lemon)
 * -------------------------------------------------------------------------- */

static toonObject *parse(const char *source, size_t length) {
    if (!source) return NULL;

    toon_lexer_t lexer;
    toon_lexer_init(&lexer, source, length);
    
    toon_parse_ctx_t ctx;
    ctx.arena = tmalloc(sizeof(mem_pool_t));
    mem_init(ctx.arena, 32768); // Increased to 32KB

    ctx.root = TOONc_newObjectArena(ctx.arena, KV_OBJ);
    ctx.root->arena = ctx.arena; // The root owns the arena
    ctx.last_node = NULL;
    ctx.columns = NULL;
    ctx.error = 0;
    ctx.error_msg[0] = '\0';
    
    // Debug Trace
    // ToonParseTrace(stderr, "parser: ");

    void *parser = ToonParseAlloc(malloc);
    toon_token_t token;
    
    ctx.line = lexer.line;
    while (toon_lexer_next(&lexer, &token) > 0) {
        ctx.line = lexer.line;
        ToonParse(parser, token.type, token, &ctx);
    }
    
    if (!ctx.error) {
        token.type = 0;
        token.length = 0;
        ToonParse(parser, 0, token, &ctx);
    }
    
    ToonParseFree(parser, free);
    
    if (ctx.error) {
        fprintf(stderr, "TOON Error: %s at line %d\n", ctx.error_msg, lexer.line);
        TOONc_free(ctx.root);
        return NULL;
    }
    
    return ctx.root;
}

toonObject *TOONc_parseFile(FILE *fp) {
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    if (file_size <= 0) { fclose(fp); return NULL; }
    char *source = tmalloc(file_size + 1);
    fseek(fp, 0, SEEK_SET);
    size_t read_size = fread(source, 1, file_size, fp);
    source[read_size] = '\0';
    fclose(fp);
    toonObject *root = parse(source, read_size);
    tfree(source);
    return root;
}

toonObject *TOONc_parseStringLen(const char *str, size_t len) {
    return parse(str, len);
}

toonObject *TOONc_parseString(const char *str) {
    if (!str) return NULL;
    return parse(str, strlen(str));
}

/* -----------------------------------------------------------------------------
 * Query and access functions
 * -------------------------------------------------------------------------- */

toonObject *TOONc_get(toonObject *root, const char *path) {
    if (!root || !path) return NULL;
    char *path_copy = strdup(path);
    char *token = strtok(path_copy, ".");
    toonObject *current = root;
    while (token) {
        toonObject *found = NULL;
        toonObject *child = current->child;
        while (child) {
            if (child->key && strcmp(child->key, token) == 0) {
                found = child;
                break;
            }
            child = child->next;
        }
        if (!found) { free(path_copy); return NULL; }
        current = found;
        token = strtok(NULL, ".");
    }
    free(path_copy);
    return current;
}

toonObject *TOONc_getArrayItem(toonObject *arr, size_t index) {
    if (!TOON_IS_LIST(arr) || index >= arr->array.len) return NULL;
    return arr->array.items[index];
}

size_t TOONc_getArrayLength(toonObject *arr) {
    return TOON_IS_LIST(arr) ? arr->array.len : 0;
}

/* -----------------------------------------------------------------------------
 * Output and debugging functions
 * -------------------------------------------------------------------------- */

void printObject(toonObject *o, int depth) {
    while (o) {
        for (int i = 0; i < depth; i++) printf("  ");
        if (o->key) printf("%s: ", o->key);
        switch (o->kvtype) {
            case KV_STRING: printf("\"%s\" (string)", o->str.ptr); break;
            case KV_INT:    printf("%d (integer)", o->i); break;
            case KV_DOUBLE: printf("%f (double)", o->d); break;
            case KV_BOOL:   printf("%s (boolean)", o->boolean ? "true" : "false"); break;
            case KV_NULL:   printf("null (null)"); break;
            case KV_LIST:
                printf("[");
                for (size_t i = 0; i < o->array.len; i++) {
                    toonObject *item = o->array.items[i];
                    if (!item) { printf("?"); continue; }
                    switch (item->kvtype) {
                        case KV_STRING: printf("\"%s\"", item->str.ptr); break;
                        case KV_INT:    printf("%d", item->i); break;
                        case KV_DOUBLE: printf("%f", item->d); break;
                        case KV_BOOL:   printf("%s", item->boolean ? "true" : "false"); break;
                        case KV_OBJ:    printf("{...}"); break;
                        default:        printf("?");
                    }
                    if (i < o->array.len - 1) printf(", ");
                }
                printf("] (array)");
                break;
            case KV_OBJ: printf("{ (object)"); break;
        }
        printf("\n");
        if (o->child) printObject(o->child, depth + 1);
        if (o->kvtype == KV_OBJ && depth > 0) {
            for (int i = 0; i < depth; i++) printf("  ");
            printf("}\n");
        }
        o = o->next;
    }
}

void TOONc_printRoot(toonObject *root) {
    if (root && root->child) printObject(root->child, 0);
}

void TOONc_printObject(toonObject *o, int depth) {
    printObject(o, depth);
}

/* -----------------------------------------------------------------------------
 * Serialization
 * -------------------------------------------------------------------------- */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} toon_sb_t;

static void sb_append(toon_sb_t *sb, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int required = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if (required < 0) return;

    if (sb->len + required + 1 > sb->cap) {
        sb->cap = sb->cap * 2 + required + 1;
        if (sb->cap < 256) sb->cap = 256;
        char *new_buf = (char *)trealloc(sb->buf, sb->cap);
        if (!new_buf) return;
        sb->buf = new_buf;
    }

    va_start(args, fmt);
    vsnprintf(sb->buf + sb->len, required + 1, fmt, args);
    va_end(args);
    sb->len += required;
}

static void serialize_internal(const toonObject *obj, int depth, toon_sb_t *sb, bool is_list_item) {
    if (!obj) return;
    
    const toonObject *curr = obj;
    while (curr) {
        if (!is_list_item) {
            if (curr->kvtype != KV_OBJ || depth > 0 || curr->key) {
                for (int i = 0; i < depth; i++) sb_append(sb, "  ");
                if (curr->key) sb_append(sb, "%s: ", curr->key);
            }
        }

        switch (curr->kvtype) {
            case KV_STRING: sb_append(sb, "\"%s\"", curr->str.ptr); break;
            case KV_INT:    sb_append(sb, "%d", curr->i); break;
            case KV_DOUBLE: sb_append(sb, "%.17g", curr->d); break;
            case KV_BOOL:   sb_append(sb, "%s", curr->boolean ? "true" : "false"); break;
            case KV_NULL:   sb_append(sb, "null"); break;
            case KV_LIST:
                sb_append(sb, "[");
                for (size_t i = 0; i < curr->array.len; i++) {
                    serialize_internal(curr->array.items[i], 0, sb, true); // Inline list items
                    if (i < curr->array.len - 1) sb_append(sb, ", ");
                }
                sb_append(sb, "]");
                break;
            case KV_OBJ:
                if (curr->child) {
                    if (!is_list_item) sb_append(sb, "\n");
                    serialize_internal(curr->child, depth + 1, sb, false);
                } else if (curr->key) {
                    sb_append(sb, "{}"); // Empty object
                    if (!is_list_item) sb_append(sb, "\n");
                } else {
                    // Root object, just serialize children
                    serialize_internal(curr->child, depth, sb, false);
                }
                break;
        }
        
        if (!is_list_item && curr->kvtype != KV_OBJ) sb_append(sb, "\n");
        
        // If we are at depth 0 and this is the root object, we only do one iteration
        if (depth == 0 && !curr->key && curr->kvtype == KV_OBJ) break;
        
        // If we are a list item, we only serialize ourselves, not our siblings
        if (is_list_item) break;
        
        curr = curr->next;
    }
}

char *TOONc_serialize(const toonObject *obj, size_t *out_len) {
    if (!obj) return NULL;
    
    toon_sb_t sb = {0};
    // If it's a root object (KV_OBJ and no key), serialize its children directly
    if (obj->kvtype == KV_OBJ && !obj->key) {
        serialize_internal(obj->child, 0, &sb, false);
    } else {
        serialize_internal(obj, 0, &sb, false);
    }
    
    if (out_len) *out_len = sb.len;
    return sb.buf;
}

void TOONc_serializeFree(char *str) {
    if (str) tfree(str);
}

/* -----------------------------------------------------------------------------
 * JSON Conversion
 * -------------------------------------------------------------------------- */

static void serialize_json_sb(const toonObject *obj, int depth, toon_sb_t *sb, bool is_list_item) {
    if (!obj) return;
    const toonObject *curr = obj;
    bool first = true;
    while (curr) {
        if (!is_list_item) {
            if (!first) sb_append(sb, ",\n");
            for (int i = 0; i < depth; i++) sb_append(sb, "  ");
            if (curr->key) sb_append(sb, "\"%s\": ", curr->key);
        }

        switch (curr->kvtype) {
            case KV_STRING: sb_append(sb, "\"%s\"", curr->str.ptr); break;
            case KV_INT:    sb_append(sb, "%d", curr->i); break;
            case KV_DOUBLE: sb_append(sb, "%.17g", curr->d); break;
            case KV_BOOL:   sb_append(sb, "%s", curr->boolean ? "true" : "false"); break;
            case KV_NULL:   sb_append(sb, "null"); break;
            case KV_LIST:
                sb_append(sb, "[");
                for (size_t i = 0; i < curr->array.len; i++) {
                    serialize_json_sb(curr->array.items[i], 0, sb, true);
                    if (i < curr->array.len - 1) sb_append(sb, ", ");
                }
                sb_append(sb, "]");
                break;
            case KV_OBJ:
                sb_append(sb, "{\n");
                if (curr->child) serialize_json_sb(curr->child, depth + 1, sb, false);
                sb_append(sb, "\n");
                for (int i = 0; i < depth; i++) sb_append(sb, "  ");
                sb_append(sb, "}");
                break;
        }

        if (is_list_item) break;
        first = false;
        curr = curr->next;
    }
}

char *TOONc_toJSONString(const toonObject *obj, size_t *out_len) {
    if (!obj) return NULL;
    toon_sb_t sb = {0};
    if (obj->kvtype == KV_OBJ && !obj->key) {
        sb_append(&sb, "{\n");
        serialize_json_sb(obj->child, 1, &sb, false);
        sb_append(&sb, "\n}");
    } else {
        serialize_json_sb(obj, 0, &sb, false);
    }
    if (out_len) *out_len = sb.len;
    return sb.buf;
}

void TOONc_toJSON(toonObject *obj, FILE *fp, int depth) {
    if (!obj || !fp) return;
    toon_sb_t sb = {0};
    if (obj->kvtype == KV_OBJ && !obj->key) {
        sb_append(&sb, "{\n");
        serialize_json_sb(obj->child, depth + 1, &sb, false);
        sb_append(&sb, "\n}");
    } else {
        serialize_json_sb(obj, depth, &sb, false);
    }
    if (sb.buf) {
        fwrite(sb.buf, 1, sb.len, fp);
        tfree(sb.buf);
    }
}

// Minimal JSON parser for fromJSONString
typedef struct {
    const char *p;
    const char *end;
    void *arena;
} j_ctx;

static void j_ws(j_ctx *c) {
    while (c->p < c->end && isspace(*c->p)) c->p++;
}

static char *j_str(j_ctx *c, size_t *len) {
    if (*c->p != '\"') return NULL;
    c->p++;
    const char *start = c->p;
    while (c->p < c->end && *c->p != '\"') {
        if (*c->p == '\\') c->p++;
        c->p++;
    }
    *len = c->p - start;
    char *res = mem_alloc(c->arena, *len + 1);
    memcpy(res, start, *len);
    res[*len] = '\0';
    if (c->p < c->end) c->p++; // skip close quote
    return res;
}

static toonObject *j_val(j_ctx *c) {
    j_ws(c);
    if (c->p >= c->end) return NULL;
    if (*c->p == '\"') {
        size_t len;
        char *s = j_str(c, &len);
        return TOONc_newStringObjArena(c->arena, s, len);
    } else if (*c->p == '{') {
        c->p++;
        toonObject *obj = TOONc_newObjectArena(c->arena, KV_OBJ);
        toonObject *last = NULL;
        while (c->p < c->end && *c->p != '}') {
            j_ws(c);
            if (*c->p == '}') break;
            size_t klen;
            char *key = j_str(c, &klen);
            j_ws(c);
            if (*c->p == ':') c->p++;
            toonObject *val = j_val(c);
            if (val) {
                val->key = key;
                if (!last) obj->child = val; else last->next = val;
                last = val;
            }
            j_ws(c);
            if (*c->p == ',') c->p++;
        }
        if (*c->p == '}') c->p++;
        return obj;
    } else if (*c->p == '[') {
        c->p++;
        toonObject *list = TOONc_newListObjArena(c->arena, 8);
        while (c->p < c->end && *c->p != ']') {
            toonObject *v = j_val(c);
            if (v) TOONc_listPushArena(c->arena, list, v);
            j_ws(c);
            if (*c->p == ',') c->p++;
            j_ws(c);
        }
        if (*c->p == ']') c->p++;
        return list;
    } else if (isdigit(*c->p) || *c->p == '-') {
        char *endptr;
        double d = strtod(c->p, &endptr);
        c->p = endptr;
        if (d == (int)d) return TOONc_newIntObjArena(c->arena, (int)d);
        return TOONc_newDoubleObjArena(c->arena, d);
    } else if (strncmp(c->p, "true", 4) == 0) { c->p += 4; return TOONc_newBoolObjArena(c->arena, 1); }
    else if (strncmp(c->p, "false", 5) == 0) { c->p += 5; return TOONc_newBoolObjArena(c->arena, 0); }
    else if (strncmp(c->p, "null", 4) == 0) { c->p += 4; return TOONc_newNullObjArena(c->arena); }
    return NULL;
}

toonObject *TOONc_fromJSONString(const char *json, size_t len) {
    if (!json) return NULL;
    void *arena = tmalloc(sizeof(mem_pool_t));
    mem_init(arena, 32768);
    j_ctx c = {json, json + len, arena};
    toonObject *root = j_val(&c);
    if (root) root->arena = arena;
    else tfree(arena);
    return root;
}

/* -----------------------------------------------------------------------------
 * Public API aliases
 * -------------------------------------------------------------------------- */

void *TOONc_malloc(size_t size) { return tmalloc(size); }
void *TOONc_calloc(size_t nmemb, size_t size) { return tcalloc(nmemb, size); }
void *TOONc_realloc(void *ptr, size_t size) { return trealloc(ptr, size); }
