/* Private C11 MIME lexical scanning; non-owning and strictly bounded. */
#include "../src/mime_scan.h"
#include "tinytest.h"

spec("mime_scan") {
  describe("counted byte search") {
    it("finds a byte without scanning after end") {
      const char data[] = {'a', 'b', '\0', 'z', 'b'};
      check(mime_scan_char(data, data + 4, 'z') == data + 3);
      check(mime_scan_char(data, data + 4, 'b') == data + 1);
      check(mime_scan_char(data, data + 4, 'x') == NULL);
      check(mime_scan_char(data, data + 4, '\0') == data + 2);
      check(mime_scan_char(data, data + 3, 'z') == NULL);
      check(mime_scan_char(data + 2, data + 2, '\0') == NULL);
    }

    it("until-returned-end is distinct from find-returned-null") {
      const char data[] = "a;b,c";
      const char *end = data + 5;
      check(mime_scan_to_char(data, end, ';') == data + 1);
      check(mime_scan_to_char(data, end, '!') == end);
      check(mime_scan_to_any2(data, end, ',', ';') == data + 1);
      check(mime_scan_to_any2(data + 2, end, ',', ';') == data + 3);
      check(mime_scan_to_any3(data, end, 'x', 'z', ',') == data + 3);
      check(mime_scan_to_any3(data, end, 'x', 'y', 'z') == end);
    }

    it("skips exactly the ASCII MIME whitespace classes") {
      const char data[] = " \t\r\nab";
      const char *end = data + sizeof(data) - 1;
      check(mime_scan_skip_sp_tab(data, end) == data + 2);
      check(mime_scan_skip_sp_tab_cr_lf(data, end) == data + 4);
      check(mime_scan_skip_sp_tab(data + 4, end) == data + 4);
      check(mime_scan_skip_sp_tab_cr_lf(data, data) == data);
    }

    it("finds counted spans including embedded NUL without reading beyond") {
      const char data[] = {'a', 'b', '\0', 'c', ';', 'x'};
      const char token[] = {'\0', 'c'};
      check(mime_scan_mem(data, sizeof(data), token, sizeof(token)) == data + 2);
      check(mime_scan_mem(data, 3, token, sizeof(token)) == NULL);
      check(mime_scan_mem(data, sizeof(data), "x", 1) == data + 5);
      check(mime_scan_mem(data, sizeof(data) - 1, "x", 1) == NULL);
      check(mime_scan_mem(data, sizeof(data), ";x?", 3) == NULL);
      check(mime_scan_mem(data, sizeof(data), "", 0) == data);
      check(mime_scan_mem(data, 1, "long", 4) == NULL);
    }

    it("does not claim a non-empty input with absent pointers") {
      check(mime_scan_char(NULL, NULL, 'x') == NULL);
      check(mime_scan_mem(NULL, 2, "x", 1) == NULL);
      check(mime_scan_mem("a", 1, NULL, 1) == NULL);
    }
  }
}
