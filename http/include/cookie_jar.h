/**
 * @file cookie_jar.h
 * @brief HTTP Cookie Jar structure definitions
 */

#ifndef COOKIE_JAR_H
#define COOKIE_JAR_H

#include "cookie_parser.h"
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cookie jar structure - enhanced */
struct http_cookie_jar_s {
  http_cookie_t *cookies;
  int count;
  time_t last_cleanup; // For periodic cleanup of expired cookies
};

typedef struct http_cookie_jar_s http_cookie_jar_t;

#ifdef __cplusplus
}
#endif

#endif // COOKIE_JAR_H