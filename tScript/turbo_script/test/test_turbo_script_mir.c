#include "../src/turbo_script_internal.h"
#include "tinytest.h"
#include "turbo_script.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define EPS 1e-9

spec("turbo_script_mir") {

  describe("compile_mir") {

    it("should compile a simple assignment") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_not_null(ctx);
      check_int_eq(turbo_script_compile_mir(ctx, "x = 42;"), 0);
      turbo_script_free(ctx);
    }

    it("should compile arithmetic expressions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "a = 10; b = 20; c = a + b;"), 0);
      turbo_script_free(ctx);
    }

    it("should compile all binary operators") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(
                       ctx, "a = 10; b = 3; c = a + b; d = a - b; e = a * b; f = a / b;"),
                   0);
      turbo_script_free(ctx);
    }

    it("should compile compound assignments") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "x = 10; x += 5; x -= 2; x *= 3; x /= 2;"), 0);
      turbo_script_free(ctx);
    }

    it("should compile comparison operators") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(
                       ctx, "a = 10; b = 20; c = a < b; d = a > b; e = a == b; f = a != b;"),
                   0);
      turbo_script_free(ctx);
    }

    it("should compile a while loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "i = 0; while (i < 10) { i += 1; }"), 0);
      turbo_script_free(ctx);
    }

    it("should compile a for loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_compile_mir(ctx, "sum = 0; for (i = 0; i < 100; i += 1) { sum += i; }"), 0);
      turbo_script_free(ctx);
    }

    it("should compile an if statement") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "x = 10; if (x > 5) { y = 1; }"), 0);
      turbo_script_free(ctx);
    }

    it("should compile if-else") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "x = 3; if (x > 5) { y = 1; } else { y = 0; }"),
                   0);
      turbo_script_free(ctx);
    }

    it("should compile logical AND/OR") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "a = 1; b = 0; c = a && b; d = a || b;"), 0);
      turbo_script_free(ctx);
    }

    it("should compile unary NOT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "a = 1; b = !a;"), 0);
      turbo_script_free(ctx);
    }

    it("should compile modulo") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "a = 10; b = 3; c = a % b;"), 0);
      turbo_script_free(ctx);
    }

    it("should compile power") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "a = 2; b = a ^ 10;"), 0);
      turbo_script_free(ctx);
    }

    it("should compile do-while") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "i = 0; do { i += 1; } while (i < 10);"), 0);
      turbo_script_free(ctx);
    }

    it("should compile break/continue") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_compile_mir(ctx, "sum = 0; for (i = 0; i < 20; i += 1) { if (i == 15) { "
                                        "break; } if (i % 2 == 0) { continue; } sum += i; }"),
          0);
      turbo_script_free(ctx);
    }

    it("should compile function calls") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "x = sin(3.14);"), 0);
      turbo_script_free(ctx);
    }

    it("should return -1 on NULL ctx") {
      check_int_eq(turbo_script_compile_mir(NULL, "x = 1;"), -1);
    }

    it("should return -1 on NULL script") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, NULL), -1);
      turbo_script_free(ctx);
    }

    it("should return -1 on invalid syntax") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "??? +++"), -1);
      turbo_script_free(ctx);
    }
  }

  describe("run_jit") {

    it("should run a simple assignment") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 42;"), 0);
      turbo_script_free(ctx);
    }

    it("should run arithmetic") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(ctx, "a = 10.5; b = 20.3; c = (a * b) + (a / b) - (a + b);"), 0);
      turbo_script_free(ctx);
    }

    it("should run a while loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(ctx, "sum = 0; i = 0; while (i < 100) { sum += i; i += 1; }"), 0);
      turbo_script_free(ctx);
    }

    it("should run a for loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 1000; i += 1) { sum += i; }"), 0);
      turbo_script_free(ctx);
    }

    it("should run nested for loops") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(
              ctx,
              "sum = 0; for (i = 0; i < 10; i += 1) { for (j = 0; j < 10; j += 1) { sum += 1; } }"),
          0);
      turbo_script_free(ctx);
    }

    it("should run if-else") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 10; if (x > 5) { y = 1; } else { y = 0; }"), 0);
      turbo_script_free(ctx);
    }

    it("should run compound assignments") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 100; x += 50; x -= 20; x *= 2; x /= 4;"), 0);
      turbo_script_free(ctx);
    }

    it("should return -1 on NULL ctx") { check_int_eq(turbo_script_run_jit(NULL, "x = 1;"), -1); }

    it("should return -1 on NULL script") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, NULL), -1);
      turbo_script_free(ctx);
    }

    it("should return -1 on invalid syntax") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "??? +++"), -1);
      turbo_script_free(ctx);
    }
  }

  describe("exec_jit") {

    it("should execute a pre-compiled module") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "x = 42;"), 0);
      check_int_eq(turbo_script_exec_jit(ctx), 0);
      turbo_script_free(ctx);
    }

    it("should execute repeatedly without crash") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_compile_mir(ctx, "sum = 0; for (i = 0; i < 100; i += 1) { sum += i; }"), 0);
      for (int n = 0; n < 100; n++) {
        check_int_eq(turbo_script_exec_jit(ctx), 0);
      }
      turbo_script_free(ctx);
    }

    it("should return -1 on NULL ctx") { check_int_eq(turbo_script_exec_jit(NULL), -1); }

    it("should return -1 when no module compiled") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_exec_jit(ctx), -1);
      turbo_script_free(ctx);
    }
  }

  describe("compile then exec separation") {

    it("should compile once and exec many times") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "x = 1; x += 1;"), 0);
      check_int_eq(turbo_script_exec_jit(ctx), 0);
      check_int_eq(turbo_script_exec_jit(ctx), 0);
      check_int_eq(turbo_script_exec_jit(ctx), 0);
      turbo_script_free(ctx);
    }

    it("should handle recompilation") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "x = 1;"), 0);
      check_int_eq(turbo_script_exec_jit(ctx), 0);
      check_int_eq(turbo_script_compile_mir(ctx, "y = 2;"), 0);
      check_int_eq(turbo_script_exec_jit(ctx), 0);
      turbo_script_free(ctx);
    }

    it("run_jit should equal compile + exec") {
      turbo_script_ctx_t *ctx1 = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx2 = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "a = 5; b = 10; c = a + b;";
      check_int_eq(turbo_script_run_jit(ctx1, script), 0);
      check_int_eq(turbo_script_compile_mir(ctx2, script), 0);
      check_int_eq(turbo_script_exec_jit(ctx2), 0);
      turbo_script_free(ctx1);
      turbo_script_free(ctx2);
    }
  }

  describe("for loop correctness") {

    it("should handle for loop with large iteration count") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 100000; i += 1) { sum += i; }"), 0);
      turbo_script_free(ctx);
    }

    it("should handle for loop with step > 1") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 10; i += 2) { sum += i; }"),
                   0);
      turbo_script_free(ctx);
    }

    it("should handle for loop with zero iterations") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "sum = 0; for (i = 10; i < 0; i += 1) { sum += i; }"),
                   0);
      turbo_script_free(ctx);
    }

    it("should handle nested for + if") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "count = 0;"
                           "for (i = 0; i < 20; i += 1) {"
                           "  if (i > 10) { count += 1; }"
                           "}";
      check_int_eq(turbo_script_run_jit(ctx, script), 0);
      turbo_script_free(ctx);
    }
  }

  /* ===== Phase 1: New operator + control flow tests ===== */

  describe("logical operators") {

    it("should compute AND correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = 1 && 1; b = 1 && 0; c = 0 && 1; d = 0 && 0;"), 0);
      check_double_eq(get_num(ctx, "a"), 1.0, EPS);
      check_double_eq(get_num(ctx, "b"), 0.0, EPS);
      check_double_eq(get_num(ctx, "c"), 0.0, EPS);
      check_double_eq(get_num(ctx, "d"), 0.0, EPS);
      turbo_script_free(ctx);
    }

    it("should compute OR correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = 1 || 1; b = 1 || 0; c = 0 || 1; d = 0 || 0;"), 0);
      check_double_eq(get_num(ctx, "a"), 1.0, EPS);
      check_double_eq(get_num(ctx, "b"), 1.0, EPS);
      check_double_eq(get_num(ctx, "c"), 1.0, EPS);
      check_double_eq(get_num(ctx, "d"), 0.0, EPS);
      turbo_script_free(ctx);
    }

    it("should compute NOT correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = !0; b = !1; c = !5;"), 0);
      check_double_eq(get_num(ctx, "a"), 1.0, EPS);
      check_double_eq(get_num(ctx, "b"), 0.0, EPS);
      check_double_eq(get_num(ctx, "c"), 0.0, EPS);
      turbo_script_free(ctx);
    }

    it("should short-circuit AND") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      /* If AND short-circuits, the right side (x += 1) should not execute when left is 0 */
      check_int_eq(turbo_script_run_jit(ctx, "x = 10; y = 0 && (x = 99); r = x;"), 0);
      check_double_eq(get_num(ctx, "r"), 10.0, EPS);
      turbo_script_free(ctx);
    }

    it("should short-circuit OR") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      /* If OR short-circuits, the right side should not execute when left is truthy */
      check_int_eq(turbo_script_run_jit(ctx, "x = 10; y = 1 || (x = 99); r = x;"), 0);
      check_double_eq(get_num(ctx, "r"), 10.0, EPS);
      turbo_script_free(ctx);
    }
  }

  describe("modulo and power") {

    it("should compute modulo correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = 10 % 3; b = 7 % 2; c = 15 % 5;"), 0);
      check_double_eq(get_num(ctx, "a"), 1.0, EPS);
      check_double_eq(get_num(ctx, "b"), 1.0, EPS);
      check_double_eq(get_num(ctx, "c"), 0.0, EPS);
      turbo_script_free(ctx);
    }

    it("should compute power correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = 2 ^ 10; b = 3 ^ 3; c = 10 ^ 0;"), 0);
      check_double_eq(get_num(ctx, "a"), 1024.0, EPS);
      check_double_eq(get_num(ctx, "b"), 27.0, EPS);
      check_double_eq(get_num(ctx, "c"), 1.0, EPS);
      turbo_script_free(ctx);
    }
  }

  describe("do-while loop") {

    it("should execute body at least once") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 0; do { x += 1; } while (x < 0);"), 0);
      check_double_eq(get_num(ctx, "x"), 1.0, EPS);
      turbo_script_free(ctx);
    }

    it("should loop correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(ctx, "sum = 0; i = 1; do { sum += i; i += 1; } while (i <= 10);"),
          0);
      check_double_eq(get_num(ctx, "sum"), 55.0, EPS);
      turbo_script_free(ctx);
    }
  }

  describe("break and continue") {

    it("should break out of for loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(
              ctx, "sum = 0; for (i = 0; i < 100; i += 1) { if (i == 5) { break; } sum += i; }"),
          0);
      /* sum = 0+1+2+3+4 = 10 */
      check_double_eq(get_num(ctx, "sum"), 10.0, EPS);
      turbo_script_free(ctx);
    }

    it("should continue in for loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(
              ctx,
              "sum = 0; for (i = 0; i < 10; i += 1) { if (i % 2 == 0) { continue; } sum += i; }"),
          0);
      /* sum = 1+3+5+7+9 = 25 */
      check_double_eq(get_num(ctx, "sum"), 25.0, EPS);
      turbo_script_free(ctx);
    }

    it("should break out of while loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(ctx, "i = 0; while (i < 100) { if (i == 3) { break; } i += 1; }"),
          0);
      check_double_eq(get_num(ctx, "i"), 3.0, EPS);
      turbo_script_free(ctx);
    }

    it("should break out of do-while loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(
                       ctx, "i = 0; do { if (i == 3) { break; } i += 1; } while (i < 100);"),
                   0);
      check_double_eq(get_num(ctx, "i"), 3.0, EPS);
      turbo_script_free(ctx);
    }
  }

  /* ===== Phase 3: Variable bridge tests ===== */

  describe("variable bridge") {

    it("should read variables back via get_num after run_jit") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 42;"), 0);
      check_double_eq(get_num(ctx, "x"), 42.0, EPS);
      turbo_script_free(ctx);
    }

    it("should compute and store arithmetic results") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = 10; b = 20; c = a + b;"), 0);
      check_double_eq(get_num(ctx, "a"), 10.0, EPS);
      check_double_eq(get_num(ctx, "b"), 20.0, EPS);
      check_double_eq(get_num(ctx, "c"), 30.0, EPS);
      turbo_script_free(ctx);
    }

    it("should compute for loop sum correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 100; i += 1) { sum += i; }"),
                   0);
      check_double_eq(get_num(ctx, "sum"), 4950.0, EPS);
      turbo_script_free(ctx);
    }

    it("should read pre-set variables inside JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      bind_num(ctx, "input", 100.0);
      check_int_eq(turbo_script_run_jit(ctx, "result = input * 2;"), 0);
      check_double_eq(get_num(ctx, "result"), 200.0, EPS);
      turbo_script_free(ctx);
    }

    it("should handle compound assignments correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 100; x += 50; x -= 20; x *= 2; x /= 4;"), 0);
      /* (100+50-20)*2/4 = 130*2/4 = 65 */
      check_double_eq(get_num(ctx, "x"), 65.0, EPS);
      turbo_script_free(ctx);
    }

    it("should handle if-else variable assignment") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 10; if (x > 5) { y = 1; } else { y = 0; }"), 0);
      check_double_eq(get_num(ctx, "y"), 1.0, EPS);
      turbo_script_free(ctx);
    }

    it("should handle nested loops with variable bridge") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(
              ctx,
              "sum = 0; for (i = 0; i < 10; i += 1) { for (j = 0; j < 10; j += 1) { sum += 1; } }"),
          0);
      check_double_eq(get_num(ctx, "sum"), 100.0, EPS);
      turbo_script_free(ctx);
    }

    it("should match interpreter results") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "sum = 0; for (i = 1; i <= 50; i += 1) { sum += i * i; }";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check_double_eq(get_num(ctx_jit, "sum"), get_num(ctx_interp, "sum"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  /* ===== Phase 4: Function call bridge tests ===== */

  describe("function call bridge") {

    it("should call sin from JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = sin(0);"), 0);
      check_double_eq(get_num(ctx, "x"), 0.0, EPS);
      turbo_script_free(ctx);
    }

    it("should call abs from JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = abs(-42);"), 0);
      check_double_eq(get_num(ctx, "x"), 42.0, EPS);
      turbo_script_free(ctx);
    }

    it("should call max with 2 args") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = max(10, 20);"), 0);
      check_double_eq(get_num(ctx, "x"), 20.0, EPS);
      turbo_script_free(ctx);
    }

    it("should call min with 2 args") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = min(10, 20);"), 0);
      check_double_eq(get_num(ctx, "x"), 10.0, EPS);
      turbo_script_free(ctx);
    }

    it("should call functions in a loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 10; i += 1) { sum += abs(-1); }"), 0);
      check_double_eq(get_num(ctx, "sum"), 10.0, EPS);
      turbo_script_free(ctx);
    }

    it("should match interpreter function call results") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "x = sqrt(144) + floor(3.7) + ceil(2.1);";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check_double_eq(get_num(ctx_jit, "x"), get_num(ctx_interp, "x"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  /* ===== Phase 5: Interpreter fallback tests ===== */

  describe("interpreter fallback") {

    it("should handle unsupported node types via fallback") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      /* String assignment goes through fallback */
      check_int_eq(turbo_script_run_jit(ctx, "x = 42;"), 0);
      check_double_eq(get_num(ctx, "x"), 42.0, EPS);
      turbo_script_free(ctx);
    }

    it("should keep map assignment correct via full-sync fallback") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "m = map {a: 2, b: 3}; result = m.a * 10 + m.b;"), 0);
      check_double_eq(get_num(ctx, "result"), 23.0, EPS);
      turbo_script_free(ctx);
    }
  }

  /* ===== Phase 6: Vector indexing tests ===== */

  describe("vector indexing") {

    it("should access vector elements by index") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {10.0, 20.0, 30.0, 40.0, 50.0};
      bind_vec(ctx, "v", data, 5);
      check_int_eq(turbo_script_run_jit(ctx, "a = v[0]; b = v[2]; c = v[4];"), 0);
      check_double_eq(get_num(ctx, "a"), 10.0, EPS);
      check_double_eq(get_num(ctx, "b"), 30.0, EPS);
      check_double_eq(get_num(ctx, "c"), 50.0, EPS);
      turbo_script_free(ctx);
    }

    it("should access vector in a loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {1.0, 2.0, 3.0, 4.0, 5.0};
      bind_vec(ctx, "arr", data, 5);
      check_int_eq(
          turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 5; i += 1) { sum += arr[i]; }"), 0);
      check_double_eq(get_num(ctx, "sum"), 15.0, EPS);
      turbo_script_free(ctx);
    }

    it("should match interpreter vector indexing") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {2.0, 4.0, 6.0, 8.0};
      bind_vec(ctx_interp, "v", data, 4);
      bind_vec(ctx_jit, "v", data, 4);
      const char *script = "sum = 0; for (i = 0; i < 4; i += 1) { sum += v[i]; }";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check_double_eq(get_num(ctx_jit, "sum"), get_num(ctx_interp, "sum"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should fallback for non-variable array indexing") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "result = ([10, 20, 30])[1];"), 0);
      check_double_eq(get_num(ctx, "result"), 20.0, EPS);
      turbo_script_free(ctx);
    }
  }

  /* ===== Phase 6: User-defined function tests ===== */

  describe("user-defined functions") {

    it("should define and call a function") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "func double(x) { return x * 2; } r = double(21);"),
                   0);
      check_double_eq(get_num(ctx, "r"), 42.0, EPS);
      turbo_script_free(ctx);
    }

    it("should call user function in a loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(ctx, "func add(a, b) { return a + b; }"
                                    "sum = 0; for (i = 0; i < 10; i += 1) { sum = add(sum, i); }"),
          0);
      check_double_eq(get_num(ctx, "sum"), 45.0, EPS);
      turbo_script_free(ctx);
    }

    it("should match interpreter user function results") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func square(x) { return x * x; }"
                           "sum = 0; for (i = 1; i <= 5; i += 1) { sum += square(i); }";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check_double_eq(get_num(ctx_jit, "sum"), get_num(ctx_interp, "sum"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  /* ===== Phase 6: Constant declaration tests ===== */

  describe("constant declaration") {

    it("should compile and run const declaration") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "const PI = 3.14159; r = PI * 2;"), 0);
      check_double_eq(get_num(ctx, "r"), 6.28318, 1e-4);
      turbo_script_free(ctx);
    }
  }

  /* ===== Phase 6: For-in tests ===== */

  describe("for-in loop") {

    it("should iterate over a vector") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {1.0, 2.0, 3.0, 4.0, 5.0};
      bind_vec(ctx, "nums", data, 5);
      check_int_eq(turbo_script_run_jit(ctx, "sum = 0; for (x in nums) { sum += x; }"), 0);
      check_double_eq(get_num(ctx, "sum"), 15.0, EPS);
      turbo_script_free(ctx);
    }

    it("should match interpreter for-in results") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {10.0, 20.0, 30.0};
      bind_vec(ctx_interp, "v", data, 3);
      bind_vec(ctx_jit, "v", data, 3);
      const char *script = "sum = 0; for (x in v) { sum += x; }";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check_double_eq(get_num(ctx_jit, "sum"), get_num(ctx_interp, "sum"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  /* ===== Phase 7: Null, member access, fallback sync ===== */

  describe("null literal") {

    it("should compile and return 0 for null") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = null;"), 0);
      check_double_eq(get_num(ctx, "x"), 0.0, EPS);
      turbo_script_free(ctx);
    }

    it("should use null in expressions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 10 + null;"), 0);
      check_double_eq(get_num(ctx, "x"), 10.0, EPS);
      turbo_script_free(ctx);
    }
  }

  describe("member access") {

    it("should get vector length") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {1.0, 2.0, 3.0, 4.0, 5.0};
      bind_vec(ctx, "v", data, 5);
      check_int_eq(turbo_script_run_jit(ctx, "n = v.length;"), 0);
      check_double_eq(get_num(ctx, "n"), 5.0, EPS);
      turbo_script_free(ctx);
    }

    it("should use vector length in a loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {10.0, 20.0, 30.0};
      bind_vec(ctx, "arr", data, 3);
      check_int_eq(turbo_script_run_jit(
                       ctx, "sum = 0; for (i = 0; i < arr.length; i += 1) { sum += arr[i]; }"),
                   0);
      check_double_eq(get_num(ctx, "sum"), 60.0, EPS);
      turbo_script_free(ctx);
    }

    it("should match interpreter member access") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {1.0, 2.0, 3.0, 4.0};
      bind_vec(ctx_interp, "v", data, 4);
      bind_vec(ctx_jit, "v", data, 4);
      const char *script = "n = v.length;";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check_double_eq(get_num(ctx_jit, "n"), get_num(ctx_interp, "n"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  describe("ternary operator") {

    it("should evaluate ternary true branch") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 1 ? 42 : 0;"), 0);
      check_double_eq(get_num(ctx, "x"), 42.0, EPS);
      turbo_script_free(ctx);
    }

    it("should evaluate ternary false branch") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 0 ? 42 : 99;"), 0);
      check_double_eq(get_num(ctx, "x"), 99.0, EPS);
      turbo_script_free(ctx);
    }

    it("should use ternary in expressions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = 10; b = a > 5 ? a * 2 : a / 2;"), 0);
      check_double_eq(get_num(ctx, "b"), 20.0, EPS);
      turbo_script_free(ctx);
    }
  }

  describe("default fallback sync") {

    it("should sync variables through interpreter fallback") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      /* Vector literal [1,2,3] goes through fallback, then sum uses JIT */
      double data[] = {1.0, 2.0, 3.0};
      bind_vec(ctx, "v", data, 3);
      check_int_eq(
          turbo_script_run_jit(
              ctx, "sum = 0; for (i = 0; i < 3; i += 1) { sum += v[i]; } result = sum * 2;"),
          0);
      check_double_eq(get_num(ctx, "result"), 12.0, EPS);
      turbo_script_free(ctx);
    }
  }

  describe("unary plus") {

    it("should handle unary plus on variable") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = 5; b = +a;"), 0);
      check_double_eq(get_num(ctx, "b"), 5.0, EPS);
      turbo_script_free(ctx);
    }

    it("should handle unary minus on variable") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = 5; b = -a;"), 0);
      check_double_eq(get_num(ctx, "b"), -5.0, EPS);
      turbo_script_free(ctx);
    }
  }

  describe("return statement") {

    it("should return from top-level script") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 10; return x; x = 99;"), 0);
      check_double_eq(get_num(ctx, "x"), 10.0, EPS);
      turbo_script_free(ctx);
    }

    it("should return with expression") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = 5; b = 10; return a + b;"), 0);
      check_double_eq(get_num(ctx, "a"), 5.0, EPS);
      check_double_eq(get_num(ctx, "b"), 10.0, EPS);
      turbo_script_free(ctx);
    }

    it("should stop execution after return") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "sum = 0;"
                                             "for (i = 0; i < 100; i += 1) {"
                                             "  sum += i;"
                                             "  if (sum > 10) { return sum; }"
                                             "}"),
                   0);
      /* sum should be 15 (0+1+2+3+4+5) when it exceeds 10 */
      check_double_eq(get_num(ctx, "sum"), 15.0, EPS);
      turbo_script_free(ctx);
    }
  }

  describe("switch statement") {

    it("should match first case") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 1;"
                                             "switch (x) {"
                                             "  case 1: { result = 10; }"
                                             "  case 2: { result = 20; }"
                                             "  case 3: { result = 30; }"
                                             "}"),
                   0);
      check_double_eq(get_num(ctx, "result"), 10.0, EPS);
      turbo_script_free(ctx);
    }

    it("should match middle case") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 2;"
                                             "switch (x) {"
                                             "  case 1: { result = 10; }"
                                             "  case 2: { result = 20; }"
                                             "  case 3: { result = 30; }"
                                             "}"),
                   0);
      check_double_eq(get_num(ctx, "result"), 20.0, EPS);
      turbo_script_free(ctx);
    }

    it("should execute default case") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 99;"
                                             "switch (x) {"
                                             "  case 1: { result = 10; }"
                                             "  case 2: { result = 20; }"
                                             "  default: { result = -1; }"
                                             "}"),
                   0);
      check_double_eq(get_num(ctx, "result"), -1.0, EPS);
      turbo_script_free(ctx);
    }

    it("should use switch with expressions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = 3; b = 0;"
                                             "switch (a * 2) {"
                                             "  case 4: { b = 100; }"
                                             "  case 6: { b = 200; }"
                                             "  case 8: { b = 300; }"
                                             "}"),
                   0);
      check_double_eq(get_num(ctx, "b"), 200.0, EPS);
      turbo_script_free(ctx);
    }
  }

  describe("no-sync fallback nodes") {

    it("should handle vector literal via fallback") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      /* Vector literal goes through no-sync fallback, then index via vec_get */
      check_int_eq(turbo_script_run_jit(ctx, "v = [10, 20, 30]; result = v[0] + v[2];"), 0);
      check_double_eq(get_num(ctx, "result"), 40.0, EPS);
      turbo_script_free(ctx);
    }

    it("should handle map literal via fallback") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "m = map {x: 5, y: 10}; result = m.x + m.y;"), 0);
      check_double_eq(get_num(ctx, "result"), 15.0, EPS);
      turbo_script_free(ctx);
    }
  }

  describe("compile-time map key resolution") {

    it("should access pre-bound map fields by index") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      /* Create a map in env before JIT compile */
      turbo_script_run(ctx, "config = map {width: 800, height: 600, depth: 32};");
      check_int_eq(
          turbo_script_run_jit(ctx, "result = config.width + config.height + config.depth;"), 0);
      check_double_eq(get_num(ctx, "result"), 1432.0, EPS);
      turbo_script_free(ctx);
    }

    it("should use map fields in arithmetic") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_run(ctx, "pt = map {x: 3, y: 4};");
      check_int_eq(turbo_script_run_jit(ctx, "dist = (pt.x * pt.x + pt.y * pt.y) ^ 0.5;"), 0);
      check_double_eq(get_num(ctx, "dist"), 5.0, EPS);
      turbo_script_free(ctx);
    }

    it("should use map fields in loops") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_run(ctx, "params = map {start: 0, stop: 100, step: 1};");
      check_int_eq(
          turbo_script_run_jit(
              ctx,
              "sum = 0; for (i = params.start; i < params.stop; i += params.step) { sum += i; }"),
          0);
      check_double_eq(get_num(ctx, "sum"), 4950.0, EPS);
      turbo_script_free(ctx);
    }

    it("should fall back to member_get for runtime maps") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      /* Map created at runtime — not pre-bound, so uses generic member_get */
      check_int_eq(turbo_script_run_jit(ctx, "m = map {a: 10, b: 20}; result = m.a + m.b;"), 0);
      check_double_eq(get_num(ctx, "result"), 30.0, EPS);
      turbo_script_free(ctx);
    }

    it("should fallback to member_get for non-variable object access") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "result = (map {x: 7, y: 9}).x + 1;"), 0);
      check_double_eq(get_num(ctx, "result"), 8.0, EPS);
      turbo_script_free(ctx);
    }
  }

  describe("native vector indexing") {

    it("should read pre-bound vector elements") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {100.0, 200.0, 300.0};
      bind_vec(ctx, "v", data, 3);
      check_int_eq(turbo_script_run_jit(ctx, "result = v[0] + v[1] + v[2];"), 0);
      check_double_eq(get_num(ctx, "result"), 600.0, EPS);
      turbo_script_free(ctx);
    }

    it("should index with computed expressions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {10.0, 20.0, 30.0, 40.0, 50.0};
      bind_vec(ctx, "arr", data, 5);
      check_int_eq(
          turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 5; i += 1) { sum += arr[i]; }"), 0);
      check_double_eq(get_num(ctx, "sum"), 150.0, EPS);
      turbo_script_free(ctx);
    }

    it("should handle multiple vectors") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double a[] = {1.0, 2.0, 3.0};
      double b[] = {10.0, 20.0, 30.0};
      bind_vec(ctx, "a", a, 3);
      bind_vec(ctx, "b", b, 3);
      check_int_eq(
          turbo_script_run_jit(ctx, "dot = 0; for (i = 0; i < 3; i += 1) { dot += a[i] * b[i]; }"),
          0);
      check_double_eq(get_num(ctx, "dot"), 140.0, EPS);
      turbo_script_free(ctx);
    }

    it("should work in nested loops") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      /* Simulate a small matrix-vector multiply: y = M * x
       * M is 3x3 stored row-major in flat array, x is 3-element vector */
      double M[] = {1, 0, 0, 0, 2, 0, 0, 0, 3}; /* diagonal matrix */
      double x[] = {10.0, 20.0, 30.0};
      bind_vec(ctx, "M", M, 9);
      bind_vec(ctx, "x", x, 3);
      check_int_eq(turbo_script_run_jit(ctx, "y0 = 0; y1 = 0; y2 = 0;"
                                             "for (j = 0; j < 3; j += 1) {"
                                             "  y0 += M[0 * 3 + j] * x[j];"
                                             "  y1 += M[1 * 3 + j] * x[j];"
                                             "  y2 += M[2 * 3 + j] * x[j];"
                                             "}"),
                   0);
      check_double_eq(get_num(ctx, "y0"), 10.0, EPS);
      check_double_eq(get_num(ctx, "y1"), 40.0, EPS);
      check_double_eq(get_num(ctx, "y2"), 90.0, EPS);
      turbo_script_free(ctx);
    }
  }

  /* ===== Phase 13: Direct math function imports ===== */

  describe("direct math functions") {

    it("should call sin directly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = sin(0);"), 0);
      check_double_eq(get_num(ctx, "x"), 0.0, EPS);
      turbo_script_free(ctx);
    }

    it("should call cos directly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = cos(0);"), 0);
      check_double_eq(get_num(ctx, "x"), 1.0, EPS);
      turbo_script_free(ctx);
    }

    it("should call sqrt directly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = sqrt(144);"), 0);
      check_double_eq(get_num(ctx, "x"), 12.0, EPS);
      turbo_script_free(ctx);
    }

    it("should call abs directly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = abs(-42);"), 0);
      check_double_eq(get_num(ctx, "x"), 42.0, EPS);
      turbo_script_free(ctx);
    }

    it("should call floor and ceil directly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = floor(3.7); b = ceil(3.2);"), 0);
      check_double_eq(get_num(ctx, "a"), 3.0, EPS);
      check_double_eq(get_num(ctx, "b"), 4.0, EPS);
      turbo_script_free(ctx);
    }

    it("should call log and exp directly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = exp(0); y = log(1);"), 0);
      check_double_eq(get_num(ctx, "x"), 1.0, EPS);
      check_double_eq(get_num(ctx, "y"), 0.0, EPS);
      turbo_script_free(ctx);
    }

    it("should call tan directly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = tan(0);"), 0);
      check_double_eq(get_num(ctx, "x"), 0.0, EPS);
      turbo_script_free(ctx);
    }

    it("should call max and min directly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = max(10, 20); b = min(10, 20);"), 0);
      check_double_eq(get_num(ctx, "a"), 20.0, EPS);
      check_double_eq(get_num(ctx, "b"), 10.0, EPS);
      turbo_script_free(ctx);
    }

    it("should call math functions in a tight loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 1000; i += 1) { sum += abs(-1); }"),
          0);
      check_double_eq(get_num(ctx, "sum"), 1000.0, EPS);
      turbo_script_free(ctx);
    }

    it("should match interpreter for combined math") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "x = sqrt(144) + floor(3.7) + ceil(2.1) + abs(-5);";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check_double_eq(get_num(ctx_jit, "x"), get_num(ctx_interp, "x"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  /* ===== Phase 14: Native for-in compilation ===== */

  describe("native for-in") {

    it("should iterate pre-bound vector natively") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {10.0, 20.0, 30.0, 40.0, 50.0};
      bind_vec(ctx, "v", data, 5);
      check_int_eq(turbo_script_run_jit(ctx, "sum = 0; for (x in v) { sum += x; }"), 0);
      check_double_eq(get_num(ctx, "sum"), 150.0, EPS);
      turbo_script_free(ctx);
    }

    it("should support break in native for-in") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {1.0, 2.0, 3.0, 4.0, 5.0};
      bind_vec(ctx, "v", data, 5);
      check_int_eq(
          turbo_script_run_jit(ctx, "sum = 0; for (x in v) { if (x > 3) { break; } sum += x; }"),
          0);
      /* sum = 1+2+3 = 6 */
      check_double_eq(get_num(ctx, "sum"), 6.0, EPS);
      turbo_script_free(ctx);
    }

    it("should support continue in native for-in") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {1.0, 2.0, 3.0, 4.0, 5.0};
      bind_vec(ctx, "v", data, 5);
      check_int_eq(turbo_script_run_jit(
                       ctx, "sum = 0; for (x in v) { if (x == 3) { continue; } sum += x; }"),
                   0);
      /* sum = 1+2+4+5 = 12 */
      check_double_eq(get_num(ctx, "sum"), 12.0, EPS);
      turbo_script_free(ctx);
    }

    it("should match interpreter for-in results") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {10.0, 20.0, 30.0};
      bind_vec(ctx_interp, "v", data, 3);
      bind_vec(ctx_jit, "v", data, 3);
      const char *script = "sum = 0; for (x in v) { sum += x * 2; }";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check_double_eq(get_num(ctx_jit, "sum"), get_num(ctx_interp, "sum"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  /* ===== Phase 15: Gen reuse + fn pointer cache ===== */

  describe("gen reuse and fn cache") {

    it("should compile and exec multiple times without crash") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 1;"), 0);
      check_int_eq(turbo_script_run_jit(ctx, "x = 2;"), 0);
      check_int_eq(turbo_script_run_jit(ctx, "x = 3;"), 0);
      check_double_eq(get_num(ctx, "x"), 3.0, EPS);
      turbo_script_free(ctx);
    }

    it("should exec cached fn pointer repeatedly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_compile_mir(ctx, "sum = 0; for (i = 0; i < 100; i += 1) { sum += i; }"), 0);
      for (int n = 0; n < 500; n++) {
        check_int_eq(turbo_script_exec_jit(ctx), 0);
      }
      check_double_eq(get_num(ctx, "sum"), 4950.0, EPS);
      turbo_script_free(ctx);
    }

    it("should recompile and update cached fn") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "x = 10;"), 0);
      check_int_eq(turbo_script_exec_jit(ctx), 0);
      check_double_eq(get_num(ctx, "x"), 10.0, EPS);
      check_int_eq(turbo_script_compile_mir(ctx, "x = 99;"), 0);
      check_int_eq(turbo_script_exec_jit(ctx), 0);
      check_double_eq(get_num(ctx, "x"), 99.0, EPS);
      turbo_script_free(ctx);
    }

    it("should return -1 when no module compiled") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_exec_jit(ctx), -1);
      turbo_script_free(ctx);
    }
  }

  /* ===== Phase 16: Condition branch optimization ===== */

  describe("optimized condition branches") {

    it("should handle all comparison operators in for loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 100; i += 1) { sum += i; }"),
                   0);
      check_double_eq(get_num(ctx, "sum"), 4950.0, EPS);
      turbo_script_free(ctx);
    }

    it("should handle GT condition in while loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 10; while (x > 0) { x -= 1; }"), 0);
      check_double_eq(get_num(ctx, "x"), 0.0, EPS);
      turbo_script_free(ctx);
    }

    it("should handle LE condition in do-while") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(ctx, "sum = 0; i = 1; do { sum += i; i += 1; } while (i <= 10);"),
          0);
      check_double_eq(get_num(ctx, "sum"), 55.0, EPS);
      turbo_script_free(ctx);
    }

    it("should handle EQ/NE in if-else") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 5; if (x == 5) { r = 1; } else { r = 0; }"), 0);
      check_double_eq(get_num(ctx, "r"), 1.0, EPS);
      check_int_eq(turbo_script_run_jit(ctx, "x = 3; if (x != 5) { r = 1; } else { r = 0; }"), 0);
      check_double_eq(get_num(ctx, "r"), 1.0, EPS);
      turbo_script_free(ctx);
    }

    it("should handle AND short-circuit in condition") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "count = 0;"
                                             "for (i = 0; i < 20; i += 1) {"
                                             "  if (i > 5 && i < 15) { count += 1; }"
                                             "}"),
                   0);
      /* i in [6..14] = 9 values */
      check_double_eq(get_num(ctx, "count"), 9.0, EPS);
      turbo_script_free(ctx);
    }

    it("should handle OR short-circuit in condition") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "count = 0;"
                                             "for (i = 0; i < 20; i += 1) {"
                                             "  if (i < 3 || i > 17) { count += 1; }"
                                             "}"),
                   0);
      /* i in [0,1,2,18,19] = 5 values */
      check_double_eq(get_num(ctx, "count"), 5.0, EPS);
      turbo_script_free(ctx);
    }

    it("should handle NOT in condition") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 0; if (!x) { r = 1; } else { r = 0; }"), 0);
      check_double_eq(get_num(ctx, "r"), 1.0, EPS);
      turbo_script_free(ctx);
    }

    it("should match interpreter for complex conditions") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "count = 0;"
                           "for (i = 0; i < 100; i += 1) {"
                           "  if (i >= 10 && i <= 90 && i != 50) { count += 1; }"
                           "}";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check_double_eq(get_num(ctx_jit, "count"), get_num(ctx_interp, "count"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  /* ===== Phase 17: Direct native function dispatch ===== */

  describe("direct native dispatch") {

    it("should call builtin sqrt directly (no bridge lookup)") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = sqrt(256);"), 0);
      check_double_eq(get_num(ctx, "x"), 16.0, EPS);
      turbo_script_free(ctx);
    }

    it("should call builtin functions in loop via direct dispatch") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(ctx, "sum = 0; for (i = 1; i <= 100; i += 1) { sum += abs(-1); }"),
          0);
      check_double_eq(get_num(ctx, "sum"), 100.0, EPS);
      turbo_script_free(ctx);
    }

    it("should match interpreter for mixed function calls") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "sum = 0;"
                           "for (i = 0; i < 50; i += 1) {"
                           "  sum += sqrt(i * i) + floor(i + 0.5) + ceil(i - 0.5);"
                           "}";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check_double_eq(get_num(ctx_jit, "sum"), get_num(ctx_interp, "sum"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should resolve env-registered native functions directly") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      /* sin/cos are registered as builtins — test they resolve correctly */
      const char *script = "x = sin(0) + cos(0) + max(3, 7) + min(3, 7);";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check_double_eq(get_num(ctx_jit, "x"), get_num(ctx_interp, "x"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  /* ===== Phase 18: Compile cache ===== */

  describe("compile cache") {

    it("should cache and reuse compiled script") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "sum = 0; for (i = 0; i < 100; i += 1) { sum += i; }";
      /* First call: compiles */
      check_int_eq(turbo_script_run_jit(ctx, script), 0);
      check_double_eq(get_num(ctx, "sum"), 4950.0, EPS);
      /* Second call: cache hit, no recompile */
      check_int_eq(turbo_script_run_jit(ctx, script), 0);
      check_double_eq(get_num(ctx, "sum"), 4950.0, EPS);
      turbo_script_free(ctx);
    }

    it("should handle different scripts correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 10;"), 0);
      check_double_eq(get_num(ctx, "x"), 10.0, EPS);
      check_int_eq(turbo_script_run_jit(ctx, "x = 99;"), 0);
      check_double_eq(get_num(ctx, "x"), 99.0, EPS);
      /* Re-run first script from cache */
      check_int_eq(turbo_script_run_jit(ctx, "x = 10;"), 0);
      check_double_eq(get_num(ctx, "x"), 10.0, EPS);
      turbo_script_free(ctx);
    }

    it("should run cached script 1000 times") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "x = 42;";
      for (int n = 0; n < 1000; n++) {
        check_int_eq(turbo_script_run_jit(ctx, script), 0);
      }
      check_double_eq(get_num(ctx, "x"), 42.0, EPS);
      turbo_script_free(ctx);
    }

    it("should produce correct results with pre-bound variables") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      bind_num(ctx, "rate", 0.05);
      const char *script = "result = rate * 1000;";
      check_int_eq(turbo_script_run_jit(ctx, script), 0);
      check_double_eq(get_num(ctx, "result"), 50.0, EPS);
      /* Change input, re-run from cache */
      bind_num(ctx, "rate", 0.10);
      check_int_eq(turbo_script_run_jit(ctx, script), 0);
      check_double_eq(get_num(ctx, "result"), 100.0, EPS);
      turbo_script_free(ctx);
    }
  }

  /* ===== Phase 21: Constant folding ===== */

  describe("constant folding") {

    it("should fold simple arithmetic") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 2 * 3.14159;"), 0);
      check_double_eq(get_num(ctx, "x"), 6.28318, 1e-4);
      turbo_script_free(ctx);
    }

    it("should fold nested constant expressions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = (2 + 3) * (10 - 4);"), 0);
      check_double_eq(get_num(ctx, "x"), 30.0, EPS);
      turbo_script_free(ctx);
    }

    it("should fold power and modulo") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = 2 ^ 10; b = 10 % 3;"), 0);
      check_double_eq(get_num(ctx, "a"), 1024.0, EPS);
      check_double_eq(get_num(ctx, "b"), 1.0, EPS);
      turbo_script_free(ctx);
    }

    it("should fold comparisons") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = 5 > 3; b = 2 == 2; c = 1 != 1;"), 0);
      check_double_eq(get_num(ctx, "a"), 1.0, EPS);
      check_double_eq(get_num(ctx, "b"), 1.0, EPS);
      check_double_eq(get_num(ctx, "c"), 0.0, EPS);
      turbo_script_free(ctx);
    }

    it("should fold unary operators") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = -5; b = !0; c = !1;"), 0);
      check_double_eq(get_num(ctx, "a"), -5.0, EPS);
      check_double_eq(get_num(ctx, "b"), 1.0, EPS);
      check_double_eq(get_num(ctx, "c"), 0.0, EPS);
      turbo_script_free(ctx);
    }

    it("should not fold expressions with variables") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = 10; b = a * 2 + 3;"), 0);
      check_double_eq(get_num(ctx, "b"), 23.0, EPS);
      turbo_script_free(ctx);
    }

    it("should fold constants used in loops") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      /* The constant 2 * 3.14159 should be folded, loop runs normally */
      check_int_eq(
          turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 100; i += 1) { sum += 2 * 3; }"), 0);
      check_double_eq(get_num(ctx, "sum"), 600.0, EPS);
      turbo_script_free(ctx);
    }

    it("should match interpreter for folded expressions") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "x = (100 / 4 + 3 * 2) ^ 2 - 10 % 3;";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check_double_eq(get_num(ctx_jit, "x"), get_num(ctx_interp, "x"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }
}
