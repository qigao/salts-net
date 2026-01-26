#ifndef IRIS_APP_H
#define IRIS_APP_H

#include "route_trie.h"
#include "middleware.h"
#include "cors.h"
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Iris application instance
 *
 * Holds all state for an iris web application, enabling multiple
 * independent app instances in the same process.
 */
typedef struct iris_app {
    /* Routing */
    route_trie_t *route_trie;

    /* Middleware */
    MiddlewareHandler *global_middleware;
    int global_middleware_count;
    int global_middleware_capacity;

    /* CORS */
    cors_t *cors_opts;

    /* RPC context (if any) */
    void *rpc_context;

    /* Lifecycle hooks */
    void (*shutdown_hook)(void);
} iris_app_t;

/**
 * @brief Create a new iris application instance
 * @return New app instance or NULL on failure
 */
CXX_C_API iris_app_t *iris_app_create(void);

/**
 * @brief Destroy an iris application instance
 * @param app Application to destroy
 */
CXX_C_API void iris_app_destroy(iris_app_t *app);

/**
 * @brief Get the default global application instance
 *
 * Creates a default app on first call. Used for backward compatibility
 * with the legacy macro-based API (get, post, etc.).
 *
 * @return Default app instance
 */
CXX_C_API iris_app_t *iris_app_default(void);

/**
 * @brief Get the default app if it exists (without creating)
 *
 * @return Default app instance or NULL if not created
 */
CXX_C_API iris_app_t *iris_app_get_default_if_exists(void);

/**
 * @brief Reset the default application (for testing)
 */
CXX_C_API void iris_app_reset_default(void);

/* ============================================================================
 * App-aware route registration
 * ============================================================================ */

CXX_C_API void iris_app_route(iris_app_t *app, const char *method, const char *path,
                    MiddlewareArray middleware, RequestHandler handler);

static inline void iris_app_get(iris_app_t *app, const char *path, RequestHandler handler) {
    iris_app_route(app, "GET", path, NO_MW, handler);
}

static inline void iris_app_get_mw(iris_app_t *app, const char *path,
                                    MiddlewareArray mw, RequestHandler handler) {
    iris_app_route(app, "GET", path, mw, handler);
}

static inline void iris_app_post(iris_app_t *app, const char *path, RequestHandler handler) {
    iris_app_route(app, "POST", path, NO_MW, handler);
}

static inline void iris_app_post_mw(iris_app_t *app, const char *path,
                                     MiddlewareArray mw, RequestHandler handler) {
    iris_app_route(app, "POST", path, mw, handler);
}

static inline void iris_app_put(iris_app_t *app, const char *path, RequestHandler handler) {
    iris_app_route(app, "PUT", path, NO_MW, handler);
}

static inline void iris_app_put_mw(iris_app_t *app, const char *path,
                                    MiddlewareArray mw, RequestHandler handler) {
    iris_app_route(app, "PUT", path, mw, handler);
}

static inline void iris_app_patch(iris_app_t *app, const char *path, RequestHandler handler) {
    iris_app_route(app, "PATCH", path, NO_MW, handler);
}

static inline void iris_app_patch_mw(iris_app_t *app, const char *path,
                                      MiddlewareArray mw, RequestHandler handler) {
    iris_app_route(app, "PATCH", path, mw, handler);
}

static inline void iris_app_delete(iris_app_t *app, const char *path, RequestHandler handler) {
    iris_app_route(app, "DELETE", path, NO_MW, handler);
}

static inline void iris_app_delete_mw(iris_app_t *app, const char *path,
                                       MiddlewareArray mw, RequestHandler handler) {
    iris_app_route(app, "DELETE", path, mw, handler);
}

/* ============================================================================
 * App-aware middleware
 * ============================================================================ */

/**
 * @brief Add global middleware to an app
 */
CXX_C_API void iris_app_hook(iris_app_t *app, MiddlewareHandler middleware);

/**
 * @brief Configure CORS for an app
 */
CXX_C_API void iris_app_cors(iris_app_t *app, cors_t *opts);

/**
 * @brief Set shutdown hook for an app
 */
CXX_C_API void iris_app_shutdown_hook(iris_app_t *app, void (*hook)(void));

/**
 * @brief Start the server and listen on specified port
 * @param app Application instance (can be NULL to use default)
 * @param port Port number to listen on
 * @return 0 on success, -1 on failure
 */
CXX_C_API int iris_app_listen(iris_app_t *app, unsigned short port);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_APP_H */
