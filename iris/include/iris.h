#ifndef IRIS_H
#define IRIS_H

#include "iris_app.h"
#include "router.h"
#include "server.h"
#include <turbo_coro.h>
#include <CoroNet.h>

/**
 * @file iris.h
 * @brief Main entry point for the Iris web framework.
 *
 * Provides a simplified macro-based API for route registration and 
 * coroutine utilities for request handlers.
 */

/* ============================================================================
 * Coroutine Utilities
 * ============================================================================ */

/**
 * @brief Get the coroutine context associated with a request.
 * Falls back to the current thread's coroutine context if req is NULL.
 */
static inline coro_context_t *iris_context(Req *req) {
  if (req && req->client) {
    return coro_socket_get_context(req->client);
  }
  return coro_context_current();
}

/**
 * @brief Spawn a new coroutine task in the same context as the request.
 */
static inline void iris_spawn(Req *req, coro_fn fn, void *arg) {
  coro_context_t *ctx = iris_context(req);
  if (ctx) {
    coro_context_spawn(ctx, fn, arg);
  }
}

/* ============================================================================
 * Global Route Registration (Default App)
 * ============================================================================ */

/* Legacy global route trie - points to default app's trie. */
extern route_trie_t *global_route_trie;

#define GET_CHOOSER(_1, _2, _3, NAME, ...) NAME
#define get(...) GET_CHOOSER(__VA_ARGS__, get_with_mw, get_no_mw)(__VA_ARGS__)

static inline void get_no_mw(const char *path, RequestHandler handler) {
  iris_app_route(iris_app_default(), "GET", path, NO_MW, handler);
}

static inline void get_with_mw(const char *path, MiddlewareArray mw, RequestHandler handler) {
  iris_app_route(iris_app_default(), "GET", path, mw, handler);
}

#define POST_CHOOSER(_1, _2, _3, NAME, ...) NAME
#define post(...) POST_CHOOSER(__VA_ARGS__, post_with_mw, post_no_mw)(__VA_ARGS__)

static inline void post_no_mw(const char *p, RequestHandler h) {
  iris_app_route(iris_app_default(), "POST", p, NO_MW, h);
}

static inline void post_with_mw(const char *p, MiddlewareArray mw, RequestHandler h) {
  iris_app_route(iris_app_default(), "POST", p, mw, h);
}

#define PUT_CHOOSER(_1, _2, _3, NAME, ...) NAME
#define put(...) PUT_CHOOSER(__VA_ARGS__, put_with_mw, put_no_mw)(__VA_ARGS__)

static inline void put_no_mw(const char *p, RequestHandler h) {
  iris_app_route(iris_app_default(), "PUT", p, NO_MW, h);
}

static inline void put_with_mw(const char *p, MiddlewareArray mw, RequestHandler h) {
  iris_app_route(iris_app_default(), "PUT", p, mw, h);
}

#define PATCH_CHOOSER(_1, _2, _3, NAME, ...) NAME
#define patch(...) PATCH_CHOOSER(__VA_ARGS__, patch_with_mw, patch_no_mw)(__VA_ARGS__)

static inline void patch_no_mw(const char *p, RequestHandler h) {
  iris_app_route(iris_app_default(), "PATCH", p, NO_MW, h);
}

static inline void patch_with_mw(const char *p, MiddlewareArray mw, RequestHandler h) {
  iris_app_route(iris_app_default(), "PATCH", p, mw, h);
}

#define DEL_CHOOSER(_1, _2, _3, NAME, ...) NAME
#define del(...) DEL_CHOOSER(__VA_ARGS__, del_with_mw, del_no_mw)(__VA_ARGS__)

static inline void del_no_mw(const char *p, RequestHandler h) {
  iris_app_route(iris_app_default(), "DELETE", p, NO_MW, h);
}

static inline void del_with_mw(const char *p, MiddlewareArray mw, RequestHandler h) {
  iris_app_route(iris_app_default(), "DELETE", p, mw, h);
}

#endif /* IRIS_H */