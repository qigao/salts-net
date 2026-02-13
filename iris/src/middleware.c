#include "middleware.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena_buffer.h" /* Phase IRIS-1: Changed from arena.h */
#include "iris.h"
#include "iris_app.h"
#include "route_trie.h"
#include "tlog.h"

/* Legacy global middleware - for backward compatibility.
 * New code should use iris_app_hook() instead. */
static MiddlewareHandler global_middleware_inline[INITIAL_MW_CAPACITY];
MiddlewareHandler* global_middleware = global_middleware_inline;
int global_middleware_count = 0;
int global_middleware_capacity = INITIAL_MW_CAPACITY;
static int global_middleware_legacy_owned = 0;

/* Add middleware to global chain (legacy API) */
void hook(MiddlewareHandler middleware_handler)
{
  /* Use default app if available, otherwise fall back to legacy globals */
  iris_app_t *app = iris_app_default();
  if (app) {
    iris_app_hook(app, middleware_handler);
    /* Also update legacy globals for backward compatibility */
    global_middleware = app->global_middleware;
    global_middleware_count = app->global_middleware_count;
    global_middleware_capacity = app->global_middleware_capacity;
    global_middleware_legacy_owned = 0;
  } else {
    /* Fallback to legacy behavior */
    if (global_middleware_count >= global_middleware_capacity) {
      int new_cap = global_middleware_capacity ? global_middleware_capacity * 2 : INITIAL_MW_CAPACITY;
      MiddlewareHandler* tmp = NULL;
      if (global_middleware == global_middleware_inline) {
        tmp = malloc(new_cap * sizeof *tmp);
        if (tmp) {
          memcpy(tmp, global_middleware_inline,
                 sizeof(MiddlewareHandler) * global_middleware_count);
        }
      } else {
        tmp = realloc(global_middleware, new_cap * sizeof *tmp);
      }
      if (!tmp) {
        TLOG_ERROR("Failed to reallocate global middleware array");
        return;
      }
      global_middleware = tmp;
      global_middleware_capacity = new_cap;
    }
    global_middleware[global_middleware_count++] = middleware_handler;
    global_middleware_legacy_owned = 1;
  }
}

// Helper function for middleware chain execution
int next(Chain* chain, Req* req, Res* res)
{
  if (!chain) {
    TLOG_ERROR("Error: NULL middleware chain");
    return -1;
  }

  if (!req || !res) {
    TLOG_ERROR("Error: NULL request or response");
    return -1;
  }

  // Check if we have more middleware to execute
  if (chain->current < chain->count) {
    // Execute the next middleware in the chain
    MiddlewareHandler next_middleware = chain->handlers[chain->current++];
    if (next_middleware) {
      return next_middleware(req, res, chain);
    } else {
      TLOG_WARN("Warning: NULL middleware handler at position {:d}", chain->current - 1);
      // Skip this middleware and try the next one
      return next(chain, req, res);
    }
  } else {
    // All middleware executed, call the route handler
    if (chain->route_handler) {
      chain->route_handler(req, res);
      return 1;  // Successfully executed the route handler
    }
    return 0;  // No route handler
  }
}

// Clean up middleware info resources
void free_middleware_info(MiddlewareInfo* info)
{
  if (info) {
    if (!info->arena_owned && info->middleware && info->middleware != info->middleware_inline) {
      free(info->middleware);
      info->middleware = NULL;
    }
    if (!info->arena_owned) {
      free(info);
    }
  }
}

// Function that runs the middleware chain
void execute_middleware_chain(Req* req, Res* res, MiddlewareInfo* middleware_info)
{
  if (!req || !res || !middleware_info) {
    TLOG_ERROR("ERROR: NULL request, response, or middleware info");
    return;
  }

  int total_middleware_count = global_middleware_count + middleware_info->middleware_count;

  // If there is no middleware, call the handler directly
  if (total_middleware_count == 0) {
    if (middleware_info->handler) {
      middleware_info->handler(req, res);
    }
    return;
  }

  // Allocate memory for combined middleware handlers
  MiddlewareHandler* combined_handlers =
      turbo_arena_alloc(req->arena, sizeof(MiddlewareHandler) * total_middleware_count);
  if (!combined_handlers) {
    TLOG_ERROR("Arena allocation failed for middleware handlers");
    if (middleware_info->handler) {
      middleware_info->handler(req, res);
    }
    return;
  }

  // Copy global middleware handlers first
  memcpy(
      combined_handlers, global_middleware, sizeof(MiddlewareHandler) * global_middleware_count);

  // Copy route-specific middleware handlers
  if (middleware_info->middleware_count > 0 && middleware_info->middleware) {
    memcpy(combined_handlers + global_middleware_count,
           middleware_info->middleware,
           sizeof(MiddlewareHandler) * middleware_info->middleware_count);
  }

  // Create middleware chain context (allocated in request arena)
  Chain* chain = turbo_arena_alloc(req->arena, sizeof(Chain));
  if (!chain) {
    TLOG_ERROR("Arena allocation failed for middleware chain");
    if (middleware_info->handler) {
      middleware_info->handler(req, res);
    }
    return;
  }

  chain->handlers = combined_handlers;
  chain->count = total_middleware_count;
  chain->current = 0;
  chain->route_handler = middleware_info->handler;

  // Start middleware chain execution
  int result = next(chain, req, res);

  // Error handling
  if (result == -1) {
    TLOG_ERROR("ERROR: Middleware chain failed, calling handler directly as fallback");
    if (middleware_info->handler) {
      middleware_info->handler(req, res);
    }
  }
}

// Helper function to register route with middleware (uses malloc for long-lived data)
void register_route(const char* method,
                    const char* path,
                    MiddlewareArray middleware,
                    RequestHandler handler)
{
  if (!handler) {
    TLOG_ERROR("Error: No handler provided for route: {:s} {:s}", method, path);
    return;
  }

  if (!method || !path) {
    TLOG_ERROR("Error: NULL method or path provided");
    return;
  }

  if (!global_route_trie) {
    TLOG_ERROR("Error: Route trie not initialized");
    return;
  }

  MiddlewareInfo* middleware_info = NULL;
  if (global_route_trie) {
    middleware_info =
        (MiddlewareInfo *)turbo_arena_alloc(&global_route_trie->param_arena, sizeof(MiddlewareInfo));
    if (middleware_info) {
      memset(middleware_info, 0, sizeof(MiddlewareInfo));
      middleware_info->arena_owned = 1;
    }
  }
  if (!middleware_info) {
    middleware_info = calloc(1, sizeof(MiddlewareInfo));
  }
  if (!middleware_info) {
    TLOG_ERROR("Memory allocation failed for middleware info");
    return;
  }

  middleware_info->handler = handler;

  if (middleware.count > 0 && middleware.handlers) {
    if (middleware_info->arena_owned) {
      middleware_info->middleware = (MiddlewareHandler *)turbo_arena_alloc(
          &global_route_trie->param_arena, sizeof(MiddlewareHandler) * middleware.count);
      if (!middleware_info->middleware) {
        TLOG_ERROR("Memory allocation failed for middleware handlers");
        return;
      }
    } else if (middleware.count <= IRIS_INLINE_ROUTE_MW_CAPACITY) {
      middleware_info->middleware = middleware_info->middleware_inline;
    } else {
      middleware_info->middleware = malloc(sizeof(MiddlewareHandler) * middleware.count);
      if (!middleware_info->middleware) {
        TLOG_ERROR("Memory allocation failed for middleware handlers");
        free(middleware_info);
        return;
      }
    }
    memcpy(middleware_info->middleware,
           middleware.handlers,
           sizeof(MiddlewareHandler) * middleware.count);
    middleware_info->middleware_count = (int)middleware.count;
  }

  int result = route_trie_add(global_route_trie, method, path, handler, middleware_info);
  if (result != 0) {
    TLOG_ERROR("Failed to add route to trie: {:s} {:s}", method, path);
    free_middleware_info(middleware_info);
    return;
  }
}

void reset_middleware(void)
{
  /* If global_middleware points to app-managed memory, just clear the pointer.
   * The memory is freed when iris_app_destroy() is called.
   * If it's legacy-owned memory, we need to free it. */

  /* Check if there's a default app and if our pointer is app-managed */
  iris_app_t *app = NULL;

  /* Only check existing app, don't create new one */
  extern iris_app_t *iris_app_default(void);
  /* We need a way to check without creating - for now, just clear the pointers
   * since iris_app_reset_default handles the actual freeing */

  /* Simple approach: if global_middleware is non-null and matches what an app
   * would have, assume app owns it. Otherwise free it.
   *
   * Actually, the safest approach after iris_app refactor:
   * - If iris_app_reset_default was called, the memory is already freed
   * - Just clear our pointers without freeing */

  if (global_middleware_legacy_owned &&
      global_middleware && global_middleware != global_middleware_inline) {
    free(global_middleware);
  }
  global_middleware = global_middleware_inline;
  global_middleware_count = 0;
  global_middleware_capacity = INITIAL_MW_CAPACITY;
  global_middleware_legacy_owned = 0;
}
