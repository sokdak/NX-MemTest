#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>
#include "nxmt/platform.h"

static PadState g_pad;
static bool g_pad_initialized;
static void *g_test_arena_addr;
static uint64_t g_test_arena_size;

static void nxmt_platform_pad_init(void) {
    if (!g_pad_initialized) {
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&g_pad);
        g_pad_initialized = true;
    }
}

static bool align_up_uintptr(uintptr_t value, uint64_t align, uintptr_t *out) {
    uint64_t mask = align - 1u;
    if (value > UINTPTR_MAX - mask) {
        return false;
    }
    *out = (value + mask) & ~(uintptr_t)mask;
    return true;
}

void __libnx_initheap(void) {
    extern char *fake_heap_start;
    extern char *fake_heap_end;
    fake_heap_start = NULL;
    fake_heap_end = NULL;
    g_test_arena_addr = NULL;
    g_test_arena_size = 0;

    if (envHasHeapOverride()) {
        uintptr_t raw_start = (uintptr_t)envGetHeapOverrideAddr();
        uint64_t raw_size = envGetHeapOverrideSize();
        uintptr_t aligned_start = 0;
        if (align_up_uintptr(raw_start, NXMT_PAGE_BYTES, &aligned_start)) {
            uint64_t prefix = (uint64_t)(aligned_start - raw_start);
            if (raw_size > prefix) {
                uint64_t usable_size = (raw_size - prefix) & ~(uint64_t)(NXMT_PAGE_BYTES - 1u);
                if (usable_size != 0) {
                    uint64_t reserve = nxmt_runtime_heap_reserve(usable_size) & ~(uint64_t)(NXMT_PAGE_BYTES - 1u);
                    if (reserve != 0 && usable_size > reserve) {
                        fake_heap_start = (char*)aligned_start;
                        fake_heap_end = (char*)aligned_start + reserve;
                        g_test_arena_addr = (void*)(aligned_start + reserve);
                        g_test_arena_size = usable_size - reserve;
                    } else {
                        fake_heap_start = (char*)aligned_start;
                        fake_heap_end = (char*)aligned_start + usable_size;
                    }
                    return;
                }
            }
        }
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
    }

    void *heap = NULL;
    uint64_t heap_size = 32ull * NXMT_MIB_BYTES;
    Result rc = svcSetHeapSize(&heap, heap_size);
    if (R_SUCCEEDED(rc) && heap != NULL) {
        fake_heap_start = (char*)heap;
        fake_heap_end = (char*)heap + heap_size;
        return;
    }
    diagAbortWithResult(R_FAILED(rc) ? rc : MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
}

void nxmt_platform_get_memory(NxmtPlatformMemory *out) {
    memset(out, 0, sizeof(*out));
    bool override_present = envHasHeapOverride();
    bool arena_usable = override_present && g_test_arena_addr != NULL && g_test_arena_size != 0;
    out->has_heap_override = arena_usable;
    uint64_t raw_override_heap_size = override_present ? envGetHeapOverrideSize() : 0;
    if (arena_usable) {
        out->override_heap_addr = g_test_arena_addr;
        out->override_heap_size = g_test_arena_size;
    }

    // HBL already communicates full-memory availability through OverrideHeap.
    // Keep startup away from extra memory-info syscalls so loader compatibility
    // problems do not abort before the UI can render.
    NxmtMemorySelection selected = nxmt_select_launch_safe_memory_total(override_present, raw_override_heap_size);
    out->effective_total_memory = selected.total;
    out->effective_total_source = selected.source;
    out->extended_memory_detected = selected.extended_memory_detected;
    out->has_effective_total = selected.source != NXMT_MEMORY_SOURCE_NONE;
    out->switch_total_memory = selected.total;
    out->has_switch_total = out->has_effective_total;
}

uint64_t nxmt_platform_core_mask(void) {
    uint64_t mask = 0;
    Result rc = svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    if (R_FAILED(rc) || mask == 0) {
        /* Conservative fallback: cores 0..2 are always available to NRO
         * homebrew under hbloader; core 3 is what we'd be testing for. */
        return 0x7ull;
    }
    return mask;
}

uint64_t nxmt_platform_seed64(void) {
    uint64_t seed_pair[2] = {0, 0};
    if (envHasRandomSeed()) {
        envGetRandomSeed(seed_pair);
        return seed_pair[0] ^ seed_pair[1];
    }
    return armGetSystemTick();
}

uint64_t nxmt_platform_ticks_ms(void) {
    return armGetSystemTick() / 19200ull;
}

void nxmt_platform_console_init(void) {
    consoleInit(NULL);
    nxmt_platform_pad_init();
}

void nxmt_platform_console_exit(void) {
    consoleExit(NULL);
}

void nxmt_platform_print(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

void nxmt_platform_console_flush(void) {
    consoleUpdate(NULL);
}

void nxmt_platform_debug_stage(const char *stage) {
    mkdir("sdmc:/switch/NX-MemTest", 0777);
    mkdir("sdmc:/switch/NX-MemTest/logs", 0777);

    FILE *stdio_file = fopen("sdmc:/switch/NX-MemTest/logs/boot_stage.txt", "wb");
    if (stdio_file != NULL) {
        fprintf(stdio_file, "stage=%s\n", stage);
        fclose(stdio_file);
        return;
    }

    FsFileSystem fs;
    if (R_FAILED(fsOpenSdCardFileSystem(&fs))) {
        return;
    }

    fsFsCreateDirectory(&fs, "/switch/NX-MemTest");
    fsFsCreateDirectory(&fs, "/switch/NX-MemTest/logs");
    fsFsCreateFile(&fs, "/switch/NX-MemTest/logs/boot_stage.txt", 512, 0);

    FsFile f;
    if (R_SUCCEEDED(fsFsOpenFile(&fs, "/switch/NX-MemTest/logs/boot_stage.txt", FsOpenMode_Write, &f))) {
        char text[512];
        memset(text, 0, sizeof(text));
        snprintf(text, sizeof(text) - 1, "stage=%s\n", stage);
        fsFileWrite(&f, 0, text, sizeof(text), FsWriteOption_Flush);
        fsFileClose(&f);
    }

    fsFsClose(&fs);
}

NxmtInput nxmt_platform_read_input(void) {
    nxmt_platform_pad_init();
    padUpdate(&g_pad);
    uint64_t down = padGetButtonsDown(&g_pad);

    NxmtInput input;
    input.a = (down & HidNpadButton_A) != 0;
    input.b = (down & HidNpadButton_B) != 0;
    input.x = (down & HidNpadButton_X) != 0;
    input.y = (down & HidNpadButton_Y) != 0;
    input.up = (down & (HidNpadButton_Up | HidNpadButton_StickLUp)) != 0;
    input.down = (down & (HidNpadButton_Down | HidNpadButton_StickLDown)) != 0;
    input.plus = (down & HidNpadButton_Plus) != 0;
    return input;
}

bool nxmt_platform_should_quit(void) {
    return nxmt_platform_read_input().plus;
}

bool nxmt_platform_write_report(const char *text) {
    mkdir("sdmc:/switch/NX-MemTest", 0777);
    mkdir("sdmc:/switch/NX-MemTest/logs", 0777);

    FILE *f = fopen("sdmc:/switch/NX-MemTest/logs/latest.txt", "wb");
    if (!f) {
        return false;
    }
    size_t len = strlen(text);
    bool ok = fwrite(text, 1, len, f) == len;
    fclose(f);
    return ok;
}
