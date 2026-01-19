/**
 * iris_app.c - Iris application instance management
 *
 * Provides multi-instance support for iris web applications.
 */

#include "iris_app.h"
#include "route_trie.h"
#include "middleware.h"
#include "cors.h"

#include <stdlib.h>
#include <string.h>
#include "tlog.h"

/* Default global app instance for backward compatibility */
static iris_app_t *g_default_app = NULL;

iris_app_t *iris_app_create(void) {
    iris_app_t *app = calloc(1, sizeof(iris_app_t));
    if (!app) {
        TLOG_ERROR("Failed to allocate iris_app_t");
        return NULL;
    }

    /* Initialize route trie */
    app->route_trie = route_trie_create();
    if (!app->route_trie) {
        TLOG_ERROR("Failed to create route trie");
        free(app);
        return NULL;
    }

    /* Initialize middleware with default capacity */
    app->global_middleware = NULL;
    app->global_middleware_count = 0;
    app->global_middleware_capacity = 0;

    /* CORS and RPC start as NULL */
    app->cors_opts = NULL;
    app->rpc_context = NULL;
    app->shutdown_hook = NULL;

    return app;
}

void iris_app_destroy(iris_app_t *app) {
    if (!app)
        return;

    /* Free route trie */
    if (app->route_trie) {
        route_trie_free(app->route_trie);
        app->route_trie = NULL;
    }

    /* Free global middleware array */
    if (app->global_middleware) {
        free(app->global_middleware);
        app->global_middleware = NULL;
    }

    /* Free CORS options */
    if (app->cors_opts) {
        free(app->cors_opts->origin);
        free(app->cors_opts->methods);
        free(app->cors_opts->headers);
        free(app->cors_opts->credentials);
        free(app->cors_opts->max_age);
        free(app->cors_opts);
        app->cors_opts = NULL;
    }

    /* Clear default app reference if this is it */
    if (g_default_app == app) {
        g_default_app = NULL;
    }

    free(app);
}

iris_app_t *iris_app_default(void) {
    if (!g_default_app) {
        g_default_app = iris_app_create();
    }
    return g_default_app;
}

iris_app_t *iris_app_get_default_if_exists(void) {
    return g_default_app;  /* Returns NULL if not created yet */
}

void iris_app_reset_default(void) {
    if (g_default_app) {
        iris_app_destroy(g_default_app);
        g_default_app = NULL;
    }
}

/* ============================================================================
 * Route registration
 * ============================================================================ */

void iris_app_route(iris_app_t *app, const char *method, const char *path,
                    MiddlewareArray middleware, RequestHandler handler) {
    if (!app || !method || !path || !handler) {
        TLOG_ERROR("iris_app_route: invalid parameters");
        return;
    }

    if (!app->route_trie) {
        TLOG_ERROR("iris_app_route: route trie not initialized");
        return;
    }

    /* Create middleware info */
    MiddlewareInfo *middleware_info = calloc(1, sizeof(MiddlewareInfo));
    if (!middleware_info) {
        TLOG_ERROR("iris_app_route: memory allocation failed");
        return;
    }

    middleware_info->handler = handler;

    if (middleware.count > 0 && middleware.handlers) {
        middleware_info->middleware = malloc(sizeof(MiddlewareHandler) * middleware.count);
        if (!middleware_info->middleware) {
            TLOG_ERROR("iris_app_route: middleware allocation failed");
            free(middleware_info);
            return;
        }
        memcpy(middleware_info->middleware, middleware.handlers,
               sizeof(MiddlewareHandler) * middleware.count);
        middleware_info->middleware_count = (int)middleware.count;
    }

    int result = route_trie_add(app->route_trie, method, path, handler, middleware_info);
    if (result != 0) {
        TLOG_ERROR("iris_app_route: failed to add route {:s} {:s}", method, path);
        free_middleware_info(middleware_info);
    }
}

/* ============================================================================
 * Middleware
 * ============================================================================ */

#define INITIAL_MW_CAPACITY 4

void iris_app_hook(iris_app_t *app, MiddlewareHandler middleware) {
    if (!app || !middleware)
        return;

    if (app->global_middleware_count >= app->global_middleware_capacity) {
        int new_cap = app->global_middleware_capacity ? app->global_middleware_capacity * 2
                                                       : INITIAL_MW_CAPACITY;
        MiddlewareHandler *tmp = realloc(app->global_middleware, new_cap * sizeof(MiddlewareHandler));
        if (!tmp) {
            TLOG_ERROR("iris_app_hook: realloc failed");
            return;
        }
        app->global_middleware = tmp;
        app->global_middleware_capacity = new_cap;
    }

    app->global_middleware[app->global_middleware_count++] = middleware;
}

/* ============================================================================
 * CORS
 * ============================================================================ */

static char *safe_strdup(const char *str) {
    return str ? strdup(str) : NULL;
}

void iris_app_cors(iris_app_t *app, cors_t *opts) {
    if (!app)
        return;

    /* Free existing CORS options */
    if (app->cors_opts) {
        free(app->cors_opts->origin);
        free(app->cors_opts->methods);
        free(app->cors_opts->headers);
        free(app->cors_opts->credentials);
        free(app->cors_opts->max_age);
        free(app->cors_opts);
        app->cors_opts = NULL;
    }

    if (!opts)
        return;

    cors_t *cors = calloc(1, sizeof(cors_t));
    if (!cors)
        return;

    static const char *def_methods = "GET, POST, PUT, DELETE, OPTIONS";
    static const char *def_headers = "Content-Type";
    static const char *def_credentials = "true";
    static const char *def_max_age = "3600";

    if (opts->origin) {
        cors->origin = safe_strdup(opts->origin);
        cors->allow_all_origins = (strcmp(opts->origin, "*") == 0);
    }

    cors->methods = safe_strdup(opts->methods ? opts->methods : def_methods);
    cors->headers = safe_strdup(opts->headers ? opts->headers : def_headers);
    cors->credentials = safe_strdup(opts->credentials ? opts->credentials : def_credentials);
    cors->max_age = safe_strdup(opts->max_age ? opts->max_age : def_max_age);
    cors->enabled = opts->enabled != 0 ? opts->enabled : 1;

    app->cors_opts = cors;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

void iris_app_shutdown_hook(iris_app_t *app, void (*hook)(void)) {
    if (app) {
        app->shutdown_hook = hook;
    }
}
