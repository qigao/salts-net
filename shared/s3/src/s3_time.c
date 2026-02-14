#include "s3/s3_time.h"
#include <stdio.h>
#include <string.h>

time_t s3_time_now(void) {
    return time(NULL);
}

time_t s3_time_from_iso8601(const char* str) {
    if (!str) return 0;
    struct tm tm;
    memset(&tm, 0, sizeof(struct tm));
    // Example: 2023-01-01T12:00:00Z or 20230101T120000Z
    if (sscanf(str, "%4d-%2d-%2dT%2d:%2d:%2d", 
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday, 
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        return mktime(&tm);
    }
    return 0;
}

tstr_t s3_time_to_iso8601(time_t t) {
    struct tm* tm = gmtime(&t);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", tm);
    return tstr_dup(buf);
}

tstr_t s3_time_to_amz_date(time_t t) {
    struct tm* tm = gmtime(&t);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", tm);
    return tstr_dup(buf);
}

tstr_t s3_time_to_signer_date(time_t t) {
    struct tm* tm = gmtime(&t);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y%m%d", tm);
    return tstr_dup(buf);
}
