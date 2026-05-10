#pragma once

#include <stdatomic.h>

#include "nxmt/arena.h"
#include "nxmt/patterns.h"
#include "nxmt/report.h"

typedef struct NxmtRunConfig {
    NxmtMode mode;
    uint64_t seed;
    uint64_t pass;
    uint32_t worker_id;
    uint32_t worker_count;
    bool inject_mismatch;
    atomic_bool *stop_requested;
    atomic_uint_fast64_t *progress_bytes;
} NxmtRunConfig;

typedef struct NxmtRunStats {
    uint64_t bytes_written;
    uint64_t bytes_verified;
    uint64_t pressure_checksum;
    NxmtPhase current_phase;
} NxmtRunStats;

NxmtStatus nxmt_runner_run_pass(
    const NxmtArena *arena,
    const NxmtRunConfig *config,
    NxmtReport *report,
    NxmtRunStats *stats);

uint32_t nxmt_runner_phase_count(NxmtMode mode);
