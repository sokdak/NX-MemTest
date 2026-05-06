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

    return failed;
}
