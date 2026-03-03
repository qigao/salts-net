#include "turbo_script.h"
#include "../src/turbo_script_internal.h"
#include "tinytest.h"
#include <stdio.h>

spec("TurboScript Benchmark") {
  
  describe("Interpreter Performance (No MIR)") {
    
    bench("Loops and Math") {
      
      benchmark("1000 iters loop", 1000) {
        turbo_script_ctx_t *ctx = turbo_script_init();
        turbo_script_run(ctx, "sum = 0; for (i = 0; i < 1000; i += 1) { sum += i; }");
        turbo_script_free(ctx);
      }

      benchmark("10000 iters loop", 100) {
        turbo_script_ctx_t *ctx = turbo_script_init();
        turbo_script_run(ctx, "sum = 0; for (i = 0; i < 10000; i += 1) { sum += i; }");
        turbo_script_free(ctx);
      }

      benchmark("complex math 1000", 1000) {
        turbo_script_ctx_t *ctx = turbo_script_init();
        const char *script = "a = 10.5; b = 20.3; for(i=0; i<1000; i+=1) { c = (a * b) + (a / b) - (a + b); }";
        turbo_script_run(ctx, script);
        turbo_script_free(ctx);
      }
    }
    
    bench("Re-using context (Eval only)") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      
      benchmark("loop 1000 (re-use ctx)", 1000) {
        turbo_script_run(ctx, "sum = 0; for (i = 0; i < 1000; i += 1) { sum += i; }");
      }
      
      turbo_script_free(ctx);
    }
    
    bench("Compile vs Run") {
      turbo_script_ctx_t *ctx = turbo_script_init();
      const char *script = "sum = 0; for (i = 0; i < 1000; i += 1) { sum += i; }";
      turbo_script_compiled_t *compiled = turbo_script_compile(ctx, script);

      benchmark("run (parse + eval)", 1000) {
        turbo_script_run(ctx, script);
      }

      benchmark("exec (eval only)", 1000) {
        turbo_script_exec(ctx, compiled);
      }

      turbo_script_compiled_free(compiled);
      turbo_script_free(ctx);
    }

    bench("Function Overhead") {
        turbo_script_ctx_t *ctx = turbo_script_init();
        turbo_script_run(ctx, "func add(a, b) { return a + b; }");
        
        benchmark("inline add 1000", 1000) {
            turbo_script_run(ctx, "sum = 0; for(i=0; i<1000; i+=1) { sum = sum + 1 + 2; }");
        }

        benchmark("func call add 1000", 1000) {
            turbo_script_run(ctx, "sum = 0; for(i=0; i<1000; i+=1) { sum = add(sum, 1); }");
        }

        turbo_script_free(ctx);
    }
  }
  describe("Native JIT Comparison") {

    bench("Small Loop (1,000 iterations)") {
        turbo_script_ctx_t *ctx = turbo_script_init();
        const char *script_1k = "sum = 0; for (i = 0; i < 1000; i += 1) { sum += i; }";
        turbo_script_compile_mir(ctx, script_1k);
        benchmark("Interpreter", 1000) {
            turbo_script_run(ctx, script_1k);
        }
        benchmark("MIR JIT", 1000) {
            turbo_script_exec_jit(ctx);
        }
        turbo_script_free(ctx);
    }

    bench("Large Loop (100,000 iterations)") {
        turbo_script_ctx_t *ctx = turbo_script_init();
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        const char *script_100k = "sum = 0; for (i = 0; i < 100000; i += 1) { sum += i; }";
        turbo_script_compile_mir(ctx, script_100k);
        benchmark("Interpreter", 100) {
            ctx->env.curr_loop_iterations = 0;
            ctx->env.curr_nodes = 0;
            ctx->env.aborted = 0;
            turbo_script_run(ctx, script_100k);
        }
        benchmark("MIR JIT", 100) {
            turbo_script_exec_jit(ctx);
        }
        turbo_script_free(ctx);
    }

    bench("Arithmetic Intensity (FMA-heavy)") {
        turbo_script_ctx_t *ctx = turbo_script_init();
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        const char *script =
            "a = 1.5; b = 2.7; c = 0.0;"
            "for (i = 0; i < 10000; i += 1) {"
            "  c = (a * b + c) * 0.999 - (a / (b + 1.0));"
            "  a = a + 0.001; b = b - 0.001;"
            "}";
        turbo_script_compile_mir(ctx, script);
        benchmark("Interpreter", 100) {
            ctx->env.curr_loop_iterations = 0;
            ctx->env.curr_nodes = 0;
            ctx->env.aborted = 0;
            turbo_script_run(ctx, script);
        }
        benchmark("MIR JIT", 1000) {
            turbo_script_exec_jit(ctx);
        }
        turbo_script_free(ctx);
    }

    bench("Nested Loops (100 x 100)") {
        turbo_script_ctx_t *ctx = turbo_script_init();
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        const char *script =
            "total = 0;"
            "for (i = 0; i < 100; i += 1) {"
            "  for (j = 0; j < 100; j += 1) {"
            "    total += i * j;"
            "  }"
            "}";
        turbo_script_compile_mir(ctx, script);
        benchmark("Interpreter", 100) {
            ctx->env.curr_loop_iterations = 0;
            ctx->env.curr_nodes = 0;
            ctx->env.aborted = 0;
            turbo_script_run(ctx, script);
        }
        benchmark("MIR JIT", 1000) {
            turbo_script_exec_jit(ctx);
        }
        turbo_script_free(ctx);
    }

    bench("Branchy Code (if/else chain)") {
        turbo_script_ctx_t *ctx = turbo_script_init();
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        const char *script =
            "count = 0;"
            "for (i = 0; i < 10000; i += 1) {"
            "  r = i % 7;"
            "  if (r == 0) { count += 1; }"
            "  else if (r == 1) { count += 2; }"
            "  else if (r == 2) { count += 3; }"
            "  else if (r == 3) { count += 4; }"
            "  else { count += 5; }"
            "}";
        turbo_script_compile_mir(ctx, script);
        benchmark("Interpreter", 100) {
            ctx->env.curr_loop_iterations = 0;
            ctx->env.curr_nodes = 0;
            ctx->env.aborted = 0;
            turbo_script_run(ctx, script);
        }
        benchmark("MIR JIT", 1000) {
            turbo_script_exec_jit(ctx);
        }
        turbo_script_free(ctx);
    }

    bench("Compound Assignment Ops") {
        turbo_script_ctx_t *ctx = turbo_script_init();
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        const char *script =
            "a = 1.0; b = 2.0; c = 3.0;"
            "for (i = 0; i < 10000; i += 1) {"
            "  a += b; b -= 0.001; c *= 0.9999;"
            "  a /= 1.0001;"
            "}";
        turbo_script_compile_mir(ctx, script);
        benchmark("Interpreter", 100) {
            ctx->env.curr_loop_iterations = 0;
            ctx->env.curr_nodes = 0;
            ctx->env.aborted = 0;
            turbo_script_run(ctx, script);
        }
        benchmark("MIR JIT", 1000) {
            turbo_script_exec_jit(ctx);
        }
        turbo_script_free(ctx);
    }

    bench("Function Calls in Loop") {
        turbo_script_ctx_t *ctx = turbo_script_init();
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 100000000;
        turbo_script_run(ctx, "func square(x) { return x * x; }");
        ctx->env.curr_loop_iterations = 0;
        ctx->env.curr_nodes = 0;
        ctx->env.aborted = 0;
        const char *script =
            "sum = 0;"
            "for (i = 0; i < 5000; i += 1) {"
            "  sum += square(i);"
            "}";
        turbo_script_compile_mir(ctx, script);
        benchmark("Interpreter", 100) {
            ctx->env.curr_loop_iterations = 0;
            ctx->env.curr_nodes = 0;
            ctx->env.aborted = 0;
            turbo_script_run(ctx, script);
        }
        benchmark("MIR JIT (bridge)", 100) {
            ctx->env.curr_loop_iterations = 0;
            ctx->env.curr_nodes = 0;
            ctx->env.aborted = 0;
            turbo_script_exec_jit(ctx);
        }
        turbo_script_free(ctx);
    }

    bench("Comparison-Heavy (min/max search)") {
        turbo_script_ctx_t *ctx = turbo_script_init();
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        const char *script =
            "lo = 0; hi = 0; val = 0;"
            "for (i = 0; i < 10000; i += 1) {"
            "  val = (i * 7 + 13) % 1000;"
            "  if (val > hi) { hi = val; }"
            "  if (val < lo) { lo = val; }"
            "}";
        turbo_script_compile_mir(ctx, script);
        benchmark("Interpreter", 100) {
            ctx->env.curr_loop_iterations = 0;
            ctx->env.curr_nodes = 0;
            ctx->env.aborted = 0;
            turbo_script_run(ctx, script);
        }
        benchmark("MIR JIT", 1000) {
            turbo_script_exec_jit(ctx);
        }
        turbo_script_free(ctx);
    }

    bench("Switch in Loop") {
        turbo_script_ctx_t *ctx = turbo_script_init();
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        const char *script =
            "total = 0;"
            "for (i = 0; i < 5000; i += 1) {"
            "  m = i % 4;"
            "  switch (m) {"
            "    case 0: { total += 1; }"
            "    case 1: { total += 2; }"
            "    case 2: { total += 3; }"
            "    default: { total += 4; }"
            "  }"
            "}";
        turbo_script_compile_mir(ctx, script);
        benchmark("Interpreter", 100) {
            ctx->env.curr_loop_iterations = 0;
            ctx->env.curr_nodes = 0;
            ctx->env.aborted = 0;
            turbo_script_run(ctx, script);
        }
        benchmark("MIR JIT", 1000) {
            turbo_script_exec_jit(ctx);
        }
        turbo_script_free(ctx);
    }

    bench("Power & Modulo (math-heavy)") {
        turbo_script_ctx_t *ctx = turbo_script_init();
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        const char *script =
            "sum = 0;"
            "for (i = 1; i < 5000; i += 1) {"
            "  sum += (i ^ 0.5) + (i % 17);"
            "}";
        turbo_script_compile_mir(ctx, script);
        benchmark("Interpreter", 100) {
            ctx->env.curr_loop_iterations = 0;
            ctx->env.curr_nodes = 0;
            ctx->env.aborted = 0;
            turbo_script_run(ctx, script);
        }
        benchmark("MIR JIT", 1000) {
            turbo_script_exec_jit(ctx);
        }
        turbo_script_free(ctx);
    }

    bench("Logical Operators (short-circuit)") {
        turbo_script_ctx_t *ctx = turbo_script_init();
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        const char *script =
            "count = 0;"
            "for (i = 0; i < 10000; i += 1) {"
            "  if ((i > 100 && i < 9000) || i % 2 == 0) {"
            "    count += 1;"
            "  }"
            "}";
        turbo_script_compile_mir(ctx, script);
        benchmark("Interpreter", 100) {
            ctx->env.curr_loop_iterations = 0;
            ctx->env.curr_nodes = 0;
            ctx->env.aborted = 0;
            turbo_script_run(ctx, script);
        }
        benchmark("MIR JIT", 1000) {
            turbo_script_exec_jit(ctx);
        }
        turbo_script_free(ctx);
    }

    bench("Vector Sum (10,000 elements)") {
        turbo_script_ctx_t *ctx = turbo_script_init();
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        /* Create a 10K element vector */
        double *vec_data = (double *)malloc(10000 * sizeof(double));
        for (int i = 0; i < 10000; i++) vec_data[i] = (double)i;
        bind_vec(ctx, "v", vec_data, 10000);
        bind_num(ctx, "n", 10000);
        const char *script =
            "sum = 0;"
            "for (i = 0; i < n; i += 1) {"
            "  sum += v[i];"
            "}";
        turbo_script_compile_mir(ctx, script);
        benchmark("Interpreter", 100) {
            ctx->env.curr_loop_iterations = 0;
            ctx->env.curr_nodes = 0;
            ctx->env.aborted = 0;
            turbo_script_run(ctx, script);
        }
        benchmark("MIR JIT", 1000) {
            turbo_script_exec_jit(ctx);
        }
        free(vec_data);
        turbo_script_free(ctx);
    }

    bench("Dot Product (1,000 elements)") {
        turbo_script_ctx_t *ctx = turbo_script_init();
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        double *a_data = (double *)malloc(1000 * sizeof(double));
        double *b_data = (double *)malloc(1000 * sizeof(double));
        for (int i = 0; i < 1000; i++) { a_data[i] = (double)i * 0.1; b_data[i] = (double)(1000 - i) * 0.1; }
        bind_vec(ctx, "a", a_data, 1000);
        bind_vec(ctx, "b", b_data, 1000);
        const char *script =
            "dot = 0;"
            "for (i = 0; i < 1000; i += 1) {"
            "  dot += a[i] * b[i];"
            "}";
        turbo_script_compile_mir(ctx, script);
        benchmark("Interpreter", 100) {
            ctx->env.curr_loop_iterations = 0;
            ctx->env.curr_nodes = 0;
            ctx->env.aborted = 0;
            turbo_script_run(ctx, script);
        }
        benchmark("MIR JIT", 1000) {
            turbo_script_exec_jit(ctx);
        }
        free(a_data);
        free(b_data);
        turbo_script_free(ctx);
    }

    bench("Map Field Access (pre-bound, 10K reads)") {
        turbo_script_ctx_t *ctx = turbo_script_init();
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        /* Create a map before JIT compile so keys get resolved at compile time */
        turbo_script_run(ctx, "cfg = map {x: 1.5, y: 2.7, z: 0.3};");
        const char *script =
            "sum = 0;"
            "for (i = 0; i < 10000; i += 1) {"
            "  sum += cfg.x * cfg.y + cfg.z;"
            "}";
        turbo_script_compile_mir(ctx, script);
        benchmark("Interpreter", 100) {
            ctx->env.curr_loop_iterations = 0;
            ctx->env.curr_nodes = 0;
            ctx->env.aborted = 0;
            turbo_script_run(ctx, script);
        }
        benchmark("MIR JIT", 1000) {
            turbo_script_exec_jit(ctx);
        }
        turbo_script_free(ctx);
    }

  }
  describe("Lifecycle & Engine Overheads") {
      bench("System Costs") {
          benchmark("init_bare + free", 100) {
              turbo_script_ctx_t *c = turbo_script_init_bare();
              turbo_script_free(c);
          }
          benchmark("init_full + free", 100) {
              turbo_script_ctx_t *c = turbo_script_init();
              turbo_script_free(c);
          }

          turbo_script_ctx_t *ctx = turbo_script_init();
          const char *s = "x = 1; y = 2; z = x + y;";
          benchmark("compile/parse only", 1000) {
              turbo_script_compiled_t *comp = turbo_script_compile(ctx, s);
              turbo_script_compiled_free(comp);
          }
          turbo_script_free(ctx);
      }
  }
}
