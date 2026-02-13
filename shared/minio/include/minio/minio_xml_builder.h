#ifndef MINIO_XML_BUILDER_H
#define MINIO_XML_BUILDER_H

#include <turbo_str.h>
#include <platform.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct minio_xml_builder_s minio_xml_builder_t;

CXX_C_API minio_xml_builder_t* minio_xml_new(void);
CXX_C_API void   minio_xml_open(minio_xml_builder_t* b, const char* tag);
CXX_C_API void   minio_xml_open_ns(minio_xml_builder_t* b, const char* tag, const char* ns);
CXX_C_API void   minio_xml_attr(minio_xml_builder_t* b, const char* name, const char* value);
CXX_C_API void   minio_xml_text(minio_xml_builder_t* b, const char* text);
CXX_C_API void   minio_xml_close(minio_xml_builder_t* b, const char* tag);
CXX_C_API void   minio_xml_elem(minio_xml_builder_t* b, const char* tag, const char* text);
CXX_C_API void   minio_xml_elem_int(minio_xml_builder_t* b, const char* tag, int value);
CXX_C_API tstr_t minio_xml_finish(minio_xml_builder_t* b);

#ifdef __cplusplus
}
#endif

#endif // MINIO_XML_BUILDER_H
