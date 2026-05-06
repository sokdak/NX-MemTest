#pragma once

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
} NxmtRunConfig;

typedef struct NxmtRunStats {
    uint64_t bytes_written;
    uint64_t bytes_verified;
    NxmtPhase current_phase;
} NxmtRunStats;

NxmtStatus nxmt_runner_run_pass(
    const NxmtArena *arena,
    const NxmtRunConfig *config,
    NxmtReport *report,
    NxmtRunStats *stats);
