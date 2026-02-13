#ifndef MINIO_TIME_H
#define MINIO_TIME_H

#include <time.h>
#include <turbo_str.h>
#include <platform.h>

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API time_t minio_time_now(void);
CXX_C_API time_t minio_time_from_iso8601(const char* str);
CXX_C_API tstr_t minio_time_to_iso8601(time_t t);

// Specific formats needed for AWS Signature V4
CXX_C_API tstr_t minio_time_to_amz_date(time_t t);     // "20060102T150405Z"
CXX_C_API tstr_t minio_time_to_signer_date(time_t t);   // "20060102"

#ifdef __cplusplus
}
#endif

#endif // MINIO_TIME_H
