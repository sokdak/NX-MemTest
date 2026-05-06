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

static bool nxmt_runner_stop_requested(const NxmtRunConfig *config) {
    return config->stop_requested != 0 && atomic_load_explicit(config->stop_requested, memory_order_relaxed);
}

static NxmtStatus nxmt_runner_abort_status(const NxmtReport *report, uint64_t initial_errors) {
    return report->error_count != initial_errors ? NXMT_STATUS_FAIL : NXMT_STATUS_ABORTED;
}

static bool nxmt_write_phase(
    uint8_t *base,
    uint64_t start,
    uint64_t count,
    NxmtPhase phase,
    const NxmtRunConfig *config,
    uint64_t *pressure_state) {
    for (uint64_t i = 0; i < count; ++i) {
        if (nxmt_runner_stop_requested(config)) {
            return false;
        }
        uint64_t word_index = start + i;
        uint64_t offset = word_index * NXMT_WORD_BYTES;
        uint64_t value = nxmt_expected_value(config->seed, phase, config->pass, offset);
        nxmt_store_word(base, word_index, value);
        if (config->mode == NXMT_MODE_EXTREME) {
            *pressure_state = nxmt_extreme_cpu_pressure(*pressure_state, value);
        }
    }

    if (config->inject_mismatch && count > 0) {
        uint64_t value = nxmt_load_word(base, start);
        nxmt_store_word(base, start, value ^ 0x100u);
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
    for (uint64_t i = 0; i < count; ++i) {
        if (nxmt_runner_stop_requested(config)) {
            return false;
        }
        uint64_t word_index = start + i;
        uint64_t offset = word_index * NXMT_WORD_BYTES;
        uint64_t expected = nxmt_expected_value(config->seed, phase, config->pass, offset);
        uint64_t actual = nxmt_load_word(base, word_index);
        if (config->mode == NXMT_MODE_EXTREME) {
            *pressure_state = nxmt_extreme_cpu_pressure(*pressure_state, actual);
        }
        if (actual != expected) {
            nxmt_report_record_error(report, config->mode, phase, config->seed, config->pass, config->worker_id, offset, expected, actual);
        }
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
        stats->bytes_written += count * NXMT_WORD_BYTES;
        if (!nxmt_verify_phase(base, start, count, phase, config, report, &pressure_state)) {
            stats->pressure_checksum = pressure_state;
            return nxmt_runner_abort_status(report, initial_errors);
        }
        stats->bytes_verified += count * NXMT_WORD_BYTES;
    }

    stats->pressure_checksum = pressure_state;
    return report->error_count != initial_errors ? NXMT_STATUS_FAIL : NXMT_STATUS_PASS;
}
