/**
 * @file test_websocket_session.c
 * @brief CoroNet transport-independent WebSocket session tests.
 */

#include "CoroNet/turbo_coro_websocket.h"
#include "websocket_frame_parser.h"
#include "tinytest.h"

#include <stdint.h>
#include <string.h>

enum {
  TEST_MAX_CAPTURE_FRAMES = 8,
  TEST_CAPTURE_FRAME_SIZE = 256,
  TEST_MAX_MESSAGES = 8,
  TEST_MESSAGE_SIZE = 32
};

typedef struct websocket_transport_capture_s {
  uint8_t frames[TEST_MAX_CAPTURE_FRAMES][TEST_CAPTURE_FRAME_SIZE];
  size_t lengths[TEST_MAX_CAPTURE_FRAMES];
  int end_stream[TEST_MAX_CAPTURE_FRAMES];
  size_t count;
} websocket_transport_capture_t;

typedef struct websocket_message_capture_s {
  coro_websocket_opcode_t opcodes[TEST_MAX_MESSAGES];
  uint8_t messages[TEST_MAX_MESSAGES][TEST_MESSAGE_SIZE];
  size_t lengths[TEST_MAX_MESSAGES];
  size_t count;
} websocket_message_capture_t;

static int capture_send(void *user_data, const uint8_t *data, size_t len,
                        int end_stream) {
  websocket_transport_capture_t *capture =
      (websocket_transport_capture_t *)user_data;

  if (!capture || !data || len == 0 || capture->count >= TEST_MAX_CAPTURE_FRAMES ||
      len > TEST_CAPTURE_FRAME_SIZE) {
    return TURBO_EINVAL;
  }
  memcpy(capture->frames[capture->count], data, len);
  capture->lengths[capture->count] = len;
  capture->end_stream[capture->count] = end_stream;
  capture->count++;
  return TURBO_OK;
}

static int capture_message(coro_websocket_t *websocket,
                           coro_websocket_opcode_t opcode, const void *data,
                           size_t len, void *user_data) {
  websocket_message_capture_t *capture =
      (websocket_message_capture_t *)user_data;

  (void)websocket;
  if (!capture || (len > 0 && !data) || capture->count >= TEST_MAX_MESSAGES ||
      len > TEST_MESSAGE_SIZE) {
    return -1;
  }
  capture->opcodes[capture->count] = opcode;
  capture->lengths[capture->count] = len;
  if (len > 0) memcpy(capture->messages[capture->count], data, len);
  capture->count++;
  return 0;
}

static int reject_message(coro_websocket_t *websocket,
                          coro_websocket_opcode_t opcode, const void *data,
                          size_t len, void *user_data) {
  (void)websocket;
  (void)opcode;
  (void)data;
  (void)len;
  (void)user_data;
  return -1;
}

static int destroy_message(coro_websocket_t *websocket,
                           coro_websocket_opcode_t opcode, const void *data,
                           size_t len, void *user_data) {
  (void)opcode;
  (void)data;
  (void)len;
  (void)user_data;
  coro_websocket_destroy(websocket);
  return 0;
}

typedef struct websocket_destroy_transport_s {
  coro_websocket_t *websocket;
  int called;
} websocket_destroy_transport_t;

static int destroy_send(void *user_data, const uint8_t *data, size_t len,
                        int end_stream) {
  websocket_destroy_transport_t *context =
      (websocket_destroy_transport_t *)user_data;

  if (!context || !data || len == 0) return TURBO_EINVAL;
  (void)end_stream;
  context->called++;
  coro_websocket_destroy(context->websocket);
  context->websocket = NULL;
  return TURBO_OK;
}

static size_t build_frame(uint8_t *output, size_t capacity, uint8_t opcode,
                          int fin, int masked, const uint8_t *payload,
                          size_t payload_len) {
  static const uint8_t mask[4] = {1, 2, 3, 4};
  uint8_t header[14];
  size_t header_len;
  size_t i;

  header_len = ws_frame_build_header(header, opcode, payload_len, fin, masked,
                                     masked ? mask : NULL);
  if (header_len > capacity || payload_len > capacity - header_len) return 0;
  memcpy(output, header, header_len);
  for (i = 0; i < payload_len; ++i) {
    output[header_len + i] = payload[i] ^ (masked ? mask[i % 4U] : 0U);
  }
  return header_len + payload_len;
}

static int create_session(coro_websocket_role_t role,
                          coro_websocket_transport_t transport,
                          size_t max_message_size,
                          coro_websocket_message_cb on_message,
                          void *message_user_data,
                          websocket_transport_capture_t *transport_capture,
                          coro_websocket_t **out_websocket) {
  coro_websocket_options_t options;
  coro_websocket_transport_ops_t transport_ops;
  int rc;

  rc = coro_websocket_options_init(&options, sizeof(options));
  if (rc != TURBO_OK) return rc;
  options.role = role;
  options.transport = transport;
  options.max_message_size = max_message_size;
  options.on_message = on_message;
  options.user_data = message_user_data;
  memset(&transport_ops, 0, sizeof(transport_ops));
  transport_ops.size = sizeof(transport_ops);
  transport_ops.send = capture_send;
  transport_ops.user_data = transport_capture;
  return coro_websocket_create(&options, &transport_ops, out_websocket);
}

spec("websocket_session") {
  it("initializes the ABI and accepts HTTP/1, HTTP/2, and HTTP/3 kinds") {
    coro_websocket_options_t options;
    coro_websocket_transport_ops_t transport;
    websocket_transport_capture_t capture;
    coro_websocket_t *websocket = NULL;
    coro_websocket_transport_t kind;

    memset(&options, 0, sizeof(options));
    check_equal(coro_websocket_options_init(
                     &options, CORO_WEBSOCKET_OPTIONS_V1_SIZE - 1U),
                 TURBO_EINVAL);
    check_equal(coro_websocket_options_init(&options, sizeof(options)),
                 TURBO_OK);
    check_equal(options.role, CORO_WEBSOCKET_CLIENT);
    check_equal(options.transport, CORO_WEBSOCKET_TRANSPORT_HTTP1);
    check_equal(options.max_message_size,
                  CORO_WEBSOCKET_DEFAULT_MAX_MESSAGE_SIZE);

    memset(&capture, 0, sizeof(capture));
    memset(&transport, 0, sizeof(transport));
    transport.size = CORO_WEBSOCKET_TRANSPORT_OPS_V1_SIZE - 1U;
    transport.send = capture_send;
    transport.user_data = &capture;
    check_equal(coro_websocket_create(&options, &transport, &websocket),
                 TURBO_EINVAL);
    check_null(websocket);

    for (kind = CORO_WEBSOCKET_TRANSPORT_HTTP1;
         kind <= CORO_WEBSOCKET_TRANSPORT_HTTP3; ++kind) {
      memset(&capture, 0, sizeof(capture));
      check_equal(create_session(CORO_WEBSOCKET_SERVER, kind, 16, NULL, NULL,
                                  &capture, &websocket),
                   TURBO_OK);
      check_not_null(websocket);
      coro_websocket_destroy(websocket);
      websocket = NULL;
    }
  }

  it("applies RFC 6455 masking according to the session role") {
    websocket_transport_capture_t capture;
    coro_websocket_t *websocket = NULL;
    ws_frame_t frame;

    memset(&capture, 0, sizeof(capture));
    check_equal(create_session(CORO_WEBSOCKET_SERVER,
                                CORO_WEBSOCKET_TRANSPORT_HTTP2, 32, NULL, NULL,
                                &capture, &websocket),
                 TURBO_OK);
    check_equal(coro_websocket_send(websocket, CORO_WEBSOCKET_TEXT, "server",
                                     6),
                 TURBO_OK);
    check_equal(ws_frame_parse(capture.frames[0], capture.lengths[0], &frame),
                 WS_PARSE_OK);
    check_equal(frame.masked, 0);
    coro_websocket_destroy(websocket);

    memset(&capture, 0, sizeof(capture));
    check_equal(create_session(CORO_WEBSOCKET_CLIENT,
                                CORO_WEBSOCKET_TRANSPORT_HTTP1, 32, NULL, NULL,
                                &capture, &websocket),
                 TURBO_OK);
    check_equal(coro_websocket_send(websocket, CORO_WEBSOCKET_TEXT, "client",
                                     6),
                 TURBO_OK);
    check_equal(ws_frame_parse(capture.frames[0], capture.lengths[0], &frame),
                 WS_PARSE_OK);
    check_equal(frame.masked, 1);
    coro_websocket_destroy(websocket);
  }

  it("handles half frames, fragmented messages, ping/pong, and close") {
    static const uint8_t first[] = {'h', 'e'};
    static const uint8_t second[] = {'l', 'l', 'o'};
    static const uint8_t ping[] = {'?'};
    static const uint8_t close_payload[] = {0x03, 0xe8};
    uint8_t frame_one[64];
    uint8_t frame_two[64];
    uint8_t frame_ping[64];
    uint8_t frame_close[64];
    size_t len_one;
    size_t len_two;
    size_t len_ping;
    size_t len_close;
    websocket_message_capture_t messages;
    websocket_transport_capture_t transport;
    coro_websocket_t *websocket = NULL;
    ws_frame_t parsed;

    memset(&messages, 0, sizeof(messages));
    memset(&transport, 0, sizeof(transport));
    check_equal(create_session(CORO_WEBSOCKET_SERVER,
                                CORO_WEBSOCKET_TRANSPORT_HTTP1, 16,
                                capture_message, &messages, &transport,
                                &websocket),
                 TURBO_OK);
    len_one = build_frame(frame_one, sizeof(frame_one), CORO_WEBSOCKET_TEXT, 0,
                          1, first, sizeof(first));
    len_two = build_frame(frame_two, sizeof(frame_two),
                          CORO_WEBSOCKET_CONTINUATION, 1, 1, second,
                          sizeof(second));
    len_ping = build_frame(frame_ping, sizeof(frame_ping), CORO_WEBSOCKET_PING,
                           1, 1, ping, sizeof(ping));
    len_close = build_frame(frame_close, sizeof(frame_close),
                            CORO_WEBSOCKET_CLOSE, 1, 1, close_payload,
                            sizeof(close_payload));
    check(len_one > 0 && len_two > 0 && len_ping > 0 && len_close > 0);

    check_equal(coro_websocket_feed(websocket, frame_one, 1), TURBO_OK);
    check_equal(coro_websocket_input_complete(websocket), 0);
    check_equal(coro_websocket_feed(websocket, frame_one + 1, len_one - 1),
                 TURBO_OK);
    check_equal(coro_websocket_feed(websocket, frame_two, len_two), TURBO_OK);
    check_equal(messages.count, 1);
    check_equal(messages.opcodes[0], CORO_WEBSOCKET_TEXT);
    check_equal(messages.lengths[0], 5);
    check_equal(messages.messages[0], "hello", 5);
    check_equal(coro_websocket_input_complete(websocket), 1);

    check_equal(coro_websocket_feed(websocket, frame_ping, len_ping), TURBO_OK);
    check_equal(transport.count, 1);
    check_equal(ws_frame_parse(transport.frames[0], transport.lengths[0],
                                &parsed),
                 WS_PARSE_OK);
    check_equal(parsed.opcode, CORO_WEBSOCKET_PONG);
    check_equal(parsed.masked, 0);
    check_equal(parsed.payload, ping, sizeof(ping));

    check_equal(coro_websocket_feed(websocket, frame_close, len_close),
                 TURBO_OK);
    check_equal(transport.count, 2);
    check_equal(transport.end_stream[1], 1);
    check_equal(coro_websocket_is_open(websocket), 0);
    coro_websocket_destroy(websocket);
  }

  it("processes multiple frames in one bounded input and rejects oversize data") {
    static const uint8_t one[] = {'o', 'n', 'e'};
    static const uint8_t two[] = {'t', 'w', 'o'};
    static const uint8_t too_large[] = {'f', 'o', 'u', 'r'};
    uint8_t combined[64];
    uint8_t oversize[64];
    size_t one_len;
    size_t two_len;
    size_t oversize_len;
    websocket_message_capture_t messages;
    websocket_transport_capture_t transport;
    coro_websocket_t *websocket = NULL;

    memset(&messages, 0, sizeof(messages));
    memset(&transport, 0, sizeof(transport));
    check_equal(create_session(CORO_WEBSOCKET_SERVER,
                                CORO_WEBSOCKET_TRANSPORT_HTTP2, 3,
                                capture_message, &messages, &transport,
                                &websocket),
                 TURBO_OK);
    one_len = build_frame(combined, sizeof(combined), CORO_WEBSOCKET_TEXT, 1,
                          1, one, sizeof(one));
    two_len = build_frame(combined + one_len, sizeof(combined) - one_len,
                          CORO_WEBSOCKET_TEXT, 1, 1, two, sizeof(two));
    check(one_len > 0 && two_len > 0);
    check_equal(coro_websocket_feed(websocket, combined, one_len + two_len),
                 TURBO_OK);
    check_equal(messages.count, 2);
    check_equal(messages.messages[0], one, sizeof(one));
    check_equal(messages.messages[1], two, sizeof(two));
    coro_websocket_destroy(websocket);

    memset(&messages, 0, sizeof(messages));
    memset(&transport, 0, sizeof(transport));
    check_equal(create_session(CORO_WEBSOCKET_SERVER,
                                CORO_WEBSOCKET_TRANSPORT_HTTP2, 3,
                                capture_message, &messages, &transport,
                                &websocket),
                 TURBO_OK);
    oversize_len = build_frame(oversize, sizeof(oversize), CORO_WEBSOCKET_TEXT,
                               1, 1, too_large, sizeof(too_large));
    check(oversize_len > 0);
    check_equal(coro_websocket_feed(websocket, oversize, oversize_len),
                 TURBO_ERANGE);
    check_equal(coro_websocket_is_open(websocket), 0);
    check_equal(messages.count, 0);
    coro_websocket_destroy(websocket);
  }

  it("enforces the message limit across fragments and rejects bad masks") {
    static const uint8_t first[] = {'a', 'b', 'c'};
    static const uint8_t second[] = {'d', 'e'};
    uint8_t first_frame[64];
    uint8_t second_frame[64];
    uint8_t unmasked_frame[64];
    size_t first_len;
    size_t second_len;
    size_t unmasked_len;
    websocket_message_capture_t messages;
    websocket_transport_capture_t transport;
    coro_websocket_t *websocket = NULL;

    memset(&messages, 0, sizeof(messages));
    memset(&transport, 0, sizeof(transport));
    check_equal(create_session(CORO_WEBSOCKET_SERVER,
                                CORO_WEBSOCKET_TRANSPORT_HTTP2, 4,
                                capture_message, &messages, &transport,
                                &websocket),
                 TURBO_OK);
    first_len = build_frame(first_frame, sizeof(first_frame),
                            CORO_WEBSOCKET_TEXT, 0, 1, first, sizeof(first));
    second_len = build_frame(second_frame, sizeof(second_frame),
                             CORO_WEBSOCKET_CONTINUATION, 1, 1, second,
                             sizeof(second));
    check_equal(coro_websocket_feed(websocket, first_frame, first_len),
                 TURBO_OK);
    check_equal(coro_websocket_feed(websocket, second_frame, second_len),
                 TURBO_ERANGE);
    check_equal(coro_websocket_is_open(websocket), 0);
    check_equal(messages.count, 0);
    coro_websocket_destroy(websocket);

    memset(&messages, 0, sizeof(messages));
    memset(&transport, 0, sizeof(transport));
    check_equal(create_session(CORO_WEBSOCKET_SERVER,
                                CORO_WEBSOCKET_TRANSPORT_HTTP2, 16,
                                capture_message, &messages, &transport,
                                &websocket),
                 TURBO_OK);
    unmasked_len = build_frame(unmasked_frame, sizeof(unmasked_frame),
                               CORO_WEBSOCKET_TEXT, 1, 0, first, sizeof(first));
    check_equal(coro_websocket_feed(websocket, unmasked_frame, unmasked_len),
                 TURBO_EPROTO);
    check_equal(coro_websocket_is_open(websocket), 0);
    coro_websocket_destroy(websocket);
  }

  it("defers destruction requested by message and transport callbacks") {
    uint8_t frame[64];
    size_t frame_len;
    websocket_transport_capture_t capture;
    websocket_destroy_transport_t destroy_transport;
    coro_websocket_options_t options;
    coro_websocket_transport_ops_t transport_ops;
    coro_websocket_t *websocket = NULL;

    memset(&capture, 0, sizeof(capture));
    check_equal(create_session(CORO_WEBSOCKET_SERVER,
                                CORO_WEBSOCKET_TRANSPORT_HTTP1, 16,
                                destroy_message, NULL, &capture, &websocket),
                 TURBO_OK);
    frame_len = build_frame(frame, sizeof(frame), CORO_WEBSOCKET_TEXT, 1, 1,
                            (const uint8_t *)"message", 7);
    check(frame_len > 0);
    check_equal(coro_websocket_feed(websocket, frame, frame_len), TURBO_OK);

    memset(&destroy_transport, 0, sizeof(destroy_transport));
    check_equal(coro_websocket_options_init(&options, sizeof(options)),
                 TURBO_OK);
    options.role = CORO_WEBSOCKET_CLIENT;
    options.max_message_size = 16;
    memset(&transport_ops, 0, sizeof(transport_ops));
    transport_ops.size = sizeof(transport_ops);
    transport_ops.send = destroy_send;
    transport_ops.user_data = &destroy_transport;
    check_equal(coro_websocket_create(&options, &transport_ops, &websocket),
                 TURBO_OK);
    destroy_transport.websocket = websocket;
    check_equal(coro_websocket_close(websocket, CORO_WEBSOCKET_CLOSE_NORMAL,
                                      NULL),
                 TURBO_OK);
    check_equal(destroy_transport.called, 1);
  }

  it("sends a policy close when the message callback rejects input") {
    static const uint8_t payload[] = {'r', 'e', 'j', 'e', 'c', 't'};
    uint8_t frame[64];
    size_t frame_len;
    websocket_message_capture_t messages;
    websocket_transport_capture_t transport;
    ws_frame_t parsed;
    coro_websocket_t *websocket = NULL;

    memset(&messages, 0, sizeof(messages));
    memset(&transport, 0, sizeof(transport));
    check_equal(create_session(CORO_WEBSOCKET_SERVER,
                                CORO_WEBSOCKET_TRANSPORT_HTTP1, 16,
                                reject_message, &messages, &transport,
                                &websocket),
                 TURBO_OK);
    frame_len = build_frame(frame, sizeof(frame), CORO_WEBSOCKET_TEXT, 1, 1,
                            payload, sizeof(payload));
    check(frame_len > 0);
    check_equal(coro_websocket_feed(websocket, frame, frame_len),
                 TURBO_ECANCELED);
    check_equal(transport.count, 1);
    check_equal(transport.end_stream[0], 1);
    check_equal(ws_frame_parse(transport.frames[0], transport.lengths[0],
                                &parsed),
                 WS_PARSE_OK);
    check_equal(parsed.opcode, CORO_WEBSOCKET_CLOSE);
    check_equal(parsed.payload_len, 2);
    check_equal(((int)parsed.payload[0] << 8) | parsed.payload[1],
                 CORO_WEBSOCKET_CLOSE_POLICY_VIOLATION);
    check_equal(coro_websocket_is_open(websocket), 0);
    coro_websocket_destroy(websocket);
  }

  it("rejects invalid close codes and UTF-8") {
    static const uint8_t invalid_utf8[] = {0xc0, 0x80};
    static const char invalid_reason[] = "\xc0\x80";
    uint8_t invalid_close[64];
    uint8_t invalid_text[64];
    uint8_t close_payload[] = {0x03, 0xf4};
    size_t invalid_close_len;
    size_t invalid_text_len;
    websocket_transport_capture_t transport;
    coro_websocket_t *websocket = NULL;

    memset(&transport, 0, sizeof(transport));
    check_equal(create_session(CORO_WEBSOCKET_CLIENT,
                                CORO_WEBSOCKET_TRANSPORT_HTTP1, 16, NULL, NULL,
                                &transport, &websocket),
                 TURBO_OK);
    check_equal(coro_websocket_send(websocket, CORO_WEBSOCKET_TEXT,
                                     invalid_utf8, sizeof(invalid_utf8)),
                 TURBO_EINVAL);
    check_equal(coro_websocket_close(websocket, 1012, NULL), TURBO_EINVAL);
    check_equal(coro_websocket_close(websocket, CORO_WEBSOCKET_CLOSE_NORMAL,
                                      invalid_reason),
                 TURBO_EINVAL);
    check_equal(transport.count, 0);
    coro_websocket_destroy(websocket);

    memset(&transport, 0, sizeof(transport));
    check_equal(create_session(CORO_WEBSOCKET_SERVER,
                                CORO_WEBSOCKET_TRANSPORT_HTTP1, 16, NULL, NULL,
                                &transport, &websocket),
                 TURBO_OK);
    invalid_close_len = build_frame(invalid_close, sizeof(invalid_close),
                                    CORO_WEBSOCKET_CLOSE, 1, 1, close_payload,
                                    sizeof(close_payload));
    check(invalid_close_len > 0);
    check_equal(coro_websocket_feed(websocket, invalid_close,
                                     invalid_close_len),
                 TURBO_EPROTO);
    check_equal(coro_websocket_is_open(websocket), 0);
    coro_websocket_destroy(websocket);

    memset(&transport, 0, sizeof(transport));
    check_equal(create_session(CORO_WEBSOCKET_SERVER,
                                CORO_WEBSOCKET_TRANSPORT_HTTP1, 16, NULL, NULL,
                                &transport, &websocket),
                 TURBO_OK);
    invalid_text_len = build_frame(invalid_text, sizeof(invalid_text),
                                   CORO_WEBSOCKET_TEXT, 1, 1, invalid_utf8,
                                   sizeof(invalid_utf8));
    check(invalid_text_len > 0);
    check_equal(coro_websocket_feed(websocket, invalid_text,
                                     invalid_text_len),
                 TURBO_EPROTO);
    check_equal(coro_websocket_is_open(websocket), 0);
    coro_websocket_destroy(websocket);
  }
}
