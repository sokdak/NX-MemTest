#include <stdio.h>
#include <stdatomic.h>
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

static const char *memory_source_name(NxmtMemorySource source) {
    switch (source) {
    case NXMT_MEMORY_SOURCE_PHYSICAL_POOLS:
        return "physical-pools";
    case NXMT_MEMORY_SOURCE_PROCESS_TOTAL:
        return "process-total";
    case NXMT_MEMORY_SOURCE_OVERRIDE_HEAP:
        return "override-heap";
    case NXMT_MEMORY_SOURCE_NONE:
        return "none";
    }
    return "unknown";
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

typedef struct NxmtWorkerContext {
    const NxmtArena *arena;
    NxmtRunConfig config;
    NxmtReport report;
    NxmtRunStats stats;
    NxmtStatus status;
} NxmtWorkerContext;

static Thread g_worker_threads[3];
static unsigned char g_worker_stacks[3][64 * 1024] __attribute__((aligned(4096)));
static NxmtWorkerContext g_worker_contexts[3];
static atomic_bool g_stop_requested;
static atomic_uint g_finished_workers;

static void nxmt_worker_entry(void *arg) {
    NxmtWorkerContext *ctx = (NxmtWorkerContext*)arg;
    nxmt_report_init(&ctx->report);
    ctx->status = nxmt_runner_run_pass(ctx->arena, &ctx->config, &ctx->report, &ctx->stats);
    atomic_fetch_add_explicit(&g_finished_workers, 1u, memory_order_release);
}

static NxmtStatus combine_worker_status(NxmtStatus current, NxmtStatus worker_status) {
    if (current == NXMT_STATUS_FAIL || worker_status == NXMT_STATUS_FAIL) {
        return NXMT_STATUS_FAIL;
    }
    if (current == NXMT_STATUS_ABORTED || worker_status == NXMT_STATUS_ABORTED) {
        return NXMT_STATUS_ABORTED;
    }
    if (current == NXMT_STATUS_UNSUPPORTED || worker_status == NXMT_STATUS_UNSUPPORTED) {
        return NXMT_STATUS_UNSUPPORTED;
    }
    return NXMT_STATUS_PASS;
}

static void init_worker_context(
    NxmtWorkerContext *ctx,
    const NxmtArena *arena,
    NxmtMode mode,
    uint64_t seed,
    uint32_t worker,
    uint32_t workers,
    atomic_bool *stop_requested) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->arena = arena;
    ctx->config.mode = mode;
    ctx->config.seed = seed;
    ctx->config.pass = 0;
    ctx->config.worker_id = worker;
    ctx->config.worker_count = workers;
    ctx->config.inject_mismatch = false;
    ctx->config.stop_requested = stop_requested;
    ctx->status = NXMT_STATUS_UNSUPPORTED;
}

static void merge_worker_context(
    const NxmtWorkerContext *ctx,
    NxmtReport *report,
    NxmtRunStats *total_stats,
    NxmtStatus *status,
    uint32_t *completed_workers) {
    total_stats->bytes_written += ctx->stats.bytes_written;
    total_stats->bytes_verified += ctx->stats.bytes_verified;
    total_stats->pressure_checksum ^= ctx->stats.pressure_checksum;
    nxmt_report_merge(report, &ctx->report);
    *status = combine_worker_status(*status, ctx->status);
    if (ctx->status != NXMT_STATUS_UNSUPPORTED && ctx->status != NXMT_STATUS_ABORTED) {
        *completed_workers += 1u;
    }
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
    bool thread_fallback,
    bool stop_requested,
    uint64_t elapsed_ms) {
    uint64_t coverage = memory->has_effective_total ? nxmt_percent_milli(arena->size, memory->effective_total_memory) : 0;

    snprintf(out, out_size,
        "NX-MemTest Report\n"
        "Mode: %s\n"
        "Workers: %u\n"
        "Workers Completed: %u\n"
        "Seed: 0x%016llx\n"
        "Status: %s\n"
        "Thread Fallback: %s\n"
        "Stop Requested: %s\n"
        "Test Arena MiB: %llu\n"
        "Effective Total MiB: %llu\n"
        "Total Source: %s\n"
        "Extended Memory Detected: %s\n"
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
        thread_fallback ? "yes" : "no",
        stop_requested ? "yes" : "no",
        (unsigned long long)(arena->size / NXMT_MIB_BYTES),
        (unsigned long long)(memory->has_effective_total ? memory->effective_total_memory / NXMT_MIB_BYTES : 0),
        memory_source_name(memory->effective_total_source),
        memory->extended_memory_detected ? "yes" : "no",
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
    if (memory.has_effective_total) {
        print_size_line("Effective Total", memory.effective_total_memory);
        nxmt_platform_print("Total Source: %s\n", memory_source_name(memory.effective_total_source));
        nxmt_platform_print("Extended Memory: %s\n", memory.extended_memory_detected ? "detected" : "no");
        print_percent("Physical Coverage", nxmt_percent_milli(arena.size, memory.effective_total_memory));
    } else {
        nxmt_platform_print("Effective Total: unavailable\n");
        nxmt_platform_print("Total Source: none\n");
        nxmt_platform_print("Extended Memory: no\n");
        nxmt_platform_print("Physical Coverage: unavailable\n");
    }

    NxmtMode mode = NXMT_MODE_QUICK;
    if (!choose_mode(arena.size, &mode)) {
        nxmt_platform_console_exit();
        return 0;
    }

    uint64_t seed = nxmt_platform_seed64();
    uint32_t workers = worker_count_for_mode(mode);
    if (workers > 3u) {
        workers = 3u;
    }
    uint32_t completed_workers = 0;

    NxmtReport report;
    NxmtRunStats total_stats;
    nxmt_report_init(&report);
    memset(&total_stats, 0, sizeof(total_stats));

    nxmt_platform_print("\nRunning %s...\n", mode_name(mode));
    uint64_t started = nxmt_platform_ticks_ms();
    NxmtStatus status = NXMT_STATUS_PASS;
    bool thread_fallback = false;
    bool use_threaded_workers = true;
    uint32_t created_threads = 0;
    uint32_t started_threads = 0;
    atomic_init(&g_stop_requested, false);
    atomic_init(&g_finished_workers, 0u);

    for (uint32_t worker = 0; worker < workers; ++worker) {
        init_worker_context(&g_worker_contexts[worker], &arena, mode, seed, worker, workers, &g_stop_requested);
        Result rc = threadCreate(
            &g_worker_threads[worker],
            nxmt_worker_entry,
            &g_worker_contexts[worker],
            g_worker_stacks[worker],
            sizeof(g_worker_stacks[worker]),
            0x2c,
            -2);
        if (rc != 0) {
            use_threaded_workers = false;
            break;
        }
        created_threads += 1u;
    }

    if (!use_threaded_workers) {
        for (uint32_t worker = 0; worker < created_threads; ++worker) {
            threadClose(&g_worker_threads[worker]);
        }
        thread_fallback = true;
        workers = 1u;
    } else {
        for (uint32_t worker = 0; worker < workers; ++worker) {
            nxmt_platform_print("Worker %u/%u...\n", worker + 1u, workers);
            Result rc = threadStart(&g_worker_threads[worker]);
            if (rc != 0) {
                use_threaded_workers = false;
                break;
            }
            started_threads += 1u;
        }

        if (!use_threaded_workers && started_threads == 0) {
            for (uint32_t worker = 0; worker < created_threads; ++worker) {
                threadClose(&g_worker_threads[worker]);
            }
            thread_fallback = true;
            workers = 1u;
        } else {
            while (atomic_load_explicit(&g_finished_workers, memory_order_acquire) < started_threads) {
                if (!appletMainLoop()) {
                    atomic_store_explicit(&g_stop_requested, true, memory_order_relaxed);
                    break;
                }
                if (nxmt_platform_read_input().plus) {
                    atomic_store_explicit(&g_stop_requested, true, memory_order_relaxed);
                    break;
                }
                svcSleepThread(10000000ll);
            }

            for (uint32_t worker = 0; worker < started_threads; ++worker) {
                threadWaitForExit(&g_worker_threads[worker]);
            }
            for (uint32_t worker = 0; worker < created_threads; ++worker) {
                threadClose(&g_worker_threads[worker]);
            }
            if (!use_threaded_workers) {
                NxmtReport partial_report;
                NxmtRunStats partial_stats;
                NxmtStatus partial_status = NXMT_STATUS_PASS;
                uint32_t partial_completed_workers = 0;
                nxmt_report_init(&partial_report);
                memset(&partial_stats, 0, sizeof(partial_stats));

                for (uint32_t worker = 0; worker < started_threads; ++worker) {
                    merge_worker_context(
                        &g_worker_contexts[worker],
                        &partial_report,
                        &partial_stats,
                        &partial_status,
                        &partial_completed_workers);
                }

                if (partial_status == NXMT_STATUS_PASS &&
                        partial_report.error_count == 0 &&
                        !atomic_load_explicit(&g_stop_requested, memory_order_relaxed)) {
                    thread_fallback = true;
                    workers = 1u;
                } else {
                    total_stats = partial_stats;
                    nxmt_report_merge(&report, &partial_report);
                    status = combine_worker_status(status, partial_status);
                    completed_workers = partial_completed_workers;
                }
            } else {
                for (uint32_t worker = 0; worker < started_threads; ++worker) {
                    merge_worker_context(&g_worker_contexts[worker], &report, &total_stats, &status, &completed_workers);
                }
            }
        }
    }

    if (thread_fallback) {
        NxmtWorkerContext context;
        atomic_store_explicit(&g_stop_requested, false, memory_order_relaxed);
        init_worker_context(&context, &arena, mode, seed, 0, 1u, &g_stop_requested);
        nxmt_report_init(&context.report);
        memset(&context.stats, 0, sizeof(context.stats));

        nxmt_platform_print("Thread setup failed; retrying single-worker fallback.\n");
        nxmt_platform_print("Worker 1/1...\n");
        context.status = nxmt_runner_run_pass(&arena, &context.config, &context.report, &context.stats);
        merge_worker_context(&context, &report, &total_stats, &status, &completed_workers);
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
    if (thread_fallback) {
        nxmt_platform_print("Thread Fallback: yes\n");
    }

    char report_text[2048];
    format_report(
        report_text,
        sizeof(report_text),
        mode,
        workers,
        completed_workers,
        seed,
        &arena,
        &memory,
        &report,
        &total_stats,
        status,
        thread_fallback,
        atomic_load_explicit(&g_stop_requested, memory_order_relaxed),
        elapsed);
    bool log_ok = nxmt_platform_write_report(report_text);
    nxmt_platform_print("Log: %s\n", log_ok ? NXMT_LOG_PATH : "unavailable");

    nxmt_platform_print("\nPress PLUS to exit.\n");
    while (appletMainLoop() && !nxmt_platform_read_input().plus) {
    }

    nxmt_platform_console_exit();
    return status == NXMT_STATUS_PASS ? 0 : 1;
}
