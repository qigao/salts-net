#ifndef TURBO_AGENT_UTIL_INTERNAL_H
#define TURBO_AGENT_UTIL_INTERNAL_H

#include <platform.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API char *turbo_agent_util_strdup(const char *src);
CXX_C_API void turbo_agent_util_free_user_data(void *user_data);
CXX_C_API int turbo_agent_util_append_bytes(char **buffer, size_t *length, const char *data,
                                            size_t data_len);
CXX_C_API int turbo_agent_util_append_text(char **buffer, size_t *length, const char *text);

#ifdef __cplusplus
}
#endif

#endif
