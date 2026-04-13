#ifndef TURBO_OPENAI_AGENT_INTERNAL_H
#define TURBO_OPENAI_AGENT_INTERNAL_H

#include "turbo_openai_agent.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*turbo_openai_agent_owned_resource_free_fn)(void *resource);

CXX_C_API int turbo_openai_agent_attach_owned_resource(
    turbo_openai_agent_t *agent, void *resource,
    turbo_openai_agent_owned_resource_free_fn free_resource);

#ifdef __cplusplus
}
#endif

#endif
