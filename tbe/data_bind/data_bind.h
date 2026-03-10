/**
 * @file data_bind.h
 * @brief MIR-based runtime binary codec (zero compilation)
 */

#ifndef DATA_BIND_H
#define DATA_BIND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DLL export/import macros */
#ifdef _WIN32
  #ifdef DATA_BIND_BUILD_DLL
    #define DATA_BIND_API __declspec(dllexport)
  #elif defined(DATA_BIND_USE_DLL)
    #define DATA_BIND_API __declspec(dllimport)
  #else
    #define DATA_BIND_API
  #endif
#else
  #define DATA_BIND_API
#endif

typedef struct DataBind DataBind;
typedef struct Value Value;

/**
 * @brief Value API function pointers (provided by host application)
 */
typedef struct DataBindValueApi {
    Value* (*create_object)(void);
    void   (*set_field_int)(Value* obj, const char* name, int32_t val);
    void   (*set_field_double)(Value* obj, const char* name, double val);
    void   (*set_field_string)(Value* obj, const char* name, const char* val);
    void   (*set_field_bytes)(Value* obj, const char* name, const uint8_t* data, size_t len);
} DataBindValueApi;

/**
 * @brief Create codec from schema file
 * @param schema_path Path to .schema file
 * @param api Value API function pointers (must remain valid for codec lifetime)
 * @return Codec instance, or NULL on failure
 */
DATA_BIND_API DataBind* data_bind_create(const char* schema_path, const DataBindValueApi* api);

/**
 * @brief Free codec
 */
DATA_BIND_API void data_bind_free(DataBind* codec);

/**
 * @brief Parse binary data to Value object
 * @param codec Codec instance
 * @param type_name Message type name (e.g. "Order")
 * @param buf Binary data
 * @param len Data length
 * @return Value object, or NULL on failure
 */
DATA_BIND_API Value* data_bind_parse(DataBind* codec, const char* type_name,
                        const uint8_t* buf, size_t len);

/**
 * @brief Get last error message
 */
DATA_BIND_API const char* data_bind_get_error(DataBind* codec);

#ifdef __cplusplus
}
#endif

#endif /* DATA_BIND_H */
