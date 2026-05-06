#include <stdint.h>
#include <string.h>

#include "nxmt/runner.h"

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

static volatile uint64_t g_extreme_sink;

static void nxmt_extreme_cpu_pressure(uint64_t value) {
    for (uint32_t i = 0; i < 64u; ++i) {
        value = nxmt_mix64(value + i);
        value ^= value << 13;
        value ^= value >> 7;
        value ^= value << 17;
    }
    g_extreme_sink ^= value;
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

static bool nxmt_runner_is_aligned(const NxmtArena *arena) {
    return ((uintptr_t)arena->base % _Alignof(uint64_t)) == 0;
}

static void nxmt_store_word(uint8_t *base, uint64_t word_index, uint64_t value) {
    memcpy(base + word_index * NXMT_WORD_BYTES, &value, sizeof(value));
}

static uint64_t nxmt_load_word(const uint8_t *base, uint64_t word_index) {
    uint64_t value;
    memcpy(&value, base + word_index * NXMT_WORD_BYTES, sizeof(value));
    return value;
}

static void nxmt_write_phase(uint8_t *base, uint64_t start, uint64_t count, NxmtPhase phase, const NxmtRunConfig *config) {
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t word_index = start + i;
        uint64_t offset = word_index * NXMT_WORD_BYTES;
        uint64_t value = nxmt_expected_value(config->seed, phase, config->pass, offset);
        nxmt_store_word(base, word_index, value);
        if (config->mode == NXMT_MODE_EXTREME) {
            nxmt_extreme_cpu_pressure(value);
        }
    }

    if (config->inject_mismatch && count > 0) {
        uint64_t value = nxmt_load_word(base, start);
        nxmt_store_word(base, start, value ^ 0x100u);
    }
}

static void nxmt_verify_phase(
    const uint8_t *base,
    uint64_t start,
    uint64_t count,
    NxmtPhase phase,
    const NxmtRunConfig *config,
    NxmtReport *report) {
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t word_index = start + i;
        uint64_t offset = word_index * NXMT_WORD_BYTES;
        uint64_t expected = nxmt_expected_value(config->seed, phase, config->pass, offset);
        uint64_t actual = nxmt_load_word(base, word_index);
        if (config->mode == NXMT_MODE_EXTREME) {
            nxmt_extreme_cpu_pressure(actual);
        }
        if (actual != expected) {
            nxmt_report_record_error(report, config->mode, phase, config->seed, config->pass, config->worker_id, offset, expected, actual);
        }
    }
}

NxmtStatus nxmt_runner_run_pass(
    const NxmtArena *arena,
    const NxmtRunConfig *config,
    NxmtReport *report,
    NxmtRunStats *stats) {
    if (arena == 0 || arena->base == 0 || arena->words == 0 || config == 0 || report == 0 || stats == 0) {
        return NXMT_STATUS_UNSUPPORTED;
    }
    if (config->worker_count == 0 || config->worker_id >= config->worker_count) {
        return NXMT_STATUS_UNSUPPORTED;
    }
    if (!nxmt_runner_is_aligned(arena)) {
        return NXMT_STATUS_UNSUPPORTED;
    }

    stats->bytes_written = 0;
    stats->bytes_verified = 0;
    stats->current_phase = NXMT_PHASE_FIXED_A;

    uint64_t initial_errors = report->error_count;
    uint64_t start = nxmt_split_block_start(arena->words, config->worker_count, config->worker_id);
    uint64_t count = nxmt_split_block_size(arena->words, config->worker_count, config->worker_id);
    uint8_t *base = arena->base;
    uint32_t phase_count = nxmt_phase_count_for_mode(config->mode);

    for (uint32_t p = 0; p < phase_count; ++p) {
        NxmtPhase phase = nxmt_phase_for_mode(config->mode, p);
        stats->current_phase = phase;
        nxmt_write_phase(base, start, count, phase, config);
        stats->bytes_written += count * NXMT_WORD_BYTES;
        nxmt_verify_phase(base, start, count, phase, config, report);
        stats->bytes_verified += count * NXMT_WORD_BYTES;
    }

    return report->error_count != initial_errors ? NXMT_STATUS_FAIL : NXMT_STATUS_PASS;
}
