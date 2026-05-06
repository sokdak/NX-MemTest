#include "nxmt/arena.h"

static bool align_up_u64(uint64_t value, uint64_t align, uint64_t *out) {
    uint64_t mask = align - 1u;
    if (value > UINT64_MAX - mask) {
        return false;
    }
    *out = (value + mask) & ~mask;
    return true;
}

static uint64_t align_down_u64(uint64_t value, uint64_t align) {
    return value & ~(align - 1u);
}

static NxmtArena zero_arena(uint64_t base) {
    NxmtArena arena;
    arena.base = (uint8_t*)base;
    arena.size = 0;
    arena.words = 0;
    return arena;
}

static uint64_t scaled_remainder_term(uint64_t remainder, uint64_t denominator, uint64_t scale) {
    uint64_t quotient = 0;
    uint64_t accumulated_remainder = 0;
    uint64_t term_quotient = 0;
    uint64_t term_remainder = remainder;

    while (scale != 0) {
        if ((scale & 1u) != 0) {
            quotient += term_quotient;
            if (quotient == UINT64_MAX) {
                return UINT64_MAX;
            }
            if (term_remainder != 0) {
                if (term_remainder >= denominator - accumulated_remainder) {
                    quotient += 1u;
                    accumulated_remainder = term_remainder - (denominator - accumulated_remainder);
                } else {
                    accumulated_remainder += term_remainder;
                }
            }
        }

        scale >>= 1u;
        if (scale == 0) {
            break;
        }

        term_quotient *= 2u;
        if (term_remainder >= denominator - term_remainder) {
            term_quotient += 1u;
            term_remainder -= denominator - term_remainder;
        } else {
            term_remainder += term_remainder;
        }
    }

    return quotient;
}

NxmtArena nxmt_arena_from_range(void *base, uint64_t size) {
    uint64_t raw_base = (uint64_t)(uintptr_t)base;
    uint64_t aligned_base = 0;
    if (!align_up_u64(raw_base, NXMT_PAGE_BYTES, &aligned_base)) {
        return zero_arena(raw_base);
    }

    if (size > UINT64_MAX - raw_base) {
        return zero_arena(aligned_base);
    }

    uint64_t raw_end = raw_base + size;
    uint64_t aligned_end = align_down_u64(raw_end, NXMT_PAGE_BYTES);

    if (aligned_end <= aligned_base) {
        return zero_arena(aligned_base);
    }

    NxmtArena arena;
    arena.base = (uint8_t*)aligned_base;
    arena.size = aligned_end - aligned_base;
    arena.words = arena.size / NXMT_WORD_BYTES;
    return arena;
}

bool nxmt_arena_is_large_enough(const NxmtArena *arena, uint64_t minimum_bytes) {
    return arena != 0 && arena->size >= minimum_bytes;
}

static void select_memory_source(
    NxmtMemorySelection *selection,
    bool available,
    uint64_t total,
    NxmtMemorySource source) {
    if (!available || total == 0) {
        return;
    }
    if (total > 4ull * NXMT_GIB_BYTES) {
        selection->extended_memory_detected = true;
    }
    if (selection->source == NXMT_MEMORY_SOURCE_NONE || total > selection->total) {
        selection->total = total;
        selection->source = source;
    }
}

NxmtMemorySelection nxmt_select_system_memory_total(
    bool has_physical_pools_total,
    uint64_t physical_pools_total,
    bool has_process_total,
    uint64_t process_total,
    bool has_override_heap,
    uint64_t override_heap_size) {
    NxmtMemorySelection selection;
    selection.total = 0;
    selection.source = NXMT_MEMORY_SOURCE_NONE;
    selection.extended_memory_detected = false;

    select_memory_source(&selection, has_physical_pools_total, physical_pools_total, NXMT_MEMORY_SOURCE_PHYSICAL_POOLS);
    select_memory_source(&selection, has_process_total, process_total, NXMT_MEMORY_SOURCE_PROCESS_TOTAL);
    select_memory_source(&selection, has_override_heap, override_heap_size, NXMT_MEMORY_SOURCE_OVERRIDE_HEAP);
    return selection;
}

uint64_t nxmt_percent_milli(uint64_t numerator, uint64_t denominator) {
    const uint64_t scale = 100000u;
    if (denominator == 0) {
        return 0;
    }

    uint64_t whole = numerator / denominator;
    uint64_t remainder = numerator % denominator;
    if (whole > UINT64_MAX / scale) {
        return UINT64_MAX;
    }

    uint64_t whole_scaled = whole * scale;
    uint64_t remainder_scaled = scaled_remainder_term(remainder, denominator, scale);
    if (whole_scaled > UINT64_MAX - remainder_scaled) {
        return UINT64_MAX;
    }
    return whole_scaled + remainder_scaled;
}

uint64_t nxmt_split_block_start(uint64_t total_words, uint32_t worker_count, uint32_t worker_index) {
    if (worker_count == 0 || worker_index > worker_count) {
        return 0;
    }
    uint64_t base = total_words / worker_count;
    uint64_t remainder = total_words % worker_count;
    uint64_t extra = worker_index < remainder ? worker_index : remainder;
    return (base * worker_index) + extra;
}

uint64_t nxmt_split_block_size(uint64_t total_words, uint32_t worker_count, uint32_t worker_index) {
    if (worker_count == 0 || worker_index >= worker_count) {
        return 0;
    }
    uint64_t base = total_words / worker_count;
    uint64_t remainder = total_words % worker_count;
    return base + (worker_index < remainder ? 1u : 0u);
}
