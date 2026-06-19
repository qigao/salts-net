/************************** INCLUDE ***************************/

#include "ring_buffer_spsc.h"
#include <assert.h>
#include <string.h>

/*************************** MACRO ****************************/

#ifndef MIN
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#endif

/******************** PRIVATE FUNCTIONS **********************/

/**
 * @brief Check if a number is power of 2
 */
static inline bool is_power_of_2(size_t n) {
  return n != 0 && (n & (n - 1)) == 0;
}

/******************** EXPORTED FUNCTIONS **********************/

bool ring_spsc_init(ring_spsc_t *inst, uint8_t *data_array, size_t size) {
  assert(inst != NULL);
  assert(data_array != NULL);
  assert(size != 0);

  /* Size must be power of 2 for fast modulo */
  if (!is_power_of_2(size)) {
    return false;
  }

  inst->data = data_array;
  inst->size = size;
  inst->mask = size - 1;

  /* Initialize atomic counters */
  atomic_store_explicit(&inst->write_pos, 0, memory_order_relaxed);
  atomic_store_explicit(&inst->read_pos, 0, memory_order_relaxed);
  atomic_store_explicit(&inst->wrap_pos, 0, memory_order_relaxed);
  atomic_store_explicit(&inst->wrap_len, 0, memory_order_relaxed);
  inst->pending_wrap_len = 0;

  return true;
}

uint8_t *ring_spsc_write_acquire(ring_spsc_t *inst, size_t size_required) {
  assert(inst != NULL);
  assert(inst->data != NULL);
  assert(size_required > 0);

  /* Can't acquire more than buffer size - 1 */
  if (size_required >= inst->size) {
    return NULL;
  }

  /* Load current write position (relaxed - only producer modifies this) */
  const size_t w = atomic_load_explicit(&inst->write_pos, memory_order_relaxed);

  /* Load read position with acquire to see consumer's updates */
  const size_t r = atomic_load(&inst->read_pos);

  /* Calculate available space (reserve 1 byte to distinguish full from empty) */
  const size_t used = w - r;
  const size_t available = inst->size - used - 1;

  inst->pending_wrap_len = 0;

  if (size_required > available) {
    return NULL;
  }

  /* Calculate buffer position and contiguous space */
  const size_t buffer_pos = w & inst->mask;
  const size_t linear_space = inst->size - buffer_pos;

  /* If the tail is too small but the ring has total room, reserve tail padding
   * and publish it on release so the consumer can skip to the beginning. */
  if (size_required > linear_space) {
    if (atomic_load_explicit(&inst->wrap_len, memory_order_acquire) != 0) {
      return NULL;
    }
    if (size_required + linear_space > available) {
      return NULL;
    }
    inst->pending_wrap_len = linear_space;
    return inst->data;
  }

  /* IMPORTANT: We don't update write_pos here!
   * It will be updated in release() after data is written
   * This is safe because only one producer exists
   */

  return &inst->data[buffer_pos];
}

void ring_spsc_write_release(ring_spsc_t *inst, size_t bytes_written) {
  assert(inst != NULL);
  assert(inst->data != NULL);

  /* Advance write position with release semantics
   * This makes the written data visible to the consumer
   */
  const size_t w = atomic_load_explicit(&inst->write_pos, memory_order_relaxed);
  if (inst->pending_wrap_len != 0) {
    atomic_store_explicit(&inst->wrap_pos, w, memory_order_release);
    atomic_store_explicit(&inst->wrap_len, inst->pending_wrap_len, memory_order_release);
    atomic_store(&inst->write_pos, w + inst->pending_wrap_len + bytes_written);
    inst->pending_wrap_len = 0;
  } else {
    atomic_store(&inst->write_pos, w + bytes_written);
  }
}

uint8_t *ring_spsc_read_acquire(ring_spsc_t *inst, size_t *available) {
  assert(inst != NULL);
  assert(inst->data != NULL);
  assert(available != NULL);

  /* Load current read position (relaxed - only consumer modifies this) */
  size_t r = atomic_load_explicit(&inst->read_pos, memory_order_relaxed);

  /* Load write position with acquire to see producer's updates */
  const size_t w = atomic_load(&inst->write_pos);
  const size_t wrap_len = atomic_load_explicit(&inst->wrap_len, memory_order_acquire);
  const size_t wrap_pos = atomic_load_explicit(&inst->wrap_pos, memory_order_acquire);

  if (wrap_len != 0 && r == wrap_pos) {
    const size_t next_r = r + wrap_len;
    if (w < next_r) {
      *available = 0;
      return NULL;
    }
    r = next_r;
    atomic_store_explicit(&inst->read_pos, r, memory_order_release);
    atomic_store_explicit(&inst->wrap_len, 0, memory_order_release);
  }

  /* Calculate available data */
  const size_t data_available =
      (wrap_len != 0 && r < wrap_pos) ? (wrap_pos - r) : (w - r);
  if (data_available == 0) {
    *available = 0;
    return NULL;
  }

  /* Calculate buffer position and contiguous data */
  const size_t buffer_pos = r & inst->mask;
  const size_t linear_available = MIN(data_available, inst->size - buffer_pos);

  *available = linear_available;
  return &inst->data[buffer_pos];
}

void ring_spsc_read_release(ring_spsc_t *inst, size_t bytes_read) {
  assert(inst != NULL);
  assert(inst->data != NULL);

  /* Advance read position with release semantics
   * This makes the freed space visible to the producer
   */
  const size_t r = atomic_load_explicit(&inst->read_pos, memory_order_relaxed);
  atomic_store(&inst->read_pos, r + bytes_read);
}

size_t ring_spsc_write_available(const ring_spsc_t *inst) {
  assert(inst != NULL);

  const size_t w = atomic_load_explicit(&inst->write_pos, memory_order_relaxed);
  const size_t r = atomic_load(&inst->read_pos);

  const size_t used = w - r;
  return inst->size - used - 1;
}

size_t ring_spsc_read_available(const ring_spsc_t *inst) {
  assert(inst != NULL);

  const size_t w = atomic_load(&inst->write_pos);
  const size_t r = atomic_load_explicit(&inst->read_pos, memory_order_relaxed);
  const size_t wrap_len = atomic_load_explicit(&inst->wrap_len, memory_order_acquire);
  const size_t wrap_pos = atomic_load_explicit(&inst->wrap_pos, memory_order_acquire);

  if (wrap_len != 0 && r == wrap_pos && w >= r + wrap_len) {
    return w - r - wrap_len;
  }
  if (wrap_len != 0 && r < wrap_pos && w >= wrap_pos + wrap_len) {
    return (wrap_pos - r) + (w - wrap_pos - wrap_len);
  }

  return w - r;
}
