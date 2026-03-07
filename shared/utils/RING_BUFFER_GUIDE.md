# Ring Buffer Implementation Guide

## Overview

This project provides two ring buffer implementations, each optimized for different use cases:

1. **ring_buffer.c** - Single-threaded (fastest)
2. **ring_buffer_spsc.c** - Single-Producer Single-Consumer (fast)

For multiple producers/consumers, use **disruptor.c** (separate implementation).

## Quick Comparison

| Feature | ring_buffer | ring_buffer_spsc |
|---------|-------------|------------------|
| **Thread Safety** | ❌ None | ✅ SPSC only |
| **Atomic Operations** | ❌ No | ✅ Yes |
| **CAS Loops** | ❌ No | ❌ No |
| **Performance** | 🟢 Fastest (1.0x) | 🟡 Fast (1.3x) |
| **Complexity** | 🟢 Simple | 🟡 Medium |
| **Use Case** | Single thread | 1 producer + 1 consumer |

**For multiple producers/consumers**: Use `disruptor.c` (see `bench_disruptor.c`)

---

## 1. ring_buffer.c - Single-Threaded

### When to Use
- ✅ Single-threaded applications
- ✅ Embedded systems without threading
- ✅ Maximum performance required
- ✅ Simple state machines

### When NOT to Use
- ❌ Multiple threads accessing the buffer
- ❌ Producer and consumer in different threads

### Example
```c
#include "ring_buffer.h"

uint8_t buffer[1024];
ring_data_type ring;

// Initialize
ring_init(&ring, buffer, sizeof(buffer));

// Write
uint8_t *ptr = ring_write_acquire(&ring, 100);
if (ptr) {
    memcpy(ptr, data, 100);
    ring_write_release(&ring, 100);
}

// Read
size_t available;
uint8_t *data = ring_read_acquire(&ring, &available);
if (data) {
    process(data, available);
    ring_read_release(&ring, available);
}
```

### Key Characteristics
- **No atomic operations** - Direct memory access
- **No memory barriers** - No synchronization overhead
- **Wrapped flags** - Handles buffer wrapping with boolean flags
- **Invalidate index** - Tracks wrapped regions

---

## 2. ring_buffer_spsc.c - Single-Producer Single-Consumer

### When to Use
- ✅ One producer thread + one consumer thread
- ✅ Audio/video streaming pipelines
- ✅ Network packet processing (RX thread → processing thread)
- ✅ Logging (worker thread → logger thread)

### When NOT to Use
- ❌ Multiple producers or multiple consumers
- ❌ Single-threaded applications (use ring_buffer.c instead)

### Example
```c
#include "ring_buffer_spsc.h"

uint8_t buffer[1024];  // Must be power of 2!
ring_spsc_t ring;

// Initialize (size MUST be power of 2)
if (!ring_spsc_init(&ring, buffer, sizeof(buffer))) {
    // Error: size not power of 2
}

// Producer thread
uint8_t *ptr = ring_spsc_write_acquire(&ring, 100);
if (ptr) {
    memcpy(ptr, data, 100);
    ring_spsc_write_release(&ring, 100);
}

// Consumer thread
size_t available;
uint8_t *data = ring_spsc_read_acquire(&ring, &available);
if (data) {
    process(data, available);
    ring_spsc_read_release(&ring, available);
}
```

### Key Characteristics
- **Atomic operations** - Lock-free thread safety
- **No CAS loops** - Single producer/consumer = no contention
- **Cache-line alignment** - Prevents false sharing
- **Power-of-2 size** - Fast modulo using bit mask
- **No wrapped flags** - Simplified algorithm (good taste!)

### Performance Notes
- Faster than disruptor (no CAS overhead)
- Slower than ring_buffer (atomic operations cost)
- Typical overhead: ~10-20 CPU cycles per operation

---

## 3. disruptor.c - Multi-Producer Multi-Consumer

**Separate implementation** - See `disruptor.h` and `bench_disruptor.c`

### When to Use
- ✅ Multiple producer threads
- ✅ Multiple consumer threads
- ✅ High-throughput event processing
- ✅ Complex data flow patterns

### Key Characteristics
- **Full MPMC support** - Any number of producers/consumers
- **CAS loops** - Handle contention between producers
- **Sequence tracking** - Ensures ordered publication
- **Batch operations** - Amortize synchronization cost

---

## Design Philosophy (Linus-Style "Good Taste")

### What Changed from Original ring_buffer.c

**❌ Removed (Bad Taste)**:
- Atomic operations in single-threaded code (unnecessary overhead)
- `RING_MULTICORE_HOSTED` macro (confusing, pick one implementation)
- Cache-line alignment in single-threaded version (pointless)

**✅ Added (Good Taste)**:
- Clear separation of concerns (3 files, 3 use cases)
- Explicit naming (ring_spsc_* makes thread model obvious)
- Power-of-2 requirement in SPSC (fast modulo, no special cases)
- Simplified algorithm in SPSC (no wrapped flags, no invalidate index)

### Key Principles Applied

1. **"Good programmers worry about data structures"**
   - SPSC uses monotonic counters (write_pos, read_pos)
   - No complex state flags, just simple arithmetic

2. **"Eliminate special cases"**
   - SPSC uses mask for wrapping (no if statements)
   - No "wrapped" flags, no "invalidate index"

3. **"Theory and practice sometimes clash. Theory loses."**
   - Could make ring_buffer.c work with threads (theory)
   - But it's faster to have separate implementations (practice)

4. **"Be explicit about constraints"**
   - ring_buffer.c: "Single-threaded ONLY"
   - ring_buffer_spsc.c: "ONE producer, ONE consumer"
   - disruptor.c: "Multiple producers/consumers OK"

---

## Performance Comparison

Approximate relative performance (lower is better):

```
Operation: Write 1KB + Read 1KB

ring_buffer.c:        1.0x  (baseline, ~50 cycles)
ring_buffer_spsc.c:   1.3x  (~65 cycles, atomic overhead)
disruptor.c:          2.5x  (~125 cycles, CAS + sequence tracking)
                      ↑ Separate implementation, see bench_disruptor.c
```

**Conclusion**: Use the simplest implementation that meets your threading requirements.

---

## Migration Guide

### From old ring_buffer.c to new versions

**If you're single-threaded**:
```c
// No changes needed! Just update includes if moved
#include "ring_buffer.h"  // Still works
```

**If you have 1 producer + 1 consumer**:
```c
// Old (incorrect for threading)
#include "ring_buffer.h"
ring_data_type ring;
ring_init(&ring, buffer, size);

// New (correct)
#include "ring_buffer_spsc.h"
ring_spsc_t ring;
ring_spsc_init(&ring, buffer, size);  // size must be power of 2!
```

**If you have multiple producers/consumers**:
```c
// Use disruptor.c instead
#include "disruptor.h"
// See disruptor.h for API
```

---

## FAQ

**Q: Why three implementations?**
A: Each is optimized for its use case. Trying to make one implementation handle all cases results in complexity and poor performance.

**Q: Why does SPSC require power-of-2 size?**
A: Fast modulo using bit mask (`pos & (size-1)`) instead of division. This eliminates branches and special cases.

**Q: Can I use ring_buffer.c with threads if I'm careful?**
A: No. Even if you think you're careful, you'll hit race conditions. Use ring_buffer_spsc.c.

**Q: Why not just use disruptor for everything?**
A: Performance. If you only have 1 producer + 1 consumer, SPSC is 2x faster. If you're single-threaded, ring_buffer is 2.5x faster.

**Q: What about MPSC (multi-producer single-consumer)?**
A: Use disruptor.c with one consumer. It handles this case efficiently.

---

## Summary

**Choose based on your threading model**:
- No threads → `ring_buffer.c`
- 1 producer + 1 consumer → `ring_buffer_spsc.c`
- Multiple producers/consumers → `disruptor.c`

**When in doubt**: Start with the simplest that works, optimize later if needed.
