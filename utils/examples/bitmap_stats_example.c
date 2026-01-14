/**
 * @file bitmap_stats_example.c
 * @brief Example showing how to use bitmap utilities for stats system optimization
 */

#include "bitmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define STATS_CAPACITY 512
#define STATS_WORD_COUNT ((STATS_CAPACITY + 63) / 64)

typedef struct {
    char name[64];
    uint64_t value;
} stat_entry_t;

typedef struct {
    stat_entry_t entries[STATS_CAPACITY];
    uint64_t used_bitmap[STATS_WORD_COUNT];
    uint64_t dirty_bitmap[STATS_WORD_COUNT];
    size_t entry_count;
} simple_stats_t;

// Hash-based slot allocation
static uint32_t hash_to_slot(const char *name) {
    return turbo_bitmap_hash_to_slot(name, STATS_CAPACITY);
}

// Find or create a stats entry using hash-based allocation
static stat_entry_t* find_or_create_entry(simple_stats_t *stats, const char *name) {
    uint32_t base_slot = hash_to_slot(name);
    
    // Linear probing for collision resolution
    for (uint32_t i = 0; i < STATS_CAPACITY; i++) {
        uint32_t slot = (base_slot + i) % STATS_CAPACITY;
        
        if (!turbo_bitmap_test(stats->used_bitmap, slot)) {
            // Found empty slot - create new entry
            turbo_bitmap_set(stats->used_bitmap, slot);
            strncpy(stats->entries[slot].name, name, sizeof(stats->entries[slot].name) - 1);
            stats->entries[slot].name[sizeof(stats->entries[slot].name) - 1] = '\0';
            stats->entries[slot].value = 0;
            stats->entry_count++;
            return &stats->entries[slot];
        }
        
        if (strcmp(stats->entries[slot].name, name) == 0) {
            // Found existing entry
            return &stats->entries[slot];
        }
    }
    
    return NULL; // Table full
}

// Increment a counter stat
static void stats_increment(simple_stats_t *stats, const char *name) {
    stat_entry_t *entry = find_or_create_entry(stats, name);
    if (entry) {
        entry->value++;
        
        // Mark as dirty for potential batch updates
        uint32_t slot = entry - stats->entries;
        turbo_bitmap_set(stats->dirty_bitmap, slot);
    }
}

// Get a stat value
static uint64_t stats_get(simple_stats_t *stats, const char *name) {
    uint32_t base_slot = hash_to_slot(name);
    
    for (uint32_t i = 0; i < STATS_CAPACITY; i++) {
        uint32_t slot = (base_slot + i) % STATS_CAPACITY;
        
        if (!turbo_bitmap_test(stats->used_bitmap, slot)) {
            return 0; // Not found
        }
        
        if (strcmp(stats->entries[slot].name, name) == 0) {
            return stats->entries[slot].value;
        }
    }
    
    return 0; // Not found
}

// Print all dirty stats (for batch processing)
static void print_dirty_stats(simple_stats_t *stats) {
    printf("Dirty stats:\n");
    
    for (size_t slot = 0; slot < STATS_CAPACITY; slot++) {
        if (turbo_bitmap_test(stats->dirty_bitmap, slot) && 
            turbo_bitmap_test(stats->used_bitmap, slot)) {
            printf("  %s: %llu\n", stats->entries[slot].name, 
                   (unsigned long long)stats->entries[slot].value);
        }
    }
    
    // Clear dirty bitmap after processing
    turbo_bitmap_clear_all(stats->dirty_bitmap, STATS_WORD_COUNT);
}

// Print stats summary
static void print_stats_summary(simple_stats_t *stats) {
    size_t used_slots = turbo_bitmap_popcount(stats->used_bitmap, STATS_CAPACITY);
    size_t dirty_slots = turbo_bitmap_popcount(stats->dirty_bitmap, STATS_CAPACITY);
    
    printf("\nStats Summary:\n");
    printf("  Capacity: %d slots\n", STATS_CAPACITY);
    printf("  Used slots: %zu (%.1f%% full)\n", used_slots, 
           (double)used_slots / STATS_CAPACITY * 100.0);
    printf("  Dirty slots: %zu\n", dirty_slots);
    printf("  Memory usage: %zu bytes\n", 
           sizeof(simple_stats_t) + used_slots * sizeof(stat_entry_t));
    
    if (used_slots > 0) {
        size_t first_used = turbo_bitmap_find_first_set(stats->used_bitmap, STATS_CAPACITY);
        size_t last_used = turbo_bitmap_find_last_set(stats->used_bitmap, STATS_CAPACITY);
        printf("  Slot range: %zu - %zu\n", first_used, last_used);
    }
}

int main() {
    printf("Bitmap-based Stats System Example\n");
    printf("==================================\n\n");
    
    // Initialize stats system
    simple_stats_t *stats = calloc(1, sizeof(simple_stats_t));
    if (!stats) {
        fprintf(stderr, "Failed to allocate stats\n");
        return 1;
    }
    
    // Simulate some stats updates
    const char *stat_names[] = {
        "tcp.packets_sent",
        "tcp.packets_received", 
        "tcp.bytes_sent",
        "tcp.bytes_received",
        "tcp.connections_active",
        "udp.packets_sent",
        "udp.packets_received",
        "http.requests_total",
        "http.requests_failed",
        "mqtt.messages_published"
    };
    
    printf("Updating stats...\n");
    for (int i = 0; i < 100; i++) {
        const char *name = stat_names[i % (sizeof(stat_names) / sizeof(stat_names[0]))];
        stats_increment(stats, name);
    }
    
    print_stats_summary(stats);
    print_dirty_stats(stats);
    
    // Test retrieval
    printf("\nSample stat values:\n");
    printf("  tcp.packets_sent: %llu\n", (unsigned long long)stats_get(stats, "tcp.packets_sent"));
    printf("  http.requests_total: %llu\n", (unsigned long long)stats_get(stats, "http.requests_total"));
    printf("  nonexistent.stat: %llu\n", (unsigned long long)stats_get(stats, "nonexistent.stat"));
    
    // Demonstrate bitmap operations
    printf("\nBitmap operations:\n");
    printf("  First used slot: %zu\n", turbo_bitmap_find_first_set(stats->used_bitmap, STATS_CAPACITY));
    printf("  Last used slot: %zu\n", turbo_bitmap_find_last_set(stats->used_bitmap, STATS_CAPACITY));
    printf("  First free slot: %zu\n", turbo_bitmap_find_first_unset(stats->used_bitmap, STATS_CAPACITY));
    
    free(stats);
    printf("\n✅ Example completed successfully!\n");
    return 0;
}