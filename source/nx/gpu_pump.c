#include "gpu_pump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>
#include <deko3d.h>

#include "nxmt/platform.h"
#include "nxmt/types.h"

/* Appending log for pump init diagnosis. boot_stage.txt gets overwritten by
 * main's run-start marker before pump init completes, hiding any earlier
 * pump-side failure. This file accumulates a trail so we can see which step
 * died after the fact. */
#define NXMT_GPU_PUMP_LOG_PATH "sdmc:/switch/NX-MemTest/logs/gpu_pump.txt"

static void pump_log(const char *msg) {
    mkdir("sdmc:/switch/NX-MemTest", 0777);
    mkdir("sdmc:/switch/NX-MemTest/logs", 0777);
    FILE *f = fopen(NXMT_GPU_PUMP_LOG_PATH, "ab");
    if (f == NULL) return;
    fprintf(f, "%s\n", msg);
    fclose(f);
}

static void pump_log_truncate(void) {
    mkdir("sdmc:/switch/NX-MemTest", 0777);
    mkdir("sdmc:/switch/NX-MemTest/logs", 0777);
    FILE *f = fopen(NXMT_GPU_PUMP_LOG_PATH, "wb");
    if (f != NULL) fclose(f);
}

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#define NXMT_GPU_PUMP_CMD_BYTES      (64u * 1024u)
#define NXMT_GPU_PUMP_SHADER_BYTES   (16u * 1024u)
#define NXMT_GPU_PUMP_RESULT_BYTES   (4u * 1024u)
#define NXMT_GPU_PUMP_PREAMBLE_BYTES (NXMT_GPU_PUMP_CMD_BYTES \
                                      + NXMT_GPU_PUMP_SHADER_BYTES \
                                      + NXMT_GPU_PUMP_RESULT_BYTES)

#define NXMT_GPU_PUMP_BATCH 8u
#define NXMT_GPU_PUMP_LOCAL_X 256u
#define NXMT_GPU_PUMP_BYTES_PER_INVOCATION 16u  /* one uvec4 per shader thread */

#define NXMT_GPU_PUMP_SHADER_PATH "romfs:/shaders/gpu_verify.dksh"

static Thread g_pump_thread;
static unsigned char g_pump_stack[64 * 1024] __attribute__((aligned(0x1000)));

static atomic_bool g_pump_init_done;
static atomic_bool g_pump_init_failed;
static volatile int g_pump_error_seen;

static void *g_pump_storage_base;
static uint64_t g_pump_buffer_bytes;
static uint64_t g_pump_seed;
static atomic_bool *g_pump_stop;
static atomic_uint_fast64_t *g_pump_progress;
static atomic_uint_fast64_t *g_pump_errors;

static DkDevice    g_pump_device;
static DkMemBlock  g_pump_cmd_mem;
static DkMemBlock  g_pump_shader_mem;
static DkMemBlock  g_pump_result_mem;
static DkMemBlock  g_pump_src;
static DkMemBlock  g_pump_dst;
static DkCmdBuf    g_pump_cmdbuf;
static DkCmdList   g_pump_copy_list;
static DkCmdList   g_pump_verify_list;
static DkQueue     g_pump_queue;
static DkShader    g_pump_verify_shader;
static void       *g_pump_shader_ctrl;     /* CPU-side control blob */
static volatile uint32_t *g_pump_result_word;

static void pump_debug_cb(void* userData, const char* context,
                          DkResult result, const char* message) {
    (void)userData;
    g_pump_error_seen = 1;
    char stage[256];
    snprintf(stage, sizeof(stage), "deko3d-err: context=%s result=%d msg=%s",
        context ? context : "?", (int)result, message ? message : "");
    pump_log(stage);
}

/* DKSH file header layout (mirrors libdeko3d's expected structure). */
typedef struct DkshHeader {
    uint32_t magic;
    uint32_t header_sz;
    uint32_t control_sz;
    uint32_t code_sz;
    uint32_t programs_off;
    uint32_t num_programs;
} DkshHeader;

static bool pump_load_shader(uint8_t *shader_code_storage, size_t shader_code_max) {
    pump_log("shader: fopen " NXMT_GPU_PUMP_SHADER_PATH);
    FILE *f = fopen(NXMT_GPU_PUMP_SHADER_PATH, "rb");
    if (f == NULL) {
        pump_log("shader: fopen FAILED (errno-flavoured)");
        return false;
    }
    DkshHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        pump_log("shader: header read FAILED");
        return false;
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "shader: magic=0x%08x ctrl_sz=%u code_sz=%u",
        (unsigned)hdr.magic, (unsigned)hdr.control_sz, (unsigned)hdr.code_sz);
    pump_log(buf);
    if (hdr.code_sz > shader_code_max) {
        fclose(f);
        pump_log("shader: code too large for reserved memblock");
        return false;
    }
    void *ctrl = malloc(hdr.control_sz);
    if (ctrl == NULL) {
        fclose(f);
        pump_log("shader: ctrl malloc FAILED");
        return false;
    }
    rewind(f);
    if (fread(ctrl, hdr.control_sz, 1, f) != 1) {
        free(ctrl);
        fclose(f);
        pump_log("shader: ctrl read FAILED");
        return false;
    }
    if (fread(shader_code_storage, hdr.code_sz, 1, f) != 1) {
        free(ctrl);
        fclose(f);
        pump_log("shader: code read FAILED");
        return false;
    }
    fclose(f);
    g_pump_shader_ctrl = ctrl;

    DkShaderMaker maker;
    dkShaderMakerDefaults(&maker, g_pump_shader_mem, 0);
    maker.control = ctrl;
    maker.programId = 0;
    dkShaderInitialize(&g_pump_verify_shader, &maker);
    pump_log("shader: dkShaderInitialize done");
    return true;
}

static void pump_fill_src_pattern(void *src_ptr, uint64_t bytes, uint64_t seed) {
    uint64_t *p = (uint64_t*)src_ptr;
    uint64_t words = bytes / 8u;
    uint64_t k = 0;
#if defined(__ARM_NEON)
    const uint64x2_t step4 = vdupq_n_u64(32u);
    const uint64x2_t seed2 = vdupq_n_u64(seed);
    uint64x2_t off01 = vsetq_lane_u64(8u, vdupq_n_u64(0u), 1);
    uint64x2_t off23 = vsetq_lane_u64(24u, vdupq_n_u64(16u), 1);
    for (; k + 4 <= words; k += 4) {
        vst1q_u64(p + k,     veorq_u64(off01, seed2));
        vst1q_u64(p + k + 2, veorq_u64(off23, seed2));
        off01 = vaddq_u64(off01, step4);
        off23 = vaddq_u64(off23, step4);
    }
#endif
    for (; k < words; ++k) {
        p[k] = (k * 8ull) ^ seed;
    }
}

static bool pump_setup(uint64_t buffer_aligned) {
    char buf[128];
    snprintf(buf, sizeof(buf),
        "pump_setup: storage=%p buffer_aligned=%llu",
        g_pump_storage_base, (unsigned long long)buffer_aligned);
    pump_log(buf);

    DkDeviceMaker dev_maker;
    dkDeviceMakerDefaults(&dev_maker);
    dev_maker.cbDebug = pump_debug_cb;
    pump_log("step: dkDeviceCreate");
    g_pump_device = dkDeviceCreate(&dev_maker);
    if (g_pump_device == NULL || g_pump_error_seen) {
        pump_log("FAIL: dkDeviceCreate returned NULL or cbDebug fired");
        return false;
    }

    uint8_t *bp = (uint8_t*)g_pump_storage_base;
    void *cmd_storage    = bp;
    void *shader_storage = bp + NXMT_GPU_PUMP_CMD_BYTES;
    void *result_storage = (uint8_t*)shader_storage + NXMT_GPU_PUMP_SHADER_BYTES;
    void *src_storage    = bp + NXMT_GPU_PUMP_PREAMBLE_BYTES;
    void *dst_storage    = (uint8_t*)src_storage + buffer_aligned;

    DkMemBlockMaker mm;
    dkMemBlockMakerDefaults(&mm, g_pump_device, NXMT_GPU_PUMP_CMD_BYTES);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    mm.storage = cmd_storage;
    pump_log("step: cmd memblock");
    g_pump_cmd_mem = dkMemBlockCreate(&mm);
    if (g_pump_cmd_mem == NULL) { pump_log("FAIL: cmd_mem null"); return false; }

    DkMemBlockMaker sm;
    dkMemBlockMakerDefaults(&sm, g_pump_device, NXMT_GPU_PUMP_SHADER_BYTES);
    sm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached
             | DkMemBlockFlags_Code;
    sm.storage = shader_storage;
    pump_log("step: shader memblock");
    g_pump_shader_mem = dkMemBlockCreate(&sm);
    if (g_pump_shader_mem == NULL) { pump_log("FAIL: shader_mem null"); return false; }

    DkMemBlockMaker rm;
    dkMemBlockMakerDefaults(&rm, g_pump_device, NXMT_GPU_PUMP_RESULT_BYTES);
    rm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    rm.storage = result_storage;
    pump_log("step: result memblock");
    g_pump_result_mem = dkMemBlockCreate(&rm);
    if (g_pump_result_mem == NULL) { pump_log("FAIL: result_mem null"); return false; }

    DkCmdBufMaker cbm;
    dkCmdBufMakerDefaults(&cbm, g_pump_device);
    pump_log("step: cmdbuf");
    g_pump_cmdbuf = dkCmdBufCreate(&cbm);
    if (g_pump_cmdbuf == NULL) { pump_log("FAIL: cmdbuf null"); return false; }

    DkMemBlockMaker bm;
    dkMemBlockMakerDefaults(&bm, g_pump_device, (uint32_t)buffer_aligned);
    bm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    bm.storage = src_storage;
    pump_log("step: src memblock");
    g_pump_src = dkMemBlockCreate(&bm);
    if (g_pump_src == NULL) { pump_log("FAIL: src null"); return false; }

    bm.storage = dst_storage;
    pump_log("step: dst memblock");
    g_pump_dst = dkMemBlockCreate(&bm);
    if (g_pump_dst == NULL) { pump_log("FAIL: dst null"); return false; }

    if (g_pump_error_seen) {
        pump_log("FAIL: cbDebug fired during memblock creation");
        return false;
    }

    /* Seed src with a deterministic pattern - GPU reads it on every copy,
     * verify shader compares against it. CPU never touches src after this. */
    pump_log("step: fill src pattern");
    pump_fill_src_pattern(src_storage, buffer_aligned, g_pump_seed);

    pump_log("step: load shader");
    if (!pump_load_shader((uint8_t*)shader_storage, NXMT_GPU_PUMP_SHADER_BYTES)) {
        return false;
    }

    g_pump_result_word = (volatile uint32_t*)result_storage;
    *g_pump_result_word = 0u;

    pump_log("step: cmdbuf addMemory");
    dkCmdBufAddMemory(g_pump_cmdbuf, g_pump_cmd_mem, 0, NXMT_GPU_PUMP_CMD_BYTES);

    /* Record the copy cmdlist (single src->dst copy, submitted in a batch). */
    pump_log("step: record copy cmdlist");
    dkCmdBufCopyBuffer(g_pump_cmdbuf,
        dkMemBlockGetGpuAddr(g_pump_src),
        dkMemBlockGetGpuAddr(g_pump_dst),
        (uint32_t)buffer_aligned);
    g_pump_copy_list = dkCmdBufFinishList(g_pump_cmdbuf);

    /* Record the verify cmdlist: bind shader + SSBOs and dispatch enough
     * workgroups to cover the buffer (16 bytes per shader invocation). */
    pump_log("step: record verify cmdlist");
    const DkShader *shaders[1] = { &g_pump_verify_shader };
    dkCmdBufBindShaders(g_pump_cmdbuf, DkStageFlag_Compute, shaders, 1);
    dkCmdBufBindStorageBuffer(g_pump_cmdbuf, DkStage_Compute, 0,
        dkMemBlockGetGpuAddr(g_pump_src), (uint32_t)buffer_aligned);
    dkCmdBufBindStorageBuffer(g_pump_cmdbuf, DkStage_Compute, 1,
        dkMemBlockGetGpuAddr(g_pump_dst), (uint32_t)buffer_aligned);
    dkCmdBufBindStorageBuffer(g_pump_cmdbuf, DkStage_Compute, 2,
        dkMemBlockGetGpuAddr(g_pump_result_mem), NXMT_GPU_PUMP_RESULT_BYTES);
    uint32_t invocations = (uint32_t)(buffer_aligned / NXMT_GPU_PUMP_BYTES_PER_INVOCATION);
    uint32_t num_groups = invocations / NXMT_GPU_PUMP_LOCAL_X;
    snprintf(buf, sizeof(buf), "dispatch: invocations=%u num_groups=%u",
        invocations, num_groups);
    pump_log(buf);
    dkCmdBufDispatchCompute(g_pump_cmdbuf, num_groups, 1u, 1u);
    g_pump_verify_list = dkCmdBufFinishList(g_pump_cmdbuf);

    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, g_pump_device);
    pump_log("step: create queue");
    g_pump_queue = dkQueueCreate(&qm);
    if (g_pump_queue == NULL) { pump_log("FAIL: queue null"); return false; }
    if (g_pump_error_seen) { pump_log("FAIL: cbDebug fired during queue setup"); return false; }

    pump_log("setup: OK");
    return true;
}

static void pump_teardown(void) {
    if (g_pump_queue)      { dkQueueWaitIdle(g_pump_queue); dkQueueDestroy(g_pump_queue); g_pump_queue = NULL; }
    if (g_pump_dst)        { dkMemBlockDestroy(g_pump_dst);        g_pump_dst = NULL; }
    if (g_pump_src)        { dkMemBlockDestroy(g_pump_src);        g_pump_src = NULL; }
    if (g_pump_cmdbuf)     { dkCmdBufDestroy(g_pump_cmdbuf);       g_pump_cmdbuf = NULL; }
    if (g_pump_result_mem) { dkMemBlockDestroy(g_pump_result_mem); g_pump_result_mem = NULL; }
    if (g_pump_shader_mem) { dkMemBlockDestroy(g_pump_shader_mem); g_pump_shader_mem = NULL; }
    if (g_pump_cmd_mem)    { dkMemBlockDestroy(g_pump_cmd_mem);    g_pump_cmd_mem = NULL; }
    if (g_pump_device)     { dkDeviceDestroy(g_pump_device);       g_pump_device = NULL; }
    if (g_pump_shader_ctrl){ free(g_pump_shader_ctrl);              g_pump_shader_ctrl = NULL; }
}

static void pump_thread_entry(void *arg) {
    (void)arg;
    uint64_t buffer_aligned = (g_pump_buffer_bytes + NXMT_PAGE_BYTES - 1u)
                              & ~(uint64_t)(NXMT_PAGE_BYTES - 1u);
    pump_log_truncate();
    pump_log("=== pump thread enter ===");
    if (!pump_setup(buffer_aligned)) {
        pump_log("=== pump setup FAILED, tearing down ===");
        pump_teardown();
        atomic_store(&g_pump_init_failed, true);
        atomic_store(&g_pump_init_done, true);
        return;
    }
    pump_log("=== pump setup OK, entering loop ===");
    atomic_store(&g_pump_init_done, true);

    /* Each pump cycle queues NXMT_GPU_PUMP_BATCH copies plus one verify
     * dispatch, then waits once. The verify reduction lives entirely on the
     * GPU; the CPU pump thread does a single uncached 32-bit load to check
     * the result. */
    const uint64_t per_iter_traffic = buffer_aligned * 2ull;
    nxmt_platform_debug_stage("gpu-pump-loop");
    while (!atomic_load_explicit(g_pump_stop, memory_order_relaxed) && !g_pump_error_seen) {
        *g_pump_result_word = 0u;
        for (uint32_t i = 0; i < NXMT_GPU_PUMP_BATCH; ++i) {
            dkQueueSubmitCommands(g_pump_queue, g_pump_copy_list);
        }
        dkQueueSubmitCommands(g_pump_queue, g_pump_verify_list);
        dkQueueWaitIdle(g_pump_queue);

        if (g_pump_progress != NULL) {
            atomic_fetch_add_explicit(g_pump_progress,
                per_iter_traffic * NXMT_GPU_PUMP_BATCH, memory_order_relaxed);
        }
        if (g_pump_errors != NULL && *g_pump_result_word != 0u) {
            atomic_fetch_add_explicit(g_pump_errors, 1u, memory_order_relaxed);
        }
    }

    nxmt_platform_debug_stage("gpu-pump-teardown");
    pump_teardown();
}

bool nxmt_gpu_pump_start(void *storage_base, uint64_t storage_size,
                         uint64_t buffer_size, int affinity_core,
                         uint64_t seed,
                         atomic_bool *stop_requested,
                         atomic_uint_fast64_t *progress_bytes,
                         atomic_uint_fast64_t *error_batches) {
    if (storage_base == NULL || stop_requested == NULL || buffer_size == 0u) {
        return false;
    }
    uint64_t buffer_aligned = (buffer_size + NXMT_PAGE_BYTES - 1u)
                              & ~(uint64_t)(NXMT_PAGE_BYTES - 1u);
    uint64_t required = (uint64_t)NXMT_GPU_PUMP_PREAMBLE_BYTES + 2u * buffer_aligned;
    if (storage_size < required) return false;

    g_pump_storage_base = storage_base;
    g_pump_buffer_bytes = buffer_aligned;
    g_pump_seed = seed;
    g_pump_stop = stop_requested;
    g_pump_progress = progress_bytes;
    g_pump_errors = error_batches;
    atomic_store(&g_pump_init_done, false);
    atomic_store(&g_pump_init_failed, false);
    g_pump_error_seen = 0;

    Result rc = threadCreate(&g_pump_thread, pump_thread_entry, NULL,
        g_pump_stack, sizeof(g_pump_stack), 0x2d, affinity_core);
    if (R_FAILED(rc)) return false;
    rc = threadStart(&g_pump_thread);
    if (R_FAILED(rc)) {
        threadClose(&g_pump_thread);
        return false;
    }

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
