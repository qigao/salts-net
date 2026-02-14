/**
 * @file test_fmt_bench.c
 * @brief Micro-benchmarks for fmt lexer/formatter (tinytest)
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fmt.h"
#include "fmt_lexer.h"
#include "tinytest.h"

#define ITERS_FAST    500000
#define ITERS_NORMAL  200000
#define ITERS_HEAVY   100000
#define BUF_SM        128
#define BUF_MD        256
#define BUF_LG        2048

static volatile size_t sink_sz = 0;
static volatile int sink_n = 0;

spec("FMT Bench") {

  bench("lexer") {
    const char *simple = "Hello {}";
    const char *mixed = "Hello {{}} {:08x} {} world {}";
    const char *heavy = "{} {} {} {} {} {:08d} {} {} {}";

    benchmark("scan_v simple", ITERS_FAST) {
      const char *cur = simple;
      tstr_v tok = tstr_v_from_buf(NULL, 0);
      while (fmt_scan_v(&cur, &tok) != FMT_TOKEN_END)
        sink_sz += tok.len;
    }

    benchmark("scan_v mixed", ITERS_FAST) {
      const char *cur = mixed;
      tstr_v tok = tstr_v_from_buf(NULL, 0);
      while (fmt_scan_v(&cur, &tok) != FMT_TOKEN_END)
        sink_sz += tok.len;
    }

    benchmark("scan_v many placeholders", ITERS_NORMAL) {
      const char *cur = heavy;
      tstr_v tok = tstr_v_from_buf(NULL, 0);
      while (fmt_scan_v(&cur, &tok) != FMT_TOKEN_END)
        sink_sz += tok.len;
    }

    benchmark("scan(old) mixed", ITERS_FAST) {
      const char *cur = mixed;
      const char *ts;
      size_t tl;
      while (fmt_scan(&cur, &ts, &tl) != FMT_TOKEN_END)
        sink_sz += tl;
    }
  }

  bench("fmt_print") {
    benchmark("single int", ITERS_FAST) {
      char buf[BUF_SM];
      fmt_arg_t args[] = {fmt_arg_int(42)};
      sink_n += fmt_print(buf, sizeof(buf), "val={}", args, 1);
    }

    benchmark("single string", ITERS_FAST) {
      char buf[BUF_SM];
      fmt_arg_t args[] = {fmt_arg_str("hello")};
      sink_n += fmt_print(buf, sizeof(buf), "msg={}", args, 1);
    }

    benchmark("3 args mixed", ITERS_NORMAL) {
      char buf[BUF_MD];
      fmt_arg_t args[] = {fmt_arg_str("example"), fmt_arg_int(12345), fmt_arg_str("suffix")};
      sink_n += fmt_print(buf, sizeof(buf), "A:{} B:{:08d} C:{}", args, 3);
    }

    benchmark("3 args hex+pad", ITERS_NORMAL) {
      char buf[BUF_MD];
      fmt_arg_t args[] = {fmt_arg_uint(0xDEAD), fmt_arg_uint(0xBEEF), fmt_arg_uint(0xCAFE)};
      sink_n += fmt_print(buf, sizeof(buf), "{:04X}-{:04X}-{:04X}", args, 3);
    }

    benchmark("double precision", ITERS_NORMAL) {
      char buf[BUF_MD];
      fmt_arg_t args[] = {fmt_arg_double(3.14159265), fmt_arg_double(2.71828)};
      sink_n += fmt_print(buf, sizeof(buf), "pi={:.6f} e={:.4f}", args, 2);
    }

    benchmark("8 args", ITERS_HEAVY) {
      char buf[BUF_LG];
      fmt_arg_t args[] = {
          fmt_arg_str("alpha"), fmt_arg_str("beta"),  fmt_arg_str("gamma"),
          fmt_arg_int(42),      fmt_arg_uint(0xFF),   fmt_arg_double(3.14),
          fmt_arg_str("delta"), fmt_arg_char('Z')};
      sink_n += fmt_print(buf, sizeof(buf),
                          "{} {} {} {:d} {:x} {:.2f} {} {}", args, 8);
    }

    benchmark("escape heavy", ITERS_NORMAL) {
      char buf[BUF_MD];
      fmt_arg_t args[] = {fmt_arg_int(1), fmt_arg_int(2)};
      sink_n += fmt_print(buf, sizeof(buf),
                          "{{a}} {} {{b}} {} {{c}}", args, 2);
    }

    benchmark("no args (text only)", ITERS_FAST) {
      char buf[BUF_MD];
      sink_n += fmt_print(buf, sizeof(buf),
                          "static text with no placeholders at all", NULL, 0);
    }
  }

  bench("tstr_cat_typed") {
    benchmark("single append", ITERS_NORMAL) {
      tstr_t s = tstr_new();
      s = tstr_cat_typed(s, "id={}", 42);
      sink_sz += tstr_len(s);
      tstr_free(s);
    }

    benchmark("3x chain append", ITERS_HEAVY) {
      tstr_t s = tstr_new();
      s = tstr_cat_typed(s, "a={}", 1);
      s = tstr_cat_typed(s, " b={}", 2);
      s = tstr_cat_typed(s, " c={}", 3);
      sink_sz += tstr_len(s);
      tstr_free(s);
    }

    benchmark("strv append", ITERS_NORMAL) {
      tstr_t s = tstr_new();
      tstr_v v = tstr_v_from_cstr("world");
      s = tstr_cat_typed(s, "hello {}", v);
      sink_sz += tstr_len(s);
      tstr_free(s);
    }
  }

  bench("vs snprintf") {
    benchmark("fmt 3 args", ITERS_NORMAL) {
      char buf[BUF_MD];
      fmt_arg_t args[] = {fmt_arg_str("test"), fmt_arg_int(42), fmt_arg_double(3.14)};
      sink_n += fmt_print(buf, sizeof(buf), "s={} i={} f={:.2f}", args, 3);
    }

    benchmark("snprintf 3 args", ITERS_NORMAL) {
      char buf[BUF_MD];
      sink_n += snprintf(buf, sizeof(buf), "s=%s i=%d f=%.2f", "test", 42, 3.14);
    }
  }
}
