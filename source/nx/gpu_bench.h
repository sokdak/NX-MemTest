#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Runs a Tegra X1 GPU copy-engine bandwidth probe via deko3d.
 *
 * Carves cmd / src / dst regions out of [storage_base, storage_base+storage_size)
 * and wraps each via DkMemBlockMaker.storage so the kernel pool isn't touched
 * (it's nearly empty when the OverrideHeap holds physical RAM). CPU fills the
 * source, issues `iterations` back-to-back dkCmdBufCopyBuffer commands
 * src -> dst, waits for the queue to drain, then verifies on CPU after a
 * cache flush.
 *
 * Requirements:
 *   - storage_base must be 4 KiB-aligned (the arena base from
 *     nxmt_arena_from_range already is).
 *   - storage_size >= 64 KiB + 2 * buffer_size.
 *
 * Returns true if the run completed and the memcmp verification passed.
 */
bool nxmt_gpu_bench_run(void *storage_base, uint64_t storage_size,
                        uint64_t buffer_size, uint32_t iterations);
