#pragma once

#include <stdatomic.h>
#include <stdbool.h>

/* Spawns a thread that imposes a ~90% duty-cycle CPU load on a single
 * pinned core. The work is pure ALU (mix64 + xorshift) and does not touch
 * memory, so it doesn't compete with the memory workers or the GPU pump
 * for DRAM bandwidth - it just keeps the assigned core near-saturated for
 * thermal / power stress purposes while still leaving a sliver of idle
 * time so the pump's dkQueueSubmitCommands wakes don't get delayed.
 *
 * core_id: aarch64 affinity index for the pinned core.
 * stop_requested: shared atomic the caller toggles to signal shutdown.
 *
 * Returns true if the thread was created and started. Pair with
 * nxmt_cpu_filler_stop after stop_requested goes true.
 */
bool nxmt_cpu_filler_start(int core_id, atomic_bool *stop_requested);
void nxmt_cpu_filler_stop(void);
