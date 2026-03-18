/**
 * @file data_bind.c
 * @brief MIR-based runtime binary codec using c2mir
 *
 * Strategy: parse schema → generate C source as string → c2mir compile → JIT.
 * This avoids hand-building MIR IR and all its register-type / proto pitfalls.
 */

#include "data_bind.h"
#include "c2mir/c2mir.h"
#include "data_bind_templates.h"
#include "mir-gen.h"
#include "mir.h"
#include "mustache.h"
#include "mustache_node_driver.h"
#include "node_tree.h"
#include "schema_parser_dsl.h"
#include "tbe_error.h"
#include "turbo_fs.h"
#include "turbo_str.h"

/* Helper functions from tbe_helpers.c (linked statically) */
extern char *tbe_read_varstring(const uint8_t *buf, size_t offset);

static const type_meta_t *find_type_meta(const char *type) {
  if (!type) return NULL;
  for (const type_meta_t *m = TYPE_METAS; m->name; m++) {
    if (strcmp(type, m->name) == 0) return m;
  }
  return NULL;
}

/* ───── c2mir string reader ───── */

typedef struct {
  const char *code;
  size_t pos;
  size_t len;
} str_reader_t;

static int str_getc(void *data) {
  str_reader_t *r = data;
  return r->pos >= r->len ? EOF : (unsigned char)r->code[r->pos++];
}

/* ───── Codec internals ───── */

typedef struct mir_func_node {
  char *type_name;
  void *parse_fn; /* JIT: Value* (*)(const uint8_t*, size_t) */
  struct mir_func_node *next;
} mir_func_node;

struct DataBind {
  MIR_context_t ctx;
  Node *schema_root;
  mir_func_node *func_head;
  char error[256];
  DataBindValueApi api; /* Cached function pointers */
};

/* ───── Code generation helpers ───── */

static Node *find_child(Node *parent, const char *name) {
  if (!parent || parent->type != NODE_MAP) return NULL;
  for (size_t i = 0; i < parent->data.map.count; i++) {
    if (strcmp(parent->data.map.items[i]->name, name) == 0) return parent->data.map.items[i];
  }
  return NULL;
}

static const char *get_string_val(Node *node) {
  return (node && node->type == NODE_STRING) ? node->data.string_val : NULL;
}

/* ───── Code generation template ───── */

/* ───── Render Model Enrichment ───── */

static void populate_render_fields(Node *dst_list, Node *src_fields, Node *schema_root,
                                   const char *prefix, int has_set_bytes) {
  if (!src_fields || src_fields->type != NODE_LIST) return;
  for (size_t i = 0; i < src_fields->data.list.count; i++) {
    Node *f_node = src_fields->data.list.items[i];
    const char *fname = get_string_val(find_child(f_node, "name"));
    const char *ftype = get_string_val(find_child(f_node, "type"));
    if (!fname || !ftype) continue;

    char full_name[256];
    if (prefix) snprintf(full_name, sizeof(full_name), "%s.%s", prefix, fname);
    else strncpy(full_name, fname, sizeof(full_name));

    Node *n;
    int is_composite = ((n = find_child(f_node, "is_composite_ref")) && n->type == NODE_STRING &&
                        strcmp(n->data.string_val, "1") == 0);
    int is_enum = ((n = find_child(f_node, "is_enum_ref")) && n->type == NODE_STRING &&
                   strcmp(n->data.string_val, "1") == 0);
    int is_var_data = ((n = find_child(f_node, "is_var_data")) && n->type == NODE_STRING &&
                       strcmp(n->data.string_val, "1") == 0);
    int is_bytes = ((n = find_child(f_node, "is_bytes")) && n->type == NODE_STRING &&
                    strcmp(n->data.string_val, "1") == 0);

    if (is_enum) {
      Node *rf = create_node_map(NULL);
      map_add(rf, create_node_string("name", full_name));
      map_add(rf, create_node_string("is_enum", "1"));

      /* Find underlying type of the enum */
      Node *enums = find_child(schema_root, "enums");
      const char *underlying = "uint8"; /* Default */
      if (enums && enums->type == NODE_LIST) {
        for (size_t j = 0; j < enums->data.list.count; j++) {
          Node *en = enums->data.list.items[j];
          const char *ename = get_string_val(find_child(en, "name"));
          if (ename && strcmp(ename, ftype) == 0) {
            const char *ut = get_string_val(find_child(en, "underlying_type"));
            if (ut) underlying = ut;
            break;
          }
        }
      }
      const type_meta_t *m = find_type_meta(underlying);
      if (m) {
        map_add(rf, create_node_string("c_type", m->c_type));
        char buf[32];
        sprintf(buf, "%d", m->size);
        map_add(rf, create_node_string("size", buf));
      } else {
        map_add(rf, create_node_string("c_type", "unsigned char"));
        map_add(rf, create_node_string("size", "1"));
      }
      list_add(dst_list, rf);
    } else if (is_bytes) {
      Node *sz_n = find_child(f_node, "size_bytes");
      if (sz_n) {
        Node *rf = create_node_map(NULL);
        map_add(rf, create_node_string("name", full_name));
        map_add(rf, create_node_string("is_bytes", "1"));
        map_add(rf, create_node_string("size_bytes", get_string_val(sz_n)));
        if (has_set_bytes) map_add(rf, create_node_string("has_set_bytes", "1"));
        list_add(dst_list, rf);
      }
    } else if (is_composite) {
      Node *composites = find_child(schema_root, "composites");
      if (composites) {
        for (size_t j = 0; j < composites->data.list.count; j++) {
          Node *comp = composites->data.list.items[j];
          const char *cname = get_string_val(find_child(comp, "name"));
          if (cname && strcmp(cname, ftype) == 0) {
            populate_render_fields(dst_list, find_child(comp, "fields"), schema_root, full_name,
                                   has_set_bytes);
            break;
          }
        }
      }
    } else if (is_var_data) {
      Node *rf = create_node_map(NULL);
      map_add(rf, create_node_string("name", full_name));
      map_add(rf, create_node_string("is_var_data", "1"));
      list_add(dst_list, rf);
    } else {
      const type_meta_t *m = find_type_meta(ftype);
      if (m) {
        Node *rf = create_node_map(NULL);
        map_add(rf, create_node_string("name", full_name));
        map_add(rf, create_node_string("is_primitive", "1"));
        map_add(rf, create_node_string("c_type", m->c_type));
        char buf[32];
        sprintf(buf, "%d", m->size);
        map_add(rf, create_node_string("size", buf));
        if (m->is_float) map_add(rf, create_node_string("is_float", "1"));
        else if (m->is_64) map_add(rf, create_node_string("is_64", "1"));
        else map_add(rf, create_node_string("is_int", "1"));
        list_add(dst_list, rf);
      }
    }
  }
}

static Node *create_render_model(Node *schema_root, int has_set_bytes) {
  Node *root = create_node_map(NULL);
  Node *msg_list = create_node_list("messages");
  map_add(root, msg_list);

  Node *messages = find_child(schema_root, "messages");
  if (!messages) return root;

  for (size_t i = 0; i < messages->data.list.count; i++) {
    Node *src_msg = messages->data.list.items[i];
    Node *dst_msg = create_node_map(NULL);
    map_add(dst_msg, create_node_string("name", get_string_val(find_child(src_msg, "name"))));

    Node *dst_fields = create_node_list("fields");
    map_add(dst_msg, dst_fields);
    list_add(msg_list, dst_msg);

    populate_render_fields(dst_fields, find_child(src_msg, "fields"), schema_root, NULL,
                           has_set_bytes);
  }
  return root;
}

/* ───── JIT Source Generation ───── */

static tstr_t gen_all_parsers_c(Node *schema_root, const DataBindValueApi *api) {
  int has_set_bytes = (api->set_field_bytes != NULL);
  Node *render_model = create_render_model(schema_root, has_set_bytes);

  MUSTACHE_TEMPLATE *tmpl =
      mustache_compile(PARSER_TEMPLATE, strlen(PARSER_TEMPLATE), NULL, NULL, 0);
  if (!tmpl) {
    node_free(render_model);
    return NULL;
  }

  MUSTACHE_STRING_RENDERER renderer;
  if (mustache_string_renderer_init(&renderer) != 0) {
    mustache_release(tmpl);
    node_free(render_model);
    return NULL;
  }

  if (mustache_process(tmpl, &renderer.base, &renderer, get_node_mustache_provider(),
                       render_model) != 0) {
    mustache_string_renderer_free(&renderer);
    mustache_release(tmpl);
    node_free(render_model);
    return NULL;
  }

  char *rendered = mustache_string_renderer_get(&renderer);
  tstr_t src = tstr_dup(rendered);

  free(rendered);
  mustache_string_renderer_free(&renderer);
  mustache_release(tmpl);
  node_free(render_model);

  return src;
}

static tstr_t get_or_generate_c_source(const char *schema_path, Node *schema_root,
                                       const DataBindValueApi *api) {
  char cache_path[512];
  snprintf(cache_path, sizeof(cache_path), "%s.c", schema_path);

  turbo_fs_stat_t schema_stat, cache_stat;
  if (turbo_fs_stat(schema_path, &schema_stat) == 0 &&
      turbo_fs_stat(cache_path, &cache_stat) == 0) {
    if (cache_stat.mtime >= schema_stat.mtime) {
      turbo_fs_buf_t buf;
      if (turbo_fs_read_file(cache_path, &buf) == 0) {
        tstr_t src = tstr_dup_len(buf.base, buf.len);
        turbo_fs_buf_free(&buf);
        return src;
      }
    }
  }

  tstr_t src = gen_all_parsers_c(schema_root, api);
  if (src) {
    turbo_fs_buf_t buf = turbo_fs_buf_init((char *)src, tstr_len(src));
    turbo_fs_write_file(cache_path, &buf);
  }
  return src;
}

/* ───── Public API ───── */

/* Load and parse schema file */
static Node *load_and_parse_schema(const char *schema_path, char *error_buf, size_t error_size) {
  turbo_fs_buf_t buf;
  if (turbo_fs_read_file(schema_path, &buf) != 0) {
    snprintf(error_buf, error_size, "Cannot read schema: %s", schema_path);
    return NULL;
  }

  Node *root = create_node_map(NULL);
  tbe_error_t err = {0};
  if (parse_schema(buf.base, buf.len, root, &err) != 0) {
    snprintf(error_buf, error_size, "Parse error: %s", err.message);
    turbo_fs_buf_free(&buf);
    node_free(root);
    return NULL;
  }
  turbo_fs_buf_free(&buf);
  return root;
 }

/* Compile and link C source with MIR */
static int compile_and_link(DataBind *codec, const char *c_source) {
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
  MIR_load_external(codec->ctx, "set_int64", codec->api.set_field_int64);
  MIR_load_external(codec->ctx, "set_dbl", codec->api.set_field_double);
  MIR_load_external(codec->ctx, "set_str", codec->api.set_field_string);
  if (codec->api.set_field_bytes)
    MIR_load_external(codec->ctx, "set_bytes", codec->api.set_field_bytes);
  MIR_load_external(codec->ctx, "read_varstr", tbe_read_varstring);
  MIR_load_external(codec->ctx, "free", free);

  MIR_gen_init(codec->ctx);
  MIR_link(codec->ctx, MIR_set_gen_interface, NULL);

  return 1;
}

/* Register parse functions from compiled module */
static void register_parse_functions(DataBind *codec, MIR_module_t module) {
  Node *messages_node = find_child(codec->schema_root, "messages");
  if (!messages_node || messages_node->type != NODE_LIST) return;

  for (size_t i = 0; i < messages_node->data.list.count; i++) {
    Node *msg = messages_node->data.list.items[i];
    const char *msg_name = get_string_val(find_child(msg, "name"));
    if (!msg_name) continue;

    char func_name[256];
    snprintf(func_name, sizeof(func_name), "parse_%s", msg_name);

    for (MIR_item_t item = DLIST_HEAD(MIR_item_t, module->items); item;
         item = DLIST_NEXT(MIR_item_t, item)) {
      if (item->item_type == MIR_func_item && strcmp(item->u.func->name, func_name) == 0) {

        mir_func_node *node = malloc(sizeof(mir_func_node));
        node->type_name = strdup(msg_name);
        node->parse_fn = item->addr;
        node->next = codec->func_head;
        codec->func_head = node;
        break;
      }
    }
  }
}

DataBind *data_bind_create(const char *schema_path, const DataBindValueApi *api) {
  if (!api || !api->create_object || !api->set_field_int || !api->set_field_double ||
      !api->set_field_string)
    return NULL;

  DataBind *codec = calloc(1, sizeof(DataBind));
  if (!codec) return NULL;
  codec->api = *api;

  codec->schema_root = load_and_parse_schema(schema_path, codec->error, sizeof(codec->error));
  if (!codec->schema_root) {
    free(codec);
    return NULL;
  }

  tstr_t c_source = get_or_generate_c_source(schema_path, codec->schema_root, api);
  if (!c_source) {
    snprintf(codec->error, sizeof(codec->error), "No messages found in schema");
    node_free(codec->schema_root);
    free(codec);
    return NULL;
  }

  if (!compile_and_link(codec, c_source)) {
    tstr_free(c_source);
    node_free(codec->schema_root);
    free(codec);
    return NULL;
  }
  tstr_free(c_source);

  MIR_module_t module = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(codec->ctx));
  register_parse_functions(codec, module);

  MIR_gen_finish(codec->ctx);
  c2mir_finish(codec->ctx);

  return codec;
}

void data_bind_free(DataBind *codec) {
  if (!codec) return;

  mir_func_node *n = codec->func_head;
  while (n) {
    mir_func_node *next = n->next;
    free(n->type_name);
    free(n);
    n = next;
  }

  if (codec->ctx) MIR_finish(codec->ctx);
  if (codec->schema_root) node_free(codec->schema_root);
  free(codec);
}

Value *data_bind_parse(DataBind *codec, const char *type_name, const uint8_t *buf, size_t len) {
  if (!codec || !type_name || !buf) return NULL;

  for (mir_func_node *n = codec->func_head; n; n = n->next) {
    if (strcmp(n->type_name, type_name) == 0) {
      Value *(*parse_fn)(const uint8_t *, size_t) = n->parse_fn;
      return parse_fn(buf, len);
    }
  }

  snprintf(codec->error, sizeof(codec->error), "Type not found: %s", type_name);
  return NULL;
}

const char *data_bind_get_error(DataBind *codec) { return codec ? codec->error : "Invalid codec"; }
