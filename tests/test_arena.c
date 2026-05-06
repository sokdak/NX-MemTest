#include <stdint.h>
#include "nxmt/arena.h"

static int expect_u64(uint64_t got, uint64_t want) {
    return got == want ? 0 : 1;
}

int main(void) {
    NxmtArena arena = nxmt_arena_from_range((void*)0x1003u, 0x5005u);
    int failed = 0;

    failed |= expect_u64((uint64_t)(uintptr_t)arena.base, 0x2000u);
    failed |= expect_u64(arena.size, 0x4000u);
    failed |= expect_u64(arena.words, 0x800u);

    failed |= nxmt_arena_is_large_enough(&arena, 0x4000u) ? 0 : 1;
    failed |= nxmt_arena_is_large_enough(&arena, 0x8000u) ? 1 : 0;

    failed |= expect_u64(nxmt_percent_milli(50, 200), 25000u);
    failed |= expect_u64(nxmt_percent_milli(0, 0), 0u);
    failed |= expect_u64(nxmt_split_block_start(1000, 4, 2), 500u);
    failed |= expect_u64(nxmt_split_block_size(1000, 4, 2), 250u);
    failed |= expect_u64(nxmt_split_block_size(1001, 4, 3), 251u);

    return failed;
}
