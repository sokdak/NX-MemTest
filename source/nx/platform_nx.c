#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>
#include "nxmt/platform.h"

static unsigned char g_internal_heap[4 * 1024 * 1024];
static PadState g_pad;
static bool g_pad_initialized;

static void nxmt_platform_pad_init(void) {
    if (!g_pad_initialized) {
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&g_pad);
        g_pad_initialized = true;
    }
}

void __libnx_initheap(void) {
    extern char *fake_heap_start;
    extern char *fake_heap_end;
    fake_heap_start = (char*)g_internal_heap;
    fake_heap_end = (char*)g_internal_heap + sizeof(g_internal_heap);
}

void nxmt_platform_get_memory(NxmtPlatformMemory *out) {
    memset(out, 0, sizeof(*out));
    out->has_heap_override = envHasHeapOverride();
    if (out->has_heap_override) {
        out->override_heap_addr = envGetHeapOverrideAddr();
        out->override_heap_size = envGetHeapOverrideSize();
    }

    uint64_t total = 0;
    uint64_t pool = 0;
    Result rc0 = svcGetSystemInfo(&pool, SystemInfoType_TotalPhysicalMemorySize, INVALID_HANDLE, PhysicalMemorySystemInfo_Application);
    if (R_SUCCEEDED(rc0)) {
        total += pool;
        svcGetSystemInfo(&pool, SystemInfoType_TotalPhysicalMemorySize, INVALID_HANDLE, PhysicalMemorySystemInfo_Applet);
        total += pool;
        svcGetSystemInfo(&pool, SystemInfoType_TotalPhysicalMemorySize, INVALID_HANDLE, PhysicalMemorySystemInfo_System);
        total += pool;
        svcGetSystemInfo(&pool, SystemInfoType_TotalPhysicalMemorySize, INVALID_HANDLE, PhysicalMemorySystemInfo_SystemUnsafe);
        total += pool;
        out->switch_total_memory = total;
        out->has_switch_total = total != 0;
    }
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
    consoleUpdate(NULL);
}

bool nxmt_platform_should_quit(void) {
    nxmt_platform_pad_init();
    padUpdate(&g_pad);
    uint64_t down = padGetButtonsDown(&g_pad);
    return (down & HidNpadButton_Plus) != 0;
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
