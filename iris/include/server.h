#ifndef SERVER_H
#define SERVER_H

#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API void shutdown_hook(void (*hook)(void));
CXX_C_API int init_router(void);
CXX_C_API void reset_router(void);
CXX_C_API int iris_server_run(unsigned short port);

#ifdef __cplusplus
}
#endif

#endif
