#ifndef MINIO_TYPES_H
#define MINIO_TYPES_H

#include <turbo_str.h>
#include <turbo_str_view.h>
#include <time.h>
#include "minio_error.h"

#ifdef __cplusplus
extern "C" {
#endif

// S3 Storage Class
typedef enum {
    MINIO_STORAGE_CLASS_STANDARD,
    MINIO_STORAGE_CLASS_REDUCED_REDUNDANCY,
    MINIO_STORAGE_CLASS_GLACIER,
    MINIO_STORAGE_CLASS_STANDARD_IA,
    MINIO_STORAGE_CLASS_ONEZONE_IA,
    MINIO_STORAGE_CLASS_INTELLIGENT_TIERING,
    MINIO_STORAGE_CLASS_DEEP_ARCHIVE,
    MINIO_STORAGE_CLASS_OUTPOSTS,
    MINIO_STORAGE_CLASS_GLACIER_IR,
    MINIO_STORAGE_CLASS_SNOW
} minio_storage_class_t;

// Bucket information
typedef struct {
    tstr_t  name;
    time_t  creation_date;
} minio_bucket_t;

// Helper to clone a bucket struct (handles tstr_t)
static inline minio_bucket_t minio_bucket_clone(minio_bucket_t b) {
    return (minio_bucket_t){
        .name = b.name ? tstr_dup(b.name) : NULL,
        .creation_date = b.creation_date
    };
}

// Helper to drop/free bucket struct
static inline void minio_bucket_drop(minio_bucket_t* b) {
    if (b->name) tstr_free(b->name);
}

// Define BucketVec using STC
#define i_static
#define i_type MinioBucketVec
#define i_key minio_bucket_t
#define i_keyclone minio_bucket_clone
#define i_keydrop minio_bucket_drop
#include <stc/vec.h>

// S3 Object/Item information
typedef struct {
    tstr_t  name;
    size_t  size;
    tstr_t  etag;
    time_t  last_modified;
    int     is_prefix;          // true for common prefixes (directories)
    tstr_t  storage_class;
} minio_item_t;

// Helper to clone an item struct
static inline minio_item_t minio_item_clone(minio_item_t i) {
    return (minio_item_t){
        .name = i.name ? tstr_dup(i.name) : NULL,
        .size = i.size,
        .etag = i.etag ? tstr_dup(i.etag) : NULL,
        .last_modified = i.last_modified,
        .is_prefix = i.is_prefix,
        .storage_class = i.storage_class ? tstr_dup(i.storage_class) : NULL
    };
}

// Helper to drop/free item struct
static inline void minio_item_drop(minio_item_t* i) {
    if (i->name) tstr_free(i->name);
    if (i->etag) tstr_free(i->etag);
    if (i->storage_class) tstr_free(i->storage_class);
}

// Define ItemVec using STC
#define i_static
#define i_type MinioItemVec
#define i_key minio_item_t
#define i_keyclone minio_item_clone
#define i_keydrop minio_item_drop
#include <stc/vec.h>

#ifdef __cplusplus
}
#endif

#endif // MINIO_TYPES_H
