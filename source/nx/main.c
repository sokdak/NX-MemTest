#include <stdio.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <string.h>
#include <switch.h>
#include "nxmt/arena.h"
#include "nxmt/platform.h"
#include "nxmt/runner.h"
#include "gpu_bench.h"
#include "gpu_pump.h"
#include "cpu_filler.h"

#define NXMT_LOG_PATH "sdmc:/switch/NX-MemTest/logs/latest.txt"

#define TUI_WIDTH 78
#define TUI_BAR_WIDTH 56

#define ANSI_RESET   "\x1b[0m"
#define ANSI_BOLD    "\x1b[1m"
#define ANSI_DIM     "\x1b[2m"
#define ANSI_RED     "\x1b[31m"
#define ANSI_GREEN   "\x1b[32m"
#define ANSI_YELLOW  "\x1b[33m"
#define ANSI_CYAN    "\x1b[36m"
#define ANSI_WHITE   "\x1b[37m"

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

static const char *status_color(NxmtStatus status) {
    switch (status) {
    case NXMT_STATUS_PASS:
        return ANSI_GREEN;
    case NXMT_STATUS_FAIL:
        return ANSI_RED;
    case NXMT_STATUS_ABORTED:
        return ANSI_YELLOW;
    case NXMT_STATUS_UNSUPPORTED:
    default:
        return ANSI_DIM;
    }
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

static void fill_chars(char *buf, size_t buf_size, char c, int count) {
    if (buf_size == 0) {
        return;
    }
    if (count < 0) {
        count = 0;
    }
    if ((size_t)count >= buf_size) {
        count = (int)buf_size - 1;
    }
    memset(buf, c, (size_t)count);
    buf[count] = '\0';
}

static int visible_width(const char *s) {
    int w = 0;
    if (s == NULL) {
        return 0;
    }
    while (*s) {
        if (*s == '\x1b' && *(s + 1) == '[') {
            s += 2;
            while (*s && !(*s >= '@' && *s <= '~')) {
                ++s;
            }
            if (*s) {
                ++s;
            }
        } else {
            ++w;
            ++s;
        }
    }
    return w;
}

static void tui_clear(void) {
    nxmt_platform_print("\x1b[2J\x1b[H");
}

static void tui_goto(uint32_t row, uint32_t col) {
    nxmt_platform_print("\x1b[%u;%uH", row, col);
}

static void tui_hide_cursor(void) {
    nxmt_platform_print("\x1b[?25l");
}

static void tui_show_cursor(void) {
    nxmt_platform_print("\x1b[?25h");
}

static void tui_section_top(const char *title) {
    int title_len = (int)strlen(title);
    int dashes = TUI_WIDTH - 1 - (4 + title_len);
    char dash_buf[TUI_WIDTH];
    fill_chars(dash_buf, sizeof(dash_buf), '-', dashes);
    nxmt_platform_print(ANSI_CYAN "+- " ANSI_BOLD "%s" ANSI_RESET ANSI_CYAN " %s+" ANSI_RESET "\n",
        title, dash_buf);
}

static void tui_section_bottom(void) {
    char dash_buf[TUI_WIDTH];
    fill_chars(dash_buf, sizeof(dash_buf), '-', TUI_WIDTH - 2);
    nxmt_platform_print(ANSI_CYAN "+%s+" ANSI_RESET "\n", dash_buf);
}

static void tui_section_blank(void) {
    char space_buf[TUI_WIDTH];
    fill_chars(space_buf, sizeof(space_buf), ' ', TUI_WIDTH - 2);
    nxmt_platform_print(ANSI_CYAN "|" ANSI_RESET "%s" ANSI_CYAN "|" ANSI_RESET "\n", space_buf);
}

static void tui_kv(const char *label, const char *value_color, const char *fmt, ...) {
    char value[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(value, sizeof(value), fmt, args);
    va_end(args);

    char line[256];
    int written = snprintf(line, sizeof(line), "  %-22s : %s", label, value);
    if (written < 0) {
        written = 0;
    }
    int padding = (TUI_WIDTH - 2) - written;
    char pad_buf[TUI_WIDTH];
    fill_chars(pad_buf, sizeof(pad_buf), ' ', padding);
    nxmt_platform_print(ANSI_CYAN "|" ANSI_RESET "  " ANSI_DIM "%-22s" ANSI_RESET " : %s%s" ANSI_RESET "%s" ANSI_CYAN "|" ANSI_RESET "\n",
        label, value_color != NULL ? value_color : "", value, pad_buf);
}

static void tui_text_line(const char *text_color, const char *fmt, ...) {
    char text[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    int visible = visible_width(text_color) + visible_width(text);
    int padding = (TUI_WIDTH - 2 - 2) - visible;
    char pad_buf[TUI_WIDTH];
    fill_chars(pad_buf, sizeof(pad_buf), ' ', padding);
    nxmt_platform_print(ANSI_CYAN "|" ANSI_RESET "  %s%s" ANSI_RESET "%s" ANSI_CYAN "|" ANSI_RESET "\n",
        text_color != NULL ? text_color : "", text, pad_buf);
}

static void format_size_mib(char *buf, size_t buf_size, uint64_t bytes) {
    snprintf(buf, buf_size, "%llu MiB", (unsigned long long)(bytes / NXMT_MIB_BYTES));
}

static void format_percent(char *buf, size_t buf_size, uint64_t milli) {
    snprintf(buf, buf_size, "%llu.%03llu%%",
        (unsigned long long)(milli / 1000ull),
        (unsigned long long)(milli % 1000ull));
}

static void format_elapsed(char *buf, size_t buf_size, uint64_t ms) {
    uint64_t secs = ms / 1000ull;
    uint64_t mm = secs / 60ull;
    uint64_t ss = secs % 60ull;
    snprintf(buf, buf_size, "%02llu:%02llu", (unsigned long long)mm, (unsigned long long)ss);
}

static void format_progress_bar(char *buf, size_t buf_size, uint64_t milli) {
    uint64_t filled = (milli * (uint64_t)TUI_BAR_WIDTH) / 100000ull;
    if (filled > (uint64_t)TUI_BAR_WIDTH) {
        filled = (uint64_t)TUI_BAR_WIDTH;
    }
    size_t pos = 0;
    if (pos < buf_size) buf[pos++] = '[';
    for (uint32_t i = 0; i < (uint32_t)TUI_BAR_WIDTH && pos < buf_size - 1; ++i) {
        buf[pos++] = (i < filled) ? '#' : '.';
    }
    if (pos < buf_size) buf[pos++] = ']';
    if (pos < buf_size) buf[pos] = '\0';
    else if (buf_size > 0) buf[buf_size - 1] = '\0';
}

static void draw_memory_section(const NxmtArena *arena, const NxmtPlatformMemory *memory) {
    char buf[64];
    tui_section_top("Memory");
    tui_section_blank();
    format_size_mib(buf, sizeof(buf), arena->size);
    tui_kv("Test Arena", ANSI_YELLOW, "%s", buf);
    if (memory->has_effective_total) {
        format_size_mib(buf, sizeof(buf), memory->effective_total_memory);
        tui_kv("Effective Total", ANSI_YELLOW, "%s", buf);
        tui_kv("Total Source", ANSI_WHITE, "%s", memory_source_name(memory->effective_total_source));
        tui_kv("Extended Memory", memory->extended_memory_detected ? ANSI_GREEN : ANSI_DIM,
            "%s", memory->extended_memory_detected ? "detected" : "no");
        format_percent(buf, sizeof(buf), nxmt_percent_milli(arena->size, memory->effective_total_memory));
        tui_kv("Physical Coverage", ANSI_WHITE, "%s", buf);
    } else {
        tui_kv("Effective Total", ANSI_DIM, "unavailable");
        tui_kv("Total Source", ANSI_DIM, "none");
        tui_kv("Extended Memory", ANSI_DIM, "no");
        tui_kv("Physical Coverage", ANSI_DIM, "unavailable");
    }
    tui_section_blank();
    tui_section_bottom();
}

static void draw_run_config_section(NxmtMode mode, uint32_t workers, uint64_t seed, uint64_t duration_seconds) {
    char buf[64];
    tui_section_top("Configuration");
    tui_section_blank();
    tui_kv("Mode", ANSI_YELLOW, "%s", mode_name(mode));
    snprintf(buf, sizeof(buf), "%u", workers);
    tui_kv("Workers", ANSI_YELLOW, "%s", buf);
    snprintf(buf, sizeof(buf), "0x%016llx", (unsigned long long)seed);
    tui_kv("Seed", ANSI_WHITE, "%s", buf);
    if (duration_seconds > 0) {
        if (duration_seconds % 60u == 0) {
            snprintf(buf, sizeof(buf), "%llu min", (unsigned long long)(duration_seconds / 60u));
        } else {
            snprintf(buf, sizeof(buf), "%llu s", (unsigned long long)duration_seconds);
        }
        tui_kv("Duration", ANSI_YELLOW, "%s", buf);
    } else {
        tui_kv("Duration", ANSI_DIM, "single pass");
    }
    tui_section_blank();
    tui_section_bottom();
}

static void draw_header(void) {
    char eq_buf[TUI_WIDTH];
    fill_chars(eq_buf, sizeof(eq_buf), '=', TUI_WIDTH - 2);
    nxmt_platform_print(ANSI_CYAN "+%s+" ANSI_RESET "\n", eq_buf);
    tui_section_blank();

    const char *title_left = "NX-MemTest 0.1.0";
    const char *title_right = "[PLUS] Exit";
    int left_len = (int)strlen(title_left);
    int right_len = (int)strlen(title_right);
    int padding = (TUI_WIDTH - 2) - 1 - left_len - right_len - 1;
    char pad_buf[TUI_WIDTH];
    fill_chars(pad_buf, sizeof(pad_buf), ' ', padding);
    nxmt_platform_print(ANSI_CYAN "|" ANSI_RESET " " ANSI_BOLD ANSI_WHITE "%s" ANSI_RESET "%s" ANSI_DIM "%s" ANSI_RESET " " ANSI_CYAN "|" ANSI_RESET "\n",
        title_left, pad_buf, title_right);
    tui_section_blank();
    nxmt_platform_print(ANSI_CYAN "+%s+" ANSI_RESET "\n", eq_buf);
}

static void draw_footer_hint(const char *hint) {
    nxmt_platform_print(ANSI_DIM "  %s" ANSI_RESET "\x1b[K\n", hint);
}

typedef struct NxmtWorkerContext {
    const NxmtArena *arena;
    NxmtRunConfig config;
    NxmtReport report;
    NxmtRunStats stats;
    NxmtStatus status;
} NxmtWorkerContext;

#define NXMT_MAX_WORKERS 4u

static Thread g_worker_threads[NXMT_MAX_WORKERS];
static unsigned char g_worker_stacks[NXMT_MAX_WORKERS][64 * 1024] __attribute__((aligned(4096)));
static NxmtWorkerContext g_worker_contexts[NXMT_MAX_WORKERS];
static atomic_bool g_stop_requested;
static atomic_uint g_finished_workers;
static atomic_uint_fast64_t g_worker_progress[NXMT_MAX_WORKERS];

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
    ctx->config.progress_bytes = NULL;
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

typedef struct DurationOption {
    const char *label;
    uint64_t seconds;
} DurationOption;

static const DurationOption kDurations[] = {
    {"3 minutes",   3u * 60u},
    {"5 minutes",   5u * 60u},
    {"10 minutes", 10u * 60u},
    {"15 minutes", 15u * 60u},
    {"30 minutes", 30u * 60u},
    {"60 minutes", 60u * 60u},
};
#define NXMT_DURATION_COUNT (sizeof(kDurations) / sizeof(kDurations[0]))

/* return: 1 = chose, 0 = exit (PLUS), -1 = back (B) */
static int tui_choose_duration(NxmtMode mode, uint64_t *out_seconds) {
    int sel = 1;
    bool need_redraw = true;
    while (appletMainLoop()) {
        if (need_redraw) {
            tui_clear();
            draw_header();
            nxmt_platform_print("\n");
            char title[64];
            snprintf(title, sizeof(title), "Test Duration  (%s)", mode_name(mode));
            tui_section_top(title);
            tui_section_blank();
            for (size_t i = 0; i < NXMT_DURATION_COUNT; ++i) {
                if ((int)i == sel) {
                    tui_text_line(ANSI_GREEN ">" ANSI_RESET, " " ANSI_BOLD "%s" ANSI_RESET, kDurations[i].label);
                } else {
                    tui_text_line(ANSI_DIM " " ANSI_RESET, " %s", kDurations[i].label);
                }
            }
            tui_section_blank();
            tui_section_bottom();
            nxmt_platform_print("\n");
            draw_footer_hint("[Up/Down] move    [A] start    [B] back    [+] exit");
            nxmt_platform_console_flush();
            need_redraw = false;
        }
        NxmtInput input = nxmt_platform_read_input();
        if (input.plus) return 0;
        if (input.b) return -1;
        if (input.a) { *out_seconds = kDurations[sel].seconds; return 1; }
        if (input.up && sel > 0) { sel--; need_redraw = true; }
        if (input.down && sel < (int)NXMT_DURATION_COUNT - 1) { sel++; need_redraw = true; }
        svcSleepThread(16000000ll);
    }
    return 0;
}

static void tui_draw_mode_menu(uint64_t arena_size, bool has_memory_load, bool has_extreme) {
    (void)arena_size;
    tui_section_top("Select Mode");
    tui_section_blank();
    tui_text_line(ANSI_GREEN "[A]" ANSI_RESET, "%s",  " Quick Check");
    tui_text_line(has_memory_load ? ANSI_GREEN "[X]" ANSI_RESET : ANSI_DIM "[X]" ANSI_RESET,
        " Memory Load%s", has_memory_load ? "" : "  (requires 256 MiB)");
    tui_text_line(has_extreme ? ANSI_GREEN "[Y]" ANSI_RESET : ANSI_DIM "[Y]" ANSI_RESET,
        " Extreme%s", has_extreme ? "" : "  (requires 512 MiB)");
    tui_text_line(ANSI_CYAN "[B]" ANSI_RESET, "%s", " GPU Bandwidth PoC");
    tui_text_line(ANSI_RED "[+]" ANSI_RESET, "%s", " Exit");
    tui_section_blank();
    tui_section_bottom();
    nxmt_platform_print("\n");
    draw_footer_hint("Press a button to choose a mode.");
    nxmt_platform_console_flush();
}

static void run_gpu_bench_screen(const NxmtArena *arena, const NxmtPlatformMemory *memory) {
    tui_clear();
    draw_header();
    nxmt_platform_print("\n");
    tui_section_top("GPU Bandwidth PoC");
    tui_section_blank();
    tui_text_line(ANSI_DIM, "%s",
        "Stepping deko3d buffer size up until the kernel runs out of headroom.");
    tui_section_blank();
    tui_section_bottom();
    nxmt_platform_print("\n");
    nxmt_platform_console_flush();

    /* Carve a slice off the tail of the test arena for GPU buffers; the
     * kernel pool is too tight to allocate fresh deko3d MemBlocks because
     * OverrideHeap owns almost all physical RAM. The slice is owned by the
     * arena anyway and gets overwritten on the next memory test, so the
     * GPU PoC reusing it is harmless. */
    static const struct {
        uint64_t size_mib;
        uint32_t iters;
    } steps[] = {
        {   4, 64 },
        {  16, 32 },
        {  32, 16 },
        {  64,  8 },
        { 128,  4 },
        { 256,  2 },
    };
    uint64_t max_step_bytes = 256ull * NXMT_MIB_BYTES;
    /* Need cmd + 2 * largest buffer; round up generously. */
    uint64_t storage_needed = max_step_bytes * 2u + NXMT_MIB_BYTES;
    if (storage_needed > arena->size) {
        storage_needed = arena->size;
    }
    void *gpu_storage = arena->base + (arena->size - storage_needed);
    nxmt_platform_print(ANSI_DIM
        "Using %llu MiB at arena tail (%p) as GPU storage.\n" ANSI_RESET,
        (unsigned long long)(storage_needed / NXMT_MIB_BYTES), gpu_storage);
    nxmt_platform_console_flush();

    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
        nxmt_platform_print(ANSI_BOLD "== buffer %llu MiB x %u iters ==" ANSI_RESET "\n",
            (unsigned long long)steps[i].size_mib, steps[i].iters);
        nxmt_platform_console_flush();
        bool ok = nxmt_gpu_bench_run(gpu_storage, storage_needed,
            steps[i].size_mib * NXMT_MIB_BYTES, steps[i].iters);
        if (!ok) {
            nxmt_platform_print(ANSI_YELLOW
                "  step failed - stopping sweep at %llu MiB\n" ANSI_RESET,
                (unsigned long long)steps[i].size_mib);
            break;
        }
    }

    nxmt_platform_print("\n");
    draw_footer_hint("[B] / [PLUS] back to menu");
    nxmt_platform_console_flush();

    while (appletMainLoop()) {
        NxmtInput in = nxmt_platform_read_input();
        if (in.plus || in.b) break;
        svcSleepThread(16000000ll);
    }

    tui_clear();
    draw_header();
    nxmt_platform_print("\n");
    draw_memory_section(arena, memory);
    nxmt_platform_print("\n");
}

static bool tui_choose_mode(const NxmtArena *arena, const NxmtPlatformMemory *memory, NxmtMode *out_mode) {
    bool has_memory_load = arena->size >= 256ull * NXMT_MIB_BYTES;
    bool has_extreme    = arena->size >= 512ull * NXMT_MIB_BYTES;
    bool need_redraw = true;

    while (appletMainLoop()) {
        if (need_redraw) {
            tui_draw_mode_menu(arena->size, has_memory_load, has_extreme);
            need_redraw = false;
        }
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
        if (input.b) {
            run_gpu_bench_screen(arena, memory);
            need_redraw = true;
        }
        svcSleepThread(16000000ll);
    }
    return false;
}

/* Fills out_cpu_map with the cores we want each worker pinned to, in worker
 * order, using only cores the kernel reports as accessible. Preferred order
 * (core 2, 0, 1, 3) preserves the existing 3-worker affinity layout when
 * core 3 isn't available, and otherwise grows to 4 workers. */
static uint32_t collect_worker_cpu_map(int *out_cpu_map, uint32_t max_workers) {
    uint64_t mask = nxmt_platform_core_mask();
    static const int preferred_order[] = { 2, 0, 1, 3 };
    uint32_t count = 0;
    for (size_t i = 0; i < sizeof(preferred_order) / sizeof(preferred_order[0])
                       && count < max_workers; ++i) {
        int core = preferred_order[i];
        if (mask & (1ull << core)) {
            out_cpu_map[count++] = core;
        }
    }
    return count;
}

typedef struct ProgressLayout {
    uint32_t row_top;
    uint32_t row_line;
    uint32_t row_bar;
    uint32_t row_info;
    uint32_t row_bottom;
} ProgressLayout;

static void draw_progress_frame(ProgressLayout *layout, uint32_t row_start) {
    /* Layout with inner top/bottom blank padding around the three content
     * rows (line, bar, info), matching the other section boxes. */
    layout->row_top    = row_start;
    /* row_start + 1: top padding blank */
    layout->row_line   = row_start + 2u;
    layout->row_bar    = row_start + 3u;
    layout->row_info   = row_start + 4u;
    /* row_start + 5: bottom padding blank */
    layout->row_bottom = row_start + 6u;

    tui_goto(layout->row_top, 1u);
    tui_section_top("Progress");
    tui_goto(row_start + 1u, 1u);
    tui_section_blank();
    tui_goto(layout->row_line, 1u);
    tui_section_blank();
    tui_goto(layout->row_bar, 1u);
    tui_section_blank();
    tui_goto(layout->row_info, 1u);
    tui_section_blank();
    tui_goto(row_start + 5u, 1u);
    tui_section_blank();
    tui_goto(layout->row_bottom, 1u);
    tui_section_bottom();
}

typedef struct ProgressInfo {
    char spinner;
    uint64_t milli;
    const char *header_extra;
    const char *info_text;
} ProgressInfo;

static void redraw_progress_lines(const ProgressLayout *layout, const ProgressInfo *info) {
    char pct[32];
    char bar[TUI_BAR_WIDTH + 4];
    format_percent(pct, sizeof(pct), info->milli);
    format_progress_bar(bar, sizeof(bar), info->milli);

    tui_goto(layout->row_line, 1u);
    char body_line[160];
    snprintf(body_line, sizeof(body_line),
        ANSI_GREEN "[%c]" ANSI_RESET "  " ANSI_BOLD "%s" ANSI_RESET "    %s",
        info->spinner, pct,
        info->header_extra != NULL ? info->header_extra : "");
    tui_text_line("", "%s", body_line);

    tui_goto(layout->row_bar, 1u);
    tui_text_line(ANSI_GREEN, "%s", bar);

    tui_goto(layout->row_info, 1u);
    tui_text_line("", "%s", info->info_text != NULL ? info->info_text : "");
}

static void draw_results_section(
    NxmtMode mode,
    uint32_t workers,
    uint32_t completed_workers,
    uint64_t passes_completed,
    const NxmtArena *arena,
    const NxmtRunStats *total_stats,
    const NxmtReport *report,
    NxmtStatus status,
    bool thread_fallback,
    uint64_t elapsed_ms,
    uint64_t gpu_pumped_bytes,
    uint64_t gpu_pump_error_batches,
    bool log_ok) {
    char buf[64];
    tui_section_top("Result");
    tui_section_blank();
    tui_kv("Status", status_color(status), "%s", status_name(status));
    format_percent(buf, sizeof(buf), nxmt_percent_milli(completed_workers, workers));
    tui_kv("System Stress Pass", ANSI_WHITE, "%s", buf);
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)passes_completed);
    tui_kv("Passes Completed", ANSI_YELLOW, "%s", buf);
    if (arena->size == 0) {
        tui_kv("Verified Arena", ANSI_DIM, "unavailable");
    } else {
        uint64_t verified_for_progress = total_stats->bytes_verified;
        if (verified_for_progress > arena->size) {
            verified_for_progress = arena->size;
        }
        uint64_t pmilli = nxmt_percent_milli(verified_for_progress, arena->size);
        char value[96];
        snprintf(value, sizeof(value), "%llu.%03llu%% of %llu MiB",
            (unsigned long long)(pmilli / 1000ull),
            (unsigned long long)(pmilli % 1000ull),
            (unsigned long long)(arena->size / NXMT_MIB_BYTES));
        tui_kv("Verified Arena", ANSI_WHITE, "%s", value);
    }
    if (total_stats->bytes_verified >= 1024ull * NXMT_MIB_BYTES) {
        uint64_t gib_x100 = (total_stats->bytes_verified * 100ull) / (1024ull * NXMT_MIB_BYTES);
        snprintf(buf, sizeof(buf), "%llu.%02llu GiB",
            (unsigned long long)(gib_x100 / 100ull),
            (unsigned long long)(gib_x100 % 100ull));
    } else {
        snprintf(buf, sizeof(buf), "%llu MiB",
            (unsigned long long)(total_stats->bytes_verified / NXMT_MIB_BYTES));
    }
    tui_kv("Tested", ANSI_YELLOW, "%s", buf);
    if (gpu_pumped_bytes > 0) {
        if (gpu_pumped_bytes >= 1024ull * NXMT_MIB_BYTES) {
            uint64_t gib_x100 = (gpu_pumped_bytes * 100ull) / (1024ull * NXMT_MIB_BYTES);
            snprintf(buf, sizeof(buf), "%llu.%02llu GiB",
                (unsigned long long)(gib_x100 / 100ull),
                (unsigned long long)(gib_x100 % 100ull));
        } else {
            snprintf(buf, sizeof(buf), "%llu MiB",
                (unsigned long long)(gpu_pumped_bytes / NXMT_MIB_BYTES));
        }
        tui_kv("GPU Pumped", ANSI_CYAN, "%s", buf);
        snprintf(buf, sizeof(buf), "%llu",
            (unsigned long long)gpu_pump_error_batches);
        tui_kv("GPU Verify Errors",
            gpu_pump_error_batches == 0 ? ANSI_GREEN : ANSI_RED,
            "%s batches", buf);
    }
    if (elapsed_ms > 0) {
        uint64_t total_bytes = total_stats->bytes_written + total_stats->bytes_verified + gpu_pumped_bytes;
        uint64_t mb_per_s = (total_bytes / 1000000ull) * 1000ull / elapsed_ms;
        if (mb_per_s >= 1000ull) {
            snprintf(buf, sizeof(buf), "%llu.%llu GB/s",
                (unsigned long long)(mb_per_s / 1000ull),
                (unsigned long long)((mb_per_s % 1000ull) / 100ull));
        } else {
            snprintf(buf, sizeof(buf), "%llu MB/s", (unsigned long long)mb_per_s);
        }
        tui_kv("Throughput", ANSI_YELLOW, "%s", buf);
    }
    format_elapsed(buf, sizeof(buf), elapsed_ms);
    tui_kv("Elapsed", ANSI_YELLOW, "%s", buf);
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)report->error_count);
    tui_kv("Errors", report->error_count == 0 ? ANSI_GREEN : ANSI_RED, "%s", buf);
    if (thread_fallback) {
        tui_kv("Thread Fallback", ANSI_YELLOW, "yes");
    }
    tui_kv("Log", log_ok ? ANSI_DIM : ANSI_RED, "%s", log_ok ? NXMT_LOG_PATH : "unavailable");
    (void)mode;
    tui_section_blank();
    tui_section_bottom();
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    nxmt_platform_debug_stage("main-entry");
    nxmt_platform_console_init();
    nxmt_platform_debug_stage("console-init");

    NxmtPlatformMemory memory;
    nxmt_platform_debug_stage("memory-query-start");
    nxmt_platform_get_memory(&memory);
    nxmt_platform_debug_stage("memory-query-done");

    tui_clear();
    tui_hide_cursor();
    draw_header();
    nxmt_platform_print("\n");
    nxmt_platform_debug_stage("banner-printed");

    if (!memory.has_heap_override) {
        tui_section_top("Memory");
        tui_section_blank();
        tui_kv("Override Heap", ANSI_RED, "missing");
        tui_section_blank();
        tui_section_bottom();
        nxmt_platform_print("\n");
        tui_section_top("Notice");
        tui_section_blank();
        tui_text_line(ANSI_RED, "%s", "No usable OverrideHeap was provided.");
        tui_text_line("", "%s", "Launch through hbmenu in title-override / full-memory mode.");
        tui_section_blank();
        tui_section_bottom();
        nxmt_platform_print("\n");
        draw_footer_hint("[PLUS] Exit");
        nxmt_platform_console_flush();
        while (appletMainLoop() && !nxmt_platform_read_input().plus) {
            svcSleepThread(33000000ll);
        }
        tui_show_cursor();
        nxmt_platform_console_exit();
        return 0;
    }

    NxmtArena arena = nxmt_arena_from_range(memory.override_heap_addr, memory.override_heap_size);
    nxmt_platform_debug_stage("arena-ready");
    draw_memory_section(&arena, &memory);
    nxmt_platform_print("\n");

    bool exit_app = false;
    while (!exit_app) {
    NxmtMode mode = NXMT_MODE_QUICK;
    uint64_t duration_seconds = 0;
    bool user_aborted = false;
    bool mode_selected = false;
    while (true) {
        nxmt_platform_debug_stage("mode-select-start");
        if (!tui_choose_mode(&arena, &memory, &mode)) {
            exit_app = true;
            break;
        }
        nxmt_platform_debug_stage("mode-selected");

        if (mode == NXMT_MODE_QUICK) {
            duration_seconds = 0;
            mode_selected = true;
            break;
        }
        int dr = tui_choose_duration(mode, &duration_seconds);
        if (dr == 0) {
            exit_app = true;
            break;
        }
        if (dr == 1) {
            mode_selected = true;
            break;
        }
        tui_clear();
        draw_header();
        nxmt_platform_print("\n");
        draw_memory_section(&arena, &memory);
        nxmt_platform_print("\n");
    }
    if (exit_app || !mode_selected) break;
    const uint64_t duration_ms = duration_seconds * 1000ull;

    uint64_t seed = nxmt_platform_seed64();
    int worker_cpu_map[NXMT_MAX_WORKERS];
    uint32_t workers = collect_worker_cpu_map(worker_cpu_map, NXMT_MAX_WORKERS);
    if (workers == 0u) {
        workers = 1u;
        worker_cpu_map[0] = -2; /* libnx default core */
    }

    /* Memory Load and Extreme are "max bus traffic" modes - in addition to
     * the CPU workers, carve a 256 MiB tail off the arena for a GPU copy
     * pump that runs concurrently and adds another stream of bus traffic.
     * The reserved region isn't tested by CPU this run; on the next pass
     * the CPU side rewrites everything anyway. Quick Check stays CPU-only.
     *
     * When the pump is active we also surrender the last CPU core for its
     * exclusive use so the pump thread isn't preempted between GPU
     * completions; otherwise wake-up latency dominates and GPU sits idle. */
    NxmtArena cpu_arena = arena;
    bool gpu_pump_active = false;
    uint64_t gpu_pump_region = 0;
    atomic_uint_fast64_t gpu_pump_bytes;
    atomic_uint_fast64_t gpu_pump_err_batches;
    atomic_init(&gpu_pump_bytes, 0u);
    atomic_init(&gpu_pump_err_batches, 0u);
    if ((mode == NXMT_MODE_MEMORY_LOAD || mode == NXMT_MODE_EXTREME)
        && arena.size >= 1024ull * NXMT_MIB_BYTES && workers >= 2u) {
        gpu_pump_region = 256ull * NXMT_MIB_BYTES;
        cpu_arena.size  = arena.size - gpu_pump_region;
        cpu_arena.words = cpu_arena.size / NXMT_WORD_BYTES;
        void *gpu_storage = arena.base + cpu_arena.size;
        atomic_init(&g_stop_requested, false);
        int pump_core = worker_cpu_map[workers - 1u]; /* steal the last worker's core */
        gpu_pump_active = nxmt_gpu_pump_start(
            gpu_storage, gpu_pump_region,
            64ull * NXMT_MIB_BYTES, pump_core,
            seed,
            &g_stop_requested, &gpu_pump_bytes, &gpu_pump_err_batches);
        if (gpu_pump_active) {
            workers -= 1u;
        } else {
            /* GPU pump failed; revert to full-arena CPU-only operation. */
            cpu_arena = arena;
            gpu_pump_region = 0;
        }
    }

    /* The GPU pump on its dedicated core spends most of its CPU time
     * blocked on dkQueueWaitIdle - the core sits at ~5-10% utilisation
     * even though every other core is hammering memory. Layer a 50%
     * duty-cycle pure-ALU filler on the same core in both Memory Load
     * and Extreme so the core also contributes useful CPU work. The
     * filler is ALU-only, so it doesn't compete with the memory workers
     * or the pump for DRAM bandwidth and bandwidth numbers stay clean. */
    bool cpu_filler_active = false;
    if ((mode == NXMT_MODE_MEMORY_LOAD || mode == NXMT_MODE_EXTREME)
        && gpu_pump_active) {
        int pump_core = worker_cpu_map[workers]; /* the just-stolen core */
        cpu_filler_active = nxmt_cpu_filler_start(pump_core, &g_stop_requested);
    }

    tui_clear();
    draw_header();
    nxmt_platform_print("\n");
    draw_memory_section(&arena, &memory);
    nxmt_platform_print("\n");
    draw_run_config_section(mode, workers, seed, duration_seconds);
    nxmt_platform_print("\n");

    /* Header (5 with inner padding) + blank + memory (9 with inner padding)
     * + blank + config (8 with inner padding) + blank = 25 rows of preamble,
     * so the progress frame top border lands on row 27. */
    const uint32_t progress_row_start = 27u;
    ProgressLayout progress_layout;
    draw_progress_frame(&progress_layout, progress_row_start);
    tui_goto(progress_layout.row_bottom + 2u, 1u);
    draw_footer_hint("[PLUS] Stop / Exit");
    nxmt_platform_console_flush();

    uint32_t completed_workers = 0;
    NxmtReport report;
    NxmtRunStats total_stats;
    nxmt_report_init(&report);
    memset(&total_stats, 0, sizeof(total_stats));

    nxmt_platform_debug_stage("run-start");
    uint64_t test_started = nxmt_platform_ticks_ms();
    NxmtStatus status = NXMT_STATUS_PASS;
    bool thread_fallback = false;
    uint64_t passes_completed = 0;
    atomic_init(&g_stop_requested, false);

    const char spinner_chars[] = {'|', '/', '-', '\\'};
    uint32_t phase_count = nxmt_runner_phase_count(mode);
    uint64_t pass_total_work = (uint64_t)phase_count * 2u * cpu_arena.size;
    if (pass_total_work == 0) {
        pass_total_work = 1;
    }
    uint64_t pass_index = 0;

    for (;;) {
        if (atomic_load_explicit(&g_stop_requested, memory_order_relaxed)) {
            break;
        }
        if (duration_ms > 0) {
            uint64_t test_elapsed = nxmt_platform_ticks_ms() - test_started;
            if (test_elapsed >= duration_ms) {
                break;
            }
        }

        atomic_init(&g_finished_workers, 0u);
        bool use_threaded_workers = true;
        uint32_t created_threads = 0;
        uint32_t started_threads = 0;

        for (uint32_t worker = 0; worker < workers; ++worker) {
            init_worker_context(&g_worker_contexts[worker], &cpu_arena, mode, seed, worker, workers, &g_stop_requested);
            g_worker_contexts[worker].config.pass = pass_index;
            atomic_init(&g_worker_progress[worker], 0u);
            g_worker_contexts[worker].config.progress_bytes = &g_worker_progress[worker];
            Result rc = threadCreate(
                &g_worker_threads[worker],
                nxmt_worker_entry,
                &g_worker_contexts[worker],
                g_worker_stacks[worker],
                sizeof(g_worker_stacks[worker]),
                0x2d,
                worker_cpu_map[worker]);
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
            break;
        }

        for (uint32_t worker = 0; worker < workers; ++worker) {
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
            break;
        }

        ProgressInfo info0;
        char header0[64];
        char info_text0[160];
        info0.spinner = spinner_chars[0];
        if (duration_ms > 0) {
            uint64_t test_elapsed = nxmt_platform_ticks_ms() - test_started;
            uint64_t mm_total = duration_seconds / 60u;
            uint64_t ss_total = duration_seconds % 60u;
            snprintf(header0, sizeof(header0), "%02llu:%02llu / %02llu:%02llu",
                (unsigned long long)((test_elapsed / 1000ull) / 60ull),
                (unsigned long long)((test_elapsed / 1000ull) % 60ull),
                (unsigned long long)mm_total,
                (unsigned long long)ss_total);
            info0.milli = nxmt_percent_milli(test_elapsed, duration_ms);
        } else {
            snprintf(header0, sizeof(header0), "0 / %llu MiB",
                (unsigned long long)(pass_total_work / NXMT_MIB_BYTES));
            info0.milli = 0;
        }
        info0.header_extra = header0;
        snprintf(info_text0, sizeof(info_text0),
            "Pass " ANSI_YELLOW "%llu" ANSI_RESET "    Errors " ANSI_YELLOW "%llu" ANSI_RESET,
            (unsigned long long)(pass_index + 1),
            (unsigned long long)report.error_count);
        info0.info_text = info_text0;
        redraw_progress_lines(&progress_layout, &info0);
        nxmt_platform_console_flush();

        uint64_t last_tick_ms = nxmt_platform_ticks_ms();
        uint32_t tick_idx = 1;
        while (atomic_load_explicit(&g_finished_workers, memory_order_acquire) < started_threads) {
            if (!appletMainLoop()) {
                atomic_store_explicit(&g_stop_requested, true, memory_order_relaxed);
                user_aborted = true;
                break;
            }
            if (nxmt_platform_read_input().plus) {
                atomic_store_explicit(&g_stop_requested, true, memory_order_relaxed);
                user_aborted = true;
                break;
            }
            if (duration_ms > 0) {
                uint64_t test_elapsed = nxmt_platform_ticks_ms() - test_started;
                if (test_elapsed >= duration_ms) {
                    atomic_store_explicit(&g_stop_requested, true, memory_order_relaxed);
                    break;
                }
            }
            uint64_t now = nxmt_platform_ticks_ms();
            if (now - last_tick_ms >= 200ull) {
                last_tick_ms = now;
                uint64_t pass_done = 0;
                for (uint32_t worker = 0; worker < started_threads; ++worker) {
                    pass_done += atomic_load_explicit(&g_worker_progress[worker], memory_order_relaxed);
                }
                if (pass_done > pass_total_work) {
                    pass_done = pass_total_work;
                }
                ProgressInfo info;
                char header[64];
                char info_text[160];
                info.spinner = spinner_chars[tick_idx & 3u];
                uint64_t test_elapsed = now - test_started;
                if (duration_ms > 0) {
                    uint64_t mm_total = duration_seconds / 60u;
                    uint64_t ss_total = duration_seconds % 60u;
                    snprintf(header, sizeof(header), "%02llu:%02llu / %02llu:%02llu",
                        (unsigned long long)((test_elapsed / 1000ull) / 60ull),
                        (unsigned long long)((test_elapsed / 1000ull) % 60ull),
                        (unsigned long long)mm_total,
                        (unsigned long long)ss_total);
                    info.milli = nxmt_percent_milli(test_elapsed, duration_ms);
                } else {
                    snprintf(header, sizeof(header), "%llu / %llu MiB",
                        (unsigned long long)(pass_done / NXMT_MIB_BYTES),
                        (unsigned long long)(pass_total_work / NXMT_MIB_BYTES));
                    info.milli = nxmt_percent_milli(pass_done, pass_total_work);
                }
                info.header_extra = header;
                uint64_t gpu_so_far = atomic_load_explicit(&gpu_pump_bytes, memory_order_relaxed);
                uint64_t cumulative_bytes = total_stats.bytes_written + total_stats.bytes_verified
                                          + pass_done + gpu_so_far;
                if (cumulative_bytes >= 1024ull * NXMT_MIB_BYTES) {
                    snprintf(info_text, sizeof(info_text),
                        "Pass " ANSI_YELLOW "%llu" ANSI_RESET
                        "    Bus " ANSI_YELLOW "%llu" ANSI_RESET " GiB"
                        "    GPU " ANSI_CYAN "%llu" ANSI_RESET " GiB"
                        "    Err " ANSI_YELLOW "%llu" ANSI_RESET,
                        (unsigned long long)(pass_index + 1),
                        (unsigned long long)(cumulative_bytes / (1024ull * NXMT_MIB_BYTES)),
                        (unsigned long long)(gpu_so_far / (1024ull * NXMT_MIB_BYTES)),
                        (unsigned long long)report.error_count);
                } else {
                    snprintf(info_text, sizeof(info_text),
                        "Pass " ANSI_YELLOW "%llu" ANSI_RESET
                        "    Bus " ANSI_YELLOW "%llu" ANSI_RESET " MiB"
                        "    GPU " ANSI_CYAN "%llu" ANSI_RESET " MiB"
                        "    Err " ANSI_YELLOW "%llu" ANSI_RESET,
                        (unsigned long long)(pass_index + 1),
                        (unsigned long long)(cumulative_bytes / NXMT_MIB_BYTES),
                        (unsigned long long)(gpu_so_far / NXMT_MIB_BYTES),
                        (unsigned long long)report.error_count);
                }
                info.info_text = info_text;
                redraw_progress_lines(&progress_layout, &info);
                nxmt_platform_console_flush();
                tick_idx++;
            }
            svcSleepThread(16000000ll);
        }

        for (uint32_t worker = 0; worker < started_threads; ++worker) {
            threadWaitForExit(&g_worker_threads[worker]);
        }
        for (uint32_t worker = 0; worker < created_threads; ++worker) {
            threadClose(&g_worker_threads[worker]);
        }
        for (uint32_t worker = 0; worker < started_threads; ++worker) {
            merge_worker_context(&g_worker_contexts[worker], &report, &total_stats, &status, &completed_workers);
        }
        passes_completed += 1u;

        if (status == NXMT_STATUS_FAIL) {
            break;
        }
        if (atomic_load_explicit(&g_stop_requested, memory_order_relaxed)) {
            break;
        }
        if (duration_ms == 0) {
            break;
        }
        pass_index += 1u;
    }

    if (thread_fallback) {
        NxmtWorkerContext context;
        atomic_store_explicit(&g_stop_requested, false, memory_order_relaxed);
        init_worker_context(&context, &cpu_arena, mode, seed, 0, 1u, &g_stop_requested);
        context.config.pass = pass_index;
        nxmt_report_init(&context.report);
        memset(&context.stats, 0, sizeof(context.stats));

        tui_goto(progress_layout.row_line, 1u);
        tui_text_line(ANSI_YELLOW, "%s", "Thread setup failed; running single-worker fallback.");
        nxmt_platform_console_flush();
        context.status = nxmt_runner_run_pass(&cpu_arena, &context.config, &context.report, &context.stats);
        merge_worker_context(&context, &report, &total_stats, &status, &completed_workers);
        passes_completed += 1u;
    }

    /* Make sure the GPU pump observes stop_requested = true and drains its
     * in-flight commands before we tally bytes pumped. */
    atomic_store_explicit(&g_stop_requested, true, memory_order_relaxed);
    if (cpu_filler_active) {
        nxmt_cpu_filler_stop();
    }
    if (gpu_pump_active) {
        nxmt_gpu_pump_stop();
    }
    uint64_t gpu_pumped = atomic_load(&gpu_pump_bytes);
    uint64_t gpu_err_batches = atomic_load(&gpu_pump_err_batches);
    if (gpu_err_batches > 0u) {
        /* GPU verify spotted mismatched bytes - escalate so the result
         * label reflects that the test caught something even if the CPU
         * side ran clean. */
        if (status != NXMT_STATUS_FAIL) {
            status = NXMT_STATUS_FAIL;
        }
    }

    /* The runner reports ABORTED whenever stop_requested triggered mid-pass,
     * but if we ended naturally on the duration timer (not a user / system
     * abort) and saw no mismatches, the run actually completed - promote to
     * PASS so the result screen says so instead of "ABORTED". */
    if (!user_aborted && status == NXMT_STATUS_ABORTED && report.error_count == 0) {
        status = NXMT_STATUS_PASS;
    }

    uint64_t elapsed = nxmt_platform_ticks_ms() - test_started;

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

    tui_clear();
    draw_header();
    nxmt_platform_print("\n");
    draw_memory_section(&arena, &memory);
    nxmt_platform_print("\n");
    draw_run_config_section(mode, workers, seed, duration_seconds);
    nxmt_platform_print("\n");
    draw_results_section(mode, workers, completed_workers, passes_completed, &arena, &total_stats, &report, status,
        thread_fallback, elapsed, gpu_pumped, gpu_err_batches, log_ok);
    nxmt_platform_print("\n");
    draw_footer_hint("[B] Back to menu    [PLUS] Exit");
    nxmt_platform_console_flush();

    while (appletMainLoop()) {
        NxmtInput in = nxmt_platform_read_input();
        if (in.plus) { exit_app = true; break; }
        if (in.b)    { break; }
        svcSleepThread(33000000ll);
    }
    if (exit_app) break;

    /* Redraw the persistent preamble before looping back to the mode menu. */
    tui_clear();
    draw_header();
    nxmt_platform_print("\n");
    draw_memory_section(&arena, &memory);
    nxmt_platform_print("\n");
    }

    tui_show_cursor();
    nxmt_platform_console_exit();
    return 0;
}
