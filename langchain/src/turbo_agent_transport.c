#include "turbo_agent_core_internal.h"
#include "turbo_agent_lifecycle_internal.h"
#include "turbo_agent_transport_internal.h"
#include "turbo_agent_sse.h"
#include "turbo_agent_util_internal.h"

#include "http_client.h"
#include "turbo_model_provider.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char *data;
  size_t length;
} turbo_agent_stream_buffer_t;

static void turbo_agent_collect_stream_chunk(const char *data, size_t len, void *user_data) {
  turbo_agent_stream_buffer_t *buffer = (turbo_agent_stream_buffer_t *)user_data;

  if (!buffer || (!data && len > 0)) {
    return;
  }

  if (buffer->length == (size_t)-1) {
    return;
  }

  if (turbo_agent_util_append_bytes(&buffer->data, &buffer->length, data, len) != 0) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->length = (size_t)-1;
  }
}

static int turbo_agent_capture_last_stream_sse(turbo_agent_t *agent, const char *data,
                                               size_t len) {
  char *copy;

  if (!agent || !data) {
    return -1;
  }

  copy = (char *)malloc(len + 1);
  if (!copy) {
    return -1;
  }

  memcpy(copy, data, len);
  copy[len] = '\0';
  turbo_agent_clear_last_stream_sse(agent);
  agent->last_stream_sse = copy;
  agent->last_stream_sse_len = len;
  return 0;
}

static int turbo_agent_provider_sse_to_response_json(const turbo_model_provider_t *provider,
                                                     const char *sse_data, size_t sse_len,
                                                     char **out_response_json) {
  if (!provider || !sse_data || !out_response_json) {
    return -1;
  }

  *out_response_json = NULL;
  if (provider == turbo_model_provider_openai_responses()) {
    return turbo_agent_responses_sse_to_json(sse_data, sse_len, out_response_json);
  }
  if (turbo_model_provider_is_legacy_chat(provider)) {
    return turbo_agent_chat_sse_to_json(sse_data, sse_len, out_response_json);
  }
  if (turbo_model_provider_is_anthropic_messages(provider)) {
    return turbo_agent_anthropic_messages_sse_to_json(sse_data, sse_len, out_response_json);
  }
  return -1;
}

static char *turbo_agent_http_transport_error_detail(const http_response_t *response,
                                                     const char *stream_data,
                                                     size_t stream_len) {
  const char *error_text = response && response->error ? response->error : "";
  const char *body_text = NULL;
  size_t preview_len = 0;
  int needed;
  char *buffer;

  if (response && response->body && response->body[0] != '\0') {
    body_text = response->body;
    preview_len = strlen(body_text);
  } else if (stream_data && stream_len > 0 && stream_len != (size_t)-1) {
    body_text = stream_data;
    preview_len = stream_len;
  }

  if (preview_len > 240) {
    preview_len = 240;
  }

  if (!response) {
    return turbo_agent_util_strdup("http transport failed before a response was created");
  }

  needed = snprintf(NULL, 0,
                    "http transport failed: status=%d error_code=%d error=%s%s%s%.*s",
                    response->status_code, (int)response->error_code,
                    error_text[0] != '\0' ? error_text : "(none)",
                    preview_len > 0 ? " body_preview=" : "", preview_len > 0 ? "\"" : "",
                    (int)preview_len, body_text ? body_text : "");
  if (needed < 0) {
    return NULL;
  }

  buffer = (char *)malloc((size_t)needed + 1 + (preview_len > 0 ? 1 : 0));
  if (!buffer) {
    return NULL;
  }

  snprintf(buffer, (size_t)needed + 1 + (preview_len > 0 ? 1 : 0),
           "http transport failed: status=%d error_code=%d error=%s%s%s%.*s%s",
           response->status_code, (int)response->error_code,
           error_text[0] != '\0' ? error_text : "(none)",
           preview_len > 0 ? " body_preview=" : "", preview_len > 0 ? "\"" : "",
           (int)preview_len, body_text ? body_text : "", preview_len > 0 ? "\"" : "");
  return buffer;
}

CXX_C_API int turbo_agent_configure_http_client_openai(const turbo_agent_t *agent,
                                                       http_client_t *http_client) {
  if (!http_client) {
    return -1;
  }

  if (agent && agent->api_key && agent->api_key[0] != '\0') {
    http_client_set_bearer_token(http_client, agent->api_key);
  }

  http_client_set_user_agent(http_client, "TurboNet-Agent/0.1");
  return 0;
}

CXX_C_API int turbo_agent_configure_http_client_anthropic(const turbo_agent_t *agent,
                                                          http_client_t *http_client) {
  if (!http_client) {
    return -1;
  }

  if (agent && agent->api_key && agent->api_key[0] != '\0') {
    http_client_set_default_header(http_client, "x-api-key", agent->api_key);
  }

  http_client_set_default_header(http_client, "anthropic-version", "2023-06-01");
  http_client_set_user_agent(http_client, "TurboNet-Agent/0.1");
  return 0;
}

CXX_C_API int turbo_agent_http_transport(const char *request_json, char **out_response_json,
                                         void *user_data) {
  turbo_agent_t *agent = (turbo_agent_t *)user_data;
  http_response_t *response;
  char *copy;
  turbo_agent_stream_buffer_t stream_buffer = {0};

  if (!agent || !agent->http_client || !request_json || !out_response_json) {
    return -1;
  }

  turbo_agent_clear_last_stream_sse(agent);
  *out_response_json = NULL;
  if (agent->stream_response) {
    response = http_sse_post_json(agent->http_client, agent->endpoint_path, request_json,
                                  turbo_agent_collect_stream_chunk, &stream_buffer);
  } else {
    response = http_post_json(agent->http_client, agent->endpoint_path, request_json);
  }
  if (!response) {
    *out_response_json =
        turbo_agent_http_transport_error_detail(NULL, stream_buffer.data, stream_buffer.length);
    free(stream_buffer.data);
    return -1;
  }

  if (stream_buffer.length == (size_t)-1) {
    *out_response_json = turbo_agent_util_strdup(
        "http streaming callback failed while collecting model response");
    http_response_free(response);
    return -1;
  }

  if (response->error_code != HTTP_ERROR_NONE || response->status_code < 200 ||
      response->status_code >= 300 || !response->body) {
    if (agent->stream_response && response->error_code == HTTP_ERROR_NONE &&
        response->status_code >= 200 && response->status_code < 300 && stream_buffer.data) {
      http_response_free(response);
      if (turbo_agent_capture_last_stream_sse(agent, stream_buffer.data,
                                              stream_buffer.length) != 0) {
        free(stream_buffer.data);
        *out_response_json = turbo_agent_util_strdup("failed to capture SSE stream");
        return -1;
      }
      if (agent->provider &&
          turbo_agent_provider_sse_to_response_json(agent->provider, stream_buffer.data,
                                                    stream_buffer.length,
                                                    out_response_json) == 0) {
        free(stream_buffer.data);
        return 0;
      }
      *out_response_json = turbo_agent_util_strdup(
          "failed to aggregate SSE stream into a JSON model response");
      free(stream_buffer.data);
      return -1;
    }

    *out_response_json =
        turbo_agent_http_transport_error_detail(response, stream_buffer.data, stream_buffer.length);
    free(stream_buffer.data);
    http_response_free(response);
    return -1;
  }

  copy = turbo_agent_util_strdup(response->body);
  http_response_free(response);
  free(stream_buffer.data);
  if (!copy) {
    return -1;
  }

  *out_response_json = copy;
  return 0;
}
