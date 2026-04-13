#include "tinytest.h"
#include <math.h>
#include <stdlib.h>

/* 使用 suite/spec 註冊測試組 */
suite("Math Benchmarks") {
  /* 使用 describe 來分組 */
  describe("Basic Arithmetic vs Libc Math") {

    /* 使用 bench(...) 標記這是一個性能測試節點 */
    bench("Square Root Calculation") {
      benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)",
                       "ops/s", NULL, NULL);

      /*
       * benchmark(Name, Iterations, Scale)
       * 會自動重複執行大括號內的代碼 Iterations 次
       * 並搜集 min, max, avg 和 ops/s
       */
      benchmark("x * x (Multiplication)", 5000000, 1) {
        volatile double x = 123.456;
        volatile double y = x * x;
        (void)y;
      }

      benchmark("pow(x, 2.0)", 5000000, 1) {
        volatile double x = 123.456;
        volatile double y = pow(x, 2.0);
        (void)y;
      }
    }

    bench("Trigonometry") {
      benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)",
                       "ops/s", NULL, NULL);
      benchmark("sin(x)", 5000000, 1) {
        volatile double x = 1.0;
        volatile double y = sin(x);
        (void)y;
      }

      benchmark("cos(x)", 5000000, 1) {
        volatile double x = 1.0;
        volatile double y = cos(x);
        (void)y;
      }
    }
  }

  describe("Memory Operations") {

    bench("Allocation") {
      benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)",
                       "ops/s", NULL, NULL);
      benchmark("malloc + free (128 bytes)", 1000000, 1) {
        void *ptr = malloc(128);
        if (ptr) {
          free(ptr);
        }
      }

      benchmark("calloc + free (128 bytes)", 1000000, 1) {
        void *ptr = calloc(1, 128);
        if (ptr) {
          free(ptr);
        }
      }

    }
  }
}
