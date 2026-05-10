#include "nxmt/patterns.h"

uint64_t nxmt_make_seed(uint32_t high, uint32_t low) {
    return ((uint64_t)high << 32) | (uint64_t)low;
}

uint64_t nxmt_mix64(uint64_t value) {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

uint64_t nxmt_expected_value(uint64_t seed, NxmtPhase phase, uint64_t pass, uint64_t offset) {
    uint64_t word_index = offset / NXMT_WORD_BYTES;

    switch (phase) {
    case NXMT_PHASE_FIXED_A:
        return 0xaaaaaaaaaaaaaaaaull;
    case NXMT_PHASE_FIXED_5:
        return 0x5555555555555555ull;
    case NXMT_PHASE_CHECKER:
        return (word_index & 1u) ? 0x5555555555555555ull : 0xaaaaaaaaaaaaaaaaull;
    case NXMT_PHASE_ADDRESS:
        return offset ^ seed ^ (pass * 0x100000001b3ull);
    case NXMT_PHASE_RANDOM:
        return __builtin_bswap64(word_index) ^ seed ^ (pass * 0x9e3779b97f4a7c15ull);
    case NXMT_PHASE_WALKING: {
        unsigned bit = (unsigned)((word_index + pass) & 63u);
        return 1ull << bit;
    }
    }

    return seed ^ offset ^ pass;
}

uint64_t nxmt_next_offset(uint64_t seed, uint64_t pass, uint64_t index, uint64_t arena_words) {
    if (arena_words == 0) {
        return 0;
    }
    uint64_t mixed = nxmt_mix64(seed ^ nxmt_mix64(pass) ^ (index * 0xd6e8feb86659fd93ull));
    return (mixed % arena_words) * NXMT_WORD_BYTES;
}
