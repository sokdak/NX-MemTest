#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Runs a Tegra X1 GPU copy-engine bandwidth probe via deko3d.
 *
 * Allocates two GPU-visible memory blocks of buffer_size bytes, fills the
 * source on CPU, issues `iterations` back-to-back dkCmdBufCopyBuffer commands
 * src -> dst, waits for the queue to drain, then verifies on CPU.
 *
 * Output is printed via nxmt_platform_print; the function returns true if
 * the run completed and the verification passed.
 */
bool nxmt_gpu_bench_run(uint64_t buffer_size, uint32_t iterations);
