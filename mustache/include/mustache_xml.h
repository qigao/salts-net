/**
 * @file mustache_xml.h
 * @brief XML data provider for Mustache4C templating engine using cxml
 */

#ifndef MUSTACHE_XML_H
#define MUSTACHE_XML_H

#include "platform.h"
#include "mustache.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _cx_doc_node cxml_root_node;
typedef struct _cx_elem_node cxml_elem_node;

/**
 * XML-based data provider for mustache templates
 */
typedef struct MUSTACHE_XML_PROVIDER {
  MUSTACHE_DATAPROVIDER base;
  void *root_node; // Can be cxml_root_node or cxml_elem_node
  MUSTACHE_TEMPLATE *(*template_loader)(const char *name, size_t size, void *user_data);
  void *user_data;
  
  // Internal cache for list nodes to handle multiple elements with same name
  void **allocated_lists;
  size_t list_count;
  size_t list_capacity;
} MUSTACHE_XML_PROVIDER;

/**
 * Initialize an XML data provider
 * @param provider The provider to initialize
 * @param xml_node Root XML node (root node or element)
 * @param template_loader Optional template loader for partials (can be NULL)
 * @param user_data User data passed to template loader
 * @return 0 on success, -1 on error
 */
CXX_C_API int mustache_xml_provider_init(MUSTACHE_XML_PROVIDER *provider, void *xml_node,
                                          MUSTACHE_TEMPLATE *(*template_loader)(const char *,
                                                                                size_t, void *),
                                          void *user_data);

/**
 * Free XML data provider resources (does not free XML nodes themselves)
 */
CXX_C_API void mustache_xml_provider_free(MUSTACHE_XML_PROVIDER *provider);

/**
 * Render a mustache template with XML data
 * @param template Compiled mustache template
 * @param xml_node XML node to use for rendering
 * @param renderer Output renderer
 * @param renderer_data Data for renderer callbacks
 * @param template_loader Optional template loader for partials
 * @param user_data User data for template loader
 * @return 0 on success, -1 on error
 */
CXX_C_API int mustache_render_xml(const MUSTACHE_TEMPLATE *templ, void *xml_node,
                                   const MUSTACHE_RENDERER *renderer, void *renderer_data,
                                   MUSTACHE_TEMPLATE *(*template_loader)(const char *, size_t,
                                                                         void *),
                                   void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* MUSTACHE_XML_H */
