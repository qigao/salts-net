#include "tinytest.h"

spec("benchmark invalid input") {
  it("rejects zero iterations") {
    benchmark("zero iterations", 0, 1) {
      check_true(1);
    }
  }
}
