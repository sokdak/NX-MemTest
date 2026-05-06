#pragma once

#include "nxmt/types.h"

typedef struct NxmtArena {
    uint8_t *base;
    uint64_t size;
    uint64_t words;
} NxmtArena;

NxmtArena nxmt_arena_from_range(void *base, uint64_t size);
bool nxmt_arena_is_large_enough(const NxmtArena *arena, uint64_t minimum_bytes);
uint64_t nxmt_percent_milli(uint64_t numerator, uint64_t denominator);
uint64_t nxmt_split_block_start(uint64_t total_words, uint32_t worker_count, uint32_t worker_index);
uint64_t nxmt_split_block_size(uint64_t total_words, uint32_t worker_count, uint32_t worker_index);
