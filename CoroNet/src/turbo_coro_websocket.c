#include "CoroNet/turbo_coro_websocket.h"

#include "turbo_buffer.h"
#include "websocket_crypto.h"
#include "websocket_frame_common.h"
#include "websocket_frame_parser.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum {
  CORO_WEBSOCKET_FRAME_OVERHEAD = 14,
  CORO_WEBSOCKET_CONTROL_PAYLOAD_LIMIT = 125
};

struct coro_websocket_s {
  coro_websocket_role_t role;
  coro_websocket_transport_t transport;
  size_t max_message_size;
  coro_websocket_message_cb on_message;
  void *message_user_data;
  coro_websocket_transport_ops_t transport_ops;
  mem_buffer_t *rx;
  mem_buffer_t *fragment;
  uint8_t fragment_opcode;
  int fragmented;
  int closed;
  size_t operation_depth;
  size_t callback_depth;
  int destroy_requested;
};

static int websocket_valid_close_code(uint16_t code) {
  switch (code) {
  case CORO_WEBSOCKET_CLOSE_NORMAL:
  case CORO_WEBSOCKET_CLOSE_GOING_AWAY:
  case CORO_WEBSOCKET_CLOSE_PROTOCOL_ERROR:
  case CORO_WEBSOCKET_CLOSE_UNSUPPORTED_DATA:
  case CORO_WEBSOCKET_CLOSE_INVALID_PAYLOAD:
  case CORO_WEBSOCKET_CLOSE_POLICY_VIOLATION:
  case CORO_WEBSOCKET_CLOSE_MESSAGE_TOO_BIG:
  case CORO_WEBSOCKET_CLOSE_MANDATORY_EXTENSION:
  case CORO_WEBSOCKET_CLOSE_INTERNAL_ERROR:
    return 1;
  default:
    return code >= 3000 && code <= 4999;
  }
}

static void websocket_release(coro_websocket_t *websocket) {
  if (!websocket) return;
  if (websocket->rx) mem_buffer_release(websocket->rx);
  if (websocket->fragment) mem_buffer_release(websocket->fragment);
  free(websocket);
}

static void websocket_maybe_release(coro_websocket_t *websocket) {
  if (websocket && websocket->destroy_requested &&
      websocket->operation_depth == 0 && websocket->callback_depth == 0) {
    websocket_release(websocket);
  }
}

static int websocket_operation_begin(coro_websocket_t *websocket) {
  if (!websocket) return TURBO_EINVAL;
  if (websocket->operation_depth == SIZE_MAX) return TURBO_ERANGE;
  ++websocket->operation_depth;
  return TURBO_OK;
}

static void websocket_operation_end(coro_websocket_t *websocket) {
  if (!websocket) return;
  if (websocket->operation_depth > 0) --websocket->operation_depth;
  websocket_maybe_release(websocket);
}

static int websocket_valid_opcode(uint8_t opcode) {
  return opcode == CORO_WEBSOCKET_CONTINUATION ||
         opcode == CORO_WEBSOCKET_TEXT || opcode == CORO_WEBSOCKET_BINARY ||
         opcode == CORO_WEBSOCKET_CLOSE || opcode == CORO_WEBSOCKET_PING ||
         opcode == CORO_WEBSOCKET_PONG;
}

static int websocket_buffer_limit(const coro_websocket_t *websocket,
                                  size_t *out_limit) {
  size_t payload_limit;

  if (!websocket || !out_limit) return TURBO_EINVAL;
  payload_limit = websocket->max_message_size;
  if (payload_limit < CORO_WEBSOCKET_CONTROL_PAYLOAD_LIMIT) {
    payload_limit = CORO_WEBSOCKET_CONTROL_PAYLOAD_LIMIT;
  }
  if (payload_limit > SIZE_MAX - CORO_WEBSOCKET_FRAME_OVERHEAD) {
    return TURBO_ERANGE;
  }
  *out_limit = payload_limit + CORO_WEBSOCKET_FRAME_OVERHEAD;
  return TURBO_OK;
}

static int websocket_buffer_reserve(coro_websocket_t *websocket,
                                    mem_buffer_t **buffer, size_t needed,
                                    size_t maximum) {
  mem_buffer_t *next;
  size_t current_capacity;
  size_t current_used;
  size_t capacity;

  if (!websocket || !buffer) return TURBO_EINVAL;
  if (needed == 0) return TURBO_OK;
  if (needed > maximum) return TURBO_ERANGE;

  current_capacity = *buffer ? mem_buffer_capacity(*buffer) : 0;
  if (current_capacity >= needed) return TURBO_OK;
  capacity = current_capacity ? current_capacity : 1024U;
  while (capacity < needed) {
    if (capacity > maximum / 2U) {
      capacity = maximum;
      break;
    }
    capacity *= 2U;
  }
  if (capacity > maximum) capacity = maximum;

  next = mem_get_buffer(mem_global(), capacity);
  if (!next) return TURBO_ENOMEM;
  current_used = *buffer ? mem_buffer_used(*buffer) : 0;
  if (current_used > 0) {
    memcpy(mem_buffer_data(next), mem_buffer_const_data(*buffer), current_used);
    mem_set_used(next, current_used);
  }
  if (*buffer) mem_buffer_release(*buffer);
  *buffer = next;
  return TURBO_OK;
}

static int websocket_buffer_append(coro_websocket_t *websocket,
                                   mem_buffer_t **buffer,
                                   const uint8_t *data, size_t len,
                                   size_t maximum) {
  int rc;
  size_t used;

  if (!websocket || !buffer || (len > 0 && !data)) return TURBO_EINVAL;
  if (len == 0) return TURBO_OK;
  used = *buffer ? mem_buffer_used(*buffer) : 0;
  if (len > SIZE_MAX - used) return TURBO_ERANGE;
  rc = websocket_buffer_reserve(websocket, buffer, used + len, maximum);
  if (rc != TURBO_OK) return rc;
  memcpy(mem_buffer_data(*buffer) + used, data, len);
  mem_set_used(*buffer, used + len);
  return TURBO_OK;
}

static void websocket_buffer_consume(mem_buffer_t *buffer, size_t len) {
  size_t used;
  size_t remaining;

  if (!buffer) return;
  used = mem_buffer_used(buffer);
  if (len >= used) {
    mem_set_used(buffer, 0);
    return;
  }
  remaining = used - len;
  memmove(mem_buffer_data(buffer), mem_buffer_data(buffer) + len, remaining);
  mem_set_used(buffer, remaining);
}

static int websocket_send_encoded(coro_websocket_t *websocket, uint8_t opcode,
                                  const void *data, size_t len,
                                  int end_stream) {
  mem_buffer_t *frame;
  uint8_t masking_key[4] = {0, 0, 0, 0};
  int masked;
  size_t header_len;
  size_t total_len;
  int rc;

  if (!websocket || !websocket->transport_ops.send || websocket->closed) {
    return TURBO_EPIPE;
  }
  if (len > 0 && !data) return TURBO_EINVAL;
  masked = websocket->role == CORO_WEBSOCKET_CLIENT;
  if (masked && websocket_generate_masking_key(masking_key) != TURBO_OK) {
    return TURBO_EIO;
  }
  header_len = ws_frame_header_len((uint64_t)len, masked);
  if (header_len > SIZE_MAX - len) return TURBO_ERANGE;
  total_len = header_len + len;
  frame = mem_get_buffer(mem_global(), total_len);
  if (!frame) return TURBO_ENOMEM;
  header_len = ws_frame_build_header((uint8_t *)mem_buffer_data(frame),
                                     opcode, (uint64_t)len, 1, masked,
                                     masked ? masking_key : NULL);
  if (header_len == 0 || (len > 0 && !data)) {
    mem_buffer_release(frame);
    return TURBO_EINVAL;
  }
  if (len > 0) {
    memcpy(mem_buffer_data(frame) + header_len, data, len);
    if (masked) {
      ws_frame_unmask((uint8_t *)mem_buffer_data(frame) + header_len, len,
                      masking_key);
    }
  }
  mem_set_used(frame, total_len);
  ++websocket->callback_depth;
  rc = websocket->transport_ops.send(
      websocket->transport_ops.user_data,
      (const uint8_t *)mem_buffer_const_data(frame), total_len, end_stream);
  --websocket->callback_depth;
  mem_buffer_release(frame);
  if (rc != TURBO_OK) return rc;
  return TURBO_OK;
}

static int websocket_dispatch_message(coro_websocket_t *websocket,
                                      coro_websocket_opcode_t opcode,
                                      const void *data, size_t len) {
  int callback_rc;
  int close_rc;
  const uint8_t close_payload[2] = {
      (uint8_t)(CORO_WEBSOCKET_CLOSE_POLICY_VIOLATION >> 8),
      (uint8_t)(CORO_WEBSOCKET_CLOSE_POLICY_VIOLATION & 0xffU)};

  if (opcode == CORO_WEBSOCKET_TEXT &&
      validate_utf8((const uint8_t *)data, len) != 0) {
    return TURBO_EPROTO;
  }
  if (!websocket->on_message) return TURBO_OK;
  ++websocket->callback_depth;
  callback_rc = websocket->on_message(websocket, opcode, data, len,
                                      websocket->message_user_data);
  --websocket->callback_depth;
  if (callback_rc != 0) {
    close_rc = websocket_send_encoded(
        websocket, CORO_WEBSOCKET_CLOSE, close_payload,
        sizeof(close_payload), 1);
    websocket->closed = 1;
    return close_rc == TURBO_OK ? TURBO_ECANCELED : close_rc;
  }
  return TURBO_OK;
}

static int websocket_dispatch_frame(coro_websocket_t *websocket,
                                    ws_frame_t *frame) {
  size_t payload_len;
  int expected_masked = websocket->role == CORO_WEBSOCKET_SERVER;

  if (!websocket_valid_opcode(frame->opcode) ||
      (frame->masked ? 1 : 0) != expected_masked) {
    return TURBO_EPROTO;
  }
  if (frame->payload_len > (uint64_t)SIZE_MAX) return TURBO_ERANGE;
  payload_len = (size_t)frame->payload_len;
  if (ws_is_control(frame->opcode) &&
      (!frame->fin || payload_len > CORO_WEBSOCKET_CONTROL_PAYLOAD_LIMIT)) {
    return TURBO_EPROTO;
  }
  if (!ws_is_control(frame->opcode) &&
      payload_len > websocket->max_message_size) {
    return TURBO_ERANGE;
  }
  if (frame->masked && payload_len > 0) {
    ws_frame_unmask((uint8_t *)frame->payload, payload_len,
                    frame->masking_key);
  }

  if (frame->opcode == CORO_WEBSOCKET_PING) {
    return websocket_send_encoded(websocket, CORO_WEBSOCKET_PONG,
                                  frame->payload, payload_len, 0);
  }
  if (frame->opcode == CORO_WEBSOCKET_PONG) return TURBO_OK;

  if (frame->opcode == CORO_WEBSOCKET_CLOSE) {
    uint16_t code = 1000;
    if (payload_len == 1U) return TURBO_EPROTO;
      if (payload_len >= 2U) {
      code = (uint16_t)(((uint16_t)frame->payload[0] << 8) |
                        frame->payload[1]);
      if (!websocket_valid_close_code(code)) return TURBO_EPROTO;
      if (payload_len > 2U &&
          validate_utf8(frame->payload + 2U, payload_len - 2U) != 0) {
        return TURBO_EPROTO;
      }
    }
    if (!websocket->closed) {
      int rc = websocket_send_encoded(websocket, CORO_WEBSOCKET_CLOSE,
                                      frame->payload, payload_len, 1);
      if (rc != TURBO_OK) return rc;
      websocket->closed = 1;
    }
    return TURBO_OK;
  }

  if (frame->opcode == CORO_WEBSOCKET_TEXT ||
      frame->opcode == CORO_WEBSOCKET_BINARY) {
    if (websocket->fragmented) return TURBO_EPROTO;
    if (frame->fin) {
      return websocket_dispatch_message(
          websocket, (coro_websocket_opcode_t)frame->opcode,
          frame->payload, payload_len);
    }
    websocket->fragmented = 1;
    websocket->fragment_opcode = frame->opcode;
    return websocket_buffer_append(websocket, &websocket->fragment,
                                   frame->payload, payload_len,
                                   websocket->max_message_size);
  }

  if (!websocket->fragmented) return TURBO_EPROTO;
  {
    int rc = websocket_buffer_append(
        websocket, &websocket->fragment, frame->payload, payload_len,
        websocket->max_message_size);
    if (rc != TURBO_OK) return rc;
  }
  if (frame->fin) {
    int rc = websocket_dispatch_message(
        websocket, (coro_websocket_opcode_t)websocket->fragment_opcode,
        mem_buffer_const_data(websocket->fragment),
        mem_buffer_used(websocket->fragment));
    mem_set_used(websocket->fragment, 0);
    websocket->fragmented = 0;
    websocket->fragment_opcode = 0;
    return rc;
  }
  return TURBO_OK;
}

static int websocket_process_input(coro_websocket_t *websocket) {
  while (websocket->rx && mem_buffer_used(websocket->rx) > 0) {
    size_t needed = 0;
    size_t available = mem_buffer_used(websocket->rx);
    ws_frame_t frame;
    ws_parse_result_t peek = ws_frame_peek_size(
        (const uint8_t *)mem_buffer_const_data(websocket->rx), available,
        &needed);

    if (peek == WS_PARSE_NEED_MORE) return TURBO_OK;
    if (peek != WS_PARSE_OK) return TURBO_EPROTO;
    {
      size_t maximum;
      if (websocket_buffer_limit(websocket, &maximum) != TURBO_OK ||
          needed > maximum) {
        return TURBO_ERANGE;
      }
    }
    if (needed > available) return TURBO_OK;
    if (ws_frame_parse((const uint8_t *)mem_buffer_const_data(websocket->rx),
                       needed, &frame) != WS_PARSE_OK) {
      return TURBO_EPROTO;
    }
    {
      int rc = websocket_dispatch_frame(websocket, &frame);
      if (rc != TURBO_OK) return rc;
    }
    websocket_buffer_consume(websocket->rx, needed);
    if (websocket->closed) return TURBO_OK;
  }
  return TURBO_OK;
}

int coro_websocket_options_init(coro_websocket_options_t *options,
                                size_t options_size) {
  if (!options || options_size < CORO_WEBSOCKET_OPTIONS_V1_SIZE) {
    return TURBO_EINVAL;
  }
  options->size = options_size;
  options->role = CORO_WEBSOCKET_CLIENT;
  options->transport = CORO_WEBSOCKET_TRANSPORT_HTTP1;
  options->max_message_size = CORO_WEBSOCKET_DEFAULT_MAX_MESSAGE_SIZE;
  options->on_message = NULL;
  options->user_data = NULL;
  return TURBO_OK;
}

int coro_websocket_create(const coro_websocket_options_t *options,
                          const coro_websocket_transport_ops_t *transport,
                          coro_websocket_t **out_websocket) {
  coro_websocket_t *websocket;

  if (!out_websocket) return TURBO_EINVAL;
  *out_websocket = NULL;
  if (!options || options->size < CORO_WEBSOCKET_OPTIONS_V1_SIZE ||
      !transport || transport->size < CORO_WEBSOCKET_TRANSPORT_OPS_V1_SIZE ||
      !transport->send ||
      (options->role != CORO_WEBSOCKET_CLIENT &&
       options->role != CORO_WEBSOCKET_SERVER) ||
      options->transport < CORO_WEBSOCKET_TRANSPORT_HTTP1 ||
      options->transport > CORO_WEBSOCKET_TRANSPORT_HTTP3 ||
      options->max_message_size == 0) {
    return TURBO_EINVAL;
  }
  if (options->max_message_size >
      SIZE_MAX - CORO_WEBSOCKET_FRAME_OVERHEAD) {
    return TURBO_ERANGE;
  }

  websocket = (coro_websocket_t *)calloc(1, sizeof(*websocket));
  if (!websocket) return TURBO_ENOMEM;
  websocket->role = options->role;
  websocket->transport = options->transport;
  websocket->max_message_size = options->max_message_size;
  websocket->on_message = options->on_message;
  websocket->message_user_data = options->user_data;
  websocket->transport_ops = *transport;
  *out_websocket = websocket;
  return TURBO_OK;
}

void coro_websocket_destroy(coro_websocket_t *websocket) {
  if (!websocket) return;
  websocket->destroy_requested = 1;
  websocket->closed = 1;
  websocket_maybe_release(websocket);
}

int coro_websocket_feed(coro_websocket_t *websocket, const void *data,
                        size_t len) {
  const uint8_t *input = (const uint8_t *)data;
  size_t input_remaining = len;
  size_t maximum;
  int rc;

  if (!websocket || (len > 0 && !data)) return TURBO_EINVAL;
  rc = websocket_operation_begin(websocket);
  if (rc != TURBO_OK) return rc;
  if (websocket->closed) {
    rc = TURBO_EPIPE;
    goto done;
  }
  rc = websocket_buffer_limit(websocket, &maximum);
  if (rc != TURBO_OK) goto done;

  while (input_remaining > 0) {
    size_t used = websocket->rx ? mem_buffer_used(websocket->rx) : 0;
    size_t available;
    size_t chunk;

    if (used > maximum) {
      websocket->closed = 1;
      rc = TURBO_ERANGE;
      goto done;
    }
    available = maximum - used;
    if (available == 0) {
      rc = websocket_process_input(websocket);
      if (rc != TURBO_OK) {
        websocket->closed = 1;
        goto done;
      }
      used = websocket->rx ? mem_buffer_used(websocket->rx) : 0;
      if (used == maximum) {
        websocket->closed = 1;
        rc = TURBO_ERANGE;
        goto done;
      }
      available = maximum - used;
    }

    chunk = input_remaining < available ? input_remaining : available;
    rc = websocket_buffer_append(websocket, &websocket->rx, input, chunk,
                                 maximum);
    if (rc != TURBO_OK) {
      websocket->closed = 1;
      goto done;
    }
    input += chunk;
    input_remaining -= chunk;

    rc = websocket_process_input(websocket);
    if (rc != TURBO_OK) {
      websocket->closed = 1;
      goto done;
    }
    if (websocket->closed) {
      rc = TURBO_OK;
      goto done;
    }
  }

  rc = websocket_process_input(websocket);
  if (rc != TURBO_OK) websocket->closed = 1;

done:
  websocket_operation_end(websocket);
  return rc;
}

int coro_websocket_send(coro_websocket_t *websocket,
                        coro_websocket_opcode_t opcode, const void *data,
                        size_t len) {
  int rc;

  if (!websocket || (len > 0 && !data)) return TURBO_EINVAL;
  rc = websocket_operation_begin(websocket);
  if (rc != TURBO_OK) return rc;
  if (websocket->closed) {
    rc = TURBO_EPIPE;
    goto done;
  }
  if (opcode != CORO_WEBSOCKET_TEXT && opcode != CORO_WEBSOCKET_BINARY &&
      opcode != CORO_WEBSOCKET_PING && opcode != CORO_WEBSOCKET_PONG) {
    rc = TURBO_EINVAL;
    goto done;
  }
  if ((opcode == CORO_WEBSOCKET_PING || opcode == CORO_WEBSOCKET_PONG) &&
      len > CORO_WEBSOCKET_CONTROL_PAYLOAD_LIMIT) {
    rc = TURBO_ERANGE;
    goto done;
  }
  if ((opcode == CORO_WEBSOCKET_TEXT || opcode == CORO_WEBSOCKET_BINARY) &&
      len > websocket->max_message_size) {
    rc = TURBO_ERANGE;
    goto done;
  }
  if (opcode == CORO_WEBSOCKET_TEXT &&
      validate_utf8((const uint8_t *)data, len) != 0) {
    rc = TURBO_EINVAL;
    goto done;
  }
  rc = websocket_send_encoded(websocket, (uint8_t)opcode, data, len, 0);

done:
  websocket_operation_end(websocket);
  return rc;
}

int coro_websocket_close(coro_websocket_t *websocket, uint16_t code,
                         const char *reason) {
  uint8_t payload[125];
  size_t reason_len;
  int rc;

  if (!websocket) return TURBO_EINVAL;
  rc = websocket_operation_begin(websocket);
  if (rc != TURBO_OK) return rc;
  if (websocket->closed) {
    rc = TURBO_EPIPE;
    goto done;
  }
  reason_len = reason ? strlen(reason) : 0;
  if (!websocket_valid_close_code(code) || reason_len > sizeof(payload) - 2U) {
    rc = TURBO_EINVAL;
    goto done;
  }
  if (reason_len > 0 &&
      validate_utf8((const uint8_t *)reason, reason_len) != 0) {
    rc = TURBO_EINVAL;
    goto done;
  }
  payload[0] = (uint8_t)(code >> 8);
  payload[1] = (uint8_t)(code & 0xffU);
  if (reason_len > 0) memcpy(payload + 2, reason, reason_len);
  rc = websocket_send_encoded(websocket, CORO_WEBSOCKET_CLOSE, payload,
                              reason_len + 2U, 1);
  if (rc == TURBO_OK) websocket->closed = 1;

done:
  websocket_operation_end(websocket);
  return rc;
}

void coro_websocket_mark_closed(coro_websocket_t *websocket) {
  if (websocket) websocket->closed = 1;
}

int coro_websocket_is_open(const coro_websocket_t *websocket) {
  return websocket && !websocket->closed && !websocket->destroy_requested;
}

int coro_websocket_input_complete(const coro_websocket_t *websocket) {
  return websocket && !websocket->closed && !websocket->destroy_requested &&
         !websocket->fragmented &&
         (!websocket->rx || mem_buffer_used(websocket->rx) == 0);
}
