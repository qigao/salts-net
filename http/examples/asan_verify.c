/*
 * asan_verify.c
 * This program intentionally contains a memory error (use-after-free)
 * to verify that AddressSanitizer is working correctly.
 */

#include <stdio.h>
#include <stdlib.h>

int main() {
    printf("=======================================================\n");
    printf("ASan Verification Test\n");
    printf("This program will intentionally crash with an ASan report.\n");
    printf("=======================================================\n");
    
    // Allocate memory
    char *ptr = (char*)malloc(10);
    printf("Allocated 10 bytes at %p\n", (void*)ptr);
    
    // Free memory
    free(ptr);
    printf("Freed memory at %p\n", (void*)ptr);
    
    // Trigger Use-After-Free
    printf("Attempting to access freed memory...\n");
    ptr[0] = 'A'; // BOOM!
    
    printf("If you see this, ASan is NOT working!\n");
    return 0;
}
