#ifndef IRIS_APP_H
#define IRIS_APP_H

#include "route_trie.h"
#include "middleware.h"
#include "cors.h"
#include "security.h"
#include "platform.h"
#include "error_recovery.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IRIS_INLINE_MW_CAPACITY 4

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
    MiddlewareHandler global_middleware_inline[IRIS_INLINE_MW_CAPACITY];
    int global_middleware_count;
    int global_middleware_capacity;

    /* CORS */
    cors_t *cors_opts;

    /* App-local RPC binding registry (internal; use bind/lookup helpers) */
    void *rpc_context;

    /* Lifecycle hooks */
    void (*shutdown_hook)(void);

    /* Security limits */
    iris_security_limits_t security_limits;

    /* Error recovery */
    iris_error_handler_t error_handler;
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
 * App-local RPC context binding
 * ============================================================================ */

typedef void (*iris_app_rpc_context_unbind_fn)(void *rpc_context, void *user_data);

/**
 * @brief Bind one RPC-like context to one path on one app.
 *
 * The same app may host multiple RPC endpoints as long as their paths differ.
 * Rebinding the same path to the same context is a no-op. Rebinding the same
 * path to a different context fails.
 *
 * @param app Application instance
 * @param path Static route path (for example "/rpc" or "/v1/runtime/jsonrpc")
 * @param rpc_context Opaque endpoint context pointer
 * @return 0 on success, -1 on failure
 */
CXX_C_API int iris_app_bind_rpc_context(iris_app_t *app, const char *path, void *rpc_context);

/**
 * @brief Bind one RPC-like context and notify it when the app/path binding is removed.
 *
 * This is the ownership-aware form used by contexts that cache their owning
 * app/path for destroy-time unbinding. The callback is invoked when the binding
 * is explicitly removed or when the app destroys its registry.
 *
 * @param app Application instance
 * @param path Static route path
 * @param rpc_context Opaque endpoint context pointer
 * @param on_unbind Optional callback invoked before the binding node is freed
 * @param user_data Callback user data
 * @return 0 on success, -1 on failure
 */
CXX_C_API int iris_app_bind_rpc_context_ex(iris_app_t *app, const char *path, void *rpc_context,
                                           iris_app_rpc_context_unbind_fn on_unbind,
                                           void *user_data);

/**
 * @brief Lookup one RPC-like context bound to one app/path pair.
 *
 * @param app Application instance
 * @param path Static route path
 * @return Bound context pointer or NULL when not found
 */
CXX_C_API void *iris_app_lookup_rpc_context(const iris_app_t *app, const char *path);

/**
 * @brief Unbind one RPC-like context from one app/path pair.
 *
 * Passing a non-NULL `rpc_context` makes the unbind conditional on pointer
 * equality. This helps avoid clearing a path rebound by another caller.
 *
 * @param app Application instance
 * @param path Static route path
 * @param rpc_context Expected bound context or NULL to ignore pointer match
 * @return 0 when one binding was removed, -1 otherwise
 */
CXX_C_API int iris_app_unbind_rpc_context(iris_app_t *app, const char *path,
                                          const void *rpc_context);

/* ============================================================================
 * App-aware route registration
 * ============================================================================ */

CXX_C_API void iris_app_route(iris_app_t *app, const char *method, const char *path,
                    MiddlewareArray middleware, RequestHandler handler);
CXX_C_API void iris_app_route_stream(iris_app_t *app, const char *method, const char *path,
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

static inline void iris_app_post_stream(iris_app_t *app, const char *path, RequestHandler handler) {
    iris_app_route_stream(app, "POST", path, NO_MW, handler);
}

static inline void iris_app_post_stream_mw(iris_app_t *app, const char *path,
                                           MiddlewareArray mw, RequestHandler handler) {
    iris_app_route_stream(app, "POST", path, mw, handler);
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
 * @brief Set custom error handler for an app
 */
CXX_C_API void iris_app_set_error_handler(iris_app_t *app, iris_error_handler_t handler);

/**
 * @brief Set custom security limits for an app
 */
CXX_C_API void iris_app_set_security_limits(iris_app_t *app, const iris_security_limits_t *limits);

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
