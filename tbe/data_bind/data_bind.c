/**
 * @file data_bind.c
 * @brief MIR-based runtime binary codec using c2mir
 *
 * Strategy: parse schema → generate C source as string → c2mir compile → JIT.
 * This avoids hand-building MIR IR and all its register-type / proto pitfalls.
 */

#include "data_bind.h"
#include "node_tree.h"
#include "schema_parser_dsl.h"
#include "tbe_error.h"
#include <mir.h>
#include <mir-gen.h>
#include <c2mir/c2mir.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>

/* Helper functions from tbe_helpers.c (linked statically) */
extern char* tbe_read_varstring_copy(const uint8_t* buf, size_t offset);



/* ───── String buffer for code generation ───── */

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} strbuf_t;

static void sb_init(strbuf_t* sb, size_t initial) {
    sb->cap = initial > 0 ? initial : 4096;
    sb->data = malloc(sb->cap);
    sb->data[0] = '\0';
    sb->len = 0;
}

static void sb_append(strbuf_t* sb, const char* s) {
    size_t slen = strlen(s);
    while (sb->len + slen + 1 > sb->cap) {
        sb->cap *= 2;
        sb->data = realloc(sb->data, sb->cap);
    }
    memcpy(sb->data + sb->len, s, slen + 1);
    sb->len += slen;
}

static void sb_appendf(strbuf_t* sb, const char* fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    sb_append(sb, tmp);
}

static void sb_free(strbuf_t* sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

/* ───── c2mir string reader ───── */

typedef struct {
    const char* code;
    size_t pos;
    size_t len;
} str_reader_t;

static int str_getc(void* data) {
    str_reader_t* r = data;
    return r->pos >= r->len ? EOF : (unsigned char)r->code[r->pos++];
}

/* ───── Codec internals ───── */

typedef struct mir_func_node {
    char* type_name;
    void* parse_fn;  /* JIT: Value* (*)(const uint8_t*, size_t) */
    struct mir_func_node* next;
} mir_func_node;

struct DataBind {
    MIR_context_t ctx;
    Node* schema_root;
    mir_func_node* func_head;
    char error[256];
    DataBindValueApi api;  /* Cached function pointers */
};

/* ───── Code generation helpers ───── */

static Node* find_child(Node* parent, const char* name) {
    if (!parent || parent->type != NODE_MAP) return NULL;
    for (size_t i = 0; i < parent->data.map.count; i++) {
        if (strcmp(parent->data.map.items[i]->name, name) == 0)
            return parent->data.map.items[i];
    }
    return NULL;
}

static const char* get_string_val(Node* node) {
    return (node && node->type == NODE_STRING) ? node->data.string_val : NULL;
}

static int get_type_size(const char* type) {
    if (!type) return 0;
    if (strcmp(type, "uint8_t") == 0 || strcmp(type, "uint8") == 0 ||
        strcmp(type, "u8") == 0 || strcmp(type, "byte") == 0) return 1;
    if (strcmp(type, "int8_t") == 0 || strcmp(type, "int8") == 0) return 1;
    if (strcmp(type, "uint16_t") == 0 || strcmp(type, "uint16") == 0 ||
        strcmp(type, "u16") == 0) return 2;
    if (strcmp(type, "int16_t") == 0 || strcmp(type, "int16") == 0) return 2;
    if (strcmp(type, "uint32_t") == 0 || strcmp(type, "uint32") == 0 ||
        strcmp(type, "u32") == 0) return 4;
    if (strcmp(type, "int32_t") == 0 || strcmp(type, "int32") == 0 ||
        strcmp(type, "i32") == 0) return 4;
    if (strcmp(type, "uint64_t") == 0 || strcmp(type, "uint64") == 0 ||
        strcmp(type, "u64") == 0) return 8;
    if (strcmp(type, "int64_t") == 0 || strcmp(type, "int64") == 0) return 8;
    if (strcmp(type, "float") == 0 || strcmp(type, "f32") == 0) return 4;
    if (strcmp(type, "double") == 0 || strcmp(type, "f64") == 0) return 8;
    return 0;
}

static const char* get_c_type(const char* type) {
    if (!type) return "int";
    if (strcmp(type, "uint8_t") == 0 || strcmp(type, "uint8") == 0 ||
        strcmp(type, "u8") == 0 || strcmp(type, "byte") == 0) return "unsigned char";
    if (strcmp(type, "int8_t") == 0 || strcmp(type, "int8") == 0) return "signed char";
    if (strcmp(type, "uint16_t") == 0 || strcmp(type, "uint16") == 0 ||
        strcmp(type, "u16") == 0) return "unsigned short";
    if (strcmp(type, "int16_t") == 0 || strcmp(type, "int16") == 0) return "short";
    if (strcmp(type, "uint32_t") == 0 || strcmp(type, "uint32") == 0 ||
        strcmp(type, "u32") == 0) return "unsigned int";
    if (strcmp(type, "int32_t") == 0 || strcmp(type, "int32") == 0 ||
        strcmp(type, "i32") == 0) return "int";
    if (strcmp(type, "uint64_t") == 0 || strcmp(type, "uint64") == 0 ||
        strcmp(type, "u64") == 0) return "unsigned long long";
    if (strcmp(type, "int64_t") == 0 || strcmp(type, "int64") == 0) return "long long";
    if (strcmp(type, "float") == 0 || strcmp(type, "f32") == 0) return "float";
    if (strcmp(type, "double") == 0 || strcmp(type, "f64") == 0) return "double";
    return "int";
}

static int is_float_type(const char* type) {
    return (strcmp(type, "float") == 0 || strcmp(type, "f32") == 0 ||
            strcmp(type, "double") == 0 || strcmp(type, "f64") == 0);
}

static int is_u64_type(const char* type) {
    return (strcmp(type, "uint64_t") == 0 || strcmp(type, "uint64") == 0 ||
            strcmp(type, "u64") == 0);
}

/* Emit C code for reading composite fields with dot-notation */
static void gen_composite_read(strbuf_t* sb, Node* schema_root, const char* field_prefix,
                                Node* composite_node, int has_set_bytes) {
    Node* fields_node = find_child(composite_node, "fields");
    if (!fields_node || fields_node->type != NODE_LIST) return;

    for (size_t i = 0; i < fields_node->data.list.count; i++) {
        Node* field = fields_node->data.list.items[i];
        const char* fname = get_string_val(find_child(field, "name"));
        const char* ftype = get_string_val(find_child(field, "type"));
        if (!fname || !ftype) continue;

        int sz = get_type_size(ftype);
        if (sz == 0) continue;

        sb_appendf(sb, "  if (off + %d > len) return (void*)0;\n", sz);

        if (is_float_type(ftype)) {
            sb_appendf(sb, "  set_dbl(obj, \"%s.%s\", (double)*(%s*)(buf + off));\n",
                       field_prefix, fname, get_c_type(ftype));
        } else if (is_u64_type(ftype)) {
            sb_appendf(sb, "  set_dbl(obj, \"%s.%s\", (double)*(unsigned long long*)(buf + off));\n",
                       field_prefix, fname);
        } else {
            sb_appendf(sb, "  set_int(obj, \"%s.%s\", (int)*(%s*)(buf + off));\n",
                       field_prefix, fname, get_c_type(ftype));
        }
        sb_appendf(sb, "  off += %d;\n", sz);
    }
}

/* Emit C code for a single field read */
static void gen_field_read_code(strbuf_t* sb, Node* schema_root, Node* field_node, int has_set_bytes) {
    const char* fname = get_string_val(find_child(field_node, "name"));
    const char* ftype = get_string_val(find_child(field_node, "type"));
    if (!fname || !ftype) return;

    Node* n;
    int is_composite = ((n = find_child(field_node, "is_composite_ref")) && n->type == NODE_STRING &&
                        strcmp(n->data.string_val, "1") == 0);
    int is_enum      = ((n = find_child(field_node, "is_enum_ref")) && n->type == NODE_STRING &&
                        strcmp(n->data.string_val, "1") == 0);
    int is_var_data  = ((n = find_child(field_node, "is_var_data")) && n->type == NODE_STRING &&
                        strcmp(n->data.string_val, "1") == 0);
    int is_bytes     = ((n = find_child(field_node, "is_bytes")) && n->type == NODE_STRING &&
                        strcmp(n->data.string_val, "1") == 0);

    if (is_enum) {
        sb_appendf(sb, "  if (off + 1 > len) return (void*)0;\n");
        sb_appendf(sb, "  set_int(obj, \"%s\", (int)*(unsigned char*)(buf + off));\n", fname);
        sb_appendf(sb, "  off += 1;\n");
    }
    else if (is_bytes) {
        Node* sz_node = find_child(field_node, "size_bytes");
        if (sz_node && sz_node->type == NODE_STRING) {
            int sz = atoi(sz_node->data.string_val);
            sb_appendf(sb, "  if (off + %d > len) return (void*)0;\n", sz);
            if (has_set_bytes) {
                sb_appendf(sb, "  set_bytes(obj, \"%s\", (unsigned char*)(buf + off), %d);\n", fname, sz);
            }
            sb_appendf(sb, "  off += %d;\n", sz);
        }
    }
    else if (is_composite) {
        /* Find composite definition and expand its fields */
        Node* composites_node = find_child(schema_root, "composites");
        if (composites_node && composites_node->type == NODE_LIST) {
            for (size_t i = 0; i < composites_node->data.list.count; i++) {
                Node* comp = composites_node->data.list.items[i];
                const char* comp_name = get_string_val(find_child(comp, "name"));
                if (comp_name && strcmp(comp_name, ftype) == 0) {
                    gen_composite_read(sb, schema_root, fname, comp, has_set_bytes);
                    return;
                }
            }
        }
        /* Composite not found — skip by size_bytes if available */
        Node* sz_node = find_child(field_node, "size_bytes");
        if (sz_node && sz_node->type == NODE_STRING) {
            int sz = atoi(sz_node->data.string_val);
            sb_appendf(sb, "  off += %d;\n", sz);
        }
    }
    else if (is_var_data) {
        /* Variable-length string: 2-byte length prefix + data */
        sb_appendf(sb, "  if (off + 2 > len) return (void*)0;\n");
        sb_appendf(sb, "  { char* s = read_varstr(buf, off);\n");
        sb_appendf(sb, "    if (s) { set_str(obj, \"%s\", s); }\n", fname);
        sb_appendf(sb, "    off += 2 + *(unsigned short*)(buf + off); }\n");
    }
    else {
        /* Primitive type */
        int sz = get_type_size(ftype);
        if (sz == 0) return;

        sb_appendf(sb, "  if (off + %d > len) return (void*)0;\n", sz);

        if (is_float_type(ftype)) {
            sb_appendf(sb, "  set_dbl(obj, \"%s\", (double)*(%s*)(buf + off));\n",
                       fname, get_c_type(ftype));
        } else if (is_u64_type(ftype)) {
            sb_appendf(sb, "  set_dbl(obj, \"%s\", (double)*(unsigned long long*)(buf + off));\n", fname);
        } else {
            sb_appendf(sb, "  set_int(obj, \"%s\", (int)*(%s*)(buf + off));\n",
                       fname, get_c_type(ftype));
        }
        sb_appendf(sb, "  off += %d;\n", sz);
    }
}

/* Generate full C source for all parse functions */
static char* gen_all_parsers_c(Node* schema_root, const DataBindValueApi* api) {
    int has_set_bytes = (api->set_field_bytes != NULL);
    strbuf_t sb;
    sb_init(&sb, 8192);

    /* Emit extern declarations (c2mir needs to see prototypes) */
    sb_append(&sb,
        "typedef unsigned long long size_t;\n"
        "extern void* create_obj(void);\n"
        "extern void set_int(void* obj, const char* name, int val);\n"
        "extern void set_dbl(void* obj, const char* name, double val);\n"
        "extern void set_str(void* obj, const char* name, const char* val);\n"
        "extern void set_bytes(void* obj, const char* name, const unsigned char* data, size_t len);\n"
        "extern char* read_varstr(const unsigned char* buf, size_t offset);\n"
        "\n"
    );

    /* For each message, generate a parse_MessageName function */
    Node* messages_node = find_child(schema_root, "messages");
    if (!messages_node || messages_node->type != NODE_LIST) {
        sb_free(&sb);
        return NULL;
    }

    for (size_t i = 0; i < messages_node->data.list.count; i++) {
        Node* msg = messages_node->data.list.items[i];
        const char* msg_name = get_string_val(find_child(msg, "name"));
        if (!msg_name) continue;

        sb_appendf(&sb, "void* parse_%s(const unsigned char* buf, size_t len) {\n", msg_name);
        sb_appendf(&sb, "  size_t off = 0;\n");
        sb_appendf(&sb, "  void* obj = create_obj();\n");
        sb_appendf(&sb, "  if (!obj) return (void*)0;\n");

        Node* fields_node = find_child(msg, "fields");
        if (fields_node && fields_node->type == NODE_LIST) {
            for (size_t j = 0; j < fields_node->data.list.count; j++) {
                gen_field_read_code(&sb, schema_root, fields_node->data.list.items[j], has_set_bytes);
            }
        }

        sb_appendf(&sb, "  return obj;\n");
        sb_appendf(&sb, "}\n\n");
    }

    char* result = sb.data;
    /* Don't free — caller takes ownership */
    return result;
}

/* Get or generate C source with caching */
static char* get_or_generate_c_source(const char* schema_path, Node* schema_root,
                                       const DataBindValueApi* api) {
    char cache_path[512];
    snprintf(cache_path, sizeof(cache_path), "%s.c", schema_path);

    struct stat schema_stat, cache_stat;
    if (stat(schema_path, &schema_stat) == 0 && stat(cache_path, &cache_stat) == 0) {
        if (cache_stat.st_mtime >= schema_stat.st_mtime) {
            FILE* f = fopen(cache_path, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                char* src = malloc(sz + 1);
                if (src && fread(src, 1, sz, f) == (size_t)sz) {
                    src[sz] = '\0';
                    fclose(f);
                    return src;
                }
                fclose(f);
                free(src);
            }
        }
    }

    char* src = gen_all_parsers_c(schema_root, api);
    if (src) {
        FILE* f = fopen(cache_path, "wb");
        if (f) {
            fwrite(src, 1, strlen(src), f);
            fclose(f);
        }
    }
    return src;
}

/* ───── Public API ───── */

/* Load and parse schema file */
static Node* load_and_parse_schema(const char* schema_path, char* error_buf, size_t error_size) {
    FILE* f = fopen(schema_path, "rb");
    if (!f) {
        snprintf(error_buf, error_size, "Cannot open schema: %s", schema_path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* text = malloc(size + 1);
    if (!text || fread(text, 1, size, f) != (size_t)size) {
        fclose(f);
        free(text);
        snprintf(error_buf, error_size, "Failed to read schema file");
        return NULL;
    }
    text[size] = '\0';
    fclose(f);

    Node* root = create_node_map(NULL);
    tbe_error_t err = {0};
    if (parse_schema(text, size, root, &err) != 0) {
        snprintf(error_buf, error_size, "Parse error: %s", err.message);
        free(text);
        node_free(root);
        return NULL;
    }
    free(text);
    return root;
}

/* Compile and link C source with MIR */
static int compile_and_link(DataBind* codec, const char* c_source) {
    codec->ctx = MIR_init();
    c2mir_init(codec->ctx);

    struct c2mir_options opts = {0};
    opts.message_file = stderr;

    str_reader_t reader = {.code = c_source, .pos = 0, .len = strlen(c_source)};

    if (!c2mir_compile(codec->ctx, &opts, str_getc, &reader, "data_bind_gen.c", NULL)) {
        snprintf(codec->error, sizeof(codec->error), "c2mir compilation failed");
        c2mir_finish(codec->ctx);
        MIR_finish(codec->ctx);
        return 0;
    }

    MIR_module_t module = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(codec->ctx));
    if (!module) {
        snprintf(codec->error, sizeof(codec->error), "No module produced by c2mir");
        c2mir_finish(codec->ctx);
        MIR_finish(codec->ctx);
        return 0;
    }

    MIR_load_module(codec->ctx, module);

    MIR_load_external(codec->ctx, "create_obj", codec->api.create_object);
    MIR_load_external(codec->ctx, "set_int", codec->api.set_field_int);
    MIR_load_external(codec->ctx, "set_dbl", codec->api.set_field_double);
    MIR_load_external(codec->ctx, "set_str", codec->api.set_field_string);
    if (codec->api.set_field_bytes)
        MIR_load_external(codec->ctx, "set_bytes", codec->api.set_field_bytes);
    MIR_load_external(codec->ctx, "read_varstr", tbe_read_varstring_copy);

    MIR_gen_init(codec->ctx);
    MIR_link(codec->ctx, MIR_set_gen_interface, NULL);

    return 1;
}

/* Register parse functions from compiled module */
static void register_parse_functions(DataBind* codec, MIR_module_t module) {
    Node* messages_node = find_child(codec->schema_root, "messages");
    if (!messages_node || messages_node->type != NODE_LIST) return;

    for (size_t i = 0; i < messages_node->data.list.count; i++) {
        Node* msg = messages_node->data.list.items[i];
        const char* msg_name = get_string_val(find_child(msg, "name"));
        if (!msg_name) continue;

        char func_name[256];
        snprintf(func_name, sizeof(func_name), "parse_%s", msg_name);

        for (MIR_item_t item = DLIST_HEAD(MIR_item_t, module->items); item;
             item = DLIST_NEXT(MIR_item_t, item)) {
            if (item->item_type == MIR_func_item &&
                strcmp(item->u.func->name, func_name) == 0) {

                mir_func_node* node = malloc(sizeof(mir_func_node));
                node->type_name = strdup(msg_name);
                node->parse_fn = item->addr;
                node->next = codec->func_head;
                codec->func_head = node;
                break;
            }
        }
    }
}

DataBind* data_bind_create(const char* schema_path, const DataBindValueApi* api) {
    if (!api || !api->create_object || !api->set_field_int || !api->set_field_double ||
        !api->set_field_string) return NULL;

    DataBind* codec = calloc(1, sizeof(DataBind));
    if (!codec) return NULL;
    codec->api = *api;

    codec->schema_root = load_and_parse_schema(schema_path, codec->error, sizeof(codec->error));
    if (!codec->schema_root) {
        free(codec);
        return NULL;
    }

    char* c_source = get_or_generate_c_source(schema_path, codec->schema_root, api);
    if (!c_source) {
        snprintf(codec->error, sizeof(codec->error), "No messages found in schema");
        node_free(codec->schema_root);
        free(codec);
        return NULL;
    }

    if (!compile_and_link(codec, c_source)) {
        free(c_source);
        node_free(codec->schema_root);
        free(codec);
        return NULL;
    }
    free(c_source);

    MIR_module_t module = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(codec->ctx));
    register_parse_functions(codec, module);

    MIR_gen_finish(codec->ctx);
    c2mir_finish(codec->ctx);

    return codec;
}

void data_bind_free(DataBind* codec) {
    if (!codec) return;

    mir_func_node* n = codec->func_head;
    while (n) {
        mir_func_node* next = n->next;
        free(n->type_name);
        free(n);
        n = next;
    }

    if (codec->ctx) MIR_finish(codec->ctx);
    if (codec->schema_root) node_free(codec->schema_root);
    free(codec);
}

Value* data_bind_parse(DataBind* codec, const char* type_name,
                        const uint8_t* buf, size_t len) {
    if (!codec || !type_name || !buf) return NULL;

    for (mir_func_node* n = codec->func_head; n; n = n->next) {
        if (strcmp(n->type_name, type_name) == 0) {
            Value* (*parse_fn)(const uint8_t*, size_t) = n->parse_fn;
            return parse_fn(buf, len);
        }
    }

    snprintf(codec->error, sizeof(codec->error), "Type not found: %s", type_name);
    return NULL;
}

const char* data_bind_get_error(DataBind* codec) {
    return codec ? codec->error : "Invalid codec";
}
