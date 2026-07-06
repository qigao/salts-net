#include "turbo_parser.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Parsers headers
#include "csv_parser.h"
#include "csv_stream_processor.h"
#include "datetime_parser.h"
#include "dsv_filter.h"
#include "frame_parser.h" // for TLV
#include "ini_parser.h"
#include "json_parser.h"
#include "ltv_parser.h"
#include "modbus_parser.h"
#include "soa_parser.h"
#include "uri_parser.h"
#include "cmd_arger.h"
#include "dotenv.h"
#include "toonc.h"
#include "toml.h"
#include "cxml/cxml.h"
#include <fmt.h>

// XML (cxml) - internal only
#include "xml/cxparser.h"
#include "core/cxdefs.h"
#ifdef CXML_USE_XPATH_MOD
#include "xpath/cxxpeval.h"
#endif

static void turbo_cxml_set_destroy(cxml_set *nodeset) {
  if (!nodeset)
    return;
  cxml_set_free(nodeset);
  free(nodeset);
}

static const char *turbo_cxml_string_raw(const cxml_string *str) {
  return str ? cxml_string_as_raw((cxml_string *)str) : NULL;
}

/* JSON */
int turbo_parse_json(const uint8_t *data, size_t len, void *out) {
  if (!data || !out)
    return -1;
  json_value_t *val = json_parse((const char *)data, len);
  if (!val)
    return -1;
  *(json_value_t **)out = val;
  return 0;
}

void turbo_free_json(void *out) {
  if (!out)
    return;
  void *ptr = *(void **)out;
  if (ptr)
    json_free((json_value_t *)ptr);
  *(void **)out = NULL;
}

json_value_t *turbo_json_path_get(const json_value_t *root, const char *expr) {
  return json_path_get(root, expr);
}

turbo_json_path_result_t *turbo_json_path_query(const json_value_t *root,
                                                const char *expr) {
  return (turbo_json_path_result_t *)json_path_query(root, expr);
}

size_t turbo_json_path_result_size(const turbo_json_path_result_t *result) {
  return json_path_result_size((const json_path_result_t *)result);
}

json_value_t *turbo_json_path_result_get(const turbo_json_path_result_t *result,
                                         size_t index) {
  return json_path_result_get((const json_path_result_t *)result, index);
}

void turbo_json_path_result_free(turbo_json_path_result_t *result) {
  json_path_result_free((json_path_result_t *)result);
}

const char *turbo_json_path_error(void) { return json_path_get_error(); }

/* XML (cxml) */
int turbo_parse_xml(const uint8_t *data, size_t len, void *out) {
  if (!data || !out)
    return -1;
  
  // Ensure data is null-terminated for cxml_parse_xml
  char *temp = (char *)malloc(len + 1);
  if (!temp)
    return -1;
  memcpy(temp, data, len);
  temp[len] = '\0';

  cxml_root_node *root = cxml_parse_xml(temp);
  free(temp);

  if (!root)
    return -1;

  *(cxml_root_node **)out = root;
  return 0;
}

void turbo_free_xml(void *out) {
  if (!out)
    return;
  void *ptr = *(void **)out;
  if (ptr)
    cxml_root_node_free((cxml_root_node *)ptr);
  *(void **)out = NULL;
}

turbo_xml_node_t *turbo_xml_root_element(const turbo_xml_doc_t *doc) {
  return doc ? (turbo_xml_node_t *)doc->root_element : NULL;
}

const char *turbo_xml_node_name(const turbo_xml_node_t *node) {
  return node ? turbo_cxml_string_raw(&node->name.qname) : NULL;
}

void turbo_xml_list_init(turbo_xml_list_t *list) {
  if (!list)
    return;
  cxml_list_init((cxml_list *)list);
}

void turbo_xml_list_free(turbo_xml_list_t *list) {
  if (!list)
    return;
  cxml_list_free((cxml_list *)list);
}

turbo_xml_node_t *turbo_xml_find(turbo_xml_node_t *root, const char *query) {
  if (!root || !query)
    return NULL;
  return (turbo_xml_node_t *)cxml_find((cxml_elem_node *)root, query);
}

void turbo_xml_find_all(turbo_xml_node_t *root, const char *query,
                        turbo_xml_list_t *out) {
  if (!out)
    return;

  if (!root || !query) {
    turbo_xml_list_init(out);
    return;
  }

  cxml_find_all((cxml_elem_node *)root, query, (cxml_list *)out);
}

char *turbo_xml_text_dup(turbo_xml_node_t *node) {
  if (!node)
    return NULL;
  return cxml_text((cxml_elem_node *)node, NULL);
}

char *turbo_xml_child_text_dup(turbo_xml_node_t *parent, const char *name) {
  if (!parent || !name)
    return strdup("");

  cxml_elem_node *elem_parent = (cxml_elem_node *)parent;
  cxml_for (node, &elem_parent->children) {
    if (cxml_get_node_type(node) == CXML_ELEM_NODE) {
      cxml_elem_node *elem = (cxml_elem_node *)node;
      if (elem->name.lname && strcmp(elem->name.lname, name) == 0) {
        char *text = cxml_text(elem, NULL);
        return text ? text : strdup("");
      }
    }
  }

  return strdup("");
}

const char *turbo_xml_get_text(const turbo_xml_doc_t *doc, const char *xpath) {
  if (!doc || !xpath) return NULL;

#ifdef CXML_USE_XPATH_MOD
  cxml_set *nodeset = cxml_xpath((void *)doc, xpath);
  if (!nodeset || cxml_set_is_empty(nodeset)) {
    turbo_cxml_set_destroy(nodeset);
    return NULL;
  }

  cxml_node_t *node = (cxml_node_t *)cxml_set_get(nodeset, 0);
  if (!node) {
    turbo_cxml_set_destroy(nodeset);
    return NULL;
  }

  const char *text = NULL;
  switch (*node) {
    case CXML_ELEM_NODE: {
      cxml_elem_node *elem = (cxml_elem_node *)node;
      if (elem->has_text && !cxml_list_is_empty(&elem->children)) {
        cxml_text_node *txt = (cxml_text_node *)cxml_list_get(&elem->children, 0);
        if (txt && txt->_type == CXML_TEXT_NODE) {
          text = cxml_string_as_raw(&txt->value);
        }
      }
      break;
    }
    case CXML_TEXT_NODE: {
      cxml_text_node *txt = (cxml_text_node *)node;
      text = cxml_string_as_raw(&txt->value);
      break;
    }
    default:
      break;
  }

  turbo_cxml_set_destroy(nodeset);
  return text;
#else
  (void)doc;
  (void>xpath;
  return NULL;
#endif
}

size_t turbo_xml_count(const turbo_xml_doc_t *doc, const char *xpath) {
  if (!doc || !xpath) return 0;

#ifdef CXML_USE_XPATH_MOD
  cxml_set *nodeset = cxml_xpath((void *)doc, xpath);
  if (!nodeset) return 0;

  size_t count = (size_t)cxml_set_size(nodeset);
  turbo_cxml_set_destroy(nodeset);
  return count;
#else
  (void)doc;
  (void>xpath;
  return 0;
#endif
}

turbo_xml_xpath_node_t *turbo_xml_xpath_get(const turbo_xml_doc_t *doc, const char *xpath) {
  if (!doc || !xpath)
    return NULL;

#ifdef CXML_USE_XPATH_MOD
  cxml_set *nodeset = cxml_xpath((void *)doc, xpath);
  if (!nodeset || cxml_set_is_empty(nodeset)) {
    turbo_cxml_set_destroy(nodeset);
    return NULL;
  }

  turbo_xml_xpath_node_t *node = (turbo_xml_xpath_node_t *)cxml_set_get(nodeset, 0);
  turbo_cxml_set_destroy(nodeset);
  return node;
#else
  (void)doc;
  (void)xpath;
  return NULL;
#endif
}

void turbo_xml_xpath_query(const turbo_xml_doc_t *doc, const char *xpath, turbo_xml_list_t *out) {
  if (!out)
    return;

  turbo_xml_list_init(out);

  if (!doc || !xpath)
    return;

#ifdef CXML_USE_XPATH_MOD
  cxml_set *nodeset = cxml_xpath((void *)doc, xpath);
  if (!nodeset)
    return;

  int count = cxml_set_size(nodeset);
  for (int i = 0; i < count; ++i) {
    void *node = cxml_set_get(nodeset, i);
    if (node)
      cxml_list_append((cxml_list *)out, node);
  }

  turbo_cxml_set_destroy(nodeset);
#else
  (void)doc;
  (void)xpath;
#endif
}

size_t turbo_xml_xpath_count(const turbo_xml_doc_t *doc, const char *xpath) {
  return turbo_xml_count(doc, xpath);
}

const char *turbo_xml_xpath_text(const turbo_xml_doc_t *doc, const char *xpath) {
  return turbo_xml_get_text(doc, xpath);
}

static cxml_node_t turbo_cxml_node_type(const turbo_xml_xpath_node_t *node) {
  if (!node)
    return (cxml_node_t)-1;
  return *(const cxml_node_t *)node;
}

turbo_xml_node_type_t turbo_xml_xpath_node_type(const turbo_xml_xpath_node_t *node) {
  switch (turbo_cxml_node_type(node)) {
  case CXML_TEXT_NODE:
    return TURBO_XML_NODE_TEXT;
  case CXML_ELEM_NODE:
    return TURBO_XML_NODE_ELEMENT;
  case CXML_COMM_NODE:
    return TURBO_XML_NODE_COMMENT;
  case CXML_ATTR_NODE:
    return TURBO_XML_NODE_ATTRIBUTE;
  case CXML_ROOT_NODE:
    return TURBO_XML_NODE_ROOT;
  case CXML_PI_NODE:
    return TURBO_XML_NODE_PI;
  case CXML_NS_NODE:
    return TURBO_XML_NODE_NAMESPACE;
  case CXML_XHDR_NODE:
    return TURBO_XML_NODE_XML_HEADER;
  case CXML_DTD_NODE:
    return TURBO_XML_NODE_DTD;
  default:
    return TURBO_XML_NODE_UNKNOWN;
  }
}

const char *turbo_xml_xpath_node_type_name(const turbo_xml_xpath_node_t *node) {
  switch (turbo_xml_xpath_node_type(node)) {
  case TURBO_XML_NODE_TEXT:
    return "text";
  case TURBO_XML_NODE_ELEMENT:
    return "element";
  case TURBO_XML_NODE_COMMENT:
    return "comment";
  case TURBO_XML_NODE_ATTRIBUTE:
    return "attribute";
  case TURBO_XML_NODE_ROOT:
    return "root";
  case TURBO_XML_NODE_PI:
    return "pi";
  case TURBO_XML_NODE_NAMESPACE:
    return "namespace";
  case TURBO_XML_NODE_XML_HEADER:
    return "xml_header";
  case TURBO_XML_NODE_DTD:
    return "dtd";
  default:
    return "unknown";
  }
}

const char *turbo_xml_xpath_node_name(const turbo_xml_xpath_node_t *node) {
  if (!node)
    return NULL;

  switch (turbo_cxml_node_type(node)) {
  case CXML_ELEM_NODE:
    return turbo_cxml_string_raw(&((const cxml_elem_node *)node)->name.qname);
  case CXML_ATTR_NODE:
    return turbo_cxml_string_raw(&((const cxml_attr_node *)node)->name.qname);
  case CXML_ROOT_NODE:
    return turbo_cxml_string_raw(&((const cxml_root_node *)node)->name);
  case CXML_PI_NODE:
    return turbo_cxml_string_raw(&((const cxml_pi_node *)node)->target);
  case CXML_NS_NODE: {
    const cxml_ns_node *ns = (const cxml_ns_node *)node;
    return ns->is_default ? "xmlns" : turbo_cxml_string_raw(&ns->prefix);
  }
  default:
    return NULL;
  }
}

const char *turbo_xml_xpath_node_text(const turbo_xml_xpath_node_t *node) {
  if (!node)
    return NULL;

  switch (turbo_cxml_node_type(node)) {
  case CXML_ELEM_NODE: {
    const cxml_elem_node *elem = (const cxml_elem_node *)node;
    if (elem->has_text && !cxml_list_is_empty((cxml_list *)&elem->children)) {
      cxml_text_node *txt = (cxml_text_node *)cxml_list_get((cxml_list *)&elem->children, 0);
      if (txt && txt->_type == CXML_TEXT_NODE)
        return cxml_string_as_raw(&txt->value);
    }
    return NULL;
  }
  case CXML_TEXT_NODE:
    return turbo_cxml_string_raw(&((const cxml_text_node *)node)->value);
  case CXML_ATTR_NODE:
    return turbo_cxml_string_raw(&((const cxml_attr_node *)node)->value);
  case CXML_COMM_NODE:
    return turbo_cxml_string_raw(&((const cxml_comm_node *)node)->value);
  case CXML_PI_NODE:
    return turbo_cxml_string_raw(&((const cxml_pi_node *)node)->value);
  case CXML_NS_NODE:
    return turbo_cxml_string_raw(&((const cxml_ns_node *)node)->uri);
  case CXML_DTD_NODE:
    return turbo_cxml_string_raw(&((const cxml_dtd_node *)node)->value);
  default:
    return NULL;
  }
}

char *turbo_xml_xpath_node_xml_dup(const turbo_xml_xpath_node_t *node) {
  if (!node)
    return NULL;
  return cxml_node_to_rstring((void *)node);
}

void turbo_xml_string_free(char *str) {
  free(str);
}

turbo_json_type_t turbo_json_type(const json_value_t *value) {
  return (turbo_json_type_t)json_type(value);
}

bool turbo_json_is_null(const json_value_t *value) { return json_is_null(value); }

bool turbo_json_bool(const json_value_t *value) { return json_bool(value); }

double turbo_json_number(const json_value_t *value) { return json_number(value); }

const char *turbo_json_string(const json_value_t *value) { return json_string(value); }

size_t turbo_json_string_len(const json_value_t *value) { return json_string_len(value); }

size_t turbo_json_object_size(const json_value_t *obj) { return json_object_size(obj); }

const char *turbo_json_object_key(const json_value_t *obj, size_t index) {
  return json_object_key(obj, index);
}

json_value_t *turbo_json_object_value(const json_value_t *obj, size_t index) {
  return json_object_value(obj, index);
}

json_value_t *turbo_json_object_get(const json_value_t *obj, const char *key) {
  return json_object_get(obj, key);
}

size_t turbo_json_array_size(const json_value_t *arr) { return json_array_size(arr); }

json_value_t *turbo_json_array_get(const json_value_t *arr, size_t index) {
  return json_array_get(arr, index);
}

int turbo_json_get_int(const json_value_t *obj, const char *key, int def) {
  return json_get_int(obj, key, def);
}

bool turbo_json_get_bool(const json_value_t *obj, const char *key, bool def) {
  return json_get_bool(obj, key, def);
}

double turbo_json_get_double(const json_value_t *obj, const char *key, double def) {
  return json_get_double(obj, key, def);
}

const char *turbo_json_get_string(const json_value_t *obj, const char *key) {
  return json_get_string(obj, key);
}

char *turbo_json_serialize(const json_value_t *value, size_t *out_len) {
  return json_serialize(value, out_len);
}

char *turbo_json_serialize_pretty(const json_value_t *value, size_t *out_len) {
  return json_serialize_pretty(value, out_len);
}

char *turbo_json_serialize_pretty_crlf(const json_value_t *value, size_t *out_len) {
  return json_serialize_pretty_crlf(value, out_len);
}

void turbo_json_serialize_free(char *str) { json_serialize_free(str); }

static json_value_t *turbo_json_clone_internal(const json_value_t *value) {
  json_value_t *clone = NULL;
  size_t i;

  if (!value) {
    return NULL;
  }

  switch (turbo_json_type(value)) {
  case TURBO_JSON_NULL:
    return turbo_json_create_null();
  case TURBO_JSON_BOOL:
    return turbo_json_create_bool(turbo_json_bool(value));
  case TURBO_JSON_NUMBER:
    return turbo_json_create_number(turbo_json_number(value));
  case TURBO_JSON_STRING:
    return turbo_json_create_string(turbo_json_string(value));
  case TURBO_JSON_ARRAY:
    clone = turbo_json_create_array();
    if (!clone) {
      return NULL;
    }
    for (i = 0; i < turbo_json_array_size(value); ++i) {
      json_value_t *item_clone = turbo_json_clone_internal(turbo_json_array_get(value, i));
      if (!item_clone) {
        turbo_free_json(&clone);
        return NULL;
      }
      turbo_json_array_add(clone, item_clone);
    }
    return clone;
  case TURBO_JSON_OBJECT:
    clone = turbo_json_create_object();
    if (!clone) {
      return NULL;
    }
    for (i = 0; i < turbo_json_object_size(value); ++i) {
      const char *key = turbo_json_object_key(value, i);
      json_value_t *item_clone = turbo_json_clone_internal(turbo_json_object_value(value, i));
      if (!key || !item_clone) {
        turbo_free_json(&item_clone);
        turbo_free_json(&clone);
        return NULL;
      }
      turbo_json_object_add(clone, key, item_clone);
    }
    return clone;
  default:
    return NULL;
  }
}

json_value_t *turbo_json_clone(const json_value_t *value) { return turbo_json_clone_internal(value); }

/* JSON Builder/Modifier */
json_value_t *turbo_json_create_object(void) { return json_create_object(); }
json_value_t *turbo_json_create_array(void) { return json_create_array(); }
json_value_t *turbo_json_create_string(const char *str) { return json_create_string(str); }
json_value_t *turbo_json_create_number(double num) { return json_create_number(num); }
json_value_t *turbo_json_create_bool(bool val) { return json_create_bool(val); }
json_value_t *turbo_json_create_null(void) { return json_create_null(); }

void turbo_json_object_add(json_value_t *obj, const char *key, json_value_t *val) {
    json_object_add(obj, key, val);
}

void turbo_json_array_add(json_value_t *arr, json_value_t *val) {
    json_array_add(arr, val);
}

void turbo_json_object_set_string(json_value_t *obj, const char *key, const char *val) {
    json_object_set_string(obj, key, val);
}

void turbo_json_object_set_number(json_value_t *obj, const char *key, double val) {
    json_object_set_number(obj, key, val);
}

void turbo_json_object_set_bool(json_value_t *obj, const char *key, bool val) {
    json_object_set_bool(obj, key, val);
}

void turbo_json_object_set_null(json_value_t *obj, const char *key) {
    json_object_set_null(obj, key);
}

/* CSV */
int turbo_parse_csv(const uint8_t *data, size_t len, void *out) {
  if (!data || !out)
    return -1;
  csv_doc_t *doc = csv_parse((const char *)data, len);
  if (!doc)
    return -1;
  *(csv_doc_t **)out = doc;
  return 0;
}

int turbo_parse_csv_opts(const uint8_t *data, size_t len, const turbo_csv_options_t *opts,
                         void *out) {
  if (!data || !out)
    return -1;

  if (!opts) {
    return turbo_parse_csv(data, len, out);
  }

  csv_options_t native_opts = {
      .has_header = opts->has_header,
      .delimiter = opts->delimiter,
      .quote = opts->quote,
      .skip_empty_rows = opts->skip_empty_rows,
  };

  csv_doc_t *doc = csv_parse_opts((const char *)data, len, &native_opts);
  if (!doc)
    return -1;

  *(csv_doc_t **)out = doc;
  return 0;
}

void turbo_free_csv(void *out) {
  if (!out)
    return;
  void *ptr = *(void **)out;
  if (ptr)
    csv_free((csv_doc_t *)ptr);
  *(void **)out = NULL;
}

size_t turbo_csv_row_count(const turbo_csv_doc_t *doc) {
  return csv_row_count((const csv_doc_t *)doc);
}

size_t turbo_csv_column_count(const turbo_csv_doc_t *doc) {
  return csv_column_count((const csv_doc_t *)doc);
}

const char *turbo_csv_get(const turbo_csv_doc_t *doc, size_t row, size_t col) {
  return csv_get((const csv_doc_t *)doc, row, col);
}

int turbo_csv_get_int(const turbo_csv_doc_t *doc, size_t row, size_t col, int def) {
  return csv_get_int((const csv_doc_t *)doc, row, col, def);
}

double turbo_csv_get_double(const turbo_csv_doc_t *doc, size_t row, size_t col, double def) {
  return csv_get_double((const csv_doc_t *)doc, row, col, def);
}

bool turbo_csv_get_bool(const turbo_csv_doc_t *doc, size_t row, size_t col, bool def) {
  return csv_get_bool((const csv_doc_t *)doc, row, col, def);
}

size_t turbo_csv_find_column(const turbo_csv_doc_t *doc, const char *header_name) {
  return csv_find_column((const csv_doc_t *)doc, header_name);
}

int turbo_csv_write_file(const turbo_csv_doc_t *doc, const char *filename) {
  return csv_write_file((const csv_doc_t *)doc, filename);
}

turbo_dsv_filter_t *turbo_dsv_filter_create(const turbo_csv_doc_t *doc,
                                            size_t header_row_index) {
  return (turbo_dsv_filter_t *)dsv_filter_create((const csv_doc_t *)doc, header_row_index);
}

void turbo_dsv_filter_destroy(turbo_dsv_filter_t *filter) {
  dsv_filter_destroy((dsv_filter_t *)filter);
}

const char *turbo_dsv_filter_error(turbo_dsv_filter_t *filter) {
  return dsv_filter_error((dsv_filter_t *)filter);
}

bool turbo_dsv_filter_compile(turbo_dsv_filter_t *filter, const char *expression) {
  return dsv_filter_compile((dsv_filter_t *)filter, expression);
}

void turbo_dsv_filter_set_output_delimiter(turbo_dsv_filter_t *filter, char delimiter) {
  dsv_filter_set_output_delimiter((dsv_filter_t *)filter, delimiter);
}

int turbo_dsv_filter_check_row(turbo_dsv_filter_t *filter, size_t row_index) {
  return dsv_filter_check_row((dsv_filter_t *)filter, row_index);
}

void turbo_dsv_filter_run(turbo_dsv_filter_t *filter, turbo_dsv_row_callback_t callback,
                          void *user_data) {
  dsv_filter_run((dsv_filter_t *)filter, (dsv_row_callback_t)callback, user_data);
}

turbo_csv_stream_processor_t *
turbo_csv_stream_processor_create(const turbo_csv_options_t *opts) {
  if (!opts) {
    return (turbo_csv_stream_processor_t *)csv_stream_processor_create(NULL);
  }

  csv_options_t native_opts = {
      .has_header = opts->has_header,
      .delimiter = opts->delimiter,
      .quote = opts->quote,
      .skip_empty_rows = opts->skip_empty_rows,
  };
  return (turbo_csv_stream_processor_t *)csv_stream_processor_create(&native_opts);
}

void turbo_csv_stream_processor_destroy(turbo_csv_stream_processor_t *p) {
  csv_stream_processor_destroy((csv_stream_processor_t *)p);
}

bool turbo_csv_stream_processor_set_filter(turbo_csv_stream_processor_t *p,
                                           const char *expr) {
  return csv_stream_processor_set_filter((csv_stream_processor_t *)p, expr);
}

void turbo_csv_stream_processor_set_columns(turbo_csv_stream_processor_t *p, const char *names) {
  csv_stream_processor_set_columns((csv_stream_processor_t *)p, names);
}

void turbo_csv_stream_processor_feed(const char *data, size_t len, void *user_data) {
  csv_stream_processor_feed(data, len, user_data);
}

void turbo_csv_stream_processor_finish(turbo_csv_stream_processor_t *p) {
  csv_stream_processor_finish((csv_stream_processor_t *)p);
}

size_t turbo_csv_stream_processor_row_count(const turbo_csv_stream_processor_t *p) {
  return csv_stream_processor_row_count((const csv_stream_processor_t *)p);
}

size_t turbo_csv_stream_processor_col_count(const turbo_csv_stream_processor_t *p) {
  return csv_stream_processor_col_count((const csv_stream_processor_t *)p);
}

const char *turbo_csv_stream_processor_col_name(const turbo_csv_stream_processor_t *p,
                                                size_t idx) {
  return csv_stream_processor_col_name((const csv_stream_processor_t *)p, idx);
}

size_t turbo_csv_stream_processor_col_index(const turbo_csv_stream_processor_t *p,
                                            const char *name) {
  return csv_stream_processor_col_index((const csv_stream_processor_t *)p, name);
}

const double *turbo_csv_stream_processor_col_data(const turbo_csv_stream_processor_t *p,
                                                  size_t col, size_t *out_len) {
  return csv_stream_processor_col_data((const csv_stream_processor_t *)p, col, out_len);
}

const char *turbo_csv_stream_processor_get_str(const turbo_csv_stream_processor_t *p,
                                               size_t row, size_t col) {
  return csv_stream_processor_get_str((const csv_stream_processor_t *)p, row, col);
}

const char *turbo_csv_stream_processor_error(const turbo_csv_stream_processor_t *p) {
  return csv_stream_processor_error((const csv_stream_processor_t *)p);
}

/* INI */
int turbo_parse_ini(const uint8_t *data, size_t len, void *out) {
  if (!data || !out)
    return -1;
  ini_t *ini = ini_parse((const char *)data, len);
  if (!ini)
    return -1;
  *(ini_t **)out = ini;
  return 0;
}

void turbo_free_ini(void *out) {
  if (!out)
    return;
  void *ptr = *(void **)out;
  if (ptr)
    ini_free((ini_t *)ptr);
  *(void **)out = NULL;
}

const char *turbo_ini_get(const turbo_ini_t *ini, const char *section, const char *key) {
  return ini_get((ini_t *)ini, section, key);
}

int turbo_ini_get_int(const turbo_ini_t *ini, const char *section, const char *key, int def) {
  return ini_get_int((ini_t *)ini, section, key, def);
}

bool turbo_ini_get_bool(const turbo_ini_t *ini, const char *section, const char *key, bool def) {
  return ini_get_bool((ini_t *)ini, section, key, def);
}

double turbo_ini_get_double(const turbo_ini_t *ini, const char *section, const char *key, double def) {
  return ini_get_double((ini_t *)ini, section, key, def);
}

/* URI */
int turbo_parse_uri(const uint8_t *data, size_t len, void *out) {
  if (!data || !out)
    return -1;

  // Ensure data is null-terminated for uri_parse
  char *temp = (char *)malloc(len + 1);
  if (!temp)
    return -1;
  memcpy(temp, data, len);
  temp[len] = '\0';

  uri_t *uri = (uri_t *)malloc(sizeof(uri_t));
  if (!uri) {
    free(temp);
    return -1;
  }

  if (!uri_parse(temp, uri)) {
    free(temp);
    free(uri);
    return -1;
  }

  free(temp);
  *(uri_t **)out = uri;
  return 0;
}

void turbo_free_uri(void *out) {
  if (!out)
    return;
  void *ptr = *(void **)out;
  if (ptr)
    free(ptr);
  *(void **)out = NULL;
}

const char *turbo_uri_scheme(const uri_t *uri) { return uri ? uri->scheme : NULL; }
const char *turbo_uri_userinfo(const uri_t *uri) { return uri ? uri->userinfo : NULL; }
const char *turbo_uri_host(const uri_t *uri) { return uri ? uri->host : NULL; }
int turbo_uri_port(const uri_t *uri) { return uri ? uri->port : -1; }
const char *turbo_uri_path(const uri_t *uri) { return uri ? uri->path : NULL; }
const char *turbo_uri_query(const uri_t *uri) { return uri ? uri->query : NULL; }
const char *turbo_uri_fragment(const uri_t *uri) { return uri ? uri->fragment : NULL; }
turbo_uri_host_type_t turbo_uri_host_type(const uri_t *uri) {
  return uri ? (turbo_uri_host_type_t)uri->host_type : TURBO_URI_HOST_UNKNOWN;
}
bool turbo_uri_is_valid(const uri_t *uri) { return uri ? uri->valid : false; }

/* TLV */
int turbo_parse_tlv(const uint8_t *data, size_t len, void *out) {
  if (!data || !out)
    return -1;
  frame_t *frame = (frame_t *)calloc(1, sizeof(frame_t));
  if (!frame)
    return -1;
  int rc = frame_parse(data, len, frame, FRAME_PARSE_FLAG_NONE);
  if (rc != FRAME_PARSE_OK) {
    free(frame);
    return rc;
  }
  *(frame_t **)out = frame;
  return 0;
}

void turbo_free_tlv(void *out) {
  if (!out)
    return;
  void *ptr = *(void **)out;
  if (ptr)
    free(ptr);
  *(void **)out = NULL;
}

uint32_t turbo_tlv_msg_id(const turbo_tlv_frame_t *frame) {
  return frame ? ((const frame_t *)frame)->msg_id : 0;
}

uint8_t turbo_tlv_version(const turbo_tlv_frame_t *frame) {
  return frame ? ((const frame_t *)frame)->version : 0;
}

uint8_t turbo_tlv_type(const turbo_tlv_frame_t *frame) {
  return frame ? ((const frame_t *)frame)->payload_type : 0;
}

size_t turbo_tlv_payload_size(const turbo_tlv_frame_t *frame) {
  return frame ? ((const frame_t *)frame)->payload_size : 0;
}

const char *turbo_tlv_payload(const turbo_tlv_frame_t *frame) {
  return frame ? ((const frame_t *)frame)->payload : NULL;
}

uint32_t turbo_tlv_crc32(const turbo_tlv_frame_t *frame) {
  return frame ? ((const frame_t *)frame)->crc32 : 0;
}

int turbo_tlv_peek_size(const uint8_t *data, size_t len, uint32_t *out_size) {
  return (int)frame_peek_size(data, len, out_size);
}

/* LTV */
int turbo_parse_ltv(const uint8_t *data, size_t len, void *out) {
  if (!data || !out)
    return -1;
  ltv_message_t *ltv = (ltv_message_t *)calloc(1, sizeof(ltv_message_t));
  if (!ltv)
    return -1;
  int rc = ltv_parse(data, len, ltv);
  if (rc != LTV_PARSE_OK) {
    free(ltv);
    return rc;
  }
  *(ltv_message_t **)out = ltv;
  return 0;
}

void turbo_free_ltv(void *out) {
  if (!out)
    return;
  void *ptr = *(void **)out;
  if (ptr)
    free(ptr);
  *(void **)out = NULL;
}

uint8_t turbo_ltv_type(const turbo_ltv_message_t *msg) {
  return msg ? ((const ltv_message_t *)msg)->type : 0;
}

const uint8_t *turbo_ltv_value(const turbo_ltv_message_t *msg) {
  return msg ? ((const ltv_message_t *)msg)->value : NULL;
}

size_t turbo_ltv_value_len(const turbo_ltv_message_t *msg) {
  return msg ? ((const ltv_message_t *)msg)->value_size : 0;
}

size_t turbo_ltv_wire_size(size_t value_size) {
  return ltv_wire_size(value_size);
}

size_t turbo_ltv_build(uint8_t type, const uint8_t *value, size_t value_size,
                       uint8_t *out, size_t out_len) {
  return ltv_build(type, value, value_size, out, out_len);
}

int turbo_ltv_peek_size(const uint8_t *data, size_t len, uint32_t *out_length, size_t *out_header) {
  return (int)ltv_peek_size(data, len, out_length, out_header);
}

turbo_ltv_stream_t *turbo_ltv_stream_create(size_t buffer_size) {
  return (turbo_ltv_stream_t *)ltv_stream_create(buffer_size);
}

void turbo_ltv_stream_destroy(turbo_ltv_stream_t *stream) {
  ltv_stream_destroy((ltv_stream_t *)stream);
}

int turbo_ltv_stream_feed(turbo_ltv_stream_t *stream, const uint8_t *data, size_t len, void **out) {
  if (!stream || !out)
    return -1;
  
  ltv_message_t *msg = (ltv_message_t *)calloc(1, sizeof(ltv_message_t));
  if (!msg)
    return -1;
    
  LtvParseResult rc = ltv_stream_feed((ltv_stream_t *)stream, data, len, msg);
  
  if (rc == LTV_PARSE_OK) {
    *(ltv_message_t **)out = msg;
    return 0; /* Complete */
  } else if (rc == LTV_PARSE_NEED_MORE) {
    free(msg);
    return 1; /* Need more */
  } else {
    free(msg);
    return -1; /* Error */
  }
}

void turbo_ltv_stream_reset(turbo_ltv_stream_t *stream) {
  ltv_stream_reset((ltv_stream_t *)stream);
}

/* Modbus */
static void turbo_modbus_pdu_from_native(turbo_modbus_pdu_t *dst,
                                         const modbus_pdu_t *src) {
  dst->function_code = src->function_code;
  dst->data = src->data;
  dst->data_size = src->data_size;
}

static void turbo_modbus_pdu_to_native(modbus_pdu_t *dst,
                                       const turbo_modbus_pdu_t *src) {
  dst->function_code = src->function_code;
  dst->data = src->data;
  dst->data_size = src->data_size;
}

static void turbo_modbus_tcp_from_native(turbo_modbus_tcp_adu_t *dst,
                                         const modbus_tcp_adu_t *src) {
  dst->transaction_id = src->transaction_id;
  dst->protocol_id = src->protocol_id;
  dst->length = src->length;
  dst->unit_id = src->unit_id;
  turbo_modbus_pdu_from_native(&dst->pdu, &src->pdu);
  dst->consumed = src->consumed;
}

static void turbo_modbus_tcp_to_native(modbus_tcp_adu_t *dst,
                                       const turbo_modbus_tcp_adu_t *src) {
  dst->transaction_id = src->transaction_id;
  dst->protocol_id = src->protocol_id;
  dst->length = src->length;
  dst->unit_id = src->unit_id;
  turbo_modbus_pdu_to_native(&dst->pdu, &src->pdu);
  dst->consumed = src->consumed;
}

static void turbo_modbus_rtu_from_native(turbo_modbus_rtu_adu_t *dst,
                                         const modbus_rtu_adu_t *src) {
  dst->address = src->address;
  turbo_modbus_pdu_from_native(&dst->pdu, &src->pdu);
  dst->crc = src->crc;
  dst->consumed = src->consumed;
}

static void turbo_modbus_rtu_to_native(modbus_rtu_adu_t *dst,
                                       const turbo_modbus_rtu_adu_t *src) {
  dst->address = src->address;
  turbo_modbus_pdu_to_native(&dst->pdu, &src->pdu);
  dst->crc = src->crc;
  dst->consumed = src->consumed;
}

int turbo_modbus_tcp_peek_size(const uint8_t *data, size_t len, size_t *out_size) {
  return (int)modbus_tcp_peek_size(data, len, out_size);
}

int turbo_modbus_tcp_read(const uint8_t *data, size_t len, turbo_modbus_tcp_adu_t *out) {
  if (!out)
    return (int)MODBUS_PARSE_INVALID_INPUT;

  modbus_tcp_adu_t native;
  ModbusParseResult rc = modbus_tcp_read(data, len, &native);
  if (rc == MODBUS_PARSE_OK)
    turbo_modbus_tcp_from_native(out, &native);
  return (int)rc;
}

size_t turbo_modbus_tcp_write(const turbo_modbus_tcp_adu_t *adu, uint8_t *out,
                              size_t out_len) {
  if (!adu)
    return 0;

  modbus_tcp_adu_t native;
  turbo_modbus_tcp_to_native(&native, adu);
  return modbus_tcp_write(&native, out, out_len);
}

uint16_t turbo_modbus_rtu_crc16(const uint8_t *data, size_t len) {
  return modbus_rtu_crc16(data, len);
}

int turbo_modbus_rtu_read(const uint8_t *data, size_t len, turbo_modbus_rtu_adu_t *out) {
  if (!out)
    return (int)MODBUS_PARSE_INVALID_INPUT;

  modbus_rtu_adu_t native;
  ModbusParseResult rc = modbus_rtu_read(data, len, &native);
  if (rc == MODBUS_PARSE_OK)
    turbo_modbus_rtu_from_native(out, &native);
  return (int)rc;
}

size_t turbo_modbus_rtu_write(const turbo_modbus_rtu_adu_t *adu, uint8_t *out,
                              size_t out_len) {
  if (!adu)
    return 0;

  modbus_rtu_adu_t native;
  turbo_modbus_rtu_to_native(&native, adu);
  return modbus_rtu_write(&native, out, out_len);
}

int turbo_modbus_read(turbo_modbus_transport_t transport, const uint8_t *data,
                      size_t len, turbo_modbus_adu_t *out) {
  if (!out)
    return (int)MODBUS_PARSE_INVALID_INPUT;

  if (transport == TURBO_MODBUS_TRANSPORT_TCP) {
    int rc = turbo_modbus_tcp_read(data, len, &out->frame.tcp);
    if (rc == TURBO_MODBUS_PARSE_OK)
      out->transport = TURBO_MODBUS_TRANSPORT_TCP;
    return rc;
  }

  if (transport == TURBO_MODBUS_TRANSPORT_RTU) {
    int rc = turbo_modbus_rtu_read(data, len, &out->frame.rtu);
    if (rc == TURBO_MODBUS_PARSE_OK)
      out->transport = TURBO_MODBUS_TRANSPORT_RTU;
    return rc;
  }

  return (int)MODBUS_PARSE_INVALID_INPUT;
}

size_t turbo_modbus_write(const turbo_modbus_adu_t *adu, uint8_t *out, size_t out_len) {
  if (!adu)
    return 0;

  if (adu->transport == TURBO_MODBUS_TRANSPORT_TCP)
    return turbo_modbus_tcp_write(&adu->frame.tcp, out, out_len);

  if (adu->transport == TURBO_MODBUS_TRANSPORT_RTU)
    return turbo_modbus_rtu_write(&adu->frame.rtu, out, out_len);

  return 0;
}

/* SOA */
int turbo_parse_soa(const uint8_t *data, size_t len, void *out) {
  if (!data || !out)
    return -1;
  soa_batch_t *batch = (soa_batch_t *)calloc(1, sizeof(soa_batch_t));
  if (!batch)
    return -1;
  int rc = soa_parse(data, len, batch);
  if (rc != SOA_PARSE_OK) {
    free(batch);
    return rc;
  }
  *(soa_batch_t **)out = batch;
  return 0;
}

void turbo_free_soa(void *out) {
  if (!out)
    return;
  void *ptr = *(void **)out;
  if (ptr)
    free(ptr);
  *(void **)out = NULL;
}

uint32_t turbo_soa_count(const turbo_soa_batch_t *batch) {
  return batch ? ((const soa_batch_t *)batch)->count : 0;
}

uint16_t turbo_soa_schema_id(const turbo_soa_batch_t *batch) {
  return batch ? ((const soa_batch_t *)batch)->schema_id : 0;
}

uint16_t turbo_soa_present_mask(const turbo_soa_batch_t *batch) {
  return batch ? ((const soa_batch_t *)batch)->present_mask : 0;
}

int8_t turbo_soa_get_i8(const turbo_soa_batch_t *b, int col, uint32_t row) {
  return soa_get_i8((const soa_batch_t *)b, col, row);
}

uint8_t turbo_soa_get_u8(const turbo_soa_batch_t *b, int col, uint32_t row) {
  return soa_get_u8((const soa_batch_t *)b, col, row);
}

int16_t turbo_soa_get_i16(const turbo_soa_batch_t *b, int col, uint32_t row) {
  return soa_get_i16((const soa_batch_t *)b, col, row);
}

uint16_t turbo_soa_get_u16(const turbo_soa_batch_t *b, int col, uint32_t row) {
  return soa_get_u16((const soa_batch_t *)b, col, row);
}

int32_t turbo_soa_get_i32(const turbo_soa_batch_t *b, int col, uint32_t row) {
  return soa_get_i32((const soa_batch_t *)b, col, row);
}

uint32_t turbo_soa_get_u32(const turbo_soa_batch_t *b, int col, uint32_t row) {
  return soa_get_u32((const soa_batch_t *)b, col, row);
}

int64_t turbo_soa_get_i64(const turbo_soa_batch_t *b, int col, uint32_t row) {
  return soa_get_i64((const soa_batch_t *)b, col, row);
}

uint64_t turbo_soa_get_u64(const turbo_soa_batch_t *b, int col, uint32_t row) {
  return soa_get_u64((const soa_batch_t *)b, col, row);
}

double turbo_soa_get_f64(const turbo_soa_batch_t *b, int col, uint32_t row) {
  return soa_get_f64((const soa_batch_t *)b, col, row);
}

size_t turbo_soa_wire_size(const turbo_soa_schema_t *schema, uint32_t count,
                           uint16_t present_mask) {
  return soa_wire_size((const soa_schema_t *)schema, count, present_mask);
}

size_t turbo_soa_build_header(const turbo_soa_schema_t *schema, uint32_t count,
                              uint16_t present_mask, uint8_t *out, size_t out_len) {
  return soa_build_header((const soa_schema_t *)schema, count, present_mask, out,
                          out_len);
}

uint8_t turbo_soa_type_width(int type) {
  return soa_type_width((SoaColumnType)type);
}

int turbo_soa_peek_header(const uint8_t *data, size_t len, uint32_t *out_count, uint16_t *out_schema) {
  return (int)soa_peek_header(data, len, out_count, out_schema);
}

int turbo_soa_schema_count(const turbo_soa_schema_t *schema) {
  return schema ? ((const soa_schema_t *)schema)->column_count : 0;
}

int turbo_soa_schema_column_type(const turbo_soa_schema_t *schema, int idx) {
  const soa_schema_t *s = (const soa_schema_t *)schema;
  if (!s || idx < 0 || idx >= s->column_count)
    return 0; /* UNKNOWN */
  return s->columns[idx].type;
}

/* CMD Parser */
struct turbo_cmd_subcommand_s {
  char *name;
  char *info;
  CmdArgerDesc *optional_args;
  uint32_t optional_count;
  uint32_t optional_capacity;
  CmdArgerDesc *required_args;
  uint32_t required_count;
  uint32_t required_capacity;
};

struct turbo_cmd_parser_s {
  char *app_name;
  char *version;
  
  CmdArgerDesc *optional_args;
  uint32_t optional_count;
  uint32_t optional_capacity;

  CmdArgerDesc *required_args;
  uint32_t required_count;
  uint32_t required_capacity;

  turbo_cmd_subcommand_t *subcommands;
  uint32_t subcommand_count;
  uint32_t subcommand_capacity;
};

turbo_cmd_parser_t *turbo_cmd_create(const char *app_name, const char *version) {
  turbo_cmd_parser_t *parser = (turbo_cmd_parser_t *)calloc(1, sizeof(turbo_cmd_parser_t));
  if (!parser) return NULL;
  
  if (app_name) parser->app_name = strdup(app_name);
  if (version) parser->version = strdup(version);
  
  parser->optional_capacity = 8;
  parser->optional_args = (CmdArgerDesc *)calloc(parser->optional_capacity, sizeof(CmdArgerDesc));
  
  parser->required_capacity = 8;
  parser->required_args = (CmdArgerDesc *)calloc(parser->required_capacity, sizeof(CmdArgerDesc));

  parser->subcommand_capacity = 4;
  parser->subcommands = (turbo_cmd_subcommand_t *)calloc(parser->subcommand_capacity, sizeof(turbo_cmd_subcommand_t));
  
  if (!parser->optional_args || !parser->required_args || !parser->subcommands) {
    turbo_cmd_destroy(parser);
    return NULL;
  }
  
  return parser;
}

void turbo_cmd_destroy(turbo_cmd_parser_t *parser) {
  if (!parser) return;
  if (parser->app_name) free(parser->app_name);
  if (parser->version) free(parser->version);
  if (parser->optional_args) free(parser->optional_args);
  if (parser->required_args) free(parser->required_args);
  for (uint32_t i = 0; i < parser->subcommand_count; i++) {
    turbo_cmd_subcommand_t *sub = &parser->subcommands[i];
    if (sub->name) free(sub->name);
    if (sub->info) free(sub->info);
    if (sub->optional_args) free(sub->optional_args);
    if (sub->required_args) free(sub->required_args);
  }
  if (parser->subcommands) free(parser->subcommands);
  free(parser);
}

static void ensure_optional_capacity(turbo_cmd_parser_t *parser) {
  if (parser->optional_count >= parser->optional_capacity) {
    parser->optional_capacity *= 2;
    parser->optional_args = (CmdArgerDesc *)realloc(parser->optional_args, 
                                                     parser->optional_capacity * sizeof(CmdArgerDesc));
  }
}

void turbo_cmd_add_flag(turbo_cmd_parser_t *parser, bool *out, const char *name,
                        const char *short_name, const char *desc) {
  if (!parser) return;
  ensure_optional_capacity(parser);
  parser->optional_args[parser->optional_count++] = 
      cmd_arger_desc_flag_sh((CmdArgerBool*)out, (char*)name, (char*)short_name, (char*)desc);
}

void turbo_cmd_add_string(turbo_cmd_parser_t *parser, char **out, const char *name,
                          const char *short_name, const char *desc) {
  if (!parser) return;
  ensure_optional_capacity(parser);
  parser->optional_args[parser->optional_count++] = 
      cmd_arger_desc_string_sh(out, (char*)name, (char*)short_name, (char*)desc);
}

void turbo_cmd_add_integer(turbo_cmd_parser_t *parser, int64_t *out, const char *name,
                           const char *short_name, const char *desc) {
  if (!parser) return;
  ensure_optional_capacity(parser);
  parser->optional_args[parser->optional_count++] = 
      cmd_arger_desc_integer_sh(out, (char*)name, (char*)short_name, (char*)desc);
}

void turbo_cmd_add_float(turbo_cmd_parser_t *parser, double *out, const char *name,
                         const char *short_name, const char *desc) {
  if (!parser) return;
  ensure_optional_capacity(parser);
  parser->optional_args[parser->optional_count++] = 
      cmd_arger_desc_float_sh(out, (char*)name, (char*)short_name, (char*)desc);
}

void turbo_cmd_add_string_list(turbo_cmd_parser_t *parser, char **out_arr,
                               uint32_t *out_count, uint32_t max_count, const char *name,
                               const char *short_name, const char *desc) {
  if (!parser) return;
  ensure_optional_capacity(parser);
  parser->optional_args[parser->optional_count++] = 
      cmd_arger_desc_string_list_sh(out_arr, out_count, max_count, (char*)name, (char*)short_name, (char*)desc);
}

void turbo_cmd_add_required_string(turbo_cmd_parser_t *parser, char **out,
                                   const char *name, const char *desc) {
  if (!parser) return;
  if (parser->required_count >= parser->required_capacity) {
    parser->required_capacity *= 2;
    parser->required_args = (CmdArgerDesc *)realloc(parser->required_args, 
                                                     parser->required_capacity * sizeof(CmdArgerDesc));
  }
  parser->required_args[parser->required_count++] = 
      cmd_arger_desc_string(out, (char*)name, (char*)desc);
}

void turbo_cmd_add_required_integer(turbo_cmd_parser_t *parser, int64_t *out,
                                    const char *name, const char *desc) {
  if (!parser) return;
  if (parser->required_count >= parser->required_capacity) {
    parser->required_capacity *= 2;
    parser->required_args = (CmdArgerDesc *)realloc(parser->required_args, 
                                                     parser->required_capacity * sizeof(CmdArgerDesc));
  }
  parser->required_args[parser->required_count++] = 
      cmd_arger_desc_integer(out, (char*)name, (char*)desc);
}

void turbo_cmd_add_enum(turbo_cmd_parser_t *parser, int64_t *out, const char *name,
                        const char *short_name, const char *desc,
                        turbo_cmd_enum_t *choices, uint32_t choices_count) {
  if (!parser) return;
  ensure_optional_capacity(parser);
  parser->optional_args[parser->optional_count++] = 
      cmd_arger_desc_enum_sh(out, (char*)name, (char*)short_name, (char*)desc,
                             (CmdArgerEnumDesc*)choices, choices_count);
}

uint32_t turbo_cmd_last_index(turbo_cmd_parser_t *parser) {
  if (!parser || parser->optional_count == 0) return 0;
  return parser->optional_count - 1;
}

void turbo_cmd_set_env(turbo_cmd_parser_t *parser, uint32_t index, const char *env_var) {
  if (!parser || index >= parser->optional_count) return;
  parser->optional_args[index] = cmd_arger_with_env(parser->optional_args[index], env_var);
}

void turbo_cmd_set_group(turbo_cmd_parser_t *parser, uint32_t index, const char *group) {
  if (!parser || index >= parser->optional_count) return;
  parser->optional_args[index] = cmd_arger_with_group(parser->optional_args[index], group);
}

void turbo_cmd_set_choices(turbo_cmd_parser_t *parser, uint32_t index,
                           const char **choices, uint32_t count) {
  if (!parser || index >= parser->optional_count) return;
  parser->optional_args[index] = cmd_arger_with_choices(parser->optional_args[index], choices, count);
}

void turbo_cmd_set_validator(turbo_cmd_parser_t *parser, uint32_t index,
                             turbo_cmd_validator_t validator) {
  if (!parser || index >= parser->optional_count) return;
  parser->optional_args[index] = cmd_arger_with_validator(parser->optional_args[index], 
                                                           (CmdArgerValidator)validator);
}

void turbo_cmd_set_required(turbo_cmd_parser_t *parser, uint32_t index) {
  if (!parser || index >= parser->optional_count) return;
  parser->optional_args[index] = cmd_arger_required(parser->optional_args[index]);
}

/* Subcommand support */
turbo_cmd_subcommand_t *turbo_cmd_add_subcommand(turbo_cmd_parser_t *parser,
                                                   const char *name, const char *desc) {
  if (!parser) return NULL;
  if (parser->subcommand_count >= parser->subcommand_capacity) {
    parser->subcommand_capacity *= 2;
    parser->subcommands = (turbo_cmd_subcommand_t *)realloc(parser->subcommands,
                          parser->subcommand_capacity * sizeof(turbo_cmd_subcommand_t));
  }
  turbo_cmd_subcommand_t *sub = &parser->subcommands[parser->subcommand_count++];
  memset(sub, 0, sizeof(*sub));
  sub->name = strdup(name);
  sub->info = desc ? strdup(desc) : NULL;
  sub->optional_capacity = 8;
  sub->optional_args = (CmdArgerDesc *)calloc(sub->optional_capacity, sizeof(CmdArgerDesc));
  sub->required_capacity = 4;
  sub->required_args = (CmdArgerDesc *)calloc(sub->required_capacity, sizeof(CmdArgerDesc));
  return sub;
}

static void ensure_sub_optional_capacity(turbo_cmd_subcommand_t *sub) {
  if (sub->optional_count >= sub->optional_capacity) {
    sub->optional_capacity *= 2;
    sub->optional_args = (CmdArgerDesc *)realloc(sub->optional_args,
                         sub->optional_capacity * sizeof(CmdArgerDesc));
  }
}

void turbo_cmd_sub_add_flag(turbo_cmd_subcommand_t *sub, bool *out, const char *name,
                            const char *short_name, const char *desc) {
  if (!sub) return;
  ensure_sub_optional_capacity(sub);
  sub->optional_args[sub->optional_count++] = 
      cmd_arger_desc_flag_sh((CmdArgerBool*)out, (char*)name, (char*)short_name, (char*)desc);
}

void turbo_cmd_sub_add_string(turbo_cmd_subcommand_t *sub, char **out, const char *name,
                              const char *short_name, const char *desc) {
  if (!sub) return;
  ensure_sub_optional_capacity(sub);
  sub->optional_args[sub->optional_count++] = 
      cmd_arger_desc_string_sh(out, (char*)name, (char*)short_name, (char*)desc);
}

void turbo_cmd_sub_add_integer(turbo_cmd_subcommand_t *sub, int64_t *out,
                               const char *name, const char *short_name, const char *desc) {
  if (!sub) return;
  ensure_sub_optional_capacity(sub);
  sub->optional_args[sub->optional_count++] = 
      cmd_arger_desc_integer_sh(out, (char*)name, (char*)short_name, (char*)desc);
}

void turbo_cmd_sub_add_required_string(turbo_cmd_subcommand_t *sub, char **out,
                                       const char *name, const char *desc) {
  if (!sub) return;
  if (sub->required_count >= sub->required_capacity) {
    sub->required_capacity *= 2;
    sub->required_args = (CmdArgerDesc *)realloc(sub->required_args,
                         sub->required_capacity * sizeof(CmdArgerDesc));
  }
  sub->required_args[sub->required_count++] = 
      cmd_arger_desc_string(out, (char*)name, (char*)desc);
}

/* TOON */
int turbo_parse_toon(const uint8_t *data, size_t len, void *out) {
  if (!data || !out)
    return -1;
  toonObject *doc = TOONc_parseStringLen((const char *)data, len);
  if (!doc)
    return -1;
  *(toonObject **)out = doc;
  return 0;
}

void turbo_free_toon(void *out) {
  if (!out)
    return;
  void *ptr = *(void **)out;
  if (ptr)
    TOONc_free((toonObject *)ptr);
  *(void **)out = NULL;
}

turbo_toon_type_t turbo_toon_type(const turbo_toon_node_t *node) {
  if (!node) return TURBO_TOON_NULL;
  return (turbo_toon_type_t)node->kvtype;
}

bool turbo_toon_is_null(const turbo_toon_node_t *node) {
  return node && node->kvtype == KV_NULL;
}

bool turbo_toon_bool(const turbo_toon_node_t *node) {
  return node && node->kvtype == KV_BOOL ? (bool)node->boolean : false;
}

double turbo_toon_number(const turbo_toon_node_t *node) {
  if (!node) return 0.0;
  if (node->kvtype == KV_DOUBLE) return node->d;
  if (node->kvtype == KV_INT) return (double)node->i;
  return 0.0;
}

int turbo_toon_int(const turbo_toon_node_t *node) {
  if (!node) return 0;
  if (node->kvtype == KV_INT) return node->i;
  if (node->kvtype == KV_DOUBLE) return (int)node->d;
  return 0;
}

const char *turbo_toon_string(const turbo_toon_node_t *node) {
  return node && node->kvtype == KV_STRING ? node->str.ptr : NULL;
}

size_t turbo_toon_string_len(const turbo_toon_node_t *node) {
  return node && node->kvtype == KV_STRING ? node->str.len : 0;
}

turbo_toon_node_t *turbo_toon_get(turbo_toon_node_t *root, const char *path) {
  return TOONc_get(root, path);
}

size_t turbo_toon_array_size(const turbo_toon_node_t *arr) {
  return TOONc_getArrayLength((toonObject *)arr);
}

turbo_toon_node_t *turbo_toon_array_get(const turbo_toon_node_t *arr, size_t index) {
  return TOONc_getArrayItem((toonObject *)arr, index);
}

char *turbo_toon_serialize(const turbo_toon_node_t *node, size_t *out_len) {
  return TOONc_serialize((const toonObject *)node, out_len);
}

void turbo_toon_serialize_free(char *str) {
  TOONc_serializeFree(str);
}

char *turbo_toon_serialize_json(const turbo_toon_node_t *node, size_t *out_len) {
  return TOONc_toJSONString((const toonObject *)node, out_len);
}

void turbo_toon_serialize_json_free(char *str) {
  TOONc_serializeFree(str);
}

turbo_toon_node_t *turbo_toon_from_json(const char *json, size_t len) {
  return TOONc_fromJSONString(json, len);
}

void turbo_cmd_parse(turbo_cmd_parser_t *parser, int argc, char **argv, bool colors) {
  if (!parser) return;
  
  char app_ver[256];
  if (parser->app_name && parser->version) {
    fmt(app_ver, sizeof(app_ver), "{} {}", parser->app_name, parser->version);
  } else if (parser->app_name) {
    fmt(app_ver, sizeof(app_ver), "{}", parser->app_name);
  } else {
    fmt(app_ver, sizeof(app_ver), "Application");
  }
  
  cmd_arger_parse(parser->optional_args, parser->optional_count,
                  parser->required_args, parser->required_count,
                  argc, argv, app_ver, (CmdArgerBool)colors);
}

int turbo_cmd_parse_subcommand(turbo_cmd_parser_t *parser, int argc, char **argv, bool colors) {
  if (!parser || parser->subcommand_count == 0) return -1;
  
  char app_ver[256];
  if (parser->app_name && parser->version) {
    fmt(app_ver, sizeof(app_ver), "{} {}", parser->app_name, parser->version);
  } else if (parser->app_name) {
    fmt(app_ver, sizeof(app_ver), "{}", parser->app_name);
  } else {
    fmt(app_ver, sizeof(app_ver), "Application");
  }

  CmdArgerSubCommand *subs = (CmdArgerSubCommand *)calloc(parser->subcommand_count, sizeof(CmdArgerSubCommand));
  for (uint32_t i = 0; i < parser->subcommand_count; i++) {
    turbo_cmd_subcommand_t *src = &parser->subcommands[i];
    subs[i].name = src->name;
    subs[i].info = src->info;
    subs[i].optional_args = src->optional_args;
    subs[i].optional_args_count = src->optional_count;
    subs[i].required_args = src->required_args;
    subs[i].required_args_count = src->required_count;
  }

  int selected = -1;
  cmd_arger_parse_subcommand(parser->optional_args, parser->optional_count,
                             subs, parser->subcommand_count,
                             &selected, argc, argv, app_ver, (CmdArgerBool)colors);
  free(subs);
  return selected;
}

void turbo_cmd_show_help(turbo_cmd_parser_t *parser, bool colors) {
  if (!parser) return;
  
  char app_ver[256];
  if (parser->app_name && parser->version) {
    fmt(app_ver, sizeof(app_ver), "{} {}", parser->app_name, parser->version);
  } else if (parser->app_name) {
    fmt(app_ver, sizeof(app_ver), "{}", parser->app_name);
  } else {
    fmt(app_ver, sizeof(app_ver), "Application");
  }
  
  cmd_arger_show_help_and_exit(parser->optional_args, parser->optional_count,
                               parser->required_args, parser->required_count,
                               NULL, app_ver, (CmdArgerBool)colors);
}

/* DotEnv */
int turbo_dotenv_load(const char *path, bool overwrite) {
  return dotenv_load(path, overwrite);
}

int turbo_dotenv_load_default(bool overwrite) {
  return dotenv_load_default(overwrite);
}

/* TOML */
int turbo_parse_toml(const uint8_t *data, size_t len, void *out) {
  if (!data || !out) return -1;
  char errbuf[200];
  char *temp = (char *)malloc(len + 1);
  if (!temp) return -1;
  memcpy(temp, data, len);
  temp[len] = '\0';

  toml_table_t *table = toml_parse(temp, errbuf, sizeof(errbuf));
  free(temp);

  if (!table) return -1;
  *(toml_table_t **)out = table;
  return 0;
}

void turbo_free_toml(void *out) {
  if (!out) return;
  void *ptr = *(void **)out;
  if (ptr) toml_free((toml_table_t *)ptr);
  *(void **)out = NULL;
}

int turbo_toml_len(const turbo_toml_t *table) {
  return toml_table_len((const toml_table_t *)table);
}

const char *turbo_toml_key(const turbo_toml_t *table, int index, int *keylen) {
  return toml_table_key((const toml_table_t *)table, index, keylen);
}

turbo_toml_value_t turbo_toml_string(const turbo_toml_t *table, const char *key) {
  toml_value_t v = toml_table_string((const toml_table_t *)table, key);
  turbo_toml_value_t ret;
  memcpy(&ret, &v, sizeof(v));
  return ret;
}

turbo_toml_value_t turbo_toml_bool(const turbo_toml_t *table, const char *key) {
  toml_value_t v = toml_table_bool((const toml_table_t *)table, key);
  turbo_toml_value_t ret;
  memcpy(&ret, &v, sizeof(v));
  return ret;
}

turbo_toml_value_t turbo_toml_int(const turbo_toml_t *table, const char *key) {
  toml_value_t v = toml_table_int((const toml_table_t *)table, key);
  turbo_toml_value_t ret;
  memcpy(&ret, &v, sizeof(v));
  return ret;
}

turbo_toml_value_t turbo_toml_double(const turbo_toml_t *table, const char *key) {
  toml_value_t v = toml_table_double((const toml_table_t *)table, key);
  turbo_toml_value_t ret;
  memcpy(&ret, &v, sizeof(v));
  return ret;
}

turbo_toml_value_t turbo_toml_timestamp(const turbo_toml_t *table, const char *key) {
  toml_value_t v = toml_table_timestamp((const toml_table_t *)table, key);
  turbo_toml_value_t ret;
  memcpy(&ret, &v, sizeof(v));
  return ret;
}

turbo_toml_array_t *turbo_toml_array(const turbo_toml_t *table, const char *key) {
  return (turbo_toml_array_t *)toml_table_array((const toml_table_t *)table, key);
}

turbo_toml_t *turbo_toml_table(const turbo_toml_t *table, const char *key) {
  return (turbo_toml_t *)toml_table_table((const toml_table_t *)table, key);
}

int turbo_toml_array_len(const turbo_toml_array_t *array) {
  return toml_array_len((const toml_array_t *)array);
}

turbo_toml_value_t turbo_toml_array_string(const turbo_toml_array_t *array, int idx) {
  toml_value_t v = toml_array_string((const toml_array_t *)array, idx);
  turbo_toml_value_t ret;
  memcpy(&ret, &v, sizeof(v));
  return ret;
}

turbo_toml_value_t turbo_toml_array_bool(const turbo_toml_array_t *array, int idx) {
  toml_value_t v = toml_array_bool((const toml_array_t *)array, idx);
  turbo_toml_value_t ret;
  memcpy(&ret, &v, sizeof(v));
  return ret;
}

turbo_toml_value_t turbo_toml_array_int(const turbo_toml_array_t *array, int idx) {
  toml_value_t v = toml_array_int((const toml_array_t *)array, idx);
  turbo_toml_value_t ret;
  memcpy(&ret, &v, sizeof(v));
  return ret;
}

turbo_toml_value_t turbo_toml_array_double(const turbo_toml_array_t *array, int idx) {
  toml_value_t v = toml_array_double((const toml_array_t *)array, idx);
  turbo_toml_value_t ret;
  memcpy(&ret, &v, sizeof(v));
  return ret;
}

turbo_toml_value_t turbo_toml_array_timestamp(const turbo_toml_array_t *array, int idx) {
  toml_value_t v = toml_array_timestamp((const toml_array_t *)array, idx);
  turbo_toml_value_t ret;
  memcpy(&ret, &v, sizeof(v));
  return ret;
}

turbo_toml_array_t *turbo_toml_array_array(const turbo_toml_array_t *array, int idx) {
  return (turbo_toml_array_t *)toml_array_array((const toml_array_t *)array, idx);
}

turbo_toml_t *turbo_toml_array_table(const turbo_toml_array_t *array, int idx) {
  return (turbo_toml_t *)toml_array_table((const toml_array_t *)array, idx);
}

/* Datetime */
int turbo_parse_datetime(const char *str, size_t len, turbo_datetime_t *out) {
  return datetime_parse(str, len, (datetime_t *)out);
}

time_t turbo_datetime_to_time(const turbo_datetime_t *dt) {
  return datetime_to_time((const datetime_t *)dt);
}

int turbo_datetime_format_rfc822(time_t t, char *buf, size_t buf_len) {
  return datetime_format_rfc822(t, buf, buf_len);
}
