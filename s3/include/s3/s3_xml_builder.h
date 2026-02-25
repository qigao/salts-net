#ifndef S3_XML_BUILDER_H
#define S3_XML_BUILDER_H

#include <turbo_str.h>
#include <platform.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct s3_xml_builder_s s3_xml_builder_t;

CXX_C_API s3_xml_builder_t* s3_xml_new(void);
CXX_C_API void   s3_xml_open(s3_xml_builder_t* b, const char* tag);
CXX_C_API void   s3_xml_open_ns(s3_xml_builder_t* b, const char* tag, const char* ns);
CXX_C_API void   s3_xml_attr(s3_xml_builder_t* b, const char* name, const char* value);
CXX_C_API void   s3_xml_text(s3_xml_builder_t* b, const char* text);
CXX_C_API void   s3_xml_close(s3_xml_builder_t* b, const char* tag);
CXX_C_API void   s3_xml_elem(s3_xml_builder_t* b, const char* tag, const char* text);
CXX_C_API void   s3_xml_elem_int(s3_xml_builder_t* b, const char* tag, int value);
CXX_C_API tstr_t s3_xml_finish(s3_xml_builder_t* b);

#ifdef __cplusplus
}
#endif

#endif // S3_XML_BUILDER_H
