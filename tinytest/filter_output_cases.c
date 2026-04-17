#include "tinytest.h"

spec("filter output") {
  it_skip("explicit skip") {
    check_true(0);
  }

  it("keep test") {
    check_true(1);
  }

  it("drop test") {
    check_true(1);
  }
}
