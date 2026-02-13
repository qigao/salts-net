#ifndef SRC_MINIO_CLIENT_INTERNAL_H
#define SRC_MINIO_CLIENT_INTERNAL_H

#include "minio/minio_client.h"
#include "minio/minio_credentials.h"

// Internal definition of the opaque client struct.
// Only used by minio_client.c and minio_http.c.
struct minio_client_s {
    minio_base_url_t base_url;
    minio_credential_provider_t* provider;
};

#endif // SRC_MINIO_CLIENT_INTERNAL_H
