#include "tinytest.h"
#include "websocket_handshake_parser.h"

#include <string.h>

static int parse_websocket_request(const char *request) {
  websocket_handshake_parser_t parser;
  websocket_handshake_token_value_t value;
  int token;
  int valid;

  websocket_handshake_parser_init(&parser, WEBSOCKET_HANDSHAKE_MODE_REQUEST,
                                  request, strlen(request));
  do {
    token = websocket_handshake_parser_scan(&parser, &value);
  } while (token == WEBSOCKET_HANDSHAKE_TOKEN_REQUEST_LINE ||
           token == WEBSOCKET_HANDSHAKE_TOKEN_RESPONSE_LINE ||
           token == WEBSOCKET_HANDSHAKE_TOKEN_HEADER ||
           token == WEBSOCKET_HANDSHAKE_TOKEN_BODY);
  valid = token == WEBSOCKET_HANDSHAKE_TOKEN_END &&
          websocket_handshake_validate(&parser) == 0 &&
          websocket_handshake_is_websocket_request(&parser);
  websocket_handshake_parser_destroy(&parser);
  return valid;
}

spec("websocket_handshake") {
  describe("Connection header tokens") {
    it("accepts Upgrade as an exact comma-separated token") {
      static const char request[] =
          "GET /chat HTTP/1.1\r\n"
          "Host: localhost\r\n"
          "Upgrade: websocket\r\n"
          "Connection: keep-alive, Upgrade\r\n"
          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
          "Sec-WebSocket-Version: 13\r\n"
          "\r\n";

      check(parse_websocket_request(request));
    }

    it("rejects Upgrade embedded inside another token") {
      static const char request[] =
          "GET /chat HTTP/1.1\r\n"
          "Host: localhost\r\n"
          "Upgrade: websocket\r\n"
          "Connection: keep-upgrade-alive\r\n"
          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
          "Sec-WebSocket-Version: 13\r\n"
          "\r\n";

      check(!parse_websocket_request(request));
    }
  }
}
