# NX-MemTest

NX-MemTest is an NRO-only Nintendo Switch memory stress and verification app.
It runs through hbmenu/nx-hbloader title-override (full-memory) mode and uses
the loader-provided `OverrideHeap` as its directly verified test arena.

## Modes

| Mode         | Arena need | Phases                                                   | Notes                                            |
| ------------ | ---------- | -------------------------------------------------------- | ------------------------------------------------ |
| Quick Check  | any        | fixed-A, address, random                                 | Smoke test, allowed in applet / small heap.      |
| Memory Load  | >= 256 MiB | fixed-A, fixed-5, checker, address, random, walking      | Long-running RAM stability.                      |
| Extreme      | >= 512 MiB | same as Memory Load + per-word CPU pressure on each word | Drives controller, thermals, power; 3 workers.   |

Each phase performs a streaming write across the worker's slice of the arena,
then a streaming verify. Extreme mode additionally runs `nxmt_extreme_cpu_pressure`
on every word to keep cores busy between memory operations.

## Progress Labels

- **System Stress Pass** — reaches 100% when the configured arena finishes one full pass.
- **Verified Arena** — bytes directly written, read, and compared this run.
- **Effective Total** — currently sourced from `OverrideHeap` size only (see "Memory Policy").
- **Total Source** — which channel supplied Effective Total (`override-heap`, `process-total`, `physical-pools`, or `none`).
- **Extended Memory** — flagged when any reported total exceeds 4 GiB.
- **Physical Coverage** — `test_arena_size / effective_total`.

NX-MemTest does not directly verify system-only memory pools; those regions
are outside the NRO addressable arena.

## Memory Policy

At startup `__libnx_initheap()` is overridden so libnx/newlib runtime state
uses a small slice of the loader heap (`nxmt_runtime_heap_reserve`, currently
16–32 MiB depending on heap size), and the remainder becomes the test arena.

The platform layer queries `OverrideHeap` only and skips
`svcGetSystemInfo` / `svcGetInfo` calls during boot, because some loader
configurations have aborted before the UI could render. See
`source/nx/platform_nx.c` and `nxmt_select_launch_safe_memory_total`.

If no `OverrideHeap` is provided the app shows a warning screen and exits on
`+`. Quick Check still runs on any arena; Memory Load and Extreme require a
larger arena (gated in `tui_choose_mode`).

## Repository Layout

```
include/nxmt/        Public headers (types, arena, patterns, report, runner,
                     platform, startup)
source/core/         Portable test core: arena, patterns, report, runner,
                     startup. Must not depend on libnx.
source/nx/           Switch platform layer: libnx heap override, console, pad
                     input, SD logging, TUI shell (main.c).
tests/               Host-side unit tests (CMake).
scripts/             check-toolchain.ps1 sanity-checks the build env.
docs/superpowers/    Design and implementation notes.
```

The runner and patterns are pure C11; they can be compiled and tested on a
host PC. Anything that touches `<switch.h>` or libnx APIs lives under
`source/nx/`.

## Host Tests

Host tests cover arena math, memory selection, pattern determinism, report
aggregation, runner pass/abort/fail paths, and the startup policy gate.

```powershell
cmake -S . -B build/host
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

The default generator depends on what's installed (Visual Studio, Ninja, or
MinGW). Pass `-G "Ninja"` or similar if your environment needs it. No libnx,
devkitPro, or aarch64 toolchain is required for host tests.

## Switch Build

Install devkitPro with devkitA64 and libnx, then run:

```powershell
pwsh -File scripts/check-toolchain.ps1
make
```

Output: `NX-MemTest.nro` at the repo root.

`Makefile` builds with `-O3 -ftree-vectorize` and the SIMD-friendly NEON
intrinsics in `runner.c` are guarded by `__aarch64__ && __ARM_NEON`, so the
host build still compiles using the scalar fallback.

## Switch Run

Copy the NRO to:

```text
sdmc:/switch/NX-MemTest.nro
```

Launch hbmenu through title override / full-memory mode, then start
NX-MemTest. Applet mode shows a warning and should only be used for Quick
Check.

Reports are written to:

```text
sdmc:/switch/NX-MemTest/logs/latest.txt
```

A boot-stage marker is written to `sdmc:/switch/NX-MemTest/logs/boot_stage.txt`
on each launch to help diagnose loaders that abort before the UI can render.

## Controls

```
A           Quick Check  (also: confirm in menus)
X           Memory Load
Y           Extreme
Up / Down   Move selection in duration menu
B           Back to mode menu
+           Stop / Exit
```

## Determinism

Every `(seed, phase, pass, offset)` produces the same expected value, so a
mismatch can be reproduced exactly given the seed printed in the report. See
`nxmt_expected_value` in `source/core/patterns.c`.

## Status / Scope

Out of scope for the first version: NSP forwarders, GPU/DMA stress,
privileged physical-memory mapping, and direct verification of system-only
pools. Design notes: `docs/superpowers/specs/2026-05-07-nx-memtest-design.md`.
