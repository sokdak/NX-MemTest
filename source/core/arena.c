#include "nxmt/arena.h"

static uint64_t align_up_u64(uint64_t value, uint64_t align) {
    return (value + align - 1u) & ~(align - 1u);
}

static uint64_t align_down_u64(uint64_t value, uint64_t align) {
    return value & ~(align - 1u);
}

NxmtArena nxmt_arena_from_range(void *base, uint64_t size) {
    uint64_t raw_base = (uint64_t)(uintptr_t)base;
    uint64_t aligned_base = align_up_u64(raw_base, NXMT_PAGE_BYTES);
    uint64_t raw_end = raw_base + size;
    uint64_t aligned_end = align_down_u64(raw_end, NXMT_PAGE_BYTES);

    NxmtArena arena;
    if (aligned_end <= aligned_base) {
        arena.base = (uint8_t*)aligned_base;
        arena.size = 0;
        arena.words = 0;
        return arena;
    }

    arena.base = (uint8_t*)aligned_base;
    arena.size = aligned_end - aligned_base;
    arena.words = arena.size / NXMT_WORD_BYTES;
    return arena;
}

bool nxmt_arena_is_large_enough(const NxmtArena *arena, uint64_t minimum_bytes) {
    return arena != 0 && arena->size >= minimum_bytes;
}

uint64_t nxmt_percent_milli(uint64_t numerator, uint64_t denominator) {
    if (denominator == 0) {
        return 0;
    }
    return (numerator * 100000u) / denominator;
}

uint64_t nxmt_split_block_start(uint64_t total_words, uint32_t worker_count, uint32_t worker_index) {
    if (worker_count == 0 || worker_index > worker_count) {
        return 0;
    }
    return (total_words * worker_index) / worker_count;
}

uint64_t nxmt_split_block_size(uint64_t total_words, uint32_t worker_count, uint32_t worker_index) {
    if (worker_count == 0 || worker_index >= worker_count) {
        return 0;
    }
    uint64_t start = nxmt_split_block_start(total_words, worker_count, worker_index);
    uint64_t end = nxmt_split_block_start(total_words, worker_count, worker_index + 1u);
    return end - start;
}
