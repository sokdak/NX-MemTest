# Agent Harness — NX-MemTest

This file briefs Claude Code (and other agents) on this repo. Read it before
making changes. The user-facing overview is in `README.md`; design intent is
in `docs/superpowers/specs/2026-05-07-nx-memtest-design.md`.

## What this project is

A Nintendo Switch NRO homebrew memory tester. Three modes (Quick / Memory
Load / Extreme) that write deterministic patterns into the loader-provided
`OverrideHeap`, then verify them. Built with libnx via devkitPro; portable
core compiles and tests on host PCs.

## Module map

```
include/nxmt/
  types.h        constants (NXMT_*_BYTES), enums (Mode, Phase, Status)
  arena.h        arena math, memory-source selection, runtime heap reserve
  patterns.h     deterministic expected-value generators
  report.h       error capture + merge across workers
  runner.h       NxmtRunConfig / NxmtRunStats; nxmt_runner_run_pass entry
  platform.h     platform-side input, console, memory query, SD logging
  startup.h      compile-time-gated startup diagnostics policy

source/core/     Portable. MUST NOT include <switch.h> or libnx headers.
  arena.c        align, page-aligned ranges, percent-milli, block split,
                 memory-source selection, runtime heap reserve
  patterns.c     nxmt_expected_value etc. (host-deterministic)
  report.c       report init / record_error / merge
  runner.c       chunked write/verify (NEON aarch64 + scalar fallback),
                 phase sequencing, progress reporting
  startup.c      gated by NXMT_ENABLE_BOOT_FILE_DIAGNOSTICS

source/nx/       Switch-only. Owns all libnx interaction.
  platform_nx.c  __libnx_initheap override, OverrideHeap discovery, pad,
                 console, SD logging, debug stage
  main.c         TUI: header, mode menu, duration menu, progress frame,
                 result section. Drives 3 worker threads.

tests/           Host tests (CMake). Use only core/ headers and stdlib.
```

Anything that needs `<switch.h>` lives under `source/nx/`. Core code is pure
C11 + `<stdatomic.h>` + `<string.h>` + sized integer types.

## Build & test commands

The repo's CMake doesn't pin a generator and `cmake.exe` is the only
toolchain entry on the default PATH. The working recipes below were
verified on this Windows host; they assume devkitPro is installed via
msys2 pacman at the standard `/c/tools/msys64/...` layout.

### Host build (Ninja + UCRT64 gcc)

The Bash tool's MSYS env keeps `gcc.exe` and `ninja.exe` under
`/c/tools/msys64/ucrt64/bin`. Prepending that to PATH lets CMake's default
Unix-Makefiles fallback or Ninja generator find a working compiler:

```bash
export PATH="/c/tools/msys64/ucrt64/bin:$PATH"
cmake -S . -B build/host -G Ninja
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

Compiler observed: GCC 16.1.0 (mingw-w64 UCRT). Host build links 6 test
executables; all six should pass.

### Switch NRO build (devkitA64 + libnx via msys2)

devkitPro is installed via the msys2 pacman repo at `/c/tools/msys64/opt/devkitpro`
(visible inside msys2 as `/opt/devkitpro`). Versions observed:
devkitA64 r29.2-1, devkita64-gcc 15.2.0, libnx 4.12.0-1.

The Makefile fails with `Please set DEVKITPRO in your environment` when
invoked from the default Bash tool shell (which is MINGW64-flavored git-bash):
exports from that shell don't reach the msys2 `make.exe` reliably. Use msys2
bash explicitly via `-lc`:

```bash
/c/tools/msys64/usr/bin/bash.exe -lc '
  cd "$(cygpath -u "/c/Users/Administrator/Desktop/dev/NX-MemTest")"
  export DEVKITPRO=/opt/devkitpro
  export PATH=$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH
  make
'
```

Output: `NX-MemTest.nro` at the repo root, ~240 KiB. `make clean` removes
the build/ directory, .elf, .nro, .nacp, and .map. The Makefile globs
`*.c` under `source/core` and `source/nx`, so adding a new core source
file is picked up automatically; CMake is **not** auto-glob (see
"Adding files").

### Sanity-check the toolchain

```powershell
powershell -ExecutionPolicy Bypass -File scripts/check-toolchain.ps1
```

This script looks for `gcc`/`clang`/`cl`, `make`, `aarch64-none-elf-gcc`,
`nxlink`, and `DEVKITPRO`. On a default shell it will report most as
missing — that's expected; the build recipes above set them inline.

## Invariants — do not break these

1. **Core stays portable.** Files under `source/core/` and `include/nxmt/`
   (except `platform.h`) must not include `<switch.h>` or any libnx header.
   They must compile with a host C11 compiler and pass host tests.

2. **Patterns are deterministic.** `nxmt_expected_value(seed, phase, pass, offset)`
   must return the same value for the same inputs across runs and across
   write/verify halves of a pass. The test report's seed must be enough to
   reproduce a mismatch.

3. **NEON paths and scalar fallbacks must agree.** `runner.c` is gated by
   `defined(__aarch64__) && defined(__ARM_NEON)`. Any pattern change must keep
   both paths writing/verifying the same values, or `test_runner.c` and
   `test_patterns.c` will fail on host.

4. **Override-heap is the test arena.** `__libnx_initheap` reserves
   `nxmt_runtime_heap_reserve(size)` bytes for libnx/newlib at the start of
   the override range and exposes the rest as `g_test_arena_addr/size`.
   Don't allocate the test arena via `malloc` or `svcSetHeapSize`; the whole
   point is to test the loader-provided range directly.

5. **Boot stays minimal.** `nxmt_platform_get_memory` deliberately avoids
   `svcGetSystemInfo`/`svcGetInfo` because some loaders abort before the UI
   renders. Use `nxmt_select_launch_safe_memory_total` if you need memory
   selection during boot. If you reintroduce those syscalls, gate them behind
   the post-UI lifecycle.

6. **Progress is best-effort.** `progress_bytes` in `NxmtRunConfig` is
   optional (`NULL` is valid). Updates use `memory_order_relaxed`. Don't
   assume monotonic writers across phases.

7. **Stop is cooperative.** Workers poll `stop_requested` between chunks.
   Don't add long uninterrupted loops that ignore it.

8. **Worker stacks are static.** `g_worker_stacks[3][64*1024]`. Don't grow
   recklessly — switch threads commit their full stack.

## Adding files / wiring

When you add a new `source/core/foo.c`:

1. Add `target_sources(nxmt_core PRIVATE source/core/foo.c)` to
   `CMakeLists.txt`. The Makefile picks it up via `wildcard $(dir)/*.c`.
2. If it has a host test, add another `add_executable` + `add_test` block in
   `CMakeLists.txt` and link `nxmt_core`. Place the test in `tests/`.

When you add a new `source/nx/foo.c`: just save it; the Makefile globs.

## Common task playbooks

### Add a new test phase

1. Append to `NxmtPhase` in `include/nxmt/types.h`.
2. Add the case in `nxmt_expected_value` (`source/core/patterns.c`) — keep
   it cheap to compute since it's hit per word in scalar fallback.
3. Add a NEON-friendly write loop in `nxmt_write_chunk` and a matching
   verify path in `nxmt_verify_chunk` (`source/core/runner.c`). Verify must
   first do a SIMD OR-reduction, then only fall back to per-word reporting
   when a mismatch is detected.
4. Append the new phase to `quick_phases` and/or `memory_load_phases` in
   `runner.c`.
5. Add a deterministic spot-check in `tests/test_patterns.c`.
6. Run host tests.

### Add a new mode

1. Append to `NxmtMode` in `include/nxmt/types.h`.
2. Update `nxmt_phase_count_for_mode` / `nxmt_phase_for_mode`.
3. Update `tui_choose_mode` button handling and arena gates in `main.c`.
4. Update `worker_count_for_mode` if the mode wants a different worker count
   (currently always 3, capped at 3 by `g_worker_threads` size).
5. If the mode adds CPU work like Extreme, hook it through the
   `pressure_state` plumbing in `nxmt_write_phase`/`nxmt_verify_phase`.

### Add a new input button

1. Add the field to `NxmtInput` in `include/nxmt/platform.h`.
2. Map the libnx pad mask in `nxmt_platform_read_input` (`platform_nx.c`).
3. Consume it in `main.c` menus.

The host has no platform layer implementation — `platform.h` is consumed only
by `source/nx/main.c` and `source/nx/platform_nx.c`. Host tests don't link
the platform layer.

### Tweak runtime heap split

`nxmt_runtime_heap_reserve` (`source/core/arena.c`) decides how much of the
override heap is set aside for libnx/newlib. Update the function and
`tests/test_arena.c` together.

## Gotchas

- The walking-bit phase indexes by `(start_word + pass) & 63`, which is
  index-relative, not address-relative. Don't "fix" this without updating
  both write and verify paths together.
- `consoleUpdate` is now only triggered by `nxmt_platform_console_flush`.
  `nxmt_platform_print` no longer flushes — call flush at frame boundaries.
- The CMake `nxmt_core` target uses explicit `target_sources` lines and is
  easy to forget when adding a new core file (this is what broke
  `test_startup_policy` initially).
- File I/O during boot is intentionally limited to the boot-stage marker
  (`sdmc:/switch/NX-MemTest/logs/boot_stage.txt`). Don't add other file
  writes between `__libnx_initheap` and `nxmt_platform_console_init`.
- The TUI assumes a 78-column console. ANSI sequences are filtered for width
  via `visible_width` in `main.c`.

## Logs and diagnostics

Runtime artifacts on the Switch:

- `sdmc:/switch/NX-MemTest/logs/latest.txt` — final run report.
- `sdmc:/switch/NX-MemTest/logs/boot_stage.txt` — last reached boot stage,
  rewritten on every `nxmt_platform_debug_stage` call.

Stage names that appear there are searchable via grep on `main.c`
(`main-entry`, `console-init`, `memory-query-start`, `mode-selected`, etc.)
to localize hangs.

## Workflow conventions

- Commit messages: short conventional prefixes (`feat:`, `fix:`, `perf:`,
  `docs:`) matching existing history.
- Group changes by intent. Keep arena, runner, platform, and TUI changes
  in separate commits when reasonable; the test files for each module ride
  with their module commit.
- Don't push, open PRs, or create new branches without explicit user
  request.
- Don't run destructive git operations (`reset --hard`, `push --force`,
  branch deletes) without explicit user request.

## Where to read more

- `docs/superpowers/specs/2026-05-07-nx-memtest-design.md` — design intent,
  full mode/phase rationale, error handling policy.
- `docs/superpowers/plans/2026-05-07-nx-memtest-implementation.md` — initial
  implementation plan (historical, may have drifted).
- `git log` — recent direction, especially around boot stability and the
  vectorized runner.
