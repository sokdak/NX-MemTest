# NX-MemTest Design

Date: 2026-05-07
Status: Ready for user review

## Goal

NX-MemTest is a Nintendo Switch NRO homebrew application for stressing and
checking memory stability under full-memory homebrew execution. It should cover
two primary use cases:

- Memory Load: maximize memory traffic and long-running RAM stability coverage.
- Extreme: combine high memory traffic with CPU pressure to stress the memory
  controller, power delivery, thermals, and overclock stability, similar in
  spirit to LinX-style stress testing.

The first version targets NRO only. NSP/forwarder packaging is out of scope
unless a later design revisits title permissions, resource limits, and install
workflow.

## Platform Assumptions

NX-MemTest runs through hbmenu/nx-hbloader as an NRO. Users are expected to run
hbmenu through title override/full-memory mode. In that path, nx-hbloader
allocates a large heap for the process and passes the remaining NRO heap to the
application through the Homebrew ABI `EntryType_OverrideHeap`.

The application must distinguish between:

- Directly verified memory: the `OverrideHeap` arena that NX-MemTest writes,
  reads, and compares.
- System stress coverage: the practical DRAM and memory-controller stress
  caused by repeatedly driving the available arena with random and streaming
  access patterns.
- Physical memory coverage estimate: `test_arena_size / switch_total_memory`.

Full physical DRAM cannot be directly verified from a normal NRO, because some
memory belongs to system pools outside the application's addressable arena.

## Memory Policy

The recommended approach is direct `OverrideHeap` testing.

At startup, NX-MemTest overrides `__libnx_initheap()` so libnx/newlib runtime
state uses a small internal heap backed by static storage. The loader-provided
heap from `envGetHeapOverrideAddr()` and `envGetHeapOverrideSize()` is treated
as the test arena instead of being consumed through `malloc()`.

This avoids allocator metadata, fragmentation, and normal runtime allocations
from reducing or polluting the test range. No additional "system memory
reserve" is held back by NX-MemTest. Any loader/kernel safety margin remains the
responsibility of nx-hbloader and Horizon OS.

If no heap override is present, or the heap is too small, the app warns that it
is not running in full-memory mode. Quick Check may still run on a small arena,
but Memory Load and Extreme require a large full-memory arena.

## Architecture

The project is split into small modules:

- `platform`: libnx initialization, heap override detection, total physical
  memory queries, input, timing, threads, and SD paths.
- `arena`: test range discovery, alignment, chunk/block partitioning, and pass
  accounting.
- `patterns`: deterministic data generation for random stream, walking bits,
  checkerboard, address-derived values, and xor-shift patterns.
- `runner`: session lifecycle, pass/phase control, worker thread dispatch,
  pause/stop handling, and atomic counters.
- `modes`: Quick Check, Memory Load, and Extreme mode definitions.
- `report`: first-error capture, aggregate error stats, checkpoint records, and
  final log serialization.
- `ui`: console-based menus and live status display.

The test algorithms should not depend directly on libnx. Core pattern and error
aggregation logic should be testable on a host PC.

## Test Modes

### Quick Check

Quick Check runs a short sanity test against a small or selected arena. It is
allowed in applet mode or constrained memory environments. It is intended for
smoke testing the app, not for validating system stability.

### Memory Load

Memory Load uses the maximum available test arena. It focuses on sustained
memory pressure with minimal extra CPU work. It runs phases such as:

- Fill and verify fixed patterns.
- Checkerboard and inverted checkerboard.
- Address-derived pattern.
- Random address write/read/verify using deterministic seed streams.
- Walking bit and walking inverse passes over aligned words.

This mode is the default long-running stability test.

### Extreme

Extreme uses the same arena but runs multiple worker threads, typically bound to
the CPU cores available to NRO homebrew. Each worker combines memory traffic
with CPU-heavy integer and NEON-friendly operations. The goal is to increase
controller, thermal, and power stress rather than only detecting isolated memory
cell errors.

If thread creation or CPU affinity fails, the runner reduces worker count and
records the fallback in the log.

## Data Flow

Startup:

1. Initialize minimal runtime state.
2. Read Homebrew ABI environment.
3. Detect `OverrideHeap` address and size.
4. Query Switch physical memory pool totals with `svcGetSystemInfo` when
   available.
5. Align the test arena to page/cache-line boundaries.
6. Run a small internal self-test for pattern generation.
7. Present mode selection.
8. Create a session seed or accept a user-provided seed.
9. Start runner workers.
10. Update live UI from atomic counters.
11. Save checkpoints and final logs to SD.

All random access and expected-value generation must be seed-deterministic. The
same mode, seed, arena size, and pass should reproduce the same address and
expected data streams.

## Progress And Metrics

The UI separates stress progress from direct verification claims:

- `System Stress Pass`: primary progress indicator for the current pass. It
  reaches 100% when the configured arena has completed one full stress pass.
- `Verified Arena`: direct byte range that has been written/read/compared,
  shown as a percentage and byte count of the test arena.
- `Physical Coverage`: estimated `test_arena_size / switch_total_memory`.
- `Switch Total`: total physical memory estimate from physical memory pools.
- `Test Arena`: actual directly addressable test arena size.
- `Passes Completed`: completed pass count.
- `Tested`: cumulative verified bytes, shown in GiB/TiB as appropriate.
- `Speed`: estimated GiB/s based on verified bytes per elapsed second.
- `Errors`: total detected mismatches.
- `First Error`: compact summary of the first detected mismatch.

If physical memory pool queries fail, the UI falls back to process memory totals
and labels the value as process total instead of Switch total.

## Error Handling

Any mismatch marks the run as FAIL.

The first error is captured in detail:

- mode
- seed
- pass
- phase
- worker id
- virtual arena offset
- expected value
- actual value
- xor diff
- timestamp

Additional errors are aggregated by count, address range, phase, and bit-diff
summary to prevent the UI and log from becoming unusable.

Fatal and degraded conditions:

- No `OverrideHeap`: warn about applet/non-full-memory execution.
- Arena too small: allow Quick Check only.
- Worker creation failure: reduce worker count and continue if possible.
- SD log failure: continue testing and report that logs are unavailable.
- User abort: set stop flags and terminate workers promptly.
- Application crash/freeze during Extreme: rely on periodic checkpoint logs to
  identify the last successful pass and phase.

## Logging

Logs are written under:

`sdmc:/switch/NX-MemTest/logs/`

The first version uses a plain text report format. Each run records:

- build/version
- loader info when available
- mode
- seed
- arena size
- Switch/process total memory
- physical coverage estimate
- worker count
- pass and phase summary
- first error detail
- aggregate errors
- start/end timestamps
- final status

Checkpoint logs are written periodically during long runs, especially Extreme
mode.

## Testing Strategy

Host unit tests cover:

- pattern expected-value generation
- deterministic random address streams
- pass/phase sequencing
- error aggregation
- progress calculations

NX integration tests cover:

- heap override detection
- arena alignment
- Quick Check one-pass run
- worker start/stop
- SD logging
- fallback behavior in applet/small-heap mode

Debug builds include an optional mismatch injection path so error reporting can
be tested without faulty hardware.

## Out Of Scope For First Version

- Standalone NSP packaging.
- Privileged physical memory mapping.
- Direct verification of system-only memory pools.
- GPU/DMA-based stress.
- GUI beyond console-style libnx output.

## References

- Homebrew ABI: https://www.switchbrew.org/wiki/Homebrew_ABI
- libnx runtime heap initialization:
  https://github.com/switchbrew/libnx/blob/master/nx/source/runtime/init.c
- libnx environment API:
  https://github.com/switchbrew/libnx/blob/master/nx/include/switch/runtime/env.h
- libnx SVC API:
  https://github.com/switchbrew/libnx/blob/master/nx/include/switch/kernel/svc.h
- nx-hbloader heap setup:
  https://github.com/switchbrew/nx-hbloader/blob/master/source/main.c
