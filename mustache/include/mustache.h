#ifndef MUSTACHE4C_H
#define MUSTACHE4C_H

#include <stdlib.h>
#include "platform.h"
#include "turbo_str_view.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MUSTACHE_TEMPLATE MUSTACHE_TEMPLATE;
typedef struct mem_pool_s mem_pool_t;
typedef struct mem_buffer_s mem_buffer_t;

#define MUSTACHE_ERR_SUCCESS (0)
#define MUSTACHE_ERR_DANGLINGTAGOPENER (1)
#define MUSTACHE_ERR_DANGLINGTAGCLOSER (2)
#define MUSTACHE_ERR_INCOMPATIBLETAGCLOSER (3)
#define MUSTACHE_ERR_NOTAGNAME (4)
#define MUSTACHE_ERR_INVALIDTAGNAME (5)
#define MUSTACHE_ERR_DANGLINGSECTIONOPENER (6)
#define MUSTACHE_ERR_DANGLINGSECTIONCLOSER (7)
#define MUSTACHE_ERR_SECTIONNAMEMISMATCH (8)
#define MUSTACHE_ERR_SECTIONOPENERHERE (9)
#define MUSTACHE_ERR_INVALIDDELIMITERS (10)

typedef struct MUSTACHE_PARSER {
  void (*parse_error)(int /*err_code*/, const char * /*msg*/, unsigned /*line*/,
                      unsigned /*column*/, void * /*parser_data*/);
} MUSTACHE_PARSER;

/**
 * An interface the application has to implement, in order to output the result
 * of template processing.
 */
typedef struct MUSTACHE_RENDERER {
  /**
   * Called to output the given text as it is.
   *
   * Non-zero return value aborts mustache_process().
   */
  int (*out_verbatim)(const char * /*output*/, size_t /*size*/, void * /*renderer_data*/);

  /**
   * Called to output the given text. Implementation has to escape it
   * appropriately with respect to the output format. E.g. for HTML output,
   * "<" should be translated to "&lt;" etc.
   *
   * Non-zero return value aborts mustache_process().
   *
   * If no escaping is desired, it can be pointer to the same function
   * as out_verbatim.
   */
  int (*out_escaped)(const char * /*output*/, size_t /*size*/, void * /*renderer_data*/);
} MUSTACHE_RENDERER;

/**
 * An interface the application has to implement, in order to feed
 * mustache_process() with data the template asks for.
 *
 * Tree hierarchy, immutable during the mustache_process() call, is assumed.
 * Each node of the hierarchy has to be uniquely identified by some pointer.
 *
 * The mustache_process() never dereferences any of the pointers. It only
 * uses them to refer to that node when calling any data provider callback.
 */
typedef struct MUSTACHE_DATAPROVIDER {
  /**
   * Called to output contents of the given node. One of the MUSTACHE_PARSER
   * output functions is provided, depending on the type of the mustache tag
   * (`{{...}}` versus `{{{...}}}` ). Implementation of dump() may call that
   * function arbitrarily.
   *
   * In many applications, it is not desirable/expected to be able dumping
   * specific nodes (e.g. if the node is list or array forming the data
   * tree hierarchy). In such cases, the implementation is allowed to just
   * return zero without calling the provided callback at all, output some
   * dummy string (e.g. "<<object>>"), or return non-zero value as an error
   * sign, depending what makes better sense for the application.
   *
   * Implementation of dump() must propagate renderer_data into the
   * callback as its last argument.
   *
   * Non-zero return value aborts mustache_process(). Typically, the
   * implementations should do so if any call of out_fn callback fails.
   */
  int (*dump)(void * /*node*/, int (* /*out_fn*/)(const char *, size_t, void *),
              void * /*renderer_data*/, void * /*provider_data*/);

  /**
   * Called once at the start of mustache_process(). It sets the initial
   * lookup context. */
  void *(*get_root)(void * /*provider_data*/);

  /**
   * Called to get named item of the current node, or NULL if there is no item.
   *
   * If the node is not of appropriate type (e.g. if it is an array of
   * values), NULL has to be returned.
   */
  void *(*get_child_by_name)(void * /*node*/, const char * /*name*/, size_t /*size*/,
                             void * /*provider_data*/);

  /**
   * Called to get an indexed item of the current node, or NULL if there is
   * no such item.
   *
   * The main use is for iterating over arrays.
   *
   * However note that accordingly to the mustache specification, single
   * values (except FALSE, NULL, or empty lists) have to be iterable too.
   * For such simple values, the callback should return the node itself
   * for index 0, and NULL for any other index.
   */
  void *(*get_child_by_index)(void * /*node*/, unsigned /*index*/, void * /*provider_data*/);

  /**
   * Called to get a partial template when mustache_process() handles
   * a partial tag `{{>name}}`.
   *
   * Implementation should perform lookup for the template (compile it, if
   * it is not), and return the template handle.
   *
   * If the lookup fails, the implementation reports it by returning NULL.
   */
  MUSTACHE_TEMPLATE *(*get_partial)(const char * /*name*/, size_t /*size*/,
                                    void * /*provider_data*/);

  /* Optional lambda support. If is_lambda returns non-zero, call_lambda is used. 
   * call_lambda should allocate *out_text with malloc; caller will free().
   * For interpolation lambdas, text is empty. For section lambdas, text is the raw section content.
   */
  int (*is_lambda)(void * /*node*/, void * /*provider_data*/);
  int (*call_lambda)(void * /*node*/, const char * /*text*/, size_t /*text_len*/,
                     char ** /*out_text*/, size_t * /*out_len*/, void * /*provider_data*/);
} MUSTACHE_DATAPROVIDER;

/**
 * Compile template text into a form suitable for mustache_process().
 *
 * If application processes multiple input data with a single template, it is
 * recommended to cache and reuse the compiled template as much as possible,
 * as the compiling may be relatively time-consuming operation.
 *
 * @param templ_data Text of the template.
 * @param templ_size Length of the template text.
 * @param parser Pointer to structure with parser callbacks. May be @c NULL.
 * @param parser_data Pointer just propagated into the parser callbacks.
 * @param flags Unused, use zero.
 * @return Pointer to the compiled template, or @c NULL on an error.
 */
CXX_C_API MUSTACHE_TEMPLATE *mustache_compile(const char *templ_data, size_t templ_size,
                                    const MUSTACHE_PARSER *parser, void *parser_data,
                                    unsigned flags);
/**
 * Compile template text from a string view.
 *
 * @param templ The template view (does not need to be null-terminated).
 * @param parser Pointer to structure with parser callbacks. May be @c NULL.
 * @param parser_data Pointer just propagated into the parser callbacks.
 * @param flags Unused, use zero.
 * @return Pointer to the compiled template, or @c NULL on an error.
 */
CXX_C_API MUSTACHE_TEMPLATE *mustache_compile_v(tstr_v templ, const MUSTACHE_PARSER *parser,
                                    void *parser_data, unsigned flags);

/**
 * Release the template compiled with @c mustache_compile().
 *
 * @param t The template.
 */
CXX_C_API void mustache_release(MUSTACHE_TEMPLATE *t);

/**
 * Process the template.
 *
 * The function outputs (via MUSTACHE_RENDERER::out_verbatim()) most of the
 * text of the template. Whenever it reaches a mustache tag, it calls
 * appropriate callback of MUSTACHE_DATAPROVIDER to change lookup context
 * or a callback of MUSTACHE_RENDERER to output contents of the current
 * context.
 *
 * @param t The template.
 * @param renderer Pointer to structure with output callbacks.
 * @param render_data Pointer just propagated to the output callbacks.
 * @param provider Pointer to structure with data-providing callbacks.
 * @param provider_dara Pointer just propagated to the data-providing callbacks.
 * @return Zero on success, non-zero on failure.
 *
 * Note this operation can fail only if any callback returns an error
 * and aborts the operation.
 */
CXX_C_API int mustache_process(const MUSTACHE_TEMPLATE *t, const MUSTACHE_RENDERER *renderer,
                     void *renderer_data, const MUSTACHE_DATAPROVIDER *provider,
                     void *provider_data);

/**
 * Simple string renderer that appends to a tstr_t internally.
 */
typedef struct MUSTACHE_STRING_RENDERER {
  MUSTACHE_RENDERER base;
  char *buffer;  /* Internal tstr_t - do not access directly */
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

/**
 * Arena-backed string renderer
 */
typedef struct MUSTACHE_STRING_RENDERER_ARENA {
  MUSTACHE_RENDERER base;
  mem_buffer_t *buffer;
} MUSTACHE_STRING_RENDERER_ARENA;

/**
 * Initialize an arena-backed string renderer
 * @param renderer The renderer to initialize
 * @param arena Arena to allocate from
 * @param min_capacity Minimum buffer size
 * @return 0 on success, -1 on error
 */
CXX_C_API int mustache_string_renderer_init_arena(MUSTACHE_STRING_RENDERER_ARENA *renderer,
                                                  mem_pool_t *arena,
                                                  size_t min_capacity);

/**
 * Get the rendered string (arena-owned)
 * @param renderer The arena string renderer
 * @return Pointer to arena buffer data or NULL on error
 */
CXX_C_API char *mustache_string_renderer_get_arena(MUSTACHE_STRING_RENDERER_ARENA *renderer);

/**
 * Free arena string renderer resources
 * @param renderer The renderer to free
 */
CXX_C_API void mustache_string_renderer_free_arena(MUSTACHE_STRING_RENDERER_ARENA *renderer);

#ifdef __cplusplus
}
#endif

#endif /* MUSTACHE4C_H */
