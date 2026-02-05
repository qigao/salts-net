/**
 * @file mustache_json.h
 * @brief JSON data provider for Mustache4C templating engine
 */

#ifndef MUSTACHE_JSON_H
#define MUSTACHE_JSON_H

#include "platform.h"
#include "mustache.h"


#ifdef __cplusplus
extern "C" {
#endif
typedef struct json_value_s json_value_t;
/**
 * JSON-based data provider for mustache templates
 */
typedef struct MUSTACHE_JSON_PROVIDER {
  MUSTACHE_DATAPROVIDER base;
  json_value_t *root_data;
  MUSTACHE_TEMPLATE *(*template_loader)(const char *name, size_t size, void *user_data);
  void *user_data;
} MUSTACHE_JSON_PROVIDER;

/**
 * Initialize a JSON data provider
 * @param provider The provider to initialize
 * @param json_data Root JSON data
 * @param template_loader Optional template loader for partials (can be NULL)
 * @param user_data User data passed to template loader
 * @return 0 on success, -1 on error
 */
CXX_C_API int mustache_json_provider_init(MUSTACHE_JSON_PROVIDER *provider, json_value_t *json_data,
                                          MUSTACHE_TEMPLATE *(*template_loader)(const char *,
                                                                                size_t, void *),
                                          void *user_data);

/**
 * Render a mustache template with JSON data
 * @param template Compiled mustache template
 * @param json_data JSON data to use for rendering
 * @param renderer Output renderer
 * @param renderer_data Data for renderer callbacks
 * @param template_loader Optional template loader for partials
 * @param user_data User data for template loader
 * @return 0 on success, -1 on error
 */
CXX_C_API int mustache_render_json(const MUSTACHE_TEMPLATE *template, json_value_t *json_data,
                                   const MUSTACHE_RENDERER *renderer, void *renderer_data,
                                   MUSTACHE_TEMPLATE *(*template_loader)(const char *, size_t,
                                                                         void *),
                                   void *user_data);

/**
 * Simple string renderer that appends to a buffer
 */
typedef struct MUSTACHE_STRING_RENDERER {
  MUSTACHE_RENDERER base;
  char *buffer;
  size_t size;
  size_t capacity;
} MUSTACHE_STRING_RENDERER;

/**
 * Initialize a string renderer
 * @param renderer The renderer to initialize
 * @return 0 on success, -1 on error
 */
CXX_C_API int mustache_string_renderer_init(MUSTACHE_STRING_RENDERER *renderer);

/**
 * Get the rendered string (caller must free)
 * @param renderer The string renderer
 * @return Allocated string or NULL on error
 */
CXX_C_API char *mustache_string_renderer_get(MUSTACHE_STRING_RENDERER *renderer);

/**
 * Free string renderer resources
 * @param renderer The renderer to free
 */
CXX_C_API void mustache_string_renderer_free(MUSTACHE_STRING_RENDERER *renderer);

#ifdef __cplusplus
}
#endif

#endif /* MUSTACHE_JSON_H */