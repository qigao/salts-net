#include "tinytest.h"

suite("tinytest cpp regression") {
  it("check_equal_warn should evaluate operands once") {
    int value = 1;
    check_equal_warn(value++, 2);
    check_int_eq(value, 2);
  }

  it("check_not_equal_warn should evaluate operands once") {
    int value = 1;
    check_not_equal_warn(value++, 1);
    check_int_eq(value, 2);
  }

  it("check_greater_warn should evaluate operands once") {
    int value = 1;
    check_greater_warn(value++, 2);
    check_int_eq(value, 2);
  }

  it("check_less_warn should evaluate operands once") {
    int value = 1;
    check_less_warn(value++, 0);
    check_int_eq(value, 2);
  }

  it("check_warn should compose with else in C++") {
    if (true)
      check_warn(true);
    else
      check_warn(false);
    check_true(true);
  }
}
