#include "gpu_bench.h"

#include <stdio.h>
#include <string.h>
#include <switch.h>
#include <deko3d.h>

#include "nxmt/platform.h"
#include "nxmt/types.h"

/* Tiny command-buffer recording region. One CopyBuffer record is a handful
 * of words so 64 KiB is generous. */
#define NXMT_GPU_CMD_MEM_BYTES (64u * 1024u)

/* deko3d's default error path is fatalThrow, which kills the process before
 * the caller sees a NULL handle. Capture errors via cbDebug and let the run
 * abort cleanly so the menu can redraw. */
static volatile int g_gpu_error_seen = 0;

static void nxmt_gpu_debug_cb(void* userData, const char* context,
                              DkResult result, const char* message) {
    (void)userData;
    g_gpu_error_seen = 1;
    char stage[128];
    snprintf(stage, sizeof(stage), "gpu-err:%s:%d:%s",
        context ? context : "?", (int)result, message ? message : "");
    nxmt_platform_debug_stage(stage);
    nxmt_platform_print("GPU bench: %s in %s (result=%d)\n",
        message ? message : "<no message>", context ? context : "?", (int)result);
    nxmt_platform_console_flush();
}

bool nxmt_gpu_bench_run(void *storage_base, uint64_t storage_size,
                        uint64_t buffer_size, uint32_t iterations) {
    if (storage_base == NULL || buffer_size == 0u || iterations == 0u) {
        return false;
    }
    /* deko3d MemBlock storage must be page-aligned and a multiple of the
     * page size. The arena base is already page-aligned; round buffer size
     * up to the next page so trailing bytes can't underflow the cmd block. */
    uint64_t buffer_size_aligned = (buffer_size + NXMT_PAGE_BYTES - 1u)
                                   & ~(uint64_t)(NXMT_PAGE_BYTES - 1u);
    uint64_t required = (uint64_t)NXMT_GPU_CMD_MEM_BYTES + 2u * buffer_size_aligned;
    if (storage_size < required) {
        nxmt_platform_print("GPU bench: storage too small "
                            "(need %llu MiB, have %llu MiB)\n",
            (unsigned long long)(required / NXMT_MIB_BYTES),
            (unsigned long long)(storage_size / NXMT_MIB_BYTES));
        return false;
    }

    uint8_t *bp = (uint8_t*)storage_base;
    void *cmd_storage = bp;
    void *src_storage = bp + NXMT_GPU_CMD_MEM_BYTES;
    void *dst_storage = bp + NXMT_GPU_CMD_MEM_BYTES + buffer_size_aligned;

    g_gpu_error_seen = 0;
    nxmt_platform_debug_stage("gpu-bench-enter");

    DkDeviceMaker dev_maker;
    dkDeviceMakerDefaults(&dev_maker);
    dev_maker.cbDebug = nxmt_gpu_debug_cb;
    nxmt_platform_debug_stage("gpu-device-create");
    DkDevice device = dkDeviceCreate(&dev_maker);
    if (device == NULL || g_gpu_error_seen) {
        nxmt_platform_print("GPU bench: dkDeviceCreate failed\n");
        if (device) dkDeviceDestroy(device);
        return false;
    }

    /* Cmd memblock - tiny, but still routed through our storage so deko3d
     * doesn't have to find headroom in the kernel pool. */
    DkMemBlockMaker cmd_mem_maker;
    dkMemBlockMakerDefaults(&cmd_mem_maker, device, NXMT_GPU_CMD_MEM_BYTES);
    cmd_mem_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    cmd_mem_maker.storage = cmd_storage;
    nxmt_platform_debug_stage("gpu-cmd-mem");
    DkMemBlock cmd_mem = dkMemBlockCreate(&cmd_mem_maker);

    DkCmdBufMaker cmd_maker;
    dkCmdBufMakerDefaults(&cmd_maker, device);
    nxmt_platform_debug_stage("gpu-cmdbuf");
    DkCmdBuf cmdbuf = dkCmdBufCreate(&cmd_maker);

    /* Src/dst memblocks wrap arena slices. CpuCached keeps the post-run
     * memcmp fast; we explicitly flush before submit and after wait_idle
     * so coherency with the GPU's view is correct. */
    DkMemBlockMaker buf_maker;
    dkMemBlockMakerDefaults(&buf_maker, device, (uint32_t)buffer_size_aligned);
    buf_maker.flags = DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached;
    buf_maker.storage = src_storage;
    nxmt_platform_debug_stage("gpu-src-mem");
    DkMemBlock src = dkMemBlockCreate(&buf_maker);

    buf_maker.storage = dst_storage;
    nxmt_platform_debug_stage("gpu-dst-mem");
    DkMemBlock dst = dkMemBlockCreate(&buf_maker);

    if (cmd_mem == NULL || cmdbuf == NULL || src == NULL || dst == NULL || g_gpu_error_seen) {
        nxmt_platform_print("GPU bench: memblock alloc failed (size=%llu MiB)\n",
            (unsigned long long)(buffer_size / NXMT_MIB_BYTES));
        if (dst) dkMemBlockDestroy(dst);
        if (src) dkMemBlockDestroy(src);
        if (cmdbuf) dkCmdBufDestroy(cmdbuf);
        if (cmd_mem) dkMemBlockDestroy(cmd_mem);
        dkDeviceDestroy(device);
        return false;
    }

    dkCmdBufAddMemory(cmdbuf, cmd_mem, 0, NXMT_GPU_CMD_MEM_BYTES);

    /* Seed the source via storage pointers; we already know the CPU VA
     * because we chose it. dkMemBlockGetCpuAddr would return the same
     * pointer for storage-backed blocks. Flush so the GPU read sees the
     * latest bytes rather than stale cache content. */
    memset(src_storage, 0xa5, buffer_size_aligned);
    memset(dst_storage, 0x00, buffer_size_aligned);
    dkMemBlockFlushCpuCache(src, 0, (uint32_t)buffer_size_aligned);
    dkMemBlockFlushCpuCache(dst, 0, (uint32_t)buffer_size_aligned);

    DkGpuAddr src_gpu = dkMemBlockGetGpuAddr(src);
    DkGpuAddr dst_gpu = dkMemBlockGetGpuAddr(dst);

    dkCmdBufCopyBuffer(cmdbuf, src_gpu, dst_gpu, (uint32_t)buffer_size_aligned);
    DkCmdList list = dkCmdBufFinishList(cmdbuf);

    DkQueueMaker q_maker;
    dkQueueMakerDefaults(&q_maker, device);
    nxmt_platform_debug_stage("gpu-queue");
    DkQueue queue = dkQueueCreate(&q_maker);
    if (queue == NULL || g_gpu_error_seen) {
        nxmt_platform_print("GPU bench: dkQueueCreate failed\n");
        dkMemBlockDestroy(dst);
        dkMemBlockDestroy(src);
        dkCmdBufDestroy(cmdbuf);
        dkMemBlockDestroy(cmd_mem);
        dkDeviceDestroy(device);
        return false;
    }

    nxmt_platform_debug_stage("gpu-submit-loop");
    uint64_t start_ms = nxmt_platform_ticks_ms();
    for (uint32_t i = 0; i < iterations; ++i) {
        dkQueueSubmitCommands(queue, list);
    }
    nxmt_platform_debug_stage("gpu-wait-idle");
    dkQueueWaitIdle(queue);
    uint64_t elapsed_ms = nxmt_platform_ticks_ms() - start_ms;
    nxmt_platform_debug_stage("gpu-done");

    /* Invalidate the CPU cache view of dst so memcmp reads what the GPU
     * actually put in DRAM. armDCacheFlush does clean + invalidate; clean
     * is harmless here since we only wrote dst on CPU as a sentinel pattern. */
    armDCacheFlush(dst_storage, buffer_size_aligned);

    bool verify_ok = (memcmp(src_storage, dst_storage, buffer_size_aligned) == 0);

    uint64_t per_iter_traffic = buffer_size_aligned * 2ull;
    uint64_t total_traffic = per_iter_traffic * iterations;
    uint64_t total_copy = buffer_size_aligned * iterations;
    uint64_t throughput_mbps = (elapsed_ms > 0)
        ? (total_traffic / 1000000ull) * 1000ull / elapsed_ms
        : 0;
    uint64_t copy_mbps = (elapsed_ms > 0)
        ? (total_copy / 1000000ull) * 1000ull / elapsed_ms
        : 0;

    nxmt_platform_print("GPU bench: %llu MiB x %u iters in %llu ms\n",
        (unsigned long long)(buffer_size_aligned / NXMT_MIB_BYTES),
        iterations,
        (unsigned long long)elapsed_ms);
    nxmt_platform_print("  copy rate     ~%llu MB/s\n", (unsigned long long)copy_mbps);
    nxmt_platform_print("  bus traffic   ~%llu MB/s  (read+write)\n",
        (unsigned long long)throughput_mbps);
    nxmt_platform_print("  verify        %s\n", verify_ok ? "ok" : "FAIL");

    dkQueueDestroy(queue);
    dkMemBlockDestroy(dst);
    dkMemBlockDestroy(src);
    dkCmdBufDestroy(cmdbuf);
    dkMemBlockDestroy(cmd_mem);
    dkDeviceDestroy(device);

    return verify_ok;
}
