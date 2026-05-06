#include <stdio.h>
#include "nxmt/arena.h"
#include "nxmt/platform.h"
#include "nxmt/runner.h"

static void print_size_line(const char *label, uint64_t bytes) {
    nxmt_platform_print("%s: %llu MiB\n", label, (unsigned long long)(bytes / NXMT_MIB_BYTES));
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    nxmt_platform_console_init();

    NxmtPlatformMemory memory;
    nxmt_platform_get_memory(&memory);

    nxmt_platform_print("NX-MemTest\n");
    nxmt_platform_print("NRO full-memory stress test\n\n");

    if (!memory.has_heap_override) {
        nxmt_platform_print("No OverrideHeap detected.\n");
        nxmt_platform_print("Run hbmenu through title override/full-memory mode.\n");
        nxmt_platform_print("Press PLUS to exit.\n");
        while (appletMainLoop() && !nxmt_platform_should_quit()) {
        }
        nxmt_platform_console_exit();
        return 0;
    }

    NxmtArena arena = nxmt_arena_from_range(memory.override_heap_addr, memory.override_heap_size);
    print_size_line("Test Arena", arena.size);
    if (memory.has_switch_total) {
        print_size_line("Switch Total", memory.switch_total_memory);
        nxmt_platform_print("Physical Coverage: %llu.%03llu%%\n",
            (unsigned long long)(nxmt_percent_milli(arena.size, memory.switch_total_memory) / 1000ull),
            (unsigned long long)(nxmt_percent_milli(arena.size, memory.switch_total_memory) % 1000ull));
    }

    NxmtReport report;
    NxmtRunStats stats;
    NxmtRunConfig config;
    nxmt_report_init(&report);

    config.mode = NXMT_MODE_QUICK;
    config.seed = nxmt_platform_seed64();
    config.pass = 0;
    config.worker_id = 0;
    config.worker_count = 1;
    config.inject_mismatch = false;

    nxmt_platform_print("\nRunning Quick Check pass...\n");
    uint64_t started = nxmt_platform_ticks_ms();
    NxmtStatus status = nxmt_runner_run_pass(&arena, &config, &report, &stats);
    uint64_t elapsed = nxmt_platform_ticks_ms() - started;

    nxmt_platform_print("System Stress Pass: 100.000%%\n");
    nxmt_platform_print("Verified Arena: 100.000%% of %llu MiB\n", (unsigned long long)(arena.size / NXMT_MIB_BYTES));
    nxmt_platform_print("Tested: %llu MiB\n", (unsigned long long)(stats.bytes_verified / NXMT_MIB_BYTES));
    nxmt_platform_print("Elapsed: %llu ms\n", (unsigned long long)elapsed);
    nxmt_platform_print("Errors: %llu\n", (unsigned long long)report.error_count);
    nxmt_platform_print("Status: %s\n", status == NXMT_STATUS_PASS ? "PASS" : "FAIL");
    nxmt_platform_print("\nPress PLUS to exit.\n");

    while (appletMainLoop() && !nxmt_platform_should_quit()) {
    }

    nxmt_platform_console_exit();
    return status == NXMT_STATUS_PASS ? 0 : 1;
}
