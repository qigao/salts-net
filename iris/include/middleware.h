#ifndef MIDDLEWARE_H
#define MIDDLEWARE_H

#include "router.h"
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration of Chain structure
typedef struct Chain Chain;

#define INITIAL_MW_CAPACITY 4
#define IRIS_INLINE_ROUTE_MW_CAPACITY 4

// Function pointer type for middleware
typedef int (*MiddlewareHandler)(Req *req, Res *res, Chain *chain);

// Structure to store middleware chain context
struct Chain {
  MiddlewareHandler *handlers;  // Array of middleware handlers
  int count;                    // Number of handlers in the chain
  int current;                  // Current position in the middleware chain
  RequestHandler route_handler; // The final route handler
};

typedef struct MiddlewareInfo {
  MiddlewareHandler *middleware;
  MiddlewareHandler middleware_inline[IRIS_INLINE_ROUTE_MW_CAPACITY];
  int middleware_count;
  RequestHandler handler;
  int arena_owned;
} MiddlewareInfo;

typedef struct {
  MiddlewareHandler *handlers;
  size_t count;
} MiddlewareArray;

#define use(...)                                                                                   \
  ((MiddlewareArray){.handlers = (MiddlewareHandler[]){__VA_ARGS__},                               \
                     .count =                                                                      \
                         sizeof((MiddlewareHandler[]){__VA_ARGS__}) / sizeof(MiddlewareHandler)})

#define NO_MW ((MiddlewareArray){.handlers = NULL, .count = 0})

// Function to add global middleware
CXX_C_API void hook(MiddlewareHandler middleware_handler);

// Function to execute the next middleware or route handler in the chain
CXX_C_API int next(Chain *chain, Req *req, Res *res);

CXX_C_API void register_route(const char *method, const char *path, MiddlewareArray middleware,
                    RequestHandler handler);

CXX_C_API void reset_middleware(void);

CXX_C_API void free_middleware_info(MiddlewareInfo *info);

// The main function that runs middleware chain
CXX_C_API void execute_middleware_chain(Req *req, Res *res, MiddlewareInfo *middleware_info);

#ifdef __cplusplus
}
#endif

#endif
