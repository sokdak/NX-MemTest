#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* Continuously runs GPU buffer-copy operations on a dedicated thread to add
 * memory-bus traffic on top of the CPU workers. The storage arena slice is
 * carved into cmd / src / dst regions wrapped via DkMemBlockMaker.storage,
 * so the kernel allocator (nearly empty after OverrideHeap is taken) isn't
 * touched.
 *
 * Caller is responsible for: providing 4 KiB-aligned storage of at least
 * 64 KiB + 2 * buffer_size; sharing the same stop_requested flag the CPU
 * workers respect; and calling nxmt_gpu_pump_stop after CPU side has
 * already signalled stop and joined.
 *
 * progress_bytes accumulates (buffer_size * 2) per completed copy (read +
 * write traffic on the memory bus), matching how CPU bytes_written +
 * bytes_verified are reported.
 *
 * Returns true if deko3d initialised and the thread started running. On
 * false the caller should fall back to CPU-only operation.
 */
bool nxmt_gpu_pump_start(void *storage_base, uint64_t storage_size,
                         uint64_t buffer_size, int affinity_core,
                         atomic_bool *stop_requested,
                         atomic_uint_fast64_t *progress_bytes);

/* Joins the pump thread. Must be paired with a successful start. Safe to
 * call after stop_requested has been set (which is what we expect). */
void nxmt_gpu_pump_stop(void);
