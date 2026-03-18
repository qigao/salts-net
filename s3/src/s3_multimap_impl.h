#ifndef S3_MULTIMAP_IMPL_H
#define S3_MULTIMAP_IMPL_H

#include "s3/s3_multimap.h"
#include <stc/cstr.h>

// Internal header: STC hmap implementation for S3Headers
// Only include this in .c files that need to manipulate S3Headers

// Define S3Headers as a map of cstr to cstr
// NOTE: S3 occasionally allows multiple values for the same header (e.g. Set-Cookie),
// but for request headers and most response headers, a unique map is sufficient.
// If multi-values are needed, we can switch to a vector of pairs or STC multimap.
#define i_static
#define i_type S3Headers
#define i_key_str
#define i_val_str
#include <stc/hmap.h>

#endif // S3_MULTIMAP_IMPL_H
