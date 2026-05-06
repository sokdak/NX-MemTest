#include <stdio.h>
#include <string.h>
#include <switch.h>
#include "nxmt/arena.h"
#include "nxmt/platform.h"
#include "nxmt/runner.h"

#define NXMT_LOG_PATH "sdmc:/switch/NX-MemTest/logs/latest.txt"

static const char *mode_name(NxmtMode mode) {
    switch (mode) {
    case NXMT_MODE_QUICK:
        return "Quick Check";
    case NXMT_MODE_MEMORY_LOAD:
        return "Memory Load";
    case NXMT_MODE_EXTREME:
        return "Extreme";
    }
    return "Unknown";
}

static const char *status_name(NxmtStatus status) {
    switch (status) {
    case NXMT_STATUS_PASS:
        return "PASS";
    case NXMT_STATUS_FAIL:
        return "FAIL";
    case NXMT_STATUS_ABORTED:
        return "ABORTED";
    case NXMT_STATUS_UNSUPPORTED:
        return "UNSUPPORTED";
    }
    return "UNKNOWN";
}

static void print_size_line(const char *label, uint64_t bytes) {
    nxmt_platform_print("%s: %llu MiB\n", label, (unsigned long long)(bytes / NXMT_MIB_BYTES));
}

static void print_percent(const char *label, uint64_t milli) {
    nxmt_platform_print("%s: %llu.%03llu%%\n",
        label,
        (unsigned long long)(milli / 1000ull),
        (unsigned long long)(milli % 1000ull));
}

static bool choose_mode(uint64_t arena_size, NxmtMode *out_mode) {
    bool has_memory_load = arena_size >= 256ull * NXMT_MIB_BYTES;
    bool has_extreme = arena_size >= 512ull * NXMT_MIB_BYTES;

    nxmt_platform_print("\nSelect mode:\n");
    nxmt_platform_print("A: Quick Check\n");
    nxmt_platform_print("X: Memory Load%s\n", has_memory_load ? "" : " (requires 256 MiB)");
    nxmt_platform_print("Y: Extreme%s\n", has_extreme ? "" : " (requires 512 MiB)");
    nxmt_platform_print("PLUS: Exit\n");

    while (appletMainLoop()) {
        NxmtInput input = nxmt_platform_read_input();
        if (input.plus) {
            return false;
        }
        if (input.a) {
            *out_mode = NXMT_MODE_QUICK;
            return true;
        }
        if (input.x && has_memory_load) {
            *out_mode = NXMT_MODE_MEMORY_LOAD;
            return true;
        }
        if (input.y && has_extreme) {
            *out_mode = NXMT_MODE_EXTREME;
            return true;
        }
    }
    return false;
}

static uint32_t worker_count_for_mode(NxmtMode mode) {
    return mode == NXMT_MODE_EXTREME ? 3u : 1u;
}

static void format_report(
    char *out,
    size_t out_size,
    NxmtMode mode,
    uint32_t workers,
    uint32_t completed_workers,
    uint64_t seed,
    const NxmtArena *arena,
    const NxmtPlatformMemory *memory,
    const NxmtReport *report,
    const NxmtRunStats *stats,
    NxmtStatus status,
    uint64_t elapsed_ms) {
    uint64_t coverage = memory->has_switch_total ? nxmt_percent_milli(arena->size, memory->switch_total_memory) : 0;

    snprintf(out, out_size,
        "NX-MemTest Report\n"
        "Mode: %s\n"
        "Workers: %u\n"
        "Workers Completed: %u\n"
        "Seed: 0x%016llx\n"
        "Status: %s\n"
        "Test Arena MiB: %llu\n"
        "Switch Total MiB: %llu\n"
        "Physical Coverage MilliPercent: %llu\n"
        "Bytes Written: %llu\n"
        "Bytes Verified: %llu\n"
        "Elapsed ms: %llu\n"
        "Errors: %llu\n"
        "First Error Offset: 0x%llx\n"
        "First Error Expected: 0x%016llx\n"
        "First Error Actual: 0x%016llx\n"
        "First Error XorDiff: 0x%016llx\n",
        mode_name(mode),
        workers,
        completed_workers,
        (unsigned long long)seed,
        status_name(status),
        (unsigned long long)(arena->size / NXMT_MIB_BYTES),
        (unsigned long long)(memory->has_switch_total ? memory->switch_total_memory / NXMT_MIB_BYTES : 0),
        (unsigned long long)coverage,
        (unsigned long long)stats->bytes_written,
        (unsigned long long)stats->bytes_verified,
        (unsigned long long)elapsed_ms,
        (unsigned long long)report->error_count,
        (unsigned long long)(report->has_first_error ? report->first.offset : 0),
        (unsigned long long)(report->has_first_error ? report->first.expected : 0),
        (unsigned long long)(report->has_first_error ? report->first.actual : 0),
        (unsigned long long)(report->has_first_error ? report->first.xor_diff : 0));
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
        while (appletMainLoop() && !nxmt_platform_read_input().plus) {
        }
        nxmt_platform_console_exit();
        return 0;
    }

    NxmtArena arena = nxmt_arena_from_range(memory.override_heap_addr, memory.override_heap_size);
    print_size_line("Test Arena", arena.size);
    if (memory.has_switch_total) {
        print_size_line("Switch Total", memory.switch_total_memory);
        print_percent("Physical Coverage", nxmt_percent_milli(arena.size, memory.switch_total_memory));
    } else {
        nxmt_platform_print("Switch Total: unavailable\n");
        nxmt_platform_print("Physical Coverage: unavailable\n");
    }

    NxmtMode mode = NXMT_MODE_QUICK;
    if (!choose_mode(arena.size, &mode)) {
        nxmt_platform_console_exit();
        return 0;
    }

    uint64_t seed = nxmt_platform_seed64();
    uint32_t workers = worker_count_for_mode(mode);
    uint32_t completed_workers = 0;
    bool saw_failure = false;

    NxmtReport report;
    NxmtRunStats total_stats;
    nxmt_report_init(&report);
    memset(&total_stats, 0, sizeof(total_stats));

    nxmt_platform_print("\nRunning %s...\n", mode_name(mode));
    uint64_t started = nxmt_platform_ticks_ms();
    NxmtStatus status = NXMT_STATUS_PASS;

    for (uint32_t worker = 0; worker < workers; ++worker) {
        NxmtRunConfig config;
        NxmtRunStats stats;
        memset(&stats, 0, sizeof(stats));

        config.mode = mode;
        config.seed = seed;
        config.pass = 0;
        config.worker_id = worker;
        config.worker_count = workers;
        config.inject_mismatch = false;

        nxmt_platform_print("Worker %u/%u...\n", worker + 1u, workers);
        NxmtStatus worker_status = nxmt_runner_run_pass(&arena, &config, &report, &stats);
        total_stats.bytes_written += stats.bytes_written;
        total_stats.bytes_verified += stats.bytes_verified;

        if (worker_status == NXMT_STATUS_UNSUPPORTED || worker_status == NXMT_STATUS_ABORTED) {
            status = worker_status;
            break;
        }

        completed_workers += 1u;
        if (worker_status == NXMT_STATUS_FAIL) {
            saw_failure = true;
        }
    }

    if (status == NXMT_STATUS_PASS && saw_failure) {
        status = NXMT_STATUS_FAIL;
    }

    uint64_t elapsed = nxmt_platform_ticks_ms() - started;
    uint64_t verified_for_progress = total_stats.bytes_verified;
    if (verified_for_progress > arena.size) {
        verified_for_progress = arena.size;
    }

    nxmt_platform_print("\n");
    print_percent("System Stress Pass", nxmt_percent_milli(completed_workers, workers));
    nxmt_platform_print("Verified Arena: ");
    if (arena.size == 0) {
        nxmt_platform_print("unavailable\n");
    } else {
        nxmt_platform_print("%llu.%03llu%% of %llu MiB\n",
            (unsigned long long)(nxmt_percent_milli(verified_for_progress, arena.size) / 1000ull),
            (unsigned long long)(nxmt_percent_milli(verified_for_progress, arena.size) % 1000ull),
            (unsigned long long)(arena.size / NXMT_MIB_BYTES));
    }
    nxmt_platform_print("Mode: %s\n", mode_name(mode));
    nxmt_platform_print("Workers: %u\n", workers);
    nxmt_platform_print("Tested: %llu MiB\n", (unsigned long long)(total_stats.bytes_verified / NXMT_MIB_BYTES));
    nxmt_platform_print("Elapsed: %llu ms\n", (unsigned long long)elapsed);
    nxmt_platform_print("Errors: %llu\n", (unsigned long long)report.error_count);
    nxmt_platform_print("Status: %s\n", status_name(status));

    char report_text[2048];
    format_report(report_text, sizeof(report_text), mode, workers, completed_workers, seed, &arena, &memory, &report, &total_stats, status, elapsed);
    bool log_ok = nxmt_platform_write_report(report_text);
    nxmt_platform_print("Log: %s\n", log_ok ? NXMT_LOG_PATH : "unavailable");

    nxmt_platform_print("\nPress PLUS to exit.\n");
    while (appletMainLoop() && !nxmt_platform_read_input().plus) {
    }

    nxmt_platform_console_exit();
    return status == NXMT_STATUS_PASS ? 0 : 1;
}
