#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* Continuously runs GPU buffer-copy operations on a dedicated thread to add
 * memory-bus traffic on top of the CPU workers. The storage arena slice is
 * carved into cmd / shader / result / src / dst regions wrapped via
 * DkMemBlockMaker.storage so the kernel allocator (nearly empty after
 * OverrideHeap is taken) isn't touched.
 *
 * After every batch of copies the pump dispatches a compute shader that
 * XOR-OR-reduces src against dst on the GPU and writes a single mismatch
 * flag to a host-visible buffer; the pump thread reads the flag with one
 * uncached load and (when non-zero) increments error_batches via atomic.
 * All verification work runs on the GPU - the CPU stays out of the path.
 *
 * Caller is responsible for: providing 4 KiB-aligned storage of at least
 * 128 KiB + 2 * buffer_size; sharing the same stop_requested flag the
 * CPU workers respect; and calling nxmt_gpu_pump_stop after CPU side has
 * already signalled stop. The verify shader is embedded into the binary
 * at build time so no romfs mount is needed.
 *
 * progress_bytes accumulates (buffer_size * 2) per completed copy (read +
 * write traffic on the memory bus), matching how CPU bytes_written +
 * bytes_verified are reported. error_batches counts each batch in which
 * the GPU detected at least one byte difference between src and dst.
 *
 * Returns true if deko3d initialised and the thread started running. On
 * false the caller should fall back to CPU-only operation.
 */
bool nxmt_gpu_pump_start(void *storage_base, uint64_t storage_size,
                         uint64_t buffer_size, int affinity_core,
                         uint64_t seed,
                         atomic_bool *stop_requested,
                         atomic_uint_fast64_t *progress_bytes,
                         atomic_uint_fast64_t *error_batches);

/* Joins the pump thread. Must be paired with a successful start. Safe to
 * call after stop_requested has been set (which is what we expect). */
void nxmt_gpu_pump_stop(void);
