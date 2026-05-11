#include <stdint.h>
#include <string.h>

#include "nxmt/runner.h"

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define NXMT_HAS_NEON 1
#else
#define NXMT_HAS_NEON 0
#endif

static const uint64_t kFixedA = 0xaaaaaaaaaaaaaaaaull;
static const uint64_t kFixed5 = 0x5555555555555555ull;
static const uint64_t kAddrPassMul = 0x100000001b3ull;
static const uint64_t kRandPassMul = 0x9e3779b97f4a7c15ull;
/* Must match the constant used in patterns.c NXMT_PHASE_STREAM case. */
static const uint64_t kStreamPassMul = 0xbf58476d1ce4e5b9ull;

/* STREAM stamp size in words. Power of 2 (so the stamp_pos wrap is a mask),
 * 1024 words = 8 KiB which fits well inside the 32 KiB A57 L1D and leaves
 * plenty of room on the 64 KiB worker stack. */
#define NXMT_STREAM_STAMP_WORDS 1024u

static const NxmtPhase quick_phases[] = {
    NXMT_PHASE_FIXED_A,
    NXMT_PHASE_ADDRESS,
    NXMT_PHASE_RANDOM
};

/* NXMT_PHASE_NARROW is intentionally excluded from the bandwidth-mode phase
 * arrays: STRB-driven byte writes are ~8x slower than 64-bit stores and would
 * dominate the average throughput. The phase remains wired through
 * patterns.c / write_chunk / verify_chunk so it can be invoked by a future
 * correctness-focused mode. */
static const NxmtPhase memory_load_phases[] = {
    NXMT_PHASE_FIXED_A,
    NXMT_PHASE_FIXED_5,
    NXMT_PHASE_CHECKER,
    NXMT_PHASE_BITSPREAD,
    NXMT_PHASE_STREAM,
    NXMT_PHASE_ADDRESS,
    NXMT_PHASE_RANDOM,
    NXMT_PHASE_WALKING
};

static const uint64_t kBitSpread0 = 0x4924924924924924ull;
static const uint64_t kBitSpread1 = 0xb6db6db6db6db6dbull;

/* Builds the STREAM stamp: stamp[k] = (k * 8) ^ seed ^ (pass * kStreamPassMul).
 * Called once per chunk; the stamp is then bulk-copied across the chunk so
 * per-word pattern compute happens once per chunk instead of per word. */
static void nxmt_fill_stream_stamp(uint64_t *stamp, uint64_t seed, uint64_t pass) {
    const uint64_t mask = seed ^ (pass * kStreamPassMul);
    uint64_t k = 0;
#if NXMT_HAS_NEON
    const uint64x2_t mask2 = vdupq_n_u64(mask);
    const uint64x2_t step4 = vdupq_n_u64(32u);
    uint64x2_t off01 = vsetq_lane_u64(8u, vdupq_n_u64(0u), 1);
    uint64x2_t off23 = vsetq_lane_u64(24u, vdupq_n_u64(16u), 1);
    for (; k + 4 <= NXMT_STREAM_STAMP_WORDS; k += 4) {
        vst1q_u64(stamp + k,     veorq_u64(off01, mask2));
        vst1q_u64(stamp + k + 2, veorq_u64(off23, mask2));
        off01 = vaddq_u64(off01, step4);
        off23 = vaddq_u64(off23, step4);
    }
#endif
    for (; k < NXMT_STREAM_STAMP_WORDS; ++k) {
        stamp[k] = (k * (uint64_t)NXMT_WORD_BYTES) ^ mask;
    }
}

/* memcpy-equivalent inner loop tuned for A57: 16 words (128 bytes) per
 * iteration, eight independent loads followed by eight stores, lets the
 * core issue close to 2 stores/cycle while the load pipeline keeps the
 * register file fed from a hot stamp in L1. */
static void nxmt_neon_copy_words(uint64_t * __restrict__ dst,
                                  const uint64_t * __restrict__ src,
                                  uint64_t words) {
    uint64_t k = 0;
#if NXMT_HAS_NEON
    for (; k + 16 <= words; k += 16) {
        uint64x2_t a0 = vld1q_u64(src + k);
        uint64x2_t a1 = vld1q_u64(src + k + 2);
        uint64x2_t a2 = vld1q_u64(src + k + 4);
        uint64x2_t a3 = vld1q_u64(src + k + 6);
        uint64x2_t a4 = vld1q_u64(src + k + 8);
        uint64x2_t a5 = vld1q_u64(src + k + 10);
        uint64x2_t a6 = vld1q_u64(src + k + 12);
        uint64x2_t a7 = vld1q_u64(src + k + 14);
        vst1q_u64(dst + k,      a0);
        vst1q_u64(dst + k + 2,  a1);
        vst1q_u64(dst + k + 4,  a2);
        vst1q_u64(dst + k + 6,  a3);
        vst1q_u64(dst + k + 8,  a4);
        vst1q_u64(dst + k + 10, a5);
        vst1q_u64(dst + k + 12, a6);
        vst1q_u64(dst + k + 14, a7);
    }
#endif
    for (; k < words; ++k) {
        dst[k] = src[k];
    }
}

static uint64_t nxmt_extreme_cpu_pressure(uint64_t state, uint64_t value) {
    for (uint32_t i = 0; i < 64u; ++i) {
        value = nxmt_mix64(value + i);
        value ^= value << 13;
        value ^= value >> 7;
        value ^= value << 17;
    }
    return state ^ value;
}

static uint32_t nxmt_phase_count_for_mode(NxmtMode mode) {
    if (mode == NXMT_MODE_QUICK) {
        return (uint32_t)(sizeof(quick_phases) / sizeof(quick_phases[0]));
    }
    return (uint32_t)(sizeof(memory_load_phases) / sizeof(memory_load_phases[0]));
}

static NxmtPhase nxmt_phase_for_mode(NxmtMode mode, uint32_t index) {
    if (mode == NXMT_MODE_QUICK) {
        return quick_phases[index];
    }
    return memory_load_phases[index];
}

uint32_t nxmt_runner_phase_count(NxmtMode mode) {
    return nxmt_phase_count_for_mode(mode);
}

static bool nxmt_runner_is_aligned(const NxmtArena *arena) {
    return ((uintptr_t)arena->base % _Alignof(uint64_t)) == 0;
}

static bool nxmt_runner_stop_requested(const NxmtRunConfig *config) {
    return config->stop_requested != 0 && atomic_load_explicit(config->stop_requested, memory_order_relaxed);
}

static NxmtStatus nxmt_runner_abort_status(const NxmtReport *report, uint64_t initial_errors) {
    return report->error_count != initial_errors ? NXMT_STATUS_FAIL : NXMT_STATUS_ABORTED;
}

static void nxmt_progress_add(const NxmtRunConfig *config, uint64_t delta) {
    if (config->progress_bytes != 0 && delta != 0) {
        atomic_fetch_add_explicit(config->progress_bytes, delta, memory_order_relaxed);
    }
}

static void nxmt_write_chunk(
    uint64_t * __restrict__ p,
    uint64_t chunk_words,
    uint64_t start_word,
    NxmtPhase phase,
    uint64_t seed,
    uint64_t pass) {
    switch (phase) {
    case NXMT_PHASE_FIXED_A:
        memset(p, 0xaa, chunk_words * sizeof(uint64_t));
        break;
    case NXMT_PHASE_FIXED_5:
        memset(p, 0x55, chunk_words * sizeof(uint64_t));
        break;
    case NXMT_PHASE_CHECKER: {
        const uint64_t a_first = (start_word & 1u) ? kFixed5 : kFixedA;
        const uint64_t b_first = a_first ^ (kFixedA ^ kFixed5);
        uint64_t k = 0;
#if NXMT_HAS_NEON
        const uint64x2_t pat = vsetq_lane_u64(b_first, vdupq_n_u64(a_first), 1);
        for (; k + 4 <= chunk_words; k += 4) {
            vst1q_u64(p + k,     pat);
            vst1q_u64(p + k + 2, pat);
        }
#endif
        for (; k + 2 <= chunk_words; k += 2) {
            p[k]     = a_first;
            p[k + 1] = b_first;
        }
        if (k < chunk_words) {
            p[k] = a_first;
        }
        break;
    }
    case NXMT_PHASE_ADDRESS: {
        const uint64_t base_off = start_word * (uint64_t)NXMT_WORD_BYTES;
        const uint64_t mask = seed ^ (pass * kAddrPassMul);
        uint64_t k = 0;
#if NXMT_HAS_NEON
        const uint64x2_t mask2 = vdupq_n_u64(mask);
        const uint64x2_t step4 = vdupq_n_u64(32u);
        uint64x2_t v01 = vsetq_lane_u64(base_off + 8u, vdupq_n_u64(base_off), 1);
        uint64x2_t v23 = vsetq_lane_u64(base_off + 24u, vdupq_n_u64(base_off + 16u), 1);
        for (; k + 4 <= chunk_words; k += 4) {
            vst1q_u64(p + k,     veorq_u64(v01, mask2));
            vst1q_u64(p + k + 2, veorq_u64(v23, mask2));
            v01 = vaddq_u64(v01, step4);
            v23 = vaddq_u64(v23, step4);
        }
#endif
        for (; k < chunk_words; ++k) {
            p[k] = (base_off + k * (uint64_t)NXMT_WORD_BYTES) ^ mask;
        }
        break;
    }
    case NXMT_PHASE_RANDOM: {
        const uint64_t mask = seed ^ (pass * kRandPassMul);
        uint64_t k = 0;
#if NXMT_HAS_NEON
        const uint64x2_t mask2 = vdupq_n_u64(mask);
        const uint64x2_t step4 = vdupq_n_u64(4u);
        uint64x2_t idx01 = vsetq_lane_u64(start_word + 1u, vdupq_n_u64(start_word), 1);
        uint64x2_t idx23 = vsetq_lane_u64(start_word + 3u, vdupq_n_u64(start_word + 2u), 1);
        for (; k + 4 <= chunk_words; k += 4) {
            uint64x2_t r01 = vreinterpretq_u64_u8(vrev64q_u8(vreinterpretq_u8_u64(idx01)));
            uint64x2_t r23 = vreinterpretq_u64_u8(vrev64q_u8(vreinterpretq_u8_u64(idx23)));
            vst1q_u64(p + k,     veorq_u64(r01, mask2));
            vst1q_u64(p + k + 2, veorq_u64(r23, mask2));
            idx01 = vaddq_u64(idx01, step4);
            idx23 = vaddq_u64(idx23, step4);
        }
#endif
        for (; k < chunk_words; ++k) {
            p[k] = __builtin_bswap64(start_word + k) ^ mask;
        }
        break;
    }
    case NXMT_PHASE_WALKING: {
        const uint64_t pos_base = start_word + pass;
        uint64_t k = 0;
#if NXMT_HAS_NEON
        const int64x2_t mask63 = vdupq_n_s64(63);
        const int64x2_t one = vdupq_n_s64(1);
        const int64x2_t step4 = vdupq_n_s64(4);
        int64x2_t idx01 = vsetq_lane_s64((int64_t)(pos_base + 1u), vdupq_n_s64((int64_t)pos_base), 1);
        int64x2_t idx23 = vsetq_lane_s64((int64_t)(pos_base + 3u), vdupq_n_s64((int64_t)(pos_base + 2u)), 1);
        for (; k + 4 <= chunk_words; k += 4) {
            int64x2_t s01 = vandq_s64(idx01, mask63);
            int64x2_t s23 = vandq_s64(idx23, mask63);
            uint64x2_t v01 = vreinterpretq_u64_s64(vshlq_s64(one, s01));
            uint64x2_t v23 = vreinterpretq_u64_s64(vshlq_s64(one, s23));
            vst1q_u64(p + k,     v01);
            vst1q_u64(p + k + 2, v23);
            idx01 = vaddq_s64(idx01, step4);
            idx23 = vaddq_s64(idx23, step4);
        }
#endif
        for (; k < chunk_words; ++k) {
            p[k] = 1ull << ((pos_base + k) & 63u);
        }
        break;
    }
    case NXMT_PHASE_NARROW: {
        /* Force 8-bit stores: write bytes via a volatile uint8_t* so the
         * compiler emits STRB and never coalesces neighbours into wider
         * stores. This exercises the CPU's narrow-write path, which has
         * different store-buffer behaviour than 64-bit stores. */
        volatile uint8_t *bp = (volatile uint8_t*)p;
        const uint64_t byte_count = chunk_words * (uint64_t)NXMT_WORD_BYTES;
        const uint64_t byte_base = start_word * (uint64_t)NXMT_WORD_BYTES
                                   + pass * (uint64_t)NXMT_WORD_BYTES;
        for (uint64_t i = 0; i < byte_count; ++i) {
            uint8_t seed_byte = (uint8_t)(seed >> ((i & 7u) * 8u));
            bp[i] = (uint8_t)(byte_base + i) ^ seed_byte;
        }
        break;
    }
    case NXMT_PHASE_BITSPREAD: {
        const uint64_t a_first = ((start_word + pass) & 1u) ? kBitSpread1 : kBitSpread0;
        const uint64_t b_first = a_first ^ (kBitSpread0 ^ kBitSpread1);
        uint64_t k = 0;
#if NXMT_HAS_NEON
        const uint64x2_t pat = vsetq_lane_u64(b_first, vdupq_n_u64(a_first), 1);
        for (; k + 4 <= chunk_words; k += 4) {
            vst1q_u64(p + k,     pat);
            vst1q_u64(p + k + 2, pat);
        }
#endif
        for (; k + 2 <= chunk_words; k += 2) {
            p[k]     = a_first;
            p[k + 1] = b_first;
        }
        if (k < chunk_words) {
            p[k] = a_first;
        }
        break;
    }
    case NXMT_PHASE_STREAM: {
        /* Pay the pattern compute once, then drive memory bandwidth with a
         * tight NEON copy loop fed from a stamp hot in L1D. */
        uint64_t stamp[NXMT_STREAM_STAMP_WORDS];
        nxmt_fill_stream_stamp(stamp, seed, pass);
        uint64_t stamp_pos = start_word & ((uint64_t)NXMT_STREAM_STAMP_WORDS - 1u);
        uint64_t k = 0;
        while (k < chunk_words) {
            uint64_t need = chunk_words - k;
            uint64_t avail = (uint64_t)NXMT_STREAM_STAMP_WORDS - stamp_pos;
            uint64_t copy = need < avail ? need : avail;
            nxmt_neon_copy_words(p + k, stamp + stamp_pos, copy);
            k += copy;
            stamp_pos = (stamp_pos + copy) & ((uint64_t)NXMT_STREAM_STAMP_WORDS - 1u);
        }
        break;
    }
    }
}

static int nxmt_chunk_has_mismatch_fixed(const uint64_t * __restrict__ p, uint64_t n, uint64_t target) {
    uint64_t acc = 0;
    for (uint64_t k = 0; k < n; ++k) {
        acc |= (p[k] ^ target);
    }
    return acc != 0;
}

static void nxmt_verify_chunk(
    const uint64_t * __restrict__ p,
    uint64_t chunk_words,
    uint64_t start_word,
    NxmtPhase phase,
    const NxmtRunConfig *config,
    NxmtReport *report) {
    const uint64_t seed = config->seed;
    const uint64_t pass = config->pass;
    const uint64_t base_off = start_word * (uint64_t)NXMT_WORD_BYTES;

    switch (phase) {
    case NXMT_PHASE_FIXED_A:
    case NXMT_PHASE_FIXED_5: {
        const uint64_t target = (phase == NXMT_PHASE_FIXED_A) ? kFixedA : kFixed5;
        if (!nxmt_chunk_has_mismatch_fixed(p, chunk_words, target)) {
            break;
        }
        for (uint64_t k = 0; k < chunk_words; ++k) {
            uint64_t actual = p[k];
            if (actual != target) {
                nxmt_report_record_error(report, config->mode, phase, seed, pass, config->worker_id,
                    base_off + k * (uint64_t)NXMT_WORD_BYTES, target, actual);
            }
        }
        break;
    }
    case NXMT_PHASE_CHECKER: {
        const uint64_t parity = start_word & 1u;
        const uint64_t a_first = parity ? kFixed5 : kFixedA;
        const uint64_t b_first = a_first ^ (kFixedA ^ kFixed5);
        uint64_t acc = 0;
        for (uint64_t k = 0; k + 2 <= chunk_words; k += 2) {
            acc |= (p[k] ^ a_first);
            acc |= (p[k + 1] ^ b_first);
        }
        if (chunk_words & 1u) {
            acc |= (p[chunk_words - 1] ^ a_first);
        }
        if (acc == 0) break;
        for (uint64_t k = 0; k < chunk_words; ++k) {
            uint64_t expected = ((start_word + k) & 1u) ? kFixed5 : kFixedA;
            uint64_t actual = p[k];
            if (actual != expected) {
                nxmt_report_record_error(report, config->mode, phase, seed, pass, config->worker_id,
                    base_off + k * (uint64_t)NXMT_WORD_BYTES, expected, actual);
            }
        }
        break;
    }
    case NXMT_PHASE_ADDRESS: {
        const uint64_t mask = seed ^ (pass * kAddrPassMul);
        uint64_t acc = 0;
        uint64_t k = 0;
#if NXMT_HAS_NEON
        const uint64x2_t mask2 = vdupq_n_u64(mask);
        const uint64x2_t step4 = vdupq_n_u64(32u);
        uint64x2_t v01 = vsetq_lane_u64(base_off + 8u, vdupq_n_u64(base_off), 1);
        uint64x2_t v23 = vsetq_lane_u64(base_off + 24u, vdupq_n_u64(base_off + 16u), 1);
        uint64x2_t acc_v = vdupq_n_u64(0);
        for (; k + 4 <= chunk_words; k += 4) {
            uint64x2_t e01 = veorq_u64(v01, mask2);
            uint64x2_t e23 = veorq_u64(v23, mask2);
            uint64x2_t a01 = vld1q_u64(p + k);
            uint64x2_t a23 = vld1q_u64(p + k + 2);
            acc_v = vorrq_u64(acc_v, veorq_u64(e01, a01));
            acc_v = vorrq_u64(acc_v, veorq_u64(e23, a23));
            v01 = vaddq_u64(v01, step4);
            v23 = vaddq_u64(v23, step4);
        }
        acc |= vgetq_lane_u64(acc_v, 0) | vgetq_lane_u64(acc_v, 1);
#endif
        for (; k < chunk_words; ++k) {
            uint64_t expected = (base_off + k * (uint64_t)NXMT_WORD_BYTES) ^ mask;
            acc |= (p[k] ^ expected);
        }
        if (acc == 0) break;
        for (uint64_t kk = 0; kk < chunk_words; ++kk) {
            uint64_t expected = (base_off + kk * (uint64_t)NXMT_WORD_BYTES) ^ mask;
            uint64_t actual = p[kk];
            if (actual != expected) {
                nxmt_report_record_error(report, config->mode, phase, seed, pass, config->worker_id,
                    base_off + kk * (uint64_t)NXMT_WORD_BYTES, expected, actual);
            }
        }
        break;
    }
    case NXMT_PHASE_RANDOM: {
        const uint64_t mask = seed ^ (pass * kRandPassMul);
        uint64_t acc = 0;
        uint64_t k = 0;
#if NXMT_HAS_NEON
        const uint64x2_t mask2 = vdupq_n_u64(mask);
        const uint64x2_t step2 = vdupq_n_u64(2u);
        uint64x2_t idx = vsetq_lane_u64(start_word + 1u, vdupq_n_u64(start_word), 1);
        uint64x2_t acc_v = vdupq_n_u64(0);
        for (; k + 2 <= chunk_words; k += 2) {
            uint8x16_t bytes = vreinterpretq_u8_u64(idx);
            uint64x2_t rev = vreinterpretq_u64_u8(vrev64q_u8(bytes));
            uint64x2_t expected = veorq_u64(rev, mask2);
            uint64x2_t actual = vld1q_u64(p + k);
            acc_v = vorrq_u64(acc_v, veorq_u64(expected, actual));
            idx = vaddq_u64(idx, step2);
        }
        acc |= vgetq_lane_u64(acc_v, 0) | vgetq_lane_u64(acc_v, 1);
#endif
        for (; k < chunk_words; ++k) {
            uint64_t expected = __builtin_bswap64(start_word + k) ^ mask;
            acc |= (p[k] ^ expected);
        }
        if (acc == 0) break;
        for (uint64_t kk = 0; kk < chunk_words; ++kk) {
            uint64_t expected = __builtin_bswap64(start_word + kk) ^ mask;
            uint64_t actual = p[kk];
            if (actual != expected) {
                nxmt_report_record_error(report, config->mode, phase, seed, pass, config->worker_id,
                    base_off + kk * (uint64_t)NXMT_WORD_BYTES, expected, actual);
            }
        }
        break;
    }
    case NXMT_PHASE_WALKING: {
        const uint64_t pos_base = start_word + pass;
        for (uint64_t k = 0; k < chunk_words; ++k) {
            uint64_t expected = 1ull << ((pos_base + k) & 63u);
            uint64_t actual = p[k];
            if (actual != expected) {
                nxmt_report_record_error(report, config->mode, phase, seed, pass, config->worker_id,
                    base_off + k * (uint64_t)NXMT_WORD_BYTES, expected, actual);
            }
        }
        break;
    }
    case NXMT_PHASE_NARROW: {
        /* X cycles through 0, 8, 16, ..., 248 every 32 words. The expected
         * word is X * 0x0101..01 ^ 0x0706..0100 ^ seed; see patterns.c
         * NXMT_PHASE_NARROW for derivation. Scalar loop is fine here — the
         * write path uses byte stores anyway, so verify isn't the bottleneck. */
        uint64_t acc = 0;
        const uint64_t pass_off = pass * (uint64_t)NXMT_WORD_BYTES;
        for (uint64_t k = 0; k < chunk_words; ++k) {
            uint64_t X = (base_off + k * (uint64_t)NXMT_WORD_BYTES + pass_off) & 0xffu;
            uint64_t expected = (X * 0x0101010101010101ull)
                              ^ 0x0706050403020100ull ^ seed;
            acc |= (p[k] ^ expected);
        }
        if (acc == 0) break;
        for (uint64_t k = 0; k < chunk_words; ++k) {
            uint64_t X = (base_off + k * (uint64_t)NXMT_WORD_BYTES + pass_off) & 0xffu;
            uint64_t expected = (X * 0x0101010101010101ull)
                              ^ 0x0706050403020100ull ^ seed;
            uint64_t actual = p[k];
            if (actual != expected) {
                nxmt_report_record_error(report, config->mode, phase, seed, pass, config->worker_id,
                    base_off + k * (uint64_t)NXMT_WORD_BYTES, expected, actual);
            }
        }
        break;
    }
    case NXMT_PHASE_BITSPREAD: {
        const uint64_t a_first = ((start_word + pass) & 1u) ? kBitSpread1 : kBitSpread0;
        const uint64_t b_first = a_first ^ (kBitSpread0 ^ kBitSpread1);
        uint64_t acc = 0;
        for (uint64_t k = 0; k + 2 <= chunk_words; k += 2) {
            acc |= (p[k] ^ a_first);
            acc |= (p[k + 1] ^ b_first);
        }
        if (chunk_words & 1u) {
            acc |= (p[chunk_words - 1] ^ a_first);
        }
        if (acc == 0) break;
        for (uint64_t k = 0; k < chunk_words; ++k) {
            uint64_t expected = ((start_word + k + pass) & 1u) ? kBitSpread1 : kBitSpread0;
            uint64_t actual = p[k];
            if (actual != expected) {
                nxmt_report_record_error(report, config->mode, phase, seed, pass, config->worker_id,
                    base_off + k * (uint64_t)NXMT_WORD_BYTES, expected, actual);
            }
        }
        break;
    }
    case NXMT_PHASE_STREAM: {
        /* Bulk compare against the same stamp the write side produced. memcmp
         * gives newlib's optimized aarch64 path; only walk per-word if a
         * mismatch is detected. */
        uint64_t stamp[NXMT_STREAM_STAMP_WORDS];
        nxmt_fill_stream_stamp(stamp, seed, pass);
        uint64_t stamp_pos = start_word & ((uint64_t)NXMT_STREAM_STAMP_WORDS - 1u);
        uint64_t k = 0;
        bool has_mismatch = false;
        while (k < chunk_words) {
            uint64_t need = chunk_words - k;
            uint64_t avail = (uint64_t)NXMT_STREAM_STAMP_WORDS - stamp_pos;
            uint64_t cmp = need < avail ? need : avail;
            if (memcmp(p + k, stamp + stamp_pos, cmp * sizeof(uint64_t)) != 0) {
                has_mismatch = true;
                break;
            }
            k += cmp;
            stamp_pos = (stamp_pos + cmp) & ((uint64_t)NXMT_STREAM_STAMP_WORDS - 1u);
        }
        if (!has_mismatch) break;
        const uint64_t mask = seed ^ (pass * kStreamPassMul);
        for (uint64_t kk = 0; kk < chunk_words; ++kk) {
            uint64_t expected = ((base_off + kk * (uint64_t)NXMT_WORD_BYTES) & 0x1fffull) ^ mask;
            uint64_t actual = p[kk];
            if (actual != expected) {
                nxmt_report_record_error(report, config->mode, phase, seed, pass, config->worker_id,
                    base_off + kk * (uint64_t)NXMT_WORD_BYTES, expected, actual);
            }
        }
        break;
    }
    }
}

static bool nxmt_write_phase(
    uint8_t *base,
    uint64_t start,
    uint64_t count,
    NxmtPhase phase,
    const NxmtRunConfig *config,
    uint64_t *pressure_state) {
    const uint64_t check_chunk = (1ull << 20) / (uint64_t)NXMT_WORD_BYTES;
    uint64_t *write_base = (uint64_t*)(base + start * (uint64_t)NXMT_WORD_BYTES);
    uint64_t processed = 0;

    while (processed < count) {
        if (nxmt_runner_stop_requested(config)) {
            return false;
        }
        uint64_t chunk = check_chunk;
        if (processed + chunk > count) {
            chunk = count - processed;
        }
        uint64_t *chunk_p = write_base + processed;
        nxmt_write_chunk(chunk_p, chunk, start + processed, phase, config->seed, config->pass);

        if (config->mode == NXMT_MODE_EXTREME) {
            for (uint64_t k = 0; k < chunk; ++k) {
                *pressure_state = nxmt_extreme_cpu_pressure(*pressure_state, chunk_p[k]);
            }
        }

        nxmt_progress_add(config, chunk * (uint64_t)NXMT_WORD_BYTES);
        processed += chunk;
    }

    if (config->inject_mismatch && count > 0) {
        write_base[0] ^= 0x100u;
    }
    return true;
}

static bool nxmt_verify_phase(
    const uint8_t *base,
    uint64_t start,
    uint64_t count,
    NxmtPhase phase,
    const NxmtRunConfig *config,
    NxmtReport *report,
    uint64_t *pressure_state) {
    const uint64_t check_chunk = (1ull << 20) / (uint64_t)NXMT_WORD_BYTES;
    const uint64_t *read_base = (const uint64_t*)(base + start * (uint64_t)NXMT_WORD_BYTES);
    uint64_t processed = 0;

    while (processed < count) {
        if (nxmt_runner_stop_requested(config)) {
            return false;
        }
        uint64_t chunk = check_chunk;
        if (processed + chunk > count) {
            chunk = count - processed;
        }
        const uint64_t *chunk_p = read_base + processed;
        nxmt_verify_chunk(chunk_p, chunk, start + processed, phase, config, report);

        if (config->mode == NXMT_MODE_EXTREME) {
            for (uint64_t k = 0; k < chunk; ++k) {
                *pressure_state = nxmt_extreme_cpu_pressure(*pressure_state, chunk_p[k]);
            }
        }

        nxmt_progress_add(config, chunk * (uint64_t)NXMT_WORD_BYTES);
        processed += chunk;
    }
    return true;
}

NxmtStatus nxmt_runner_run_pass(
    const NxmtArena *arena,
    const NxmtRunConfig *config,
    NxmtReport *report,
    NxmtRunStats *stats) {
    if (stats == 0) {
        return NXMT_STATUS_UNSUPPORTED;
    }

    stats->bytes_written = 0;
    stats->bytes_verified = 0;
    stats->pressure_checksum = 0;
    stats->current_phase = NXMT_PHASE_FIXED_A;

    if (arena == 0 || config == 0 || report == 0) {
        return NXMT_STATUS_UNSUPPORTED;
    }
    if (arena->base == 0 || arena->words == 0 || config->worker_count == 0 || config->worker_id >= config->worker_count) {
        return NXMT_STATUS_UNSUPPORTED;
    }
    if (!nxmt_runner_is_aligned(arena)) {
        return NXMT_STATUS_UNSUPPORTED;
    }

    uint64_t initial_errors = report->error_count;
    uint64_t pressure_state = config->seed ^ config->worker_id;
    uint64_t start = nxmt_split_block_start(arena->words, config->worker_count, config->worker_id);
    uint64_t count = nxmt_split_block_size(arena->words, config->worker_count, config->worker_id);
    uint8_t *base = arena->base;
    uint32_t phase_count = nxmt_phase_count_for_mode(config->mode);

    for (uint32_t p = 0; p < phase_count; ++p) {
        if (nxmt_runner_stop_requested(config)) {
            stats->pressure_checksum = pressure_state;
            return nxmt_runner_abort_status(report, initial_errors);
        }
        NxmtPhase phase = nxmt_phase_for_mode(config->mode, p);
        stats->current_phase = phase;
        if (!nxmt_write_phase(base, start, count, phase, config, &pressure_state)) {
            stats->pressure_checksum = pressure_state;
            return nxmt_runner_abort_status(report, initial_errors);
        }
        stats->bytes_written += count * (uint64_t)NXMT_WORD_BYTES;
        if (!nxmt_verify_phase(base, start, count, phase, config, report, &pressure_state)) {
            stats->pressure_checksum = pressure_state;
            return nxmt_runner_abort_status(report, initial_errors);
        }
        stats->bytes_verified += count * (uint64_t)NXMT_WORD_BYTES;
    }

    stats->pressure_checksum = pressure_state;
    return report->error_count != initial_errors ? NXMT_STATUS_FAIL : NXMT_STATUS_PASS;
}
