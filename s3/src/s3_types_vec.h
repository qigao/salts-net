#ifndef S3_TYPES_VEC_H
#define S3_TYPES_VEC_H

#include "s3/s3_types.h"

// This header defines the STC vector types for S3
// Include this when you need the complete vector definitions

// Define BucketVec using STC
#define i_static
#define i_type S3BucketVec
#define i_key s3_bucket_t
#define i_keyclone s3_bucket_clone
#define i_keydrop s3_bucket_drop
#include <stc/vec.h>

// Define ItemVec using STC
#define i_static
#define i_type S3ItemVec
#define i_key s3_item_t
#define i_keyclone s3_item_clone
#define i_keydrop s3_item_drop
#include <stc/vec.h>

#endif // S3_TYPES_VEC_H
