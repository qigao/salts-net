#ifndef SRC_S3_CLIENT_INTERNAL_H
#define SRC_S3_CLIENT_INTERNAL_H

#include "s3/s3_client.h"
#include "s3/s3_credentials.h"

// Internal definition of the opaque client struct.
// Only used by s3_client.c and s3_http.c.
struct s3_client_s {
    s3_base_url_t base_url;
    s3_credential_provider_t* provider;
};

#endif // SRC_S3_CLIENT_INTERNAL_H
