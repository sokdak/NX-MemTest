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

static const NxmtPhase quick_phases[] = {
    NXMT_PHASE_FIXED_A,
    NXMT_PHASE_ADDRESS,
    NXMT_PHASE_RANDOM
};

static const NxmtPhase memory_load_phases[] = {
    NXMT_PHASE_FIXED_A,
    NXMT_PHASE_FIXED_5,
    NXMT_PHASE_CHECKER,
    NXMT_PHASE_ADDRESS,
    NXMT_PHASE_RANDOM,
    NXMT_PHASE_WALKING
};

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
