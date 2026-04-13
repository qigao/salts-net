#ifndef TURBO_EVENT_H
#define TURBO_EVENT_H

#include <platform.h>

#include "turbo_runtime_data_bind.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*turbo_event_sink_bind_fn)(const turbo_runtime_data_bind_value_t *event,
                                         void *user_data);

/**
 * @brief Return the canonical event kind string for one bind-native event.
 */
CXX_C_API const char *turbo_event_kind_bind(const turbo_runtime_data_bind_value_t *event);

/**
 * @brief Validate one canonical bind-native event by dispatching on its kind.
 */
CXX_C_API int turbo_event_validate_bind(const turbo_runtime_data_bind_value_t *event);

/**
 * @brief Return the canonical bind-native schema for model events.
 */
CXX_C_API turbo_runtime_data_bind_value_t *turbo_event_model_schema_bind(void);

/**
 * @brief Validate one bind-native model event against the canonical shape.
 */
CXX_C_API int turbo_event_model_validate_bind(const turbo_runtime_data_bind_value_t *event);

/**
 * @brief Create one canonical bind-native model event.
 */
CXX_C_API turbo_runtime_data_bind_value_t *
turbo_event_model_create_bind(const char *response_id, const char *output_text,
                              const turbo_runtime_data_bind_value_t *tool_calls);

/**
 * @brief Return the canonical bind-native schema for trace events.
 */
CXX_C_API turbo_runtime_data_bind_value_t *turbo_event_trace_schema_bind(void);

/**
 * @brief Validate one bind-native trace event.
 */
CXX_C_API int turbo_event_trace_validate_bind(const turbo_runtime_data_bind_value_t *event);

/**
 * @brief Create one canonical bind-native trace event.
 */
CXX_C_API turbo_runtime_data_bind_value_t *turbo_event_trace_create_bind(
    const char *name, const char *detail, const char *payload, int64_t status);

/**
 * @brief Return the canonical bind-native schema for tool result events.
 */
CXX_C_API turbo_runtime_data_bind_value_t *turbo_event_tool_result_schema_bind(void);

/**
 * @brief Validate one bind-native tool result event.
 */
CXX_C_API int turbo_event_tool_result_validate_bind(const turbo_runtime_data_bind_value_t *event);

/**
 * @brief Create one canonical bind-native tool result event.
 */
CXX_C_API turbo_runtime_data_bind_value_t *turbo_event_tool_result_create_bind(
    const char *name, const char *arguments_json, const char *output,
    const turbo_runtime_data_bind_value_t *output_value, int64_t status);

#ifdef __cplusplus
}
#endif

#endif
