/**
 * @file data_bind_templates.h
 * @brief Mustache templates for JIT code generation
 */

#ifndef DATA_BIND_TEMPLATES_H
#define DATA_BIND_TEMPLATES_H

static const char *PARSER_TEMPLATE =
    "typedef unsigned long long size_t;\n"
    "extern void* create_obj(void);\n"
    "extern void set_int(void* obj, const char* name, int val);\n"
    "extern void set_int64(void* obj, const char* name, long long val);\n"
    "extern void set_dbl(void* obj, const char* name, double val);\n"
    "extern void set_str(void* obj, const char* name, const char* val);\n"
    "extern void set_bytes(void* obj, const char* name, const unsigned char* data, size_t len);\n"
    "extern char* read_varstr(const unsigned char* buf, size_t offset);\n"
    "extern void free(void* ptr);\n"
    "\n"
    "{{#messages}}\n"
    "void* parse_{{name}}(const unsigned char* buf, size_t len) {\n"
    "  size_t off = 0;\n"
    "  void* obj = create_obj();\n"
    "  if (!obj) return (void*)0;\n"
    "{{#fields}}\n"
    "  {{#is_enum}}\n"
    "  if (off + {{size}} > len) return (void*)0;\n"
    "  set_int(obj, \"{{name}}\", (int)*({{c_type}}*)(buf + off));\n"
    "  off += {{size}};\n"
    "  {{/is_enum}}\n"
    "  {{#is_bytes}}\n"
    "  if (off + {{size_bytes}} > len) return (void*)0;\n"
    "  {{#has_set_bytes}}set_bytes(obj, \"{{name}}\", (unsigned char*)(buf + off), "
    "{{size_bytes}});{{/has_set_bytes}}\n"
    "  off += {{size_bytes}};\n"
    "  {{/is_bytes}}\n"
    "  {{#is_var_data}}\n"
    "  if (off + 2 > len) return (void*)0;\n"
    "  { char* s_tmp = read_varstr(buf, off);\n"
    "    if (s_tmp) { set_str(obj, \"{{name}}\", s_tmp); free(s_tmp); }\n"
    "    off += 2 + *(unsigned short*)(buf + off); }\n"
    "  {{/is_var_data}}\n"
    "  {{#is_primitive}}\n"
    "  if (off + {{size}} > len) return (void*)0;\n"
    "  {{#is_float}}set_dbl(obj, \"{{name}}\", (double)*({{c_type}}*)(buf + off));{{/is_float}}\n"
    "  {{#is_64}}set_int64(obj, \"{{name}}\", (long long)*({{c_type}}*)(buf + off));{{/is_64}}\n"
    "  {{#is_int}}set_int(obj, \"{{name}}\", (int)*({{c_type}}*)(buf + off));{{/is_int}}\n"
    "  off += {{size}};\n"
    "  {{/is_primitive}}\n"
    "{{/fields}}\n"
    "  return obj;\n"
    "}\n"
    "{{/messages}}\n";
    
typedef struct {
  const char *name;
  int size;
  const char *c_type;
  unsigned char is_float : 1;
  unsigned char is_64 : 1;
} type_meta_t;

static const type_meta_t TYPE_METAS[] = {{"uint8_t", 1, "unsigned char", 0, 0},
                                         {"uint8", 1, "unsigned char", 0, 0},
                                         {"u8", 1, "unsigned char", 0, 0},
                                         {"byte", 1, "unsigned char", 0, 0},
                                         {"int8_t", 1, "signed char", 0, 0},
                                         {"int8", 1, "signed char", 0, 0},
                                         {"uint16_t", 2, "unsigned short", 0, 0},
                                         {"uint16", 2, "unsigned short", 0, 0},
                                         {"u16", 2, "unsigned short", 0, 0},
                                         {"int16_t", 2, "short", 0, 0},
                                         {"int16", 2, "short", 0, 0},
                                         {"uint32_t", 4, "unsigned int", 0, 0},
                                         {"uint32", 4, "unsigned int", 0, 0},
                                         {"u32", 4, "unsigned int", 0, 0},
                                         {"int32_t", 4, "int", 0, 0},
                                         {"int32", 4, "int", 0, 0},
                                         {"i32", 4, "int", 0, 0},
                                         {"uint64_t", 8, "unsigned long long", 0, 1},
                                         {"uint64", 8, "unsigned long long", 0, 1},
                                         {"u64", 8, "unsigned long long", 0, 1},
                                         {"int64_t", 8, "long long", 0, 1},
                                         {"int64", 8, "long long", 0, 1},
                                         {"float", 4, "float", 1, 0},
                                         {"f32", 4, "float", 1, 0},
                                         {"double", 8, "double", 1, 0},
                                         {"f64", 8, "double", 1, 0},
                                         {"bool", 1, "unsigned char", 0, 0},
                                         {NULL, 0, NULL, 0, 0}};
#endif /* DATA_BIND_TEMPLATES_H */
