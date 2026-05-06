#include "nxmt/report.h"

int main(void) {
    NxmtReport report;
    nxmt_report_init(&report);

    int failed = 0;
    failed |= report.error_count == 0 ? 0 : 1;
    failed |= report.has_first_error ? 1 : 0;

    nxmt_report_record_error(&report, NXMT_MODE_MEMORY_LOAD, NXMT_PHASE_RANDOM, 7, 3, 2, 0x80, 0x1111, 0x1011);
    failed |= report.error_count == 1 ? 0 : 1;
    failed |= report.has_first_error ? 0 : 1;
    failed |= report.first.offset == 0x80 ? 0 : 1;
    failed |= report.first.xor_diff == 0x0100 ? 0 : 1;

    nxmt_report_record_error(&report, NXMT_MODE_EXTREME, NXMT_PHASE_ADDRESS, 8, 4, 1, 0x100, 0x2222, 0x3333);
    failed |= report.error_count == 2 ? 0 : 1;
    failed |= report.first.mode == NXMT_MODE_MEMORY_LOAD ? 0 : 1;
    failed |= report.min_error_offset == 0x80 ? 0 : 1;
    failed |= report.max_error_offset == 0x100 ? 0 : 1;

    NxmtReport other;
    nxmt_report_init(&other);
    nxmt_report_record_error(&other, NXMT_MODE_EXTREME, NXMT_PHASE_WALKING, 9, 5, 2, 0x40, 0xf0f0, 0xffff);
    nxmt_report_merge(&report, &other);
    failed |= report.error_count == 3 ? 0 : 1;
    failed |= report.first.mode == NXMT_MODE_MEMORY_LOAD ? 0 : 1;
    failed |= report.min_error_offset == 0x40 ? 0 : 1;
    failed |= report.max_error_offset == 0x100 ? 0 : 1;

    return failed;
}
