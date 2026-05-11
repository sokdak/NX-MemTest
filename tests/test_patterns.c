#include <stdint.h>
#include "nxmt/patterns.h"

static int expect_u64(uint64_t got, uint64_t want) {
    return got == want ? 0 : 1;
}

int main(void) {
    uint64_t seed = nxmt_make_seed(0x12345678u, 0x9abcdef0u);
    int failed = 0;

    failed |= expect_u64(seed, 0x123456789abcdef0ull);
    failed |= expect_u64(nxmt_mix64(0), 0xe220a8397b1dcdafull);
    failed |= expect_u64(nxmt_mix64(1), 0x910a2dec89025cc1ull);

    uint64_t a = nxmt_expected_value(seed, NXMT_PHASE_ADDRESS, 2, 0x40);
    uint64_t b = nxmt_expected_value(seed, NXMT_PHASE_ADDRESS, 2, 0x40);
    uint64_t c = nxmt_expected_value(seed, NXMT_PHASE_ADDRESS, 2, 0x48);
    failed |= a == b ? 0 : 1;
    failed |= a != c ? 0 : 1;

    failed |= expect_u64(nxmt_expected_value(seed, NXMT_PHASE_FIXED_A, 0, 0), 0xaaaaaaaaaaaaaaaaull);
    failed |= expect_u64(nxmt_expected_value(seed, NXMT_PHASE_FIXED_5, 0, 0), 0x5555555555555555ull);
    failed |= expect_u64(nxmt_expected_value(seed, NXMT_PHASE_CHECKER, 0, 0), 0xaaaaaaaaaaaaaaaaull);
    failed |= expect_u64(nxmt_expected_value(seed, NXMT_PHASE_CHECKER, 0, 8), 0x5555555555555555ull);

    /* NARROW at offset 0 / pass 0: X = 0, so expected = 0x0706050403020100 ^ seed. */
    failed |= expect_u64(nxmt_expected_value(seed, NXMT_PHASE_NARROW, 0, 0),
                         0x1532537c99bedff0ull);
    /* Same inputs must return the same byte-composed value. */
    uint64_t narrow_a = nxmt_expected_value(seed, NXMT_PHASE_NARROW, 0, 0);
    uint64_t narrow_b = nxmt_expected_value(seed, NXMT_PHASE_NARROW, 0, 0);
    failed |= narrow_a == narrow_b ? 0 : 1;
    /* Different offset must shift X and therefore the composed word. */
    failed |= nxmt_expected_value(seed, NXMT_PHASE_NARROW, 0, 0)
              != nxmt_expected_value(seed, NXMT_PHASE_NARROW, 0, 8)
              ? 0 : 1;

    /* BITSPREAD alternates two sparse-bit values per (word_index + pass) parity. */
    failed |= expect_u64(nxmt_expected_value(seed, NXMT_PHASE_BITSPREAD, 0, 0),
                         0x4924924924924924ull);
    failed |= expect_u64(nxmt_expected_value(seed, NXMT_PHASE_BITSPREAD, 0, 8),
                         0xb6db6db6db6db6dbull);
    failed |= expect_u64(nxmt_expected_value(seed, NXMT_PHASE_BITSPREAD, 1, 0),
                         0xb6db6db6db6db6dbull);

    /* STREAM repeats every 8 KiB (offset & 0x1fff). At offset 0 with pass 0
     * the formula reduces to seed. Period rollover at offset 0x2000 must
     * return the same value as offset 0. */
    failed |= expect_u64(nxmt_expected_value(seed, NXMT_PHASE_STREAM, 0, 0), seed);
    failed |= expect_u64(nxmt_expected_value(seed, NXMT_PHASE_STREAM, 0, 8),
                         0x8ull ^ seed);
    failed |= expect_u64(nxmt_expected_value(seed, NXMT_PHASE_STREAM, 0, 0x2000),
                         seed);
    failed |= expect_u64(nxmt_expected_value(seed, NXMT_PHASE_STREAM, 1, 0),
                         seed ^ 0xbf58476d1ce4e5b9ull);

    return failed;
}
