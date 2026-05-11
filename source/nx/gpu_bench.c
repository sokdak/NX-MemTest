#include "gpu_bench.h"

#include <stdio.h>
#include <string.h>
#include <switch.h>
#include <deko3d.h>

#include "nxmt/platform.h"
#include "nxmt/types.h"

/* Block sizes the copy engine handles in one record. The cmd memblock holds
 * the recorded GPU commands; one CopyBuffer record is tiny so 64 KiB is
 * generous. */
#define NXMT_GPU_CMD_MEM_BYTES (64u * 1024u)

/* deko3d default error handling is to fatalThrow on any internal failure,
 * which kills the process. Install a callback that surfaces the error via
 * the boot_stage marker and console and let the run abort gracefully. */
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

bool nxmt_gpu_bench_run(uint64_t buffer_size, uint32_t iterations) {
    if (buffer_size == 0u || iterations == 0u) {
        return false;
    }

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

    /* Command buffer recording memory. */
    DkMemBlockMaker cmd_mem_maker;
    dkMemBlockMakerDefaults(&cmd_mem_maker, device, NXMT_GPU_CMD_MEM_BYTES);
    cmd_mem_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    nxmt_platform_debug_stage("gpu-cmd-mem");
    DkMemBlock cmd_mem = dkMemBlockCreate(&cmd_mem_maker);

    DkCmdBufMaker cmd_maker;
    dkCmdBufMakerDefaults(&cmd_maker, device);
    nxmt_platform_debug_stage("gpu-cmdbuf");
    DkCmdBuf cmdbuf = dkCmdBufCreate(&cmd_maker);

    /* Source and destination buffers. CpuCached on the host side so the
     * post-run memcmp doesn't pay an uncached read penalty. */
    DkMemBlockMaker buf_maker;
    dkMemBlockMakerDefaults(&buf_maker, device, (uint32_t)buffer_size);
    buf_maker.flags = DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached;
    nxmt_platform_debug_stage("gpu-src-mem");
    DkMemBlock src = dkMemBlockCreate(&buf_maker);
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

    /* Fill source with a deterministic pattern and flush so the GPU side
     * actually reads the new bytes, not stale cache content. */
    void *src_cpu = dkMemBlockGetCpuAddr(src);
    void *dst_cpu = dkMemBlockGetCpuAddr(dst);
    if (src_cpu != NULL) {
        memset(src_cpu, 0xa5, buffer_size);
        dkMemBlockFlushCpuCache(src, 0, (uint32_t)buffer_size);
    }
    if (dst_cpu != NULL) {
        memset(dst_cpu, 0x00, buffer_size);
        dkMemBlockFlushCpuCache(dst, 0, (uint32_t)buffer_size);
    }

    DkGpuAddr src_gpu = dkMemBlockGetGpuAddr(src);
    DkGpuAddr dst_gpu = dkMemBlockGetGpuAddr(dst);

    /* Record one buffer-to-buffer copy; the cmd list can be submitted
     * repeatedly to measure sustained copy-engine throughput. */
    dkCmdBufCopyBuffer(cmdbuf, src_gpu, dst_gpu, (uint32_t)buffer_size);
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

    bool verify_ok = false;
    if (src_cpu != NULL && dst_cpu != NULL) {
        verify_ok = (memcmp(src_cpu, dst_cpu, buffer_size) == 0);
    }

    /* The copy moves `buffer_size` bytes per iteration but touches DRAM
     * twice per byte (read src, write dst), so the effective memory-system
     * traffic is 2x. Report both numbers. */
    uint64_t per_iter_traffic = buffer_size * 2ull;
    uint64_t total_traffic = per_iter_traffic * iterations;
    uint64_t total_copy = buffer_size * iterations;
    uint64_t throughput_mbps = (elapsed_ms > 0)
        ? (total_traffic / 1000000ull) * 1000ull / elapsed_ms
        : 0;
    uint64_t copy_mbps = (elapsed_ms > 0)
        ? (total_copy / 1000000ull) * 1000ull / elapsed_ms
        : 0;

    nxmt_platform_print("GPU bench: %llu MiB x %u iters in %llu ms\n",
        (unsigned long long)(buffer_size / NXMT_MIB_BYTES),
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
