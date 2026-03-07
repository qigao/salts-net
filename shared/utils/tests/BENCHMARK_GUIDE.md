# Ring Buffer Benchmarks

## Overview

This directory contains comprehensive benchmarks for all three ring buffer implementations:

1. **bench_turbo_ring.c** - Single-threaded ring_buffer.c benchmarks
2. **bench_ring_comparison.c** - Comprehensive comparison of all three implementations
3. **bench_disruptor.c** - Disruptor-specific benchmarks (existing)

## Quick Start

```bash
# Run all benchmarks
./run_benchmarks

# Run specific benchmark
./bench_turbo_ring        # Single-threaded only
./bench_ring_comparison   # Compare all three
./bench_disruptor         # Disruptor detailed tests
```

## Benchmark Files

### 1. bench_turbo_ring.c

**Tests**: `ring_buffer.c` (single-threaded version)

**What it measures**:
- Basic operations (write acquire, read acquire)
- Different data sizes (16B, 64B, 256B, 1KB, 4KB)
- With/without memcpy
- High throughput sustained operations

**Expected results**:
- ~50 cycles per write+read operation
- No atomic operations overhead
- Fastest of all three implementations

**Use this when**: You want to verify single-threaded performance

---

### 2. bench_ring_comparison.c ⭐ RECOMMENDED

**Tests**: All three implementations side-by-side

**What it measures**:
- `ring_buffer.c` - Single-threaded baseline
- `ring_buffer_spsc.c` - SPSC thread-safe
- `disruptor.c` - MPMC full-featured

**Output format**:
```
=== Single-Threaded (ring_buffer.c) ===
  Write+Read 64B: 1250.5 MB/s, 40.2 ns/op

=== SPSC (ring_buffer_spsc.c) ===
  SPSC Batch 64B: 980.3 MB/s, 51.3 ns/op
  SPSC Batch 256B: 1150.2 MB/s, 43.7 ns/op
  SPSC Batch 1KB: 1320.8 MB/s, 38.1 ns/op

=== Disruptor (MPMC) ===
  Disruptor 64B: 520.1 MB/s, 96.8 ns/op
  Disruptor 256B: 680.5 MB/s, 73.9 ns/op
```

**Use this when**: You want to compare implementations and choose the right one

---

### 3. bench_disruptor.c

**Tests**: `disruptor.c` detailed scenarios

**What it measures**:
- Multiple producers/consumers
- Batch operations
- Sequence tracking overhead
- Complex data flow patterns

**Use this when**: You're specifically using disruptor and want detailed metrics

---

## Expected Performance Ratios

Based on typical x86-64 hardware:

| Implementation | Relative Speed | Typical ns/op | Use Case |
|----------------|----------------|---------------|----------|
| ring_buffer.c | 1.0x (baseline) | ~40-50 ns | Single-threaded |
| ring_buffer_spsc.c | ~1.3x slower | ~50-65 ns | 1 producer + 1 consumer |
| disruptor.c | ~2.5x slower | ~100-125 ns | Multiple producers/consumers |

**Note**: Actual performance depends on:
- CPU architecture (cache size, memory bandwidth)
- Data size (small vs large batches)
- Contention level (how often threads compete)
- Memory access patterns

---

## Understanding the Results

### Good Performance Indicators

✅ **ring_buffer.c**:
- < 50 ns/op for 64B operations
- Scales linearly with data size
- No variance between runs (deterministic)

✅ **ring_buffer_spsc.c**:
- < 70 ns/op for 64B operations
- 1.2-1.5x slower than single-threaded
- Consistent throughput under load

✅ **disruptor.c**:
- < 150 ns/op for 64B operations
- Handles multiple producers without deadlock
- Throughput increases with batch size

### Red Flags

❌ **If you see**:
- ring_buffer.c slower than SPSC → Something wrong with compiler optimization
- SPSC 3x+ slower than single-threaded → False sharing or cache issues
- Disruptor slower than 200 ns/op → Excessive contention or wrong configuration

---

## Interpreting Throughput

### MB/s (Megabytes per second)

```
Throughput = (Total bytes transferred) / (Time in seconds)
```

**Example**:
- 1M operations × 64 bytes = 64 MB
- Completed in 0.05 seconds
- Throughput = 64 / 0.05 = 1280 MB/s

**What's good?**:
- Single-threaded: > 1000 MB/s
- SPSC: > 800 MB/s
- MPMC: > 400 MB/s

### ns/op (Nanoseconds per operation)

```
ns/op = (Total time in ns) / (Number of operations)
```

**Example**:
- 1M operations in 50 ms
- ns/op = 50,000,000 / 1,000,000 = 50 ns

**What's good?**:
- Single-threaded: < 50 ns
- SPSC: < 70 ns
- MPMC: < 150 ns

---

## Benchmark Configuration

### Buffer Sizes

All benchmarks use:
```c
#define BUFFER_SIZE (1024 * 1024)  // 1MB ring buffer
```

**Why 1MB?**:
- Large enough to avoid constant wrapping
- Small enough to fit in L3 cache
- Realistic for most applications

### Iteration Counts

```c
#define BENCH_ITERS (1024 * 1024)         // 1M ops - standard
#define BENCH_ITERS_LARGE (10 * 1024 * 1024) // 10M ops - stress test
```

**Why these numbers?**:
- 1M ops: ~50ms runtime, good for quick tests
- 10M ops: ~500ms runtime, better statistical significance

### Data Sizes Tested

- **16 bytes**: Small messages (e.g., control packets)
- **64 bytes**: Typical cache line size
- **256 bytes**: Medium messages
- **1KB**: Large messages (e.g., JSON payloads)
- **4KB**: Page-sized transfers

---

## Running Custom Benchmarks

### Modify Iteration Count

```c
// In bench_ring_comparison.c
#define BENCH_ITERS (10 * 1024 * 1024)  // Increase for longer test
```

### Test Different Batch Sizes

```c
// In bench_ring_comparison.c, add new test:
run_spsc_bench(__bdd_config__, "SPSC Custom Batch", BENCH_ITERS, 2048, false);
```

### Test Different Buffer Sizes

```c
// Change BUFFER_SIZE at the top of the file
#define BUFFER_SIZE (4 * 1024 * 1024)  // 4MB buffer
```

---

## Troubleshooting

### Benchmark runs too fast (< 1ms)

**Problem**: Not enough iterations, results unreliable

**Solution**: Increase `BENCH_ITERS`

### Benchmark runs too slow (> 10s)

**Problem**: Too many iterations, wasting time

**Solution**: Decrease `BENCH_ITERS` or reduce data size

### Inconsistent results

**Problem**: System interference (other processes, thermal throttling)

**Solutions**:
- Close other applications
- Run multiple times and average
- Pin threads to specific CPU cores
- Disable CPU frequency scaling

### SPSC slower than expected

**Problem**: False sharing between producer and consumer

**Check**:
- Cache line alignment (should be 64 bytes)
- CPU topology (producer/consumer on same core?)

**Solution**:
```c
// Verify alignment in ring_buffer_spsc.h
alignas(64) turbo_atomic_size_t write_pos;
alignas(64) turbo_atomic_size_t read_pos;
```

---

## Comparison with Other Implementations

### vs. Linux kfifo

- **kfifo**: Kernel-space, similar to ring_buffer_spsc.c
- **Expected**: Our SPSC should be comparable or faster (no kernel overhead)

### vs. Boost lockfree::spsc_queue

- **Boost**: C++ template-based, similar design
- **Expected**: Similar performance (both use atomic operations)

### vs. LMAX Disruptor (Java)

- **LMAX**: Original disruptor pattern
- **Expected**: Our C implementation should be faster (no GC, direct memory)

---

## Summary

**Quick decision guide**:

1. **Run bench_ring_comparison.c first** - See all three side-by-side
2. **Choose based on your threading model**:
   - No threads → ring_buffer.c
   - 1 producer + 1 consumer → ring_buffer_spsc.c
   - Multiple producers/consumers → disruptor.c
3. **Verify with specific benchmark** - Run detailed tests for your chosen implementation

**Remember**: The fastest code is the simplest code that meets your requirements!
