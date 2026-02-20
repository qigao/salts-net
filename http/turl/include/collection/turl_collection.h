#ifndef TURL_COLLECTION_H
#define TURL_COLLECTION_H

#include <json_parser.h>
#include "../turl_http.h"

/**
 * @brief Run a JSON collection of requests
 * 
 * @param collection_file Path to the JSON collection file
 * @param global_config Base configuration (template context, etc)
 * @return 0 on success, non-zero on error
 */
int turl_run_collection(const char *collection_file, const turl_http_config_t *global_config);

#endif // TURL_COLLECTION_H
