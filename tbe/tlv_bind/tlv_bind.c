/**
 * @file tlv_bind.c
 * @brief Dynamic TLV codec implementation (MIR-based)
 */

#include "tlv_bind.h"
#include "tlv_schema_parser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* TlvBind context */
struct TlvBind {
    TlvSchema* schema;
    const TlvBindValueApi* api;
    char error[256];
};

/* Dynamic buffer for building TLV data */
typedef struct {
    uint8_t* data;
    size_t len;
    size_t cap;
} DynBuf;

/* Forward declarations */
static inline uint8_t* write_varint(uint8_t* buf, uint64_t value);

static void dynbuf_ensure(DynBuf* buf, size_t needed) {
    if (buf->len + needed > buf->cap) {
        buf->cap = (buf->cap == 0) ? 256 : buf->cap * 2;
        while (buf->len + needed > buf->cap) buf->cap *= 2;
        buf->data = realloc(buf->data, buf->cap);
    }
}

static void dynbuf_write_varint(DynBuf* buf, uint64_t value) {
    uint8_t tmp[10];
    uint8_t* p = write_varint(tmp, value);
    size_t len = p - tmp;
    dynbuf_ensure(buf, len);
    memcpy(buf->data + buf->len, tmp, len);
    buf->len += len;
}

/**
 * @brief Read varint from buffer (optimized with fast path)
 */
static inline const uint8_t* read_varint(const uint8_t* buf, const uint8_t* end, uint64_t* out) {
    if (buf >= end) return NULL;
    
    /* Fast path: single byte (90% of cases for field numbers and small lengths) */
    uint8_t byte = *buf++;
    if ((byte & 0x80) == 0) {
        *out = byte;
        return buf;
    }
    
    /* Slow path: multi-byte varint (unrolled for first 4 bytes) */
    uint64_t result = byte & 0x7F;
    
    if (buf >= end) return NULL;
    byte = *buf++;
    result |= (uint64_t)(byte & 0x7F) << 7;
    if ((byte & 0x80) == 0) {
        *out = result;
        return buf;
    }
    
    if (buf >= end) return NULL;
    byte = *buf++;
    result |= (uint64_t)(byte & 0x7F) << 14;
    if ((byte & 0x80) == 0) {
        *out = result;
        return buf;
    }
    
    if (buf >= end) return NULL;
    byte = *buf++;
    result |= (uint64_t)(byte & 0x7F) << 21;
    if ((byte & 0x80) == 0) {
        *out = result;
        return buf;
    }
    
    /* Remaining bytes (rare for field numbers/lengths) */
    int shift = 28;
    while (buf < end && shift < 64) {
        byte = *buf++;
        result |= (uint64_t)(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            *out = result;
            return buf;
        }
        shift += 7;
    }
    
    return NULL;  /* overflow or truncated */
}

/**
 * @brief Write varint to buffer (optimized with fast path)
 */
static inline uint8_t* write_varint(uint8_t* buf, uint64_t value) {
    /* Fast path: single byte (90% of cases) */
    if (value < 0x80) {
        *buf++ = (uint8_t)value;
        return buf;
    }
    
    /* Slow path: multi-byte varint (unrolled) */
    *buf++ = (uint8_t)(value | 0x80);
    value >>= 7;
    
    if (value < 0x80) {
        *buf++ = (uint8_t)value;
        return buf;
    }
    
    *buf++ = (uint8_t)(value | 0x80);
    value >>= 7;
    
    if (value < 0x80) {
        *buf++ = (uint8_t)value;
        return buf;
    }
    
    *buf++ = (uint8_t)(value | 0x80);
    value >>= 7;
    
    if (value < 0x80) {
        *buf++ = (uint8_t)value;
        return buf;
    }
    
    /* Remaining bytes (rare) */
    while (value >= 0x80) {
        *buf++ = (uint8_t)(value | 0x80);
        value >>= 7;
    }
    *buf++ = (uint8_t)value;
    return buf;
}

/**
 * @brief Parse TLV field value (tag already read by caller)
 */
static const uint8_t* parse_field_value(TlvBind* codec, Value* obj, TlvField* field,
                                        const uint8_t* buf, const uint8_t* end) {
    uint64_t length;
    
    /* Read length */
    buf = read_varint(buf, end, &length);
    if (!buf || buf + length > end) {
        snprintf(codec->error, sizeof(codec->error), "Invalid length");
        return NULL;
    }
    
    /* Parse value based on type */
    switch (field->type) {
        case 0: /* TLV_TYPE_INT32 */ {
            if (length != 4) {
                snprintf(codec->error, sizeof(codec->error), "Invalid int32 length");
                return NULL;
            }
            int32_t val = *(int32_t*)buf;
            codec->api->set_field_int32(obj, field->name, val);
            buf += 4;
            break;
        }
        
        case 1: /* TLV_TYPE_INT64 */ {
            if (length != 8) {
                snprintf(codec->error, sizeof(codec->error), "Invalid int64 length");
                return NULL;
            }
            int64_t val = *(int64_t*)buf;
            codec->api->set_field_int64(obj, field->name, val);
            buf += 8;
            break;
        }
        
        case 2: /* TLV_TYPE_DOUBLE */ {
            if (length != 8) {
                snprintf(codec->error, sizeof(codec->error), "Invalid double length");
                return NULL;
            }
            double val = *(double*)buf;
            codec->api->set_field_double(obj, field->name, val);
            buf += 8;
            break;
        }
        
        case 3: /* TLV_TYPE_STRING */ {
            /* Zero-copy: pass pointer directly to buffer */
            codec->api->set_field_string(obj, field->name, (const char*)buf, length);
            buf += length;
            break;
        }
        
        case 4: /* TLV_TYPE_BYTES */ {
            codec->api->set_field_bytes(obj, field->name, buf, length);
            buf += length;
            break;
        }
        
        case 5: /* TLV_TYPE_MESSAGE */ {
            /* Recursively parse nested message */
            Value* nested = tlv_bind_parse(codec, field->type_name, buf, length);
            if (!nested) return NULL;
            codec->api->set_field_object(obj, field->name, nested);
            buf += length;
            break;
        }
        
        default:
            snprintf(codec->error, sizeof(codec->error), "Unknown field type");
            return NULL;
    }
    
    return buf;
}

/* Public API */

TlvBind* tlv_bind_create(const char* schema_path, const TlvBindValueApi* api) {
    if (!schema_path || !api) return NULL;
    
    TlvBind* codec = calloc(1, sizeof(TlvBind));
    codec->api = api;
    
    codec->schema = tlv_schema_parse_file(schema_path);
    if (!codec->schema) {
        snprintf(codec->error, sizeof(codec->error), "Failed to parse schema: %s", schema_path);
        free(codec);
        return NULL;
    }
    
    return codec;
}

void tlv_bind_free(TlvBind* codec) {
    if (!codec) return;
    tlv_schema_free(codec->schema);
    free(codec);
}

Value* tlv_bind_parse(TlvBind* codec, const char* type_name,
                      const uint8_t* buf, size_t len) {
    if (!codec || !type_name || !buf) {
        if (codec) snprintf(codec->error, sizeof(codec->error), "Invalid arguments");
        return NULL;
    }
    
    /* Find message definition */
    TlvMessage* msg = tlv_schema_find_message(codec->schema, type_name);
    if (!msg) {
        snprintf(codec->error, sizeof(codec->error), "Message type not found: %s", type_name);
        return NULL;
    }
    
    /* Create result object */
    Value* obj = codec->api->create_object();
    if (!obj) {
        snprintf(codec->error, sizeof(codec->error), "Failed to create object");
        return NULL;
    }
    
    /* Parse all fields */
    const uint8_t* p = buf;
    const uint8_t* end = buf + len;
    
    while (p < end) {
        /* Read tag to get field number */
        uint64_t tag;
        const uint8_t* next = read_varint(p, end, &tag);
        if (!next) {
            snprintf(codec->error, sizeof(codec->error), "Failed to read tag");
            codec->api->free_value(obj);
            return NULL;
        }
        
        int field_num = tag >> 3;
        
        /* O(1) lookup using field_map (field_num >= 256 not supported) */
        TlvField* field = (field_num < 256) ? msg->field_map[field_num] : NULL;
        
        if (!field) {
            /* Unknown field - skip it */
            uint64_t length;
            next = read_varint(next, end, &length);
            if (!next || next + length > end) {
                snprintf(codec->error, sizeof(codec->error), "Invalid field length");
                codec->api->free_value(obj);
                return NULL;
            }
            p = next + length;
            continue;
        }
        
        /* Parse known field (pass 'next' which already skipped tag) */
        p = parse_field_value(codec, obj, field, next, end);
        if (!p) {
            codec->api->free_value(obj);
            return NULL;
        }
    }
    
    return obj;
}

/**
 * @brief Build TLV field to dynamic buffer (single-pass, no size calculation)
 */
static void build_field_to_dynbuf(TlvBind* codec, DynBuf* buf, TlvField* field, Value* obj) {
    /* Write tag */
    uint32_t tag = (field->field_number << 3) | field->wire_type;
    dynbuf_write_varint(buf, tag);
    
    /* Write length and value based on type */
    switch (field->type) {
        case TLV_TYPE_INT32: {
            dynbuf_write_varint(buf, 4);
            int32_t val = codec->api->get_field_int32(obj, field->name);
            dynbuf_ensure(buf, 4);
            memcpy(buf->data + buf->len, &val, 4);
            buf->len += 4;
            break;
        }
        
        case TLV_TYPE_INT64: {
            dynbuf_write_varint(buf, 8);
            int64_t val = codec->api->get_field_int64(obj, field->name);
            dynbuf_ensure(buf, 8);
            memcpy(buf->data + buf->len, &val, 8);
            buf->len += 8;
            break;
        }
        
        case TLV_TYPE_DOUBLE: {
            dynbuf_write_varint(buf, 8);
            double val = codec->api->get_field_double(obj, field->name);
            dynbuf_ensure(buf, 8);
            memcpy(buf->data + buf->len, &val, 8);
            buf->len += 8;
            break;
        }
        
        case TLV_TYPE_STRING: {
            size_t len = 0;
            const char* str = codec->api->get_field_string(obj, field->name, &len);
            dynbuf_write_varint(buf, len);
            if (len > 0 && str) {
                dynbuf_ensure(buf, len);
                memcpy(buf->data + buf->len, str, len);
                buf->len += len;
            }
            break;
        }
        
        case TLV_TYPE_BYTES: {
            size_t len = 0;
            const uint8_t* data = codec->api->get_field_bytes(obj, field->name, &len);
            dynbuf_write_varint(buf, len);
            if (len > 0 && data) {
                dynbuf_ensure(buf, len);
                memcpy(buf->data + buf->len, data, len);
                buf->len += len;
            }
            break;
        }
        
        case TLV_TYPE_MESSAGE: {
            Value* nested = codec->api->get_field_object(obj, field->name);
            if (nested) {
                /* Build nested message to temporary buffer */
                DynBuf nested_buf = {0};
                TlvMessage* nested_msg = tlv_schema_find_message(codec->schema, field->type_name);
                if (nested_msg) {
                    for (int i = 0; i < nested_msg->field_count; i++) {
                        build_field_to_dynbuf(codec, &nested_buf, &nested_msg->fields[i], nested);
                    }
                }
                /* Write length + data */
                dynbuf_write_varint(buf, nested_buf.len);
                if (nested_buf.len > 0) {
                    dynbuf_ensure(buf, nested_buf.len);
                    memcpy(buf->data + buf->len, nested_buf.data, nested_buf.len);
                    buf->len += nested_buf.len;
                }
                free(nested_buf.data);
            } else {
                dynbuf_write_varint(buf, 0);
            }
            break;
        }
    }
}

uint8_t* tlv_bind_build(TlvBind* codec, const char* type_name,
                        Value* obj, size_t* out_len) {
    if (!codec || !type_name || !obj || !out_len) {
        if (codec) snprintf(codec->error, sizeof(codec->error), "Invalid arguments");
        return NULL;
    }
    
    /* Find message definition */
    TlvMessage* msg = tlv_schema_find_message(codec->schema, type_name);
    if (!msg) {
        snprintf(codec->error, sizeof(codec->error), "Message type not found: %s", type_name);
        return NULL;
    }
    
    /* Build all fields to dynamic buffer (single pass) */
    DynBuf buf = {0};
    for (int i = 0; i < msg->field_count; i++) {
        build_field_to_dynbuf(codec, &buf, &msg->fields[i], obj);
    }
    
    *out_len = buf.len;
    return buf.data;
}

const char* tlv_bind_get_error(TlvBind* codec) {
    return codec ? codec->error : "Invalid codec";
}
