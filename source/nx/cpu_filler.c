#include "cpu_filler.h"

#include <switch.h>

#include "nxmt/platform.h"
#include "nxmt/patterns.h"

#define NXMT_FILLER_BURST_MS 50ull
#define NXMT_FILLER_SLEEP_NS (50ull * 1000000ull)

static Thread g_filler_thread;
static unsigned char g_filler_stack[16 * 1024] __attribute__((aligned(0x1000)));
static atomic_bool *g_filler_stop;
static volatile uint64_t g_filler_sink;

static void filler_entry(void *arg) {
    (void)arg;
    uint64_t state = 0xc0ffeebaadf00d05ull;
    while (!atomic_load_explicit(g_filler_stop, memory_order_relaxed)) {
        /* Spin doing pure-ALU mixing for one burst window. nxmt_mix64 plus
         * xorshift mirrors the Extreme-mode CPU pressure body so the heat /
         * power profile lines up with the rest of the run. */
        uint64_t deadline = nxmt_platform_ticks_ms() + NXMT_FILLER_BURST_MS;
        while (nxmt_platform_ticks_ms() < deadline) {
            for (uint32_t i = 0; i < 4096u; ++i) {
                state = nxmt_mix64(state + i);
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
            }
            if (atomic_load_explicit(g_filler_stop, memory_order_relaxed)) {
                break;
            }
        }
        /* Store into volatile so the optimiser can't eliminate the loop. */
        g_filler_sink = state;
        svcSleepThread((s64)NXMT_FILLER_SLEEP_NS);
    }
}

bool nxmt_cpu_filler_start(int core_id, atomic_bool *stop_requested) {
    if (stop_requested == NULL) return false;
    g_filler_stop = stop_requested;
    /* Lower priority than the workers (0x2d) and the GPU pump so the
     * filler never preempts pump submits. Priority alone isn't relied on
     * for the 50% duty cycle - the explicit burst+sleep window enforces
     * that regardless of scheduling. */
    Result rc = threadCreate(&g_filler_thread, filler_entry, NULL,
        g_filler_stack, sizeof(g_filler_stack), 0x30, core_id);
    if (R_FAILED(rc)) return false;
    rc = threadStart(&g_filler_thread);
    if (R_FAILED(rc)) {
        threadClose(&g_filler_thread);
        return false;
    }
    return true;
}

void nxmt_cpu_filler_stop(void) {
    threadWaitForExit(&g_filler_thread);
    threadClose(&g_filler_thread);
}
