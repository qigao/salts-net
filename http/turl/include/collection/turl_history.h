#ifndef TURL_HISTORY_H
#define TURL_HISTORY_H

#include "../turl_http.h"
#include <turbo_parser.h>

/**
 * @brief Log a request and its response to the history file
 * 
 * @param config The request configuration used
 * @param rendered_url The final URL after templating
 * @param status_code HTTP response status
 * @param response_body The response body
 * @param response_len Length of response body
 * @param response_headers The raw response headers
 */
int turl_history_log(const turl_http_config_t *config, const char *rendered_url,
                     char **rendered_headers, uint32_t rendered_header_count,
                     int status_code, const char *response_body, size_t response_len,
                     const char *response_headers);

#endif // TURL_HISTORY_H
