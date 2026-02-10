/**
 * @file websocket_frame_parser.h
 * @brief WebSocket Frame Parser - Zero-copy, single-call API
 */

#ifndef WEBSOCKET_FRAME_PARSER_H
#define WEBSOCKET_FRAME_PARSER_H

#include <stddef.h>
#include <stdint.h> 

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parse result codes
 */
typedef enum {
    WS_PARSE_OK = 0,
    WS_PARSE_NEED_MORE,
    WS_PARSE_INVALID_OPCODE,
    WS_PARSE_INVALID_RSV,
    WS_PARSE_CONTROL_TOO_LARGE,
    WS_PARSE_FRAGMENTED_CONTROL,
    WS_PARSE_INVALID_LENGTH,
} ws_parse_result_t;

/**
 * @brief Parsed WebSocket frame (zero-copy)
 */
typedef struct {
    uint8_t  fin;           /* FIN bit */
    uint8_t  opcode;        /* Frame opcode */
    uint8_t  masked;        /* MASK bit */
    uint8_t  masking_key[4];/* Masking key (if masked) */
    uint64_t payload_len;   /* Payload length */
    const uint8_t *payload; /* Pointer to payload (zero-copy) */
    size_t   header_len;    /* Total header length consumed */
} ws_frame_t;

/**
 * @brief Peek minimum bytes needed for complete frame
 * @param data   Input buffer
 * @param len    Buffer length
 * @param out_needed Output: total bytes needed (header + payload)
 * @return WS_PARSE_OK if calculable, WS_PARSE_NEED_MORE if insufficient header
 */
ws_parse_result_t ws_frame_peek_size(const uint8_t *data, size_t len, size_t *out_needed);

/**
 * @brief Parse WebSocket frame - zero-copy
 * @param data   Input buffer containing complete frame
 * @param len    Buffer length
 * @param frame  Output frame structure
 * @return WS_PARSE_OK on success
 *
 * NOTE: frame->payload points directly into data buffer.
 * Caller must ensure data outlives frame usage.
 * Call ws_frame_unmask() if frame->masked is set.
 */
ws_parse_result_t ws_frame_parse(const uint8_t *data, size_t len, ws_frame_t *frame);

/**
 * @brief Unmask payload in-place (optimized, 4-byte bulk XOR)
 * @param payload Payload data to unmask (modified in place)
 * @param len     Payload length
 * @param masking_key 4-byte masking key
 */
void ws_frame_unmask(uint8_t *payload, size_t len, const uint8_t masking_key[4]);

/**
 * @brief Calculate header length for building frames
 * @param payload_len Payload length
 * @param masked      Whether frame will be masked
 * @return Header length in bytes (2, 4, or 10 + 4 if masked)
 */
size_t ws_frame_header_len(uint64_t payload_len, int masked);

/**
 * @brief Check if opcode is control frame (CLOSE, PING, PONG)
 */
static inline int ws_is_control(uint8_t opcode) {
    return opcode >= 0x8;
}

/**
 * @brief Check if opcode is data frame (CONTINUATION, TEXT, BINARY)
 */
static inline int ws_is_data(uint8_t opcode) {
    return opcode <= 0x2;
}

#ifdef __cplusplus
}
#endif

#endif /* WEBSOCKET_FRAME_PARSER_H */
