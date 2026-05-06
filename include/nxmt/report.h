#pragma once

#include "nxmt/types.h"

typedef struct NxmtError {
    NxmtMode mode;
    NxmtPhase phase;
    uint64_t seed;
    uint64_t pass;
    uint32_t worker_id;
    uint64_t offset;
    uint64_t expected;
    uint64_t actual;
    uint64_t xor_diff;
} NxmtError;

typedef struct NxmtReport {
    bool has_first_error;
    NxmtError first;
    uint64_t error_count;
    uint64_t min_error_offset;
    uint64_t max_error_offset;
    uint64_t bit_diff_or;
} NxmtReport;

void nxmt_report_init(NxmtReport *report);
void nxmt_report_record_error(
    NxmtReport *report,
    NxmtMode mode,
    NxmtPhase phase,
    uint64_t seed,
    uint64_t pass,
    uint32_t worker_id,
    uint64_t offset,
    uint64_t expected,
    uint64_t actual);
