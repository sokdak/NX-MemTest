#include "gpu_pump.h"

#include <stdio.h>
#include <string.h>
#include <switch.h>
#include <deko3d.h>

#include "nxmt/platform.h"
#include "nxmt/types.h"

#define NXMT_GPU_PUMP_CMD_BYTES (64u * 1024u)

static Thread g_pump_thread;
static unsigned char g_pump_stack[64 * 1024] __attribute__((aligned(0x1000)));

static atomic_bool g_pump_init_done;
static atomic_bool g_pump_init_failed;
static volatile int g_pump_error_seen;

static void *g_pump_storage_base;
static uint64_t g_pump_buffer_bytes;
static atomic_bool *g_pump_stop;
static atomic_uint_fast64_t *g_pump_progress;

static DkDevice    g_pump_device;
static DkMemBlock  g_pump_cmd_mem;
static DkCmdBuf    g_pump_cmdbuf;
static DkMemBlock  g_pump_src;
static DkMemBlock  g_pump_dst;
static DkCmdList   g_pump_list;
static DkQueue     g_pump_queue;

static void pump_debug_cb(void* userData, const char* context,
                          DkResult result, const char* message) {
    (void)userData;
    g_pump_error_seen = 1;
    char stage[128];
    snprintf(stage, sizeof(stage), "gpu-pump-err:%s:%d:%s",
        context ? context : "?", (int)result, message ? message : "");
    nxmt_platform_debug_stage(stage);
}

static bool pump_setup(uint64_t buffer_aligned) {
    DkDeviceMaker dev_maker;
    dkDeviceMakerDefaults(&dev_maker);
    dev_maker.cbDebug = pump_debug_cb;
    g_pump_device = dkDeviceCreate(&dev_maker);
    if (g_pump_device == NULL || g_pump_error_seen) return false;

    uint8_t *bp = (uint8_t*)g_pump_storage_base;
    void *cmd_storage = bp;
    void *src_storage = bp + NXMT_GPU_PUMP_CMD_BYTES;
    void *dst_storage = bp + NXMT_GPU_PUMP_CMD_BYTES + buffer_aligned;

    DkMemBlockMaker mm;
    dkMemBlockMakerDefaults(&mm, g_pump_device, NXMT_GPU_PUMP_CMD_BYTES);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    mm.storage = cmd_storage;
    g_pump_cmd_mem = dkMemBlockCreate(&mm);

    DkCmdBufMaker cbm;
    dkCmdBufMakerDefaults(&cbm, g_pump_device);
    g_pump_cmdbuf = dkCmdBufCreate(&cbm);

    DkMemBlockMaker bm;
    dkMemBlockMakerDefaults(&bm, g_pump_device, (uint32_t)buffer_aligned);
    bm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    bm.storage = src_storage;
    g_pump_src = dkMemBlockCreate(&bm);
    bm.storage = dst_storage;
    g_pump_dst = dkMemBlockCreate(&bm);

    if (g_pump_cmd_mem == NULL || g_pump_cmdbuf == NULL
        || g_pump_src == NULL || g_pump_dst == NULL || g_pump_error_seen) {
        return false;
    }

    dkCmdBufAddMemory(g_pump_cmdbuf, g_pump_cmd_mem, 0, NXMT_GPU_PUMP_CMD_BYTES);
    dkCmdBufCopyBuffer(g_pump_cmdbuf,
        dkMemBlockGetGpuAddr(g_pump_src),
        dkMemBlockGetGpuAddr(g_pump_dst),
        (uint32_t)buffer_aligned);
    g_pump_list = dkCmdBufFinishList(g_pump_cmdbuf);

    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, g_pump_device);
    g_pump_queue = dkQueueCreate(&qm);
    if (g_pump_queue == NULL || g_pump_error_seen) return false;

    return true;
}

static void pump_teardown(void) {
    if (g_pump_queue)   { dkQueueWaitIdle(g_pump_queue); dkQueueDestroy(g_pump_queue); g_pump_queue = NULL; }
    if (g_pump_dst)     { dkMemBlockDestroy(g_pump_dst);     g_pump_dst = NULL; }
    if (g_pump_src)     { dkMemBlockDestroy(g_pump_src);     g_pump_src = NULL; }
    if (g_pump_cmdbuf)  { dkCmdBufDestroy(g_pump_cmdbuf);    g_pump_cmdbuf = NULL; }
    if (g_pump_cmd_mem) { dkMemBlockDestroy(g_pump_cmd_mem); g_pump_cmd_mem = NULL; }
    if (g_pump_device)  { dkDeviceDestroy(g_pump_device);    g_pump_device = NULL; }
}

static void pump_thread_entry(void *arg) {
    (void)arg;
    uint64_t buffer_aligned = (g_pump_buffer_bytes + NXMT_PAGE_BYTES - 1u)
                              & ~(uint64_t)(NXMT_PAGE_BYTES - 1u);
    nxmt_platform_debug_stage("gpu-pump-setup");
    if (!pump_setup(buffer_aligned)) {
        pump_teardown();
        atomic_store(&g_pump_init_failed, true);
        atomic_store(&g_pump_init_done, true);
        return;
    }
    atomic_store(&g_pump_init_done, true);

    const uint64_t per_iter_traffic = buffer_aligned * 2ull;
    nxmt_platform_debug_stage("gpu-pump-loop");
    while (!atomic_load_explicit(g_pump_stop, memory_order_relaxed) && !g_pump_error_seen) {
        dkQueueSubmitCommands(g_pump_queue, g_pump_list);
        dkQueueWaitIdle(g_pump_queue);
        if (g_pump_progress != NULL) {
            atomic_fetch_add_explicit(g_pump_progress, per_iter_traffic, memory_order_relaxed);
        }
    }

    nxmt_platform_debug_stage("gpu-pump-teardown");
    pump_teardown();
}

bool nxmt_gpu_pump_start(void *storage_base, uint64_t storage_size,
                         uint64_t buffer_size,
                         atomic_bool *stop_requested,
                         atomic_uint_fast64_t *progress_bytes) {
    if (storage_base == NULL || stop_requested == NULL || buffer_size == 0u) {
        return false;
    }
    uint64_t buffer_aligned = (buffer_size + NXMT_PAGE_BYTES - 1u)
                              & ~(uint64_t)(NXMT_PAGE_BYTES - 1u);
    uint64_t required = (uint64_t)NXMT_GPU_PUMP_CMD_BYTES + 2u * buffer_aligned;
    if (storage_size < required) return false;

    g_pump_storage_base = storage_base;
    g_pump_buffer_bytes = buffer_aligned;
    g_pump_stop = stop_requested;
    g_pump_progress = progress_bytes;
    atomic_store(&g_pump_init_done, false);
    atomic_store(&g_pump_init_failed, false);
    g_pump_error_seen = 0;

    Result rc = threadCreate(&g_pump_thread, pump_thread_entry, NULL,
        g_pump_stack, sizeof(g_pump_stack), 0x2d, -2);
    if (R_FAILED(rc)) return false;
    rc = threadStart(&g_pump_thread);
    if (R_FAILED(rc)) {
        threadClose(&g_pump_thread);
        return false;
    }

    /* Wait up to ~500 ms for init to complete one way or the other; deko3d
     * device / queue creation is fast in practice but bounded so a hung
     * driver doesn't freeze the menu. */
    for (int i = 0; i < 50; ++i) {
        if (atomic_load(&g_pump_init_done)) break;
        svcSleepThread(10000000ll);
    }
    if (atomic_load(&g_pump_init_failed)) {
        threadWaitForExit(&g_pump_thread);
        threadClose(&g_pump_thread);
        return false;
    }
    return true;
}

void nxmt_gpu_pump_stop(void) {
    threadWaitForExit(&g_pump_thread);
    threadClose(&g_pump_thread);
}
