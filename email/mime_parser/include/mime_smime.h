/**
 * @file mime_smime.h
 * @brief S/MIME content-type detection helpers
 *
 * SaltsNet does not currently expose a qualified S/MIME crypto pipeline.
 * These helpers only classify MIME content types that advertise S/MIME.
 */

#ifndef MIME_SMIME_H
#define MIME_SMIME_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Check if a content type advertises S/MIME enveloped data.
 */
int mime_is_smime_encrypted(const char *content_type, size_t len);

/**
 * Check if a content type advertises S/MIME signed data.
 */
int mime_is_smime_signed(const char *content_type, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* MIME_SMIME_H */
