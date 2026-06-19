#include "platform.h"
#include "tinytest.h"

#include <string.h>

spec("platform_datetime") {
  it("should convert UTC broken-down time to epoch seconds") {
    struct tm tm_value;
    time_t ts;

    memset(&tm_value, 0, sizeof(tm_value));
    tm_value.tm_year = 124;
    tm_value.tm_mon = 0;
    tm_value.tm_mday = 1;
    tm_value.tm_hour = 12;

    ts = turbo_timegm(&tm_value);
    check(ts != (time_t)-1);
    check_int_eq((int)ts, 1704110400);
  }

  it("should decompose UTC time safely") {
    struct tm tm_value;

    check_int_eq(turbo_gmtime((time_t)1704110400, &tm_value), 0);
    check_int_eq(tm_value.tm_year + 1900, 2024);
    check_int_eq(tm_value.tm_mon + 1, 1);
    check_int_eq(tm_value.tm_mday, 1);
    check_int_eq(tm_value.tm_hour, 12);
  }

  it("should format UTC time with strftime") {
    char buf[32];
    int rc = turbo_strftime_utc((time_t)1704110400, "%Y-%m-%dT%H:%M:%SZ",
                                buf, sizeof(buf));

    check(rc > 0);
    check_str_eq(buf, "2024-01-01T12:00:00Z");
  }
}
