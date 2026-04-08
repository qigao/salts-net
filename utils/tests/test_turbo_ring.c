#include "ring_buffer.h"
#include "tinytest.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define BUFFER_SIZE 32
static uint8_t buffer_data[BUFFER_SIZE];
static ring_data_type ring;

spec("Turbo Ring (LFBB) Tests") {
    before_each() {
        memset(buffer_data, 0, sizeof(buffer_data));
        ring_init(&ring, buffer_data, BUFFER_SIZE);
    }

    it("should initialize correctly") {
        check_ptr_eq(ring.data, buffer_data);
        check_size_eq(ring.size, BUFFER_SIZE);
        // Using t_atomic_load_explicit to be safe, although ring matches struct members
        check_size_eq(ring.r, 0);
        check_size_eq(ring.w, 0);
        check_size_eq(ring.i, 0);
        check_int_eq(ring.write_wrapped, false);
        check_int_eq(ring.read_wrapped, false);
    }

    it("should allow a simple write and read") {
        size_t write_len = 10;
        uint8_t *write_ptr = ring_write_acquire(&ring, write_len);
        check_not_null(write_ptr);
        check_ptr_eq(write_ptr, buffer_data);

        for (size_t i = 0; i < write_len; i++) {
            write_ptr[i] = (uint8_t)i;
        }
        ring_write_release(&ring, write_len);

        size_t available = 0;
        uint8_t *read_ptr = ring_read_acquire(&ring, &available);
        check_not_null(read_ptr);
        check_size_eq(available, write_len);
        check_ptr_eq(read_ptr, buffer_data);

        for (size_t i = 0; i < write_len; i++) {
            check_int_eq(read_ptr[i], (uint8_t)i);
        }
        ring_read_release(&ring, write_len);

        // After release, should be empty
        read_ptr = ring_read_acquire(&ring, &available);
        check_null(read_ptr);
        check_size_eq(available, 0);
    }

    it("should handle wrap-around correctly") {
        // 1. Fill partial
        size_t len1 = 20;
        uint8_t *p = ring_write_acquire(&ring, len1);
        memset(p, 0xA1, len1);
        ring_write_release(&ring, len1);

        // 2. Read some
        size_t avail = 0;
        p = ring_read_acquire(&ring, &avail);
        check_size_eq(avail, 20);
        ring_read_release(&ring, 15); // Read 15 bytes, 5 left. r=15, w=20

        // 3. Write until end
        // CalcFree: w=20, r=15, size=32. Free = (32 - (20-15)) - 1 = 26.
        // Linear free: min(26, 32-20) = 12.
        size_t len2 = 12;
        p = ring_write_acquire(&ring, len2);
        check_not_null(p);
        check_ptr_eq(p, &buffer_data[20]);
        memset(p, 0xB2, len2);
        ring_write_release(&ring, len2); // w=32 -> wraps to 0. i=32.

        // 4. Try write more (should wrap to beginning)
        // w=0, r=15, size=32. Free = (15-0) - 1 = 14.
        size_t len3 = 10;
        p = ring_write_acquire(&ring, len3);
        check_not_null(p);
        check_ptr_eq(p, &buffer_data[0]);
        memset(p, 0xC3, len3);
        ring_write_release(&ring, len3); // w=10, write_wrapped was true -> i=0? No, w was 0, so i=0, w=10.

        // Verify read sequence
        // Combined part: 17 bytes (5 from first write, 12 from second contiguous write)
        p = ring_read_acquire(&ring, &avail);
        check_size_eq(avail, 17); 
        check_ptr_eq(p, &buffer_data[15]);
        ring_read_release(&ring, 17); // r=32 -> wraps to 0

        // Third part: 10 bytes at index 0
        p = ring_read_acquire(&ring, &avail);
        check_size_eq(avail, 10);
        check_ptr_eq(p, &buffer_data[0]);
    }

    it("should handle bipartite split when writing") {
        // Fill most of it
        size_t len1 = 25;
        ring_write_release(&ring, 25);
        check_ptr_eq(ring_write_acquire(&ring, 5), &buffer_data[25]); // fits linear
        
        // Read some from front
        size_t avail;
        ring_read_acquire(&ring, &avail);
        ring_read_release(&ring, 10); // r=10, w=25

        /*
         r=10, w=25, size=32
         Free = (32 - (25-10)) - 1 = 32 - 15 - 1 = 16
         Linear free at end = 32 - 25 = 7
         Linear free at start = 16 - 7 = 9
        */

        // Request 8 bytes - shouldn't fit linear at end, should wrap to front
        uint8_t *p = ring_write_acquire(&ring, 8);
        check_not_null(p);
        check_ptr_eq(p, &buffer_data[0]);
        ring_write_release(&ring, 8); // i becomes 25, w becomes 8
        
        // Verify read
        p = ring_read_acquire(&ring, &avail);
        check_size_eq(avail, 15); // 25 - 10
        check_ptr_eq(p, &buffer_data[10]);
        ring_read_release(&ring, 15); // r=25
        
        // Next read should wrap
        p = ring_read_acquire(&ring, &avail);
        check_size_eq(avail, 8);
        check_ptr_eq(p, &buffer_data[0]);
    }
}
