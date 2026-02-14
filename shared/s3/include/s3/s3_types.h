#ifndef S3_TYPES_H
#define S3_TYPES_H

#include <turbo_str.h>
#include <turbo_str_view.h>
#include <time.h>
#include "s3_error.h"

#ifdef __cplusplus
extern "C" {
#endif

// S3 Storage Class
typedef enum {
    S3_STORAGE_CLASS_STANDARD,
    S3_STORAGE_CLASS_REDUCED_REDUNDANCY,
    S3_STORAGE_CLASS_GLACIER,
    S3_STORAGE_CLASS_STANDARD_IA,
    S3_STORAGE_CLASS_ONEZONE_IA,
    S3_STORAGE_CLASS_INTELLIGENT_TIERING,
    S3_STORAGE_CLASS_DEEP_ARCHIVE,
    S3_STORAGE_CLASS_OUTPOSTS,
    S3_STORAGE_CLASS_GLACIER_IR,
    S3_STORAGE_CLASS_SNOW
} s3_storage_class_t;

// Bucket information
typedef struct {
    tstr_t  name;
    time_t  creation_date;
} s3_bucket_t;

// Helper to clone a bucket struct (handles tstr_t)
static inline s3_bucket_t s3_bucket_clone(s3_bucket_t b) {
    return (s3_bucket_t){
        .name = b.name ? tstr_dup(b.name) : NULL,
        .creation_date = b.creation_date
    };
}

// Helper to drop/free bucket struct
static inline void s3_bucket_drop(s3_bucket_t* b) {
    if (b->name) tstr_free(b->name);
}

// Define BucketVec using STC
#define i_static
#define i_type S3BucketVec
#define i_key s3_bucket_t
#define i_keyclone s3_bucket_clone
#define i_keydrop s3_bucket_drop
#include <stc/vec.h>

// S3 Object/Item information
typedef struct {
    tstr_t  name;
    size_t  size;
    tstr_t  etag;
    time_t  last_modified;
    int     is_prefix;          // true for common prefixes (directories)
    tstr_t  storage_class;
} s3_item_t;

// Helper to clone an item struct
static inline s3_item_t s3_item_clone(s3_item_t i) {
    return (s3_item_t){
        .name = i.name ? tstr_dup(i.name) : NULL,
        .size = i.size,
        .etag = i.etag ? tstr_dup(i.etag) : NULL,
        .last_modified = i.last_modified,
        .is_prefix = i.is_prefix,
        .storage_class = i.storage_class ? tstr_dup(i.storage_class) : NULL
    };
}

// Helper to drop/free item struct
static inline void s3_item_drop(s3_item_t* i) {
    if (i->name) tstr_free(i->name);
    if (i->etag) tstr_free(i->etag);
    if (i->storage_class) tstr_free(i->storage_class);
}

// Define ItemVec using STC
#define i_static
#define i_type S3ItemVec
#define i_key s3_item_t
#define i_keyclone s3_item_clone
#define i_keydrop s3_item_drop
#include <stc/vec.h>

#ifdef __cplusplus
}
#endif

#endif // S3_TYPES_H
