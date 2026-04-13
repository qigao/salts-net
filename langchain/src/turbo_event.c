#include "turbo_event.h"

#include <stdlib.h>
#include <string.h>

static int turbo_event_set_string_field(turbo_runtime_data_bind_value_t *object, const char *key,
                                        const char *value) {
  turbo_runtime_data_bind_value_t *field_value;

  field_value = turbo_runtime_data_bind_value_create_string(value ? value : "");
  if (!field_value) {
    return -1;
  }

  if (turbo_runtime_data_bind_object_set(object, key, field_value) != TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(field_value);
    return -1;
  }

  return 0;
}

static turbo_runtime_data_bind_value_t *turbo_event_tool_call_schema_bind(void) {
  turbo_runtime_data_bind_value_t *schema = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *properties = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *required = turbo_runtime_data_bind_value_create_array();
  turbo_runtime_data_bind_value_t *field = NULL;

  if (!schema || !properties || !required) {
    turbo_runtime_data_bind_value_destroy(schema);
    turbo_runtime_data_bind_value_destroy(properties);
    turbo_runtime_data_bind_value_destroy(required);
    return NULL;
  }

  turbo_event_set_string_field(schema, "type", "object");
  turbo_runtime_data_bind_object_set(schema, "properties", properties);
  turbo_runtime_data_bind_object_set(schema, "required", required);

  field = turbo_runtime_data_bind_value_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_data_bind_object_set(properties, "call_id", field);
  turbo_runtime_data_bind_array_append(required, turbo_runtime_data_bind_value_create_string("call_id"));

  field = turbo_runtime_data_bind_value_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_data_bind_object_set(properties, "name", field);
  turbo_runtime_data_bind_array_append(required, turbo_runtime_data_bind_value_create_string("name"));

  field = turbo_runtime_data_bind_value_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_data_bind_object_set(properties, "arguments", field);
  turbo_runtime_data_bind_array_append(required,
                                       turbo_runtime_data_bind_value_create_string("arguments"));

  return schema;
}

const char *turbo_event_kind_bind(const turbo_runtime_data_bind_value_t *event) {
  const turbo_runtime_data_bind_value_t *kind;

  if (!event || turbo_runtime_data_bind_value_kind(event) != TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
    return NULL;
  }

  kind = turbo_runtime_data_bind_object_get(event, "kind");
  if (!kind || turbo_runtime_data_bind_value_kind(kind) != TURBO_RUNTIME_DATA_BIND_VALUE_STRING) {
    return NULL;
  }

  return turbo_runtime_data_bind_value_as_string(kind);
}

int turbo_event_validate_bind(const turbo_runtime_data_bind_value_t *event) {
  const char *kind = turbo_event_kind_bind(event);

  if (!kind) {
    return -1;
  }

  if (strcmp(kind, "model") == 0) {
    return turbo_event_model_validate_bind(event);
  }
  if (strcmp(kind, "trace") == 0) {
    return turbo_event_trace_validate_bind(event);
  }
  if (strcmp(kind, "tool_result") == 0) {
    return turbo_event_tool_result_validate_bind(event);
  }

  return -1;
}

turbo_runtime_data_bind_value_t *turbo_event_model_schema_bind(void) {
  turbo_runtime_data_bind_value_t *schema = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *properties = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *required = turbo_runtime_data_bind_value_create_array();
  turbo_runtime_data_bind_value_t *field = NULL;
  turbo_runtime_data_bind_value_t *items = NULL;

  if (!schema || !properties || !required) {
    turbo_runtime_data_bind_value_destroy(schema);
    turbo_runtime_data_bind_value_destroy(properties);
    turbo_runtime_data_bind_value_destroy(required);
    return NULL;
  }

  turbo_event_set_string_field(schema, "type", "object");
  turbo_runtime_data_bind_object_set(schema, "properties", properties);
  turbo_runtime_data_bind_object_set(schema, "required", required);

  field = turbo_runtime_data_bind_value_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_data_bind_object_set(properties, "kind", field);
  turbo_runtime_data_bind_array_append(required, turbo_runtime_data_bind_value_create_string("kind"));

  field = turbo_runtime_data_bind_value_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_data_bind_object_set(properties, "response_id", field);
  turbo_runtime_data_bind_array_append(required,
                                       turbo_runtime_data_bind_value_create_string("response_id"));

  field = turbo_runtime_data_bind_value_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_data_bind_object_set(properties, "output_text", field);
  turbo_runtime_data_bind_array_append(required,
                                       turbo_runtime_data_bind_value_create_string("output_text"));

  field = turbo_runtime_data_bind_value_create_object();
  turbo_event_set_string_field(field, "type", "array");
  items = turbo_event_tool_call_schema_bind();
  if (!field || !items) {
    turbo_runtime_data_bind_value_destroy(field);
    turbo_runtime_data_bind_value_destroy(items);
    turbo_runtime_data_bind_value_destroy(schema);
    return NULL;
  }
  turbo_runtime_data_bind_object_set(field, "items", items);
  turbo_runtime_data_bind_object_set(properties, "tool_calls", field);
  turbo_runtime_data_bind_array_append(required,
                                       turbo_runtime_data_bind_value_create_string("tool_calls"));

  return schema;
}

turbo_runtime_data_bind_value_t *turbo_event_trace_schema_bind(void) {
  turbo_runtime_data_bind_value_t *schema = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *properties = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *required = turbo_runtime_data_bind_value_create_array();
  turbo_runtime_data_bind_value_t *field = NULL;

  if (!schema || !properties || !required) {
    turbo_runtime_data_bind_value_destroy(schema);
    turbo_runtime_data_bind_value_destroy(properties);
    turbo_runtime_data_bind_value_destroy(required);
    return NULL;
  }

  turbo_event_set_string_field(schema, "type", "object");
  turbo_runtime_data_bind_object_set(schema, "properties", properties);
  turbo_runtime_data_bind_object_set(schema, "required", required);

  field = turbo_runtime_data_bind_value_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_data_bind_object_set(properties, "kind", field);
  turbo_runtime_data_bind_array_append(required, turbo_runtime_data_bind_value_create_string("kind"));

  field = turbo_runtime_data_bind_value_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_data_bind_object_set(properties, "name", field);
  turbo_runtime_data_bind_array_append(required, turbo_runtime_data_bind_value_create_string("name"));

  field = turbo_runtime_data_bind_value_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_data_bind_object_set(properties, "detail", field);
  turbo_runtime_data_bind_array_append(required, turbo_runtime_data_bind_value_create_string("detail"));

  field = turbo_runtime_data_bind_value_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_data_bind_object_set(properties, "payload", field);
  turbo_runtime_data_bind_array_append(required,
                                       turbo_runtime_data_bind_value_create_string("payload"));

  field = turbo_runtime_data_bind_value_create_object();
  turbo_event_set_string_field(field, "type", "integer");
  turbo_runtime_data_bind_object_set(properties, "status", field);
  turbo_runtime_data_bind_array_append(required, turbo_runtime_data_bind_value_create_string("status"));

  return schema;
}

turbo_runtime_data_bind_value_t *turbo_event_tool_result_schema_bind(void) {
  turbo_runtime_data_bind_value_t *schema = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *properties = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *required = turbo_runtime_data_bind_value_create_array();
  turbo_runtime_data_bind_value_t *field = NULL;

  if (!schema || !properties || !required) {
    turbo_runtime_data_bind_value_destroy(schema);
    turbo_runtime_data_bind_value_destroy(properties);
    turbo_runtime_data_bind_value_destroy(required);
    return NULL;
  }

  turbo_event_set_string_field(schema, "type", "object");
  turbo_runtime_data_bind_object_set(schema, "properties", properties);
  turbo_runtime_data_bind_object_set(schema, "required", required);

  field = turbo_runtime_data_bind_value_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_data_bind_object_set(properties, "kind", field);
  turbo_runtime_data_bind_array_append(required, turbo_runtime_data_bind_value_create_string("kind"));

  field = turbo_runtime_data_bind_value_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_data_bind_object_set(properties, "name", field);
  turbo_runtime_data_bind_array_append(required, turbo_runtime_data_bind_value_create_string("name"));

  field = turbo_runtime_data_bind_value_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_data_bind_object_set(properties, "arguments_json", field);
  turbo_runtime_data_bind_array_append(required,
                                       turbo_runtime_data_bind_value_create_string("arguments_json"));

  field = turbo_runtime_data_bind_value_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_data_bind_object_set(properties, "output", field);
  turbo_runtime_data_bind_array_append(required, turbo_runtime_data_bind_value_create_string("output"));

  field = turbo_runtime_data_bind_value_create_object();
  turbo_event_set_string_field(field, "type", "integer");
  turbo_runtime_data_bind_object_set(properties, "status", field);
  turbo_runtime_data_bind_array_append(required, turbo_runtime_data_bind_value_create_string("status"));

  return schema;
}

int turbo_event_model_validate_bind(const turbo_runtime_data_bind_value_t *event) {
  const turbo_runtime_data_bind_value_t *kind;
  const turbo_runtime_data_bind_value_t *response_id;
  const turbo_runtime_data_bind_value_t *output_text;
  const turbo_runtime_data_bind_value_t *tool_calls;
  size_t i;

  if (!event || turbo_runtime_data_bind_value_kind(event) != TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
    return -1;
  }

  kind = turbo_runtime_data_bind_object_get(event, "kind");
  response_id = turbo_runtime_data_bind_object_get(event, "response_id");
  output_text = turbo_runtime_data_bind_object_get(event, "output_text");
  tool_calls = turbo_runtime_data_bind_object_get(event, "tool_calls");

  if (!kind || !response_id || !output_text || !tool_calls) {
    return -1;
  }
  if (strcmp(turbo_runtime_data_bind_value_as_string(kind), "model") != 0) {
    return -1;
  }
  if (turbo_runtime_data_bind_value_kind(response_id) != TURBO_RUNTIME_DATA_BIND_VALUE_STRING ||
      turbo_runtime_data_bind_value_kind(output_text) != TURBO_RUNTIME_DATA_BIND_VALUE_STRING ||
      turbo_runtime_data_bind_value_kind(tool_calls) != TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY) {
    return -1;
  }

  for (i = 0; i < turbo_runtime_data_bind_value_size(tool_calls); ++i) {
    const turbo_runtime_data_bind_value_t *call = turbo_runtime_data_bind_array_get(tool_calls, i);

    if (!call || turbo_runtime_data_bind_value_kind(call) != TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
      return -1;
    }
    if (!turbo_runtime_data_bind_object_get(call, "call_id") ||
        !turbo_runtime_data_bind_object_get(call, "name") ||
        !turbo_runtime_data_bind_object_get(call, "arguments")) {
      return -1;
    }
  }

  return 0;
}

int turbo_event_trace_validate_bind(const turbo_runtime_data_bind_value_t *event) {
  const turbo_runtime_data_bind_value_t *kind;
  const turbo_runtime_data_bind_value_t *name;
  const turbo_runtime_data_bind_value_t *detail;
  const turbo_runtime_data_bind_value_t *payload;
  const turbo_runtime_data_bind_value_t *status;

  if (!event || turbo_runtime_data_bind_value_kind(event) != TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
    return -1;
  }

  kind = turbo_runtime_data_bind_object_get(event, "kind");
  name = turbo_runtime_data_bind_object_get(event, "name");
  detail = turbo_runtime_data_bind_object_get(event, "detail");
  payload = turbo_runtime_data_bind_object_get(event, "payload");
  status = turbo_runtime_data_bind_object_get(event, "status");
  if (!kind || !name || !detail || !payload || !status) {
    return -1;
  }
  if (strcmp(turbo_runtime_data_bind_value_as_string(kind), "trace") != 0) {
    return -1;
  }
  if (turbo_runtime_data_bind_value_kind(name) != TURBO_RUNTIME_DATA_BIND_VALUE_STRING ||
      turbo_runtime_data_bind_value_kind(detail) != TURBO_RUNTIME_DATA_BIND_VALUE_STRING ||
      turbo_runtime_data_bind_value_kind(payload) != TURBO_RUNTIME_DATA_BIND_VALUE_STRING) {
    return -1;
  }
  return turbo_runtime_data_bind_value_kind(status) == TURBO_RUNTIME_DATA_BIND_VALUE_INT64 ? 0 : -1;
}

int turbo_event_tool_result_validate_bind(const turbo_runtime_data_bind_value_t *event) {
  const turbo_runtime_data_bind_value_t *kind;
  const turbo_runtime_data_bind_value_t *name;
  const turbo_runtime_data_bind_value_t *arguments_json;
  const turbo_runtime_data_bind_value_t *output;
  const turbo_runtime_data_bind_value_t *status;

  if (!event || turbo_runtime_data_bind_value_kind(event) != TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
    return -1;
  }

  kind = turbo_runtime_data_bind_object_get(event, "kind");
  name = turbo_runtime_data_bind_object_get(event, "name");
  arguments_json = turbo_runtime_data_bind_object_get(event, "arguments_json");
  output = turbo_runtime_data_bind_object_get(event, "output");
  status = turbo_runtime_data_bind_object_get(event, "status");
  if (!kind || !name || !arguments_json || !output || !status) {
    return -1;
  }
  if (strcmp(turbo_runtime_data_bind_value_as_string(kind), "tool_result") != 0) {
    return -1;
  }
  if (turbo_runtime_data_bind_value_kind(name) != TURBO_RUNTIME_DATA_BIND_VALUE_STRING ||
      turbo_runtime_data_bind_value_kind(arguments_json) != TURBO_RUNTIME_DATA_BIND_VALUE_STRING ||
      turbo_runtime_data_bind_value_kind(output) != TURBO_RUNTIME_DATA_BIND_VALUE_STRING ||
      turbo_runtime_data_bind_value_kind(status) != TURBO_RUNTIME_DATA_BIND_VALUE_INT64) {
    return -1;
  }

  return 0;
}

turbo_runtime_data_bind_value_t *
turbo_event_model_create_bind(const char *response_id, const char *output_text,
                              const turbo_runtime_data_bind_value_t *tool_calls) {
  turbo_runtime_data_bind_value_t *event = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *tool_calls_copy;

  if (!event) {
    return NULL;
  }

  if (turbo_event_set_string_field(event, "kind", "model") != 0 ||
      turbo_event_set_string_field(event, "response_id", response_id ? response_id : "") != 0 ||
      turbo_event_set_string_field(event, "output_text", output_text ? output_text : "") != 0) {
    turbo_runtime_data_bind_value_destroy(event);
    return NULL;
  }

  if (tool_calls) {
    tool_calls_copy = turbo_runtime_data_bind_value_clone(tool_calls);
  } else {
    tool_calls_copy = turbo_runtime_data_bind_value_create_array();
  }
  if (!tool_calls_copy) {
    turbo_runtime_data_bind_value_destroy(event);
    return NULL;
  }

  if (turbo_runtime_data_bind_object_set(event, "tool_calls", tool_calls_copy) !=
      TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(tool_calls_copy);
    turbo_runtime_data_bind_value_destroy(event);
    return NULL;
  }

  return event;
}

turbo_runtime_data_bind_value_t *turbo_event_trace_create_bind(const char *name,
                                                               const char *detail,
                                                               const char *payload,
                                                               int64_t status) {
  turbo_runtime_data_bind_value_t *event = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *status_value;

  if (!event) {
    return NULL;
  }

  status_value = turbo_runtime_data_bind_value_create_int64(status);
  if (!status_value || turbo_event_set_string_field(event, "kind", "trace") != 0 ||
      turbo_event_set_string_field(event, "name", name ? name : "") != 0 ||
      turbo_event_set_string_field(event, "detail", detail ? detail : "") != 0 ||
      turbo_event_set_string_field(event, "payload", payload ? payload : "") != 0 ||
      turbo_runtime_data_bind_object_set(event, "status", status_value) !=
          TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(status_value);
    turbo_runtime_data_bind_value_destroy(event);
    return NULL;
  }

  return event;
}

turbo_runtime_data_bind_value_t *turbo_event_tool_result_create_bind(
    const char *name, const char *arguments_json, const char *output,
    const turbo_runtime_data_bind_value_t *output_value, int64_t status) {
  turbo_runtime_data_bind_value_t *event = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *status_value;
  turbo_runtime_data_bind_value_t *output_value_copy = NULL;

  if (!event) {
    return NULL;
  }

  status_value = turbo_runtime_data_bind_value_create_int64(status);
  if (!status_value || turbo_event_set_string_field(event, "kind", "tool_result") != 0 ||
      turbo_event_set_string_field(event, "name", name ? name : "") != 0 ||
      turbo_event_set_string_field(event, "arguments_json", arguments_json ? arguments_json : "{}") !=
          0 ||
      turbo_event_set_string_field(event, "output", output ? output : "") != 0 ||
      turbo_runtime_data_bind_object_set(event, "status", status_value) !=
          TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(status_value);
    turbo_runtime_data_bind_value_destroy(event);
    return NULL;
  }

  if (output_value) {
    output_value_copy = turbo_runtime_data_bind_value_clone(output_value);
    if (!output_value_copy ||
        turbo_runtime_data_bind_object_set(event, "output_value", output_value_copy) !=
            TURBO_RUNTIME_DATA_BIND_OK) {
      turbo_runtime_data_bind_value_destroy(output_value_copy);
      turbo_runtime_data_bind_value_destroy(event);
      return NULL;
    }
  }

  return event;
}
