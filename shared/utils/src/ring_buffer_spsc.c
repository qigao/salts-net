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
  t_atomic_store_size_relaxed(&inst->write_pos, 0);
  t_atomic_store_size_relaxed(&inst->read_pos, 0);

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
  const size_t w = t_atomic_load_size_relaxed(&inst->write_pos);

  /* Load read position with acquire to see consumer's updates */
  const size_t r = t_atomic_load_size_acquire(&inst->read_pos);

  /* Calculate available space (reserve 1 byte to distinguish full from empty) */
  const size_t used = w - r;
  const size_t available = inst->size - used - 1;

  if (size_required > available) {
    return NULL;
  }

  /* Calculate buffer position and contiguous space */
  const size_t buffer_pos = w & inst->mask;
  const size_t linear_space = inst->size - buffer_pos;

  /* Only return pointer if we have enough contiguous space */
  if (size_required > linear_space) {
    return NULL;  /* Need to wrap, caller should try again after consumer reads */
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
  const size_t w = t_atomic_load_size_relaxed(&inst->write_pos);
  t_atomic_store_size_release(&inst->write_pos, w + bytes_written);
}

uint8_t *ring_spsc_read_acquire(ring_spsc_t *inst, size_t *available) {
  assert(inst != NULL);
  assert(inst->data != NULL);
  assert(available != NULL);

  /* Load current read position (relaxed - only consumer modifies this) */
  const size_t r = t_atomic_load_size_relaxed(&inst->read_pos);

  /* Load write position with acquire to see producer's updates */
  const size_t w = t_atomic_load_size_acquire(&inst->write_pos);

  /* Calculate available data */
  const size_t data_available = w - r;
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
  const size_t r = t_atomic_load_size_relaxed(&inst->read_pos);
  t_atomic_store_size_release(&inst->read_pos, r + bytes_read);
}

size_t ring_spsc_write_available(const ring_spsc_t *inst) {
  assert(inst != NULL);

  const size_t w = t_atomic_load_size_relaxed(&inst->write_pos);
  const size_t r = t_atomic_load_size_acquire(&inst->read_pos);

  const size_t used = w - r;
  return inst->size - used - 1;
}

size_t ring_spsc_read_available(const ring_spsc_t *inst) {
  assert(inst != NULL);

  const size_t w = t_atomic_load_size_acquire(&inst->write_pos);
  const size_t r = t_atomic_load_size_relaxed(&inst->read_pos);

  return w - r;
}
