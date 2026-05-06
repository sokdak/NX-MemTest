#include <stdint.h>
#include "nxmt/arena.h"

static int expect_u64(uint64_t got, uint64_t want) {
    return got == want ? 0 : 1;
}

static int expect_memory_selection(
    NxmtMemorySelection got,
    uint64_t total,
    NxmtMemorySource source,
    bool extended_memory_detected) {
    int failed = 0;
    failed |= expect_u64(got.total, total);
    failed |= got.source == source ? 0 : 1;
    failed |= got.extended_memory_detected == extended_memory_detected ? 0 : 1;
    return failed;
}

int main(void) {
    NxmtArena arena = nxmt_arena_from_range((void*)0x1003u, 0x5005u);
    int failed = 0;

    failed |= expect_u64((uint64_t)(uintptr_t)arena.base, 0x2000u);
    failed |= expect_u64(arena.size, 0x4000u);
    failed |= expect_u64(arena.words, 0x800u);

    failed |= nxmt_arena_is_large_enough(&arena, 0x4000u) ? 0 : 1;
    failed |= nxmt_arena_is_large_enough(&arena, 0x8000u) ? 1 : 0;

    NxmtArena overflow_end = nxmt_arena_from_range((void*)(uintptr_t)(UINT64_MAX - 0x1fffULL), 0x3000u);
    failed |= expect_u64((uint64_t)(uintptr_t)overflow_end.base, UINT64_MAX - 0x1fffULL);
    failed |= expect_u64(overflow_end.size, 0u);
    failed |= expect_u64(overflow_end.words, 0u);

    NxmtArena overflow_align = nxmt_arena_from_range((void*)(uintptr_t)(UINT64_MAX - 0x7ffULL), 0x800u);
    failed |= expect_u64(overflow_align.size, 0u);
    failed |= expect_u64(overflow_align.words, 0u);

    failed |= expect_u64(nxmt_percent_milli(50, 200), 25000u);
    failed |= expect_u64(nxmt_percent_milli(0, 0), 0u);
    failed |= expect_u64(nxmt_percent_milli(UINT64_MAX, UINT64_MAX), 100000u);
    failed |= expect_u64(nxmt_split_block_start(1000, 4, 2), 500u);
    failed |= expect_u64(nxmt_split_block_size(1000, 4, 2), 250u);
    failed |= expect_u64(nxmt_split_block_start(1001, 4, 0), 0u);
    failed |= expect_u64(nxmt_split_block_size(1001, 4, 0), 251u);
    failed |= expect_u64(nxmt_split_block_start(1001, 4, 1), 251u);
    failed |= expect_u64(nxmt_split_block_size(1001, 4, 1), 250u);
    failed |= expect_u64(nxmt_split_block_start(1001, 4, 2), 501u);
    failed |= expect_u64(nxmt_split_block_size(1001, 4, 2), 250u);
    failed |= expect_u64(nxmt_split_block_start(1001, 4, 3), 751u);
    failed |= expect_u64(nxmt_split_block_size(1001, 4, 3), 250u);

    failed |= expect_memory_selection(
        nxmt_select_system_memory_total(false, 0, false, 0, false, 0),
        0,
        NXMT_MEMORY_SOURCE_NONE,
        false);

    failed |= expect_memory_selection(
        nxmt_select_system_memory_total(true, 0, true, 0, true, 0),
        0,
        NXMT_MEMORY_SOURCE_NONE,
        false);

    failed |= expect_memory_selection(
        nxmt_select_system_memory_total(
            true,
            4ull * NXMT_GIB_BYTES,
            true,
            8ull * NXMT_GIB_BYTES,
            true,
            6ull * NXMT_GIB_BYTES),
        8ull * NXMT_GIB_BYTES,
        NXMT_MEMORY_SOURCE_PROCESS_TOTAL,
        true);

    failed |= expect_memory_selection(
        nxmt_select_system_memory_total(
            true,
            8ull * NXMT_GIB_BYTES,
            true,
            6ull * NXMT_GIB_BYTES,
            true,
            4ull * NXMT_GIB_BYTES),
        8ull * NXMT_GIB_BYTES,
        NXMT_MEMORY_SOURCE_PHYSICAL_POOLS,
        true);

    failed |= expect_memory_selection(
        nxmt_select_system_memory_total(
            true,
            8ull * NXMT_GIB_BYTES,
            true,
            6ull * NXMT_GIB_BYTES,
            true,
            10ull * NXMT_GIB_BYTES),
        10ull * NXMT_GIB_BYTES,
        NXMT_MEMORY_SOURCE_OVERRIDE_HEAP,
        true);

    failed |= expect_memory_selection(
        nxmt_select_system_memory_total(
            true,
            5ull * NXMT_GIB_BYTES,
            true,
            3ull * NXMT_GIB_BYTES,
            true,
            6ull * NXMT_GIB_BYTES),
        6ull * NXMT_GIB_BYTES,
        NXMT_MEMORY_SOURCE_OVERRIDE_HEAP,
        true);

    failed |= expect_u64(nxmt_runtime_heap_reserve(0), 0u);
    failed |= expect_u64(nxmt_runtime_heap_reserve(2ull * NXMT_MIB_BYTES), 0u);
    failed |= expect_u64(nxmt_runtime_heap_reserve(32ull * NXMT_MIB_BYTES), 16ull * NXMT_MIB_BYTES);
    failed |= expect_u64(nxmt_runtime_heap_reserve(128ull * NXMT_MIB_BYTES), 16ull * NXMT_MIB_BYTES);
    failed |= expect_u64(nxmt_runtime_heap_reserve(512ull * NXMT_MIB_BYTES), 32ull * NXMT_MIB_BYTES);
    failed |= expect_u64(nxmt_runtime_heap_reserve(8ull * NXMT_GIB_BYTES), 32ull * NXMT_MIB_BYTES);

    return failed;
}
