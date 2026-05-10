#pragma once

#include "nxmt/types.h"

typedef struct NxmtArena {
    uint8_t *base;
    uint64_t size;
    uint64_t words;
} NxmtArena;

typedef enum NxmtMemorySource {
    NXMT_MEMORY_SOURCE_NONE = 0,
    NXMT_MEMORY_SOURCE_PHYSICAL_POOLS = 1,
    NXMT_MEMORY_SOURCE_PROCESS_TOTAL = 2,
    NXMT_MEMORY_SOURCE_OVERRIDE_HEAP = 3
} NxmtMemorySource;

typedef struct NxmtMemorySelection {
    uint64_t total;
    NxmtMemorySource source;
    bool extended_memory_detected;
} NxmtMemorySelection;

NxmtArena nxmt_arena_from_range(void *base, uint64_t size);
bool nxmt_arena_is_large_enough(const NxmtArena *arena, uint64_t minimum_bytes);
NxmtMemorySelection nxmt_select_system_memory_total(
    bool has_physical_pools_total,
    uint64_t physical_pools_total,
    bool has_process_total,
    uint64_t process_total,
    bool has_override_heap,
    uint64_t override_heap_size);
NxmtMemorySelection nxmt_select_launch_safe_memory_total(bool has_override_heap, uint64_t override_heap_size);
uint64_t nxmt_runtime_heap_reserve(uint64_t override_heap_size);
uint64_t nxmt_percent_milli(uint64_t numerator, uint64_t denominator);
uint64_t nxmt_split_block_start(uint64_t total_words, uint32_t worker_count, uint32_t worker_index);
uint64_t nxmt_split_block_size(uint64_t total_words, uint32_t worker_count, uint32_t worker_index);
