#include "nxmt/report.h"

void nxmt_report_init(NxmtReport *report) {
    report->has_first_error = false;
    report->error_count = 0;
    report->min_error_offset = 0;
    report->max_error_offset = 0;
    report->bit_diff_or = 0;
}

void nxmt_report_record_error(
    NxmtReport *report,
    NxmtMode mode,
    NxmtPhase phase,
    uint64_t seed,
    uint64_t pass,
    uint32_t worker_id,
    uint64_t offset,
    uint64_t expected,
    uint64_t actual) {
    uint64_t diff = expected ^ actual;

    if (!report->has_first_error) {
        report->has_first_error = true;
        report->first.mode = mode;
        report->first.phase = phase;
        report->first.seed = seed;
        report->first.pass = pass;
        report->first.worker_id = worker_id;
        report->first.offset = offset;
        report->first.expected = expected;
        report->first.actual = actual;
        report->first.xor_diff = diff;
        report->min_error_offset = offset;
        report->max_error_offset = offset;
    } else {
        if (offset < report->min_error_offset) {
            report->min_error_offset = offset;
        }
        if (offset > report->max_error_offset) {
            report->max_error_offset = offset;
        }
    }

    report->error_count += 1u;
    report->bit_diff_or |= diff;
}
