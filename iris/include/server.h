#ifndef SERVER_H
#define SERVER_H

#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "netcore.h"
 
struct iris_app;

CXX_C_API void shutdown_hook(void (*hook)(void));
CXX_C_API int init_router(void);
CXX_C_API void reset_router(void);
CXX_C_API int iris_server_run(unsigned short port);
CXX_C_API int iris_app_run(struct iris_app *app, unsigned short port);
CXX_C_API coro_server_t* iris_server_start(struct iris_app *app, coro_context_t *ctx, unsigned short port);

#ifdef __cplusplus
}
#endif

#endif
