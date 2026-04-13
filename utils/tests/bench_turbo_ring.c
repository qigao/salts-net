#include "ring_buffer.h"
#include "tinytest.h"
#include "platform.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

/**
 * @file bench_turbo_ring.c
 * @brief Benchmarks for ring_buffer.c (SINGLE-THREADED version)
 *
 * NOTE: This file tests ring_buffer.c which is designed for single-threaded use.
 * For thread-safe benchmarks, see:
 * - bench_ring_comparison.c (compares all three implementations)
 */

#define BUFFER_SIZE (1024 * 1024)
#define BENCH_ITERS (1024 * 1024)         // 1,048,576
#define BENCH_ITERS_LARGE (10 * 1024 * 1024) // 10,485,760

static uint8_t *bench_data;
static ring_data_type bench_ring;

spec("Ring Buffer - Single Threaded Only") {
    before_each() {
        bench_data = (uint8_t *)malloc(BUFFER_SIZE);
        ring_init(&bench_ring, bench_data, BUFFER_SIZE);
    }

    after_each() {
        free(bench_data);
    }

    it("should be used in single-threaded context only") {
        printf("\n");
        printf("╔════════════════════════════════════════════════════════════════╗\n");
        printf("║  WARNING: ring_buffer.c is for SINGLE-THREADED use only!      ║\n");
        printf("║                                                                ║\n");
        printf("║  For thread-safe versions, use:                               ║\n");
        printf("║  - ring_buffer_spsc.c (1 producer + 1 consumer)               ║\n");
        printf("║  - disruptor.c (multiple producers/consumers)                 ║\n");
        printf("╚════════════════════════════════════════════════════════════════╝\n");
        printf("\n");
    }

    bench("Basic Operations") {

        benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
        benchmark("Write Acquire (64 bytes)", BENCH_ITERS, 1) {
            uint8_t *p = ring_write_acquire(&bench_ring, 64);
            if (p) {
                ring_write_release(&bench_ring, 64);
            }
        }

        benchmark("Read Acquire (Empty)", BENCH_ITERS, 1) {
            size_t avail = 0;
            (void)ring_read_acquire(&bench_ring, &avail);
        }

        benchmark("Write+Read (64 bytes)", BENCH_ITERS, 1) {
            uint8_t *p = ring_write_acquire(&bench_ring, 64);
            if (p) {
                ring_write_release(&bench_ring, 64);
                size_t avail = 0;
                uint8_t *rp = ring_read_acquire(&bench_ring, &avail);
                if (rp) {
                    ring_read_release(&bench_ring, (avail < 64) ? avail : 64);
                }
            }
        }
    }

    bench("Different Sizes") {

        benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
        benchmark("Write+Read (16 bytes)", BENCH_ITERS, 1) {
            uint8_t *p = ring_write_acquire(&bench_ring, 16);
            if (p) {
                ring_write_release(&bench_ring, 16);
                size_t avail = 0;
                uint8_t *rp = ring_read_acquire(&bench_ring, &avail);
                if (rp) {
                    ring_read_release(&bench_ring, (avail < 16) ? avail : 16);
                }
            }
        }

        benchmark("Write+Read (256 bytes)", BENCH_ITERS, 1) {
            uint8_t *p = ring_write_acquire(&bench_ring, 256);
            if (p) {
                ring_write_release(&bench_ring, 256);
                size_t avail = 0;
                uint8_t *rp = ring_read_acquire(&bench_ring, &avail);
                if (rp) {
                    ring_read_release(&bench_ring, (avail < 256) ? avail : 256);
                }
            }
        }

        benchmark("Write+Read (1KB)", BENCH_ITERS, 1) {
            uint8_t *p = ring_write_acquire(&bench_ring, 1024);
            if (p) {
                ring_write_release(&bench_ring, 1024);
                size_t avail = 0;
                uint8_t *rp = ring_read_acquire(&bench_ring, &avail);
                if (rp) {
                    ring_read_release(&bench_ring, (avail < 1024) ? avail : 1024);
                }
            }
        }

        benchmark("Write+Read (4KB)", BENCH_ITERS, 1) {
            uint8_t *p = ring_write_acquire(&bench_ring, 4096);
            if (p) {
                ring_write_release(&bench_ring, 4096);
                size_t avail = 0;
                uint8_t *rp = ring_read_acquire(&bench_ring, &avail);
                if (rp) {
                    ring_read_release(&bench_ring, (avail < 4096) ? avail : 4096);
                }
            }
        }
    }

    bench("With Memory Operations") {

        benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
        uint8_t local_buf[4096];
        memset(local_buf, 0xAA, sizeof(local_buf));

        benchmark("Write+Memcpy+Read (256 bytes)", BENCH_ITERS, 1) {
            uint8_t *p = ring_write_acquire(&bench_ring, 256);
            if (p) {
                memcpy(p, local_buf, 256);
                ring_write_release(&bench_ring, 256);
                size_t avail = 0;
                uint8_t *rp = ring_read_acquire(&bench_ring, &avail);
                if (rp) {
                    memcpy(local_buf, rp, (avail < 256) ? avail : 256);
                    ring_read_release(&bench_ring, (avail < 256) ? avail : 256);
                }
            }
        }

        benchmark("Write+Memcpy+Read (1KB)", BENCH_ITERS, 1) {
            uint8_t *p = ring_write_acquire(&bench_ring, 1024);
            if (p) {
                memcpy(p, local_buf, 1024);
                ring_write_release(&bench_ring, 1024);
                size_t avail = 0;
                uint8_t *rp = ring_read_acquire(&bench_ring, &avail);
                if (rp) {
                    memcpy(local_buf, rp, (avail < 1024) ? avail : 1024);
                    ring_read_release(&bench_ring, (avail < 1024) ? avail : 1024);
                }
            }
        }

        benchmark("Write+Memcpy+Read (4KB)", BENCH_ITERS, 1) {
            uint8_t *p = ring_write_acquire(&bench_ring, 4096);
            if (p) {
                memcpy(p, local_buf, 4096);
                ring_write_release(&bench_ring, 4096);
                size_t avail = 0;
                uint8_t *rp = ring_read_acquire(&bench_ring, &avail);
                if (rp) {
                    memcpy(local_buf, rp, (avail < 4096) ? avail : 4096);
                    ring_read_release(&bench_ring, (avail < 4096) ? avail : 4096);
                }
            }
        }
    }

    bench("High Throughput") {

        benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
        benchmark("Sustained Write+Read (64B)", BENCH_ITERS_LARGE, 1) {
            uint8_t *p = ring_write_acquire(&bench_ring, 64);
            if (p) {
                ring_write_release(&bench_ring, 64);
                size_t avail = 0;
                uint8_t *rp = ring_read_acquire(&bench_ring, &avail);
                if (rp) {
                    ring_read_release(&bench_ring, (avail < 64) ? avail : 64);
                }
            }
        }
    }
}
