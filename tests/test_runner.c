#include <stdint.h>
#include <string.h>
#include "nxmt/arena.h"
#include "nxmt/runner.h"

int main(void) {
    uint8_t buffer[12288];
    memset(buffer, 0, sizeof(buffer));

    NxmtArena arena = nxmt_arena_from_range(buffer, sizeof(buffer));
    NxmtReport report;
    NxmtRunStats stats;
    NxmtRunConfig config;

    config.mode = NXMT_MODE_QUICK;
    config.seed = 0x1234;
    config.pass = 0;
    config.worker_id = 0;
    config.worker_count = 1;
    config.inject_mismatch = false;

    nxmt_report_init(&report);
    NxmtStatus status = nxmt_runner_run_pass(&arena, &config, &report, &stats);

    int failed = 0;
    failed |= status == NXMT_STATUS_PASS ? 0 : 1;
    failed |= report.error_count == 0 ? 0 : 1;
    failed |= stats.bytes_verified == arena.size * 3u ? 0 : 1;

    config.inject_mismatch = true;
    nxmt_report_init(&report);
    status = nxmt_runner_run_pass(&arena, &config, &report, &stats);
    failed |= status == NXMT_STATUS_FAIL ? 0 : 1;
    failed |= report.error_count >= 1 ? 0 : 1;

    return failed;
}
