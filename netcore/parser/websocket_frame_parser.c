/**
 * @file websocket_frame_parser.c
 * @brief WebSocket Frame Parser - Zero-copy implementation
 */

#include "websocket_frame_parser.h"
#include <limits.h>
#include <string.h>

ws_parse_result_t ws_frame_peek_size(const uint8_t *data, size_t len, size_t *out_needed) {
    if (len < 2)
        return WS_PARSE_NEED_MORE;

    size_t header_len = 2;
    int masked = (data[1] & 0x80) != 0;
    uint8_t len_byte = data[1] & 0x7F;
    uint64_t payload_len;

    if (len_byte <= 125) {
        payload_len = len_byte;
    } else if (len_byte == 126) {
        if (len < 4) return WS_PARSE_NEED_MORE;
        payload_len = ((uint64_t)data[2] << 8) | data[3];
        header_len = 4;
    } else { /* 127 */
        if (len < 10) return WS_PARSE_NEED_MORE;
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | data[2 + i];
        }
        header_len = 10;
    }

    if (masked) header_len += 4;

    if (payload_len > (uint64_t)(SIZE_MAX - header_len)) {
        return WS_PARSE_INVALID_LENGTH;
    }
    *out_needed = header_len + (size_t)payload_len;
    return WS_PARSE_OK;
}

ws_parse_result_t ws_frame_parse(const uint8_t *data, size_t len, ws_frame_t *frame) {
    if (len < 2)
        return WS_PARSE_NEED_MORE;

    /* Parse first byte */
    uint8_t b0 = data[0];
    frame->fin = (b0 & 0x80) != 0;
    uint8_t rsv = (b0 & 0x70) >> 4;
    frame->opcode = b0 & 0x0F;

    /* Validate RSV bits */
    if (rsv != 0)
        return WS_PARSE_INVALID_RSV;

    /* Parse second byte */
    uint8_t b1 = data[1];
    frame->masked = (b1 & 0x80) != 0;
    uint8_t len_byte = b1 & 0x7F;

    size_t header_len = 2;
    uint64_t payload_len;

    /* Determine payload length */
    if (len_byte <= 125) {
        payload_len = len_byte;
    } else if (len_byte == 126) {
        if (len < 4) return WS_PARSE_NEED_MORE;
        payload_len = ((uint64_t)data[2] << 8) | data[3];
        header_len = 4;
    } else { /* 127 */
        if (len < 10) return WS_PARSE_NEED_MORE;
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | data[2 + i];
        }
        header_len = 10;
    }

    /* Extract masking key if present */
    if (frame->masked) {
        if (len < header_len + 4) return WS_PARSE_NEED_MORE;
        memcpy(frame->masking_key, data + header_len, 4);
        header_len += 4;
    } else {
        frame->masking_key[0] = 0;
        frame->masking_key[1] = 0;
        frame->masking_key[2] = 0;
        frame->masking_key[3] = 0;
    }

    /* Check we have complete frame */
    if (payload_len > (uint64_t)(SIZE_MAX - header_len)) {
        return WS_PARSE_INVALID_LENGTH;
    }
    if (len < header_len + (size_t)payload_len)
        return WS_PARSE_NEED_MORE;

    /* Validate control frames */
    if (ws_is_control(frame->opcode)) {
        if (payload_len > 125)
            return WS_PARSE_CONTROL_TOO_LARGE;
        if (!frame->fin)
            return WS_PARSE_FRAGMENTED_CONTROL;
    }

    /* Fill result - zero-copy payload */
    frame->payload_len = payload_len;
    frame->header_len = header_len;
    frame->payload = payload_len > 0 ? data + header_len : NULL;

    return WS_PARSE_OK;
}

void ws_frame_unmask(uint8_t *payload, size_t len, const uint8_t masking_key[4]) {
    if (len == 0) return;

    /* Build 32-bit mask for bulk XOR */
    uint32_t mask32;
    memcpy(&mask32, masking_key, 4);

    /* Process 4 bytes at a time */
    size_t bulk_len = len & ~(size_t)3;
    uint8_t *p = payload;
    uint8_t *bulk_end = payload + bulk_len;

    while (p < bulk_end) {
        uint32_t chunk;
        memcpy(&chunk, p, 4);
        chunk ^= mask32;
        memcpy(p, &chunk, 4);
        p += 4;
    }

    /* Handle remaining 0-3 bytes */
    size_t remaining = len - bulk_len;
    for (size_t i = 0; i < remaining; i++) {
        p[i] ^= masking_key[i];
    }
}

size_t ws_frame_header_len(uint64_t payload_len, int masked) {
    size_t len = 2;

    if (payload_len > 125) {
        if (payload_len <= 0xFFFF) {
            len += 2;  /* 16-bit length */
        } else {
            len += 8;  /* 64-bit length */
        }
    }

    if (masked) {
        len += 4;  /* masking key */
    }

    return len;
}
