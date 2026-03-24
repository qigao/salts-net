/**
 * @file tlv_bind.h
 * @brief MIR-based runtime TLV codec (zero compilation)
 *
 * Provides dynamic TLV parsing/building from schema files.
 * API compatible with DataBind for easy integration.
 */

#ifndef TLV_BIND_H
#define TLV_BIND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DLL export/import macros */
#ifdef _WIN32
  #ifdef TLV_BIND_BUILD_DLL
    #define TLV_BIND_API __declspec(dllexport)
  #elif defined(TLV_BIND_USE_DLL)
    #define TLV_BIND_API __declspec(dllimport)
  #else
    #define TLV_BIND_API
  #endif
#else
  #define TLV_BIND_API
#endif

typedef struct TlvBind TlvBind;
typedef struct Value Value;

/**
 * @brief Value API function pointers (provided by host application)
 *
 * Host must implement these to integrate with their data structure
 * (e.g., cJSON, msgpack, custom structs)
 *
 * ZERO-COPY MODE:
 * - Strings and bytes are NOT copied, they point directly to the TLV buffer
 * - The TLV buffer MUST remain valid for the lifetime of the Value object
 * - Strings are NOT null-terminated, use length parameter
 */
typedef struct TlvBindValueApi {
  /* Object creation */
  Value *(*create_object)(void);
  void (*free_value)(Value *obj);

  /* Setters (for parsing) - ZERO-COPY */
  void (*set_field_int32)(Value *obj, const char *name, int32_t val);
  void (*set_field_int64)(Value *obj, const char *name, int64_t val);
  void (*set_field_double)(Value *obj, const char *name, double val);
  void (*set_field_string)(Value *obj, const char *name, const char *val,
                           size_t len); /* CHANGED: added len */
  void (*set_field_bytes)(Value *obj, const char *name, const uint8_t *data, size_t len);
  void (*set_field_object)(Value *obj, const char *name, Value *nested);

  /* Getters (for building) */
  int32_t (*get_field_int32)(Value *obj, const char *name);
  int64_t (*get_field_int64)(Value *obj, const char *name);
  double (*get_field_double)(Value *obj, const char *name);
  const char *(*get_field_string)(Value *obj, const char *name,
                                  size_t *len); /* CHANGED: added len */
  const uint8_t *(*get_field_bytes)(Value *obj, const char *name, size_t *len);
  Value *(*get_field_object)(Value *obj, const char *name);
} TlvBindValueApi;

/**
 * @brief Create dynamic TLV codec from schema file
 *
 * @param schema_path Path to .tlvschema file
 * @param api Value API function pointers (must remain valid for codec lifetime)
 * @return Codec instance, or NULL on failure
 *
 * Example:
 *   TlvBind* codec = tlv_bind_create("order.tlvschema", &my_api);
 */
TLV_BIND_API TlvBind *tlv_bind_create(const char *schema_path, const TlvBindValueApi *api);

/**
 * @brief Free codec and release resources
 */
TLV_BIND_API void tlv_bind_free(TlvBind *codec);

/**
 * @brief Parse TLV binary data to Value object
 *
 * @param codec Codec instance
 * @param type_name Message type name (e.g., "Order")
 * @param buf TLV binary data
 * @param len Data length in bytes
 * @return Value object, or NULL on failure (check tlv_bind_get_error)
 *
 * TLV format: tag(varint) + length(varint) + value(bytes)
 *
 * This is a custom TLV layout. Every field payload is length-prefixed.
 * Scalar fields still use fixed-width payload bytes inside that envelope,
 * so do not treat this as protobuf-style varint/fixed wire encoding.
 */
TLV_BIND_API Value *tlv_bind_parse(TlvBind *codec, const char *type_name, const uint8_t *buf,
                                   size_t len);

/**
 * @brief Build TLV binary data from Value object
 *
 * @param codec Codec instance
 * @param type_name Message type name
 * @param obj Value object
 * @param out_len Output: length of returned buffer
 * @return TLV binary data (caller must free), or NULL on failure
 */
TLV_BIND_API uint8_t *tlv_bind_build(TlvBind *codec, const char *type_name, Value *obj,
                                     size_t *out_len);

/**
 * @brief Get last error message
 * @return Error string, or NULL if no error
 */
TLV_BIND_API const char *tlv_bind_get_error(TlvBind *codec);

#ifdef __cplusplus
}
#endif

#endif /* TLV_BIND_H */
