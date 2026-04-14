#include "turbo_buffer.h"
#include "tinytest.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

/* 
 * Linus's Law: Keep it simple.
 * StressWorker is at file scope to satisfy C standard.
 */
#ifdef _WIN32
static DWORD WINAPI StressWorker(LPVOID lpParam) {
    mem_pool_t* pool = (mem_pool_t*)lpParam;
    for (int i = 0; i < 1000; i++) {
        void* p = mem_alloc(pool, (i % 8) * 64 + 1);
        if (p) mem_free(pool, p);
        
        mem_buffer_t* b = mem_get_buffer(pool, (i % 4) * 128 + 1);
        if (b) mem_release(b);
    }
    return 0;
}
#else
static void* StressWorker(void* arg) {
    mem_pool_t* pool = (mem_pool_t*)arg;
    for (int i = 0; i < 1000; i++) {
        void* p = mem_alloc(pool, (i % 8) * 64 + 1);
        if (p) mem_free(pool, p);
        
        mem_buffer_t* b = mem_get_buffer(pool, (i % 4) * 128 + 1);
        if (b) mem_release(b);
    }
    return NULL;
}
#endif

spec("Turbo Buffer (mem_pool_t) Tests") {
  
  it("should maintain ABI stability (112 bytes)") {
    check_size_eq(sizeof(mem_pool_t), 112);
  }

  it("should initialize and destroy a pool") {
    mem_pool_t pool;
    int rc = mem_init(&pool, 0);
    check_size_eq((size_t)rc, 0);
    
    check_size_eq(atomic_load(&pool.total_allocated), 0);
    check_size_eq(atomic_load(&pool.total_used), 0);
    
    mem_destroy(&pool);
  }

  it("should perform slab allocations (small)") {
    mem_pool_t pool;
    mem_init(&pool, 0);
    
    void* p1 = mem_alloc(&pool, 32);
    check_not_null(p1);
    
    void* p2 = mem_alloc(&pool, 32);
    check_not_null(p2);
    check_ptr_ne(p1, p2);
    
    check_size_gt(atomic_load(&pool.total_allocated), 0);
    check_size_gt(atomic_load(&pool.total_used), 64);
    
    mem_free(&pool, p1);
    mem_free(&pool, p2);
    
    mem_destroy(&pool);
  }

  it("should perform oversize allocations (large)") {
    mem_pool_t pool;
    mem_init(&pool, 0);
    
    size_t large_size = 16 * 1024;
    void* p = mem_alloc(&pool, large_size);
    check_not_null(p);
    
    check_size_ge(atomic_load(&pool.total_used), large_size);
    
    mem_free(&pool, p);
    mem_destroy(&pool);
  }

  it("should support buffer refcounting") {
    mem_pool_t pool;
    mem_init(&pool, 0);
    
    mem_buffer_t* buffer = mem_get_buffer(&pool, 100);
    check_not_null(buffer);
    check_size_eq((size_t)atomic_load(&buffer->ref_count), 1);
    check_ptr_eq(buffer->pool, &pool);
    
    mem_ref(buffer);
    check_size_eq((size_t)atomic_load(&buffer->ref_count), 2);
    
    mem_unref(buffer);
    check_size_eq((size_t)atomic_load(&buffer->ref_count), 1);
    
    mem_release(buffer);
    
    mem_destroy(&pool);
  }

  it("should support buffer recycling") {
    mem_pool_t pool;
    mem_init(&pool, 0);
    
    mem_buffer_t* b1 = mem_get_buffer(&pool, 100);
    void* data1 = b1->data;
    mem_release(b1);
    
    mem_buffer_t* b2 = mem_get_buffer(&pool, 100);
    check_ptr_eq(b2->data, data1);
    
    mem_release(b2);
    mem_destroy(&pool);
  }

  it("should support slicing") {
    mem_pool_t pool;
    mem_init(&pool, 0);
    
    mem_buffer_t* buffer = mem_get_buffer(&pool, 200);
    if (buffer) {
        strncpy(buffer->data, "Hello World from TurboBuffer", 200);
        buffer->used = strlen(buffer->data) + 1;
        
        mem_slice_t slice = mem_slice(buffer, 6, 5);
        check_ptr_eq(slice.data, buffer->data + 6);
        check_size_eq(slice.length, 5);
        check_ptr_eq(slice.buffer, buffer);
        check_size_eq((size_t)atomic_load(&buffer->ref_count), 2);
        
        mem_slice_release(&slice);
        check_size_eq((size_t)atomic_load(&buffer->ref_count), 1);
        
        mem_release(buffer);
    }
    mem_destroy(&pool);
  }

  it("should handle external wrapping") {
    char data[] = "External static data";
    mem_buffer_t* buffer = mem_wrap_external(data, sizeof(data), NULL, NULL);
    
    check_not_null(buffer);
    check_size_eq((size_t)mem_is_external(buffer), 1);
    check_ptr_eq(buffer->data, data);
    
    mem_release(buffer);
  }

  it("should survive concurrency stress") {
    mem_pool_t pool;
    mem_init(&pool, 0);
    
    const int num_threads = 8;
#ifdef _WIN32
    HANDLE threads[8];
    for (int i = 0; i < num_threads; i++) {
        threads[i] = CreateThread(NULL, 0, StressWorker, &pool, 0, NULL);
    }
    WaitForMultipleObjects(num_threads, threads, TRUE, INFINITE);
    for (int i = 0; i < num_threads; i++) CloseHandle(threads[i]);
#else
    pthread_t threads[8];
    for (int i = 0; i < num_threads; i++) {
        pthread_create(&threads[i], NULL, StressWorker, &pool);
    }
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
#endif
    
    mem_destroy(&pool);
    // Success if we reached here
    check_size_eq(1, 1);
  }
}
