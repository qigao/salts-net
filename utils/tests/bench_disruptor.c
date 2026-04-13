#include "disruptor.h"
#include "tinytest.h"
#include "platform.h"
#include "turbo_thread.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#define CAPACITY 16384
#define BENCH_ITERS (1024 * 1024)         
#define BENCH_ITERS_LARGE (10 * 1024 * 1024) 

typedef struct {
    disruptor_t *disruptor;
    size_t count;
    size_t batch_size;
    bool use_memcpy;
    atomic_int *start;
} producer_ctx_t;

typedef struct {
    disruptor_t *disruptor;
    disruptor_consumer_t consumer;
    size_t count;
    size_t batch_size;
    bool use_memcpy;
    atomic_int *start;
} consumer_ctx_t;

static void producer_thread(void *arg) {
    producer_ctx_t *ctx = (producer_ctx_t *)arg;
    uint8_t local_buf[4096];
    memset(local_buf, 0xAA, sizeof(local_buf));
    size_t entry_size = disruptor_entry_size(ctx->disruptor);

    while (atomic_load(ctx->start) == 0) {
        turbo_sleep_ms(1);
    }

    size_t produced = 0;
    while (produced < ctx->count) {
        size_t to_write = (ctx->count - produced < ctx->batch_size) ? (ctx->count - produced) : ctx->batch_size;
        
        disruptor_sequence_range_t range;
        if (disruptor_publisher_claim_n_blocking(ctx->disruptor, (uint32_t)to_write, &range)) {
            for (uint64_t seq = range.first_sequence; seq <= range.last_sequence; ++seq) {
                disruptor_cursor_t c = { .sequence = seq };
                uint8_t *p = (uint8_t*)disruptor_acquire_entry(ctx->disruptor, &c);
                if (ctx->use_memcpy) {
                    memcpy(p, local_buf, entry_size);
                }
            }
            disruptor_publisher_publish_range(ctx->disruptor, &range);
            produced += to_write;
        } else {
            turbo_sleep_ms(0);
        }
    }
}

static void consumer_thread(void *arg) {
    consumer_ctx_t *ctx = (consumer_ctx_t *)arg;
    uint8_t local_buf[4096];
    size_t entry_size = disruptor_entry_size(ctx->disruptor);

    while (atomic_load(ctx->start) == 0) {
        turbo_sleep_ms(1);
    }

    uint64_t next_seq = 1;
    size_t consumed = 0;
    
    while (consumed < ctx->count) {
        disruptor_cursor_t c = { .sequence = next_seq };
        disruptor_consumer_wait_for_blocking(ctx->disruptor, &c);
        
        uint64_t available_seq = c.sequence;
        if (available_seq < next_seq) {
            turbo_sleep_ms(0);
            continue;
        }
        
        uint64_t available_count = available_seq - next_seq + 1;
        uint64_t limit_seq = available_seq;
        if (available_count > ctx->batch_size) {
            limit_seq = next_seq + ctx->batch_size - 1;
        }
        if (consumed + (limit_seq - next_seq + 1) > ctx->count) {
            limit_seq = next_seq + (ctx->count - consumed) - 1;
        }
        
        for (uint64_t seq = next_seq; seq <= limit_seq; ++seq) {
            disruptor_cursor_t seq_c = { .sequence = seq };
            const uint8_t *p = (const uint8_t*)disruptor_show_entry(ctx->disruptor, &seq_c);
            if (ctx->use_memcpy) {
                memcpy(local_buf, p, entry_size);
            } else {
                volatile uint8_t sink = p[0];
                (void)sink;
            }
        }
        
        disruptor_cursor_t release_c = { .sequence = limit_seq };
        disruptor_consumer_release_entry(ctx->disruptor, &ctx->consumer, &release_c);
        
        consumed += (size_t)(limit_seq - next_seq + 1);
        next_seq = limit_seq + 1;
    }
}

static void run_bench(__bdd_config_type__ *__bdd_config__, const char *name, size_t count, size_t batch, size_t entry_size, bool use_memcpy, size_t num_prods, size_t num_cons) {
    if (num_cons > 8 || num_prods > 8 || num_cons == 0 || num_prods == 0) return;
    
    disruptor_config_t d_cfg = {
        .entry_size = entry_size,
        .capacity = CAPACITY,
        .consumer_capacity = (uint32_t)num_cons
    };
    disruptor_t *disruptor = disruptor_create(&d_cfg);
    
    atomic_int start_flag;
    atomic_store(&start_flag, 0);

    producer_ctx_t p_ctx[8];
    consumer_ctx_t c_ctx[8];
    turbo_thread_t prods[8];
    turbo_thread_t cons[8];

    size_t total_messages = count * num_prods;

    for (size_t i = 0; i < num_cons; ++i) {
        c_ctx[i].disruptor = disruptor;
        c_ctx[i].count = total_messages;
        c_ctx[i].batch_size = batch;
        c_ctx[i].use_memcpy = use_memcpy;
        c_ctx[i].start = &start_flag;
        disruptor_consumer_register(disruptor, &c_ctx[i].consumer);
        turbo_thread_create(&cons[i], consumer_thread, &c_ctx[i]);
    }

    for (size_t i = 0; i < num_prods; ++i) {
        p_ctx[i].disruptor = disruptor;
        p_ctx[i].count = count;
        p_ctx[i].batch_size = batch;
        p_ctx[i].use_memcpy = use_memcpy;
        p_ctx[i].start = &start_flag;
        turbo_thread_create(&prods[i], producer_thread, &p_ctx[i]);
    }

    turbo_sleep_ms(50);
    uint64_t start_time = turbo_hrtime();
    atomic_store(&start_flag, 1);

    for (size_t i = 0; i < num_prods; ++i) {
        turbo_thread_join(&prods[i]);
    }
    for (size_t i = 0; i < num_cons; ++i) {
        turbo_thread_join(&cons[i]);
    }
    
    uint64_t end_time = turbo_hrtime();

    double dur_ms = (double)(end_time - start_time) / 1000000.0;
    double avg_ms = dur_ms / (double)total_messages;
    
    __bdd_bench_add__(__bdd_config__, name, total_messages, dur_ms, avg_ms, avg_ms, 1.0);
    
    for (size_t i = 0; i < num_cons; ++i) {
        disruptor_consumer_unregister(disruptor, &c_ctx[i].consumer);
    }
    disruptor_destroy(disruptor);
}

static disruptor_t *bench_disruptor;
static disruptor_consumer_t bench_consumer;
static uint64_t global_seq = 1;

spec("Turbo Disruptor Benchmarks") {
    before_each() {
        disruptor_config_t cfg = {
            .entry_size = 64,
            .capacity = CAPACITY,
            .consumer_capacity = 1
        };
        bench_disruptor = disruptor_create(&cfg);
        global_seq = disruptor_consumer_register(bench_disruptor, &bench_consumer);
    }

    after_each() {
        disruptor_consumer_unregister(bench_disruptor, &bench_consumer);
        disruptor_destroy(bench_disruptor);
    }

    bench("Single Threaded Operations") {

        benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
        benchmark("ReadAcquire (Empty)", BENCH_ITERS, 1) {
            disruptor_cursor_t rc = { .sequence = global_seq };
            (void)disruptor_consumer_wait_for_nonblocking(bench_disruptor, &rc);
        }

        benchmark("Write+Read (64 bytes)", BENCH_ITERS, 1) {
            disruptor_cursor_t wc;
            if (disruptor_publisher_try_claim(bench_disruptor, &wc)) {
                disruptor_publisher_publish(bench_disruptor, &wc);
                
                disruptor_cursor_t rc = { .sequence = global_seq };
                if (disruptor_consumer_wait_for_nonblocking(bench_disruptor, &rc) && rc.sequence >= global_seq) {
                    disruptor_cursor_t rel = { .sequence = global_seq };
                    disruptor_consumer_release_entry(bench_disruptor, &bench_consumer, &rel);
                    global_seq++;
                }
            }
        }
    }

    bench("SPSC Multithreaded Detailed Benchmarks") {

        benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
        run_bench(__bdd_config__, "SPSC (Batch 64)", BENCH_ITERS, 64, 64, false, 1, 1);
        run_bench(__bdd_config__, "SPSC No Batching (Batch 1)", 100000, 1, 64, false, 1, 1);
        run_bench(__bdd_config__, "SPSC High Cont (Batch 128)", BENCH_ITERS_LARGE, 128, 64, false, 1, 1);
        run_bench(__bdd_config__, "SPSC with Memcpy (Batch 512)", BENCH_ITERS, 512, 64, true, 1, 1);
        run_bench(__bdd_config__, "SPSC with Memcpy (1K)", BENCH_ITERS, 1024, 64, true, 1, 1);
        run_bench(__bdd_config__, "SPSC with Memcpy (4K)", BENCH_ITERS, 4096, 64, true, 1, 1);
    }

    bench("MPMC Multithreaded Detailed Benchmarks") {

        benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
        run_bench(__bdd_config__, "MPSC 2P 1C (Batch 64)", BENCH_ITERS, 64, 64, false, 2, 1);
        run_bench(__bdd_config__, "MPSC 4P 1C (Batch 64)", BENCH_ITERS, 64, 64, false, 4, 1);
        
        run_bench(__bdd_config__, "SPMC 1P 2C (Batch 64)", BENCH_ITERS, 64, 64, false, 1, 2);
        run_bench(__bdd_config__, "SPMC 1P 4C (Batch 64)", BENCH_ITERS, 64, 64, false, 1, 4);

        run_bench(__bdd_config__, "MPMC 2P 2C (Batch 64)", BENCH_ITERS, 64, 64, false, 2, 2);
        run_bench(__bdd_config__, "MPMC 4P 4C (Batch 64)", BENCH_ITERS, 64, 64, false, 4, 4);
        
        run_bench(__bdd_config__, "MPMC 4P 4C with Memcpy (Batch 512)", BENCH_ITERS/4, 512, 64, true, 4, 4);
    }
}
