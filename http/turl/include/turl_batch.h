/**
 * @file turl_batch.h
 * @brief Batch download functionality for turl (aria2c-like)
 */

#ifndef TURL_BATCH_H
#define TURL_BATCH_H

#include <turbo_parser.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Execute batch download from a list of URLs
 * @param input_file Path to file containing URLs (one per line)
 * @param concurrency Maximum number of concurrent downloads
 * @param headers Array of HTTP headers to include
 * @param header_count Number of headers
 * @param mustache_context JSON context for template rendering
 * @param output_directory Directory to save downloaded files (NULL for current dir)
 * @param follow_redirects Whether to follow HTTP redirects
 * @param verbose Enable verbose logging
 * @return 0 on success, non-zero on error
 */
int turl_batch_download(const char *input_file, int64_t concurrency,
                        char **headers, uint32_t header_count,
                        json_value_t *mustache_context,
                        const char *output_directory,
                        int follow_redirects, int verbose);

#ifdef __cplusplus
}
#endif

#endif // TURL_BATCH_H
