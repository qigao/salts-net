#include "ltv_parser.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Varint encoding/decoding
 * ============================================================================ */

int ltv_decode_varint(const uint8_t *data, size_t len, uint32_t *out) {
    if (!data || !out || len == 0)
        return 0;

    uint32_t result = 0;
    int shift = 0;

    for (size_t i = 0; i < len && i < LTV_MAX_VARINT_BYTES; i++) {
        uint8_t byte = data[i];
        result |= (uint32_t)(byte & 0x7F) << shift;

        if ((byte & 0x80) == 0) {
            *out = result;
            return (int)(i + 1);
        }

        shift += 7;
        if (shift >= 35)
            return -1;  /* Overflow: more than 5 bytes */
    }

    return 0;  /* Need more data */
}

int ltv_encode_varint(uint32_t value, uint8_t *out) {
    int i = 0;

    while (value >= 0x80) {
        out[i++] = (uint8_t)(value | 0x80);
        value >>= 7;
    }
    out[i++] = (uint8_t)value;

    return i;
}

int ltv_varint_size(uint32_t value) {
    if (value < (1U << 7))  return 1;
    if (value < (1U << 14)) return 2;
    if (value < (1U << 21)) return 3;
    if (value < (1U << 28)) return 4;
    return 5;
}

/* ============================================================================
 * Message parsing
 * ============================================================================ */

LtvParseResult ltv_peek_size(const uint8_t *data, size_t len,
                             uint32_t *out_length, size_t *out_header) {
    if (!data || !out_length || !out_header)
        return LTV_PARSE_INVALID_VARINT;

    uint32_t length;
    int header_size = ltv_decode_varint(data, len, &length);

    if (header_size == 0)
        return LTV_PARSE_NEED_MORE;

    if (header_size < 0)
        return LTV_PARSE_INVALID_VARINT;

    if (length == 0)
        return LTV_PARSE_INVALID_VARINT;  /* Length must include type byte */

    if (length > LTV_MAX_PAYLOAD_SIZE + 1)
        return LTV_PARSE_SIZE_OVERFLOW;

    *out_length = length;
    *out_header = (size_t)header_size;
    return LTV_PARSE_OK;
}

LtvParseResult ltv_parse(const uint8_t *data, size_t len, ltv_message_t *out) {
    if (!data || !out)
        return LTV_PARSE_INVALID_VARINT;

    uint32_t length;
    size_t header_size;
    LtvParseResult peek = ltv_peek_size(data, len, &length, &header_size);
    if (peek != LTV_PARSE_OK)
        return peek;

    size_t total = ltv_total_size(length, header_size);
    if (len < total)
        return LTV_PARSE_NEED_MORE;

    out->length = length;
    out->type = data[header_size];
    out->value_size = length - 1;
    out->value = out->value_size > 0 ? data + header_size + 1 : NULL;
    out->consumed = total;

    return LTV_PARSE_OK;
}

/* ============================================================================
 * Message building
 * ============================================================================ */

size_t ltv_wire_size(size_t value_size) {
    uint32_t length = (uint32_t)(value_size + 1);  /* +1 for type byte */
    return ltv_varint_size(length) + length;
}

size_t ltv_build(uint8_t type, const uint8_t *value, size_t value_size,
                 uint8_t *out, size_t out_len) {
    uint32_t length = (uint32_t)(value_size + 1);
    size_t wire_size = ltv_varint_size(length) + length;

    if (out_len < wire_size)
        return 0;

    int header_size = ltv_encode_varint(length, out);
    out[header_size] = type;

    if (value_size > 0 && value)
        memcpy(out + header_size + 1, value, value_size);

    return wire_size;
}

/* ============================================================================
 * Stream parser
 * ============================================================================ */

struct ltv_stream_s {
    uint8_t *buffer;
    size_t   buffer_size;
    size_t   buffered;
    size_t   expected;
    size_t   header_size;
};

ltv_stream_t *ltv_stream_create(size_t buffer_size) {
    if (buffer_size == 0)
        buffer_size = 65536;

    ltv_stream_t *s = malloc(sizeof(ltv_stream_t));
    if (!s)
        return NULL;

    s->buffer = malloc(buffer_size);
    if (!s->buffer) {
        free(s);
        return NULL;
    }

    s->buffer_size = buffer_size;
    s->buffered = 0;
    s->expected = 0;
    s->header_size = 0;

    return s;
}

LtvParseResult ltv_stream_feed(ltv_stream_t *s, const uint8_t *data,
                               size_t len, ltv_message_t *out) {
    if (!s || !out)
        return LTV_PARSE_INVALID_VARINT;

    /* Append new data */
    if (data && len > 0) {
        if (s->buffered + len > s->buffer_size)
            return LTV_PARSE_BUFFER_OVERFLOW;

        memcpy(s->buffer + s->buffered, data, len);
        s->buffered += len;
    }

    /* Peek size if not yet known */
    if (s->expected == 0) {
        uint32_t length;
        size_t header_size;
        LtvParseResult peek = ltv_peek_size(s->buffer, s->buffered,
                                            &length, &header_size);
        if (peek != LTV_PARSE_OK)
            return peek;

        s->expected = ltv_total_size(length, header_size);
        s->header_size = header_size;

        if (s->expected > s->buffer_size)
            return LTV_PARSE_BUFFER_OVERFLOW;
    }

    /* Wait for complete message */
    if (s->buffered < s->expected)
        return LTV_PARSE_NEED_MORE;

    /* Parse complete message */
    LtvParseResult result = ltv_parse(s->buffer, s->expected, out);
    if (result != LTV_PARSE_OK)
        return result;

    /* Shift remaining data */
    size_t remaining = s->buffered - s->expected;
    if (remaining > 0)
        memmove(s->buffer, s->buffer + s->expected, remaining);

    s->buffered = remaining;
    s->expected = 0;
    s->header_size = 0;

    return LTV_PARSE_OK;
}

size_t ltv_stream_remaining(ltv_stream_t *s, const uint8_t **out_remaining) {
    if (!s)
        return 0;

    if (out_remaining)
        *out_remaining = s->buffer;

    return s->buffered;
}

void ltv_stream_reset(ltv_stream_t *s) {
    if (s) {
        s->buffered = 0;
        s->expected = 0;
        s->header_size = 0;
    }
}

void ltv_stream_destroy(ltv_stream_t *s) {
    if (s) {
        free(s->buffer);
        free(s);
    }
}

const char *ltv_parse_result_string(LtvParseResult result) {
    switch (result) {
    case LTV_PARSE_OK:              return "OK";
    case LTV_PARSE_NEED_MORE:       return "Need more data";
    case LTV_PARSE_INVALID_VARINT:  return "Invalid varint";
    case LTV_PARSE_SIZE_OVERFLOW:   return "Size overflow";
    case LTV_PARSE_BUFFER_OVERFLOW: return "Buffer overflow";
    default:                        return "Unknown error";
    }
}
