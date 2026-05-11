# Memory Test Algorithm

This document describes the deterministic memory test algorithm implemented in
`source/core/patterns.c` and `source/core/runner.c`. It is the spec a future
change must preserve — the patterns and pass structure here define what
NX-MemTest considers a "verified" run.

## Contract

For any inputs `(seed, phase, pass, offset)` the function

```c
nxmt_expected_value(seed, phase, pass, offset)  // patterns.c
```

returns the same 64-bit value across runs, machines, and write/verify halves.
A mismatch between actual memory contents and this value is reported as a
fault. The seed printed in the run report is sufficient to reproduce any
recorded mismatch.

The runner also keeps NEON (aarch64) and scalar fallbacks bit-identical: both
paths must produce the same per-word value. This is why patterns avoid 64-bit
multiplications and shifts that don't have cheap NEON equivalents.

## Word and Offset Convention

- Word size: 8 bytes (`NXMT_WORD_BYTES`).
- `word_index = offset / 8`. Patterns are addressed by absolute `word_index`
  (not by the worker's local position) so worker partitioning doesn't change
  the expected stream.
- `offset` is byte-relative to the arena base; `base_off = start_word * 8` is
  the worker's slice origin.

## Phases

| Phase         | Expected word                                              | Targets                                                                  |
| ------------- | ---------------------------------------------------------- | ------------------------------------------------------------------------ |
| `FIXED_A`     | `0xAAAA...AAAA`                                            | Stuck-at-0 cells; per-bit DC stress.                                     |
| `FIXED_5`     | `0x5555...5555`                                            | Stuck-at-1 cells; complement of FIXED_A.                                 |
| `CHECKER`     | `(word_index & 1) ? 0x55..55 : 0xAA..AA`                   | Inter-word coupling; bus toggling between adjacent words.                |
| `ADDRESS`     | `offset ^ seed ^ (pass * 0x100000001b3)`                   | Address-line faults: each word's expected value depends on its position. |
| `RANDOM`      | `bswap64(word_index) ^ seed ^ (pass * 0x9e3779b97f4a7c15)` | Pseudo-random byte pattern derived from index; high entropy per word.    |
| `WALKING`     | `1ULL << ((word_index + pass) & 63)`                       | Single-bit walking pattern; one bit set per word, position cycles per pass. |
| `NARROW`      | `(X * 0x0101..01) ^ 0x0706050403020100 ^ seed`, `X = ((offset + pass*8) & 0xff)` | Narrow-store correctness. Written via 8-bit `STRB` instructions through a `volatile uint8_t*`; verifies the per-byte store path doesn't corrupt neighbours. |
| `BITSPREAD`   | `((word_index + pass) & 1) ? 0xb6db..6db : 0x4924..924`    | Long-range bit coupling. Set bits are spaced every 3 positions, stressing cells whose neighbours are far apart in the bit layout. |

Constants:

- `0x100000001b3` — FNV-1a 64-bit prime, used as a non-trivial pass mixer for the address pattern.
- `0x9e3779b97f4a7c15` — 64-bit golden-ratio constant, used to make the random pattern's per-pass mask change unpredictably.

The patterns are intentionally cheap to recompute. None require a large
state machine, so verify can race ahead and only walk word-by-word when a
mismatch is detected (see "Two-stage verify" below).

## Mode → Phase Sequence

Defined in `source/core/runner.c`:

- **Quick Check** (`NXMT_MODE_QUICK`): `FIXED_A`, `ADDRESS`, `RANDOM`.
- **Memory Load** (`NXMT_MODE_MEMORY_LOAD`): `FIXED_A`, `FIXED_5`, `CHECKER`,
  `BITSPREAD`, `ADDRESS`, `RANDOM`, `WALKING`.
- **Extreme** (`NXMT_MODE_EXTREME`): same as Memory Load, plus
  `nxmt_extreme_cpu_pressure` is folded over every word during both write and
  verify (see "Extreme CPU pressure").

`nxmt_runner_phase_count(mode)` returns 3 or 7 accordingly.

`NXMT_PHASE_NARROW` is defined and tested but not currently included in any
mode's phase table: byte-granularity `STRB` stores are roughly an order of
magnitude slower than 64-bit stores and would dominate the bandwidth-mode
average. It is reserved for a future correctness-focused mode. The TUI uses this
to size the progress bar before launching workers.

## Pass Structure

`nxmt_runner_run_pass` performs **one pass** for the calling worker:

```text
for each phase in mode:
    write_phase(slice, phase)
    verify_phase(slice, phase)
```

Where `slice` is the worker's contiguous range
`[start_word, start_word + count)` from `nxmt_split_block_start/size`, and
each phase fully writes-then-verifies before the next phase begins. Phases
are sequential (not interleaved) so coupling effects don't mask each other.

The TUI loops `nxmt_runner_run_pass` per worker until duration elapses or
the user presses `+` (`g_stop_requested`). Each iteration uses `pass++` so
`ADDRESS`/`RANDOM`/`WALKING` shift their content between passes.

## Worker Partitioning

`nxmt_split_block_start/size` (in `source/core/arena.c`):

```text
base       = total_words / worker_count
remainder  = total_words % worker_count
worker i gets:
    start = base * i + min(i, remainder)
    count = base + (i < remainder ? 1 : 0)
```

This produces non-overlapping contiguous slices that cover the arena
exactly, with at most a one-word imbalance. Workers don't communicate
during a pass other than:

- `progress_bytes` — shared atomic counter the TUI reads to drive the bar.
- `stop_requested` — shared atomic flag the TUI sets on `+` or duration
  expiry.
- `report` (per-worker) — merged into the global report after thread join.

## Streaming Implementation

`nxmt_write_chunk` and `nxmt_verify_chunk` operate on **1 MiB chunks**
(`(1 << 20) / 8 = 131072` words). Between chunks the runner:

1. Polls `stop_requested` (cooperative cancellation).
2. Updates `progress_bytes` with `chunk * NXMT_WORD_BYTES`.

On aarch64 with `__ARM_NEON` defined, each phase has a 4-word-per-iteration
NEON loop (two `uint64x2_t` registers) that writes/verifies 32 bytes per
iteration; the tail uses the scalar fallback. On host (x86, etc.) only the
scalar fallback runs.

An earlier revision used paired non-temporal stores (`stnp q, q`) and
software prefetch ahead of the verify cursor, intending to bypass L1/L2 and
push more traffic to DRAM. On Cortex-A57 (Switch) this measurably
regressed throughput — the A57 implementation aliases `STNP` to `STP` per
its TRM (the non-temporal hint is dropped), and software prefetch contended
with the hardware stride prefetcher. That experiment was reverted; the
write path is back to ordinary cache-friendly `vst1q_u64` pairs and the
verify path relies on the hardware prefetcher alone.

### Two-stage verify

For phases where the expected value is cheap to compute SIMD-wide
(`FIXED_A`, `FIXED_5`, `CHECKER`, `ADDRESS`, `RANDOM`), verify runs two
passes per chunk:

1. **OR-reduction**: XOR each actual word against expected, OR the result
   into an accumulator. If the accumulator is zero, no mismatch in the
   chunk — skip to the next chunk.
2. **Per-word walk** (only on nonzero accumulator): re-walk the chunk
   word-by-word and call `nxmt_report_record_error` for each mismatching
   word.

This keeps the hot path branch-free and SIMD-friendly while still producing
detailed error records when a fault occurs. `WALKING` only does the
per-word walk because the expected value depends on the bit position and
is already a cheap shift — there's no SIMD savings.

## Extreme CPU Pressure

`nxmt_extreme_cpu_pressure(state, value)`:

```c
for i in 0..63:
    value = mix64(value + i)
    value ^= value << 13
    value ^= value >> 7
    value ^= value << 17
return state ^ value
```

In Extreme mode this is folded over every word during both write and
verify, after the chunk's main NEON/scalar pass. The result is XOR'd into
`pressure_state`, kept per-worker, and reported as `pressure_checksum` in
`NxmtRunStats`. The checksum is non-zero and divergent across workers when
they ran their own work (asserted by `tests/test_runner.c`); it is **not**
used for memory verification — its purpose is to keep cores busy and to
confirm Extreme mode actually executed CPU work.

`mix64` is the SplitMix64 finalizer; it provides good avalanche so each
input bit affects all output bits within the loop.

## Determinism and Reproduction

Given a printed report's `Seed`, `Mode`, and `Pass` count, any third-party
host build can reproduce the exact word stream:

```
expected = nxmt_expected_value(seed, phase, pass, offset)
```

`tests/test_patterns.c` pins specific values (`mix64(0)`, `mix64(1)`, the
fixed pattern words) and validates that ADDRESS produces the same value
for repeated calls and a different value for a different offset. Any change
to the constants above must update these tests.

## Error Reporting

`NxmtReport` (in `include/nxmt/report.h`):

- **First error**: full detail captured once per report — `mode`, `phase`,
  `seed`, `pass`, `worker_id`, `offset`, `expected`, `actual`, `xor_diff`.
- **Aggregate**: `error_count`, `min_error_offset`, `max_error_offset`,
  `bit_diff_or` (OR of every `expected ^ actual` seen — flags which bit
  positions are flipping across the run).

After all workers finish their pass, `nxmt_report_merge` folds each
worker's report into the global report. The first-error detail is whichever
worker reported it first in the merge order; aggregate stats span all
workers.

## Stop and Abort Semantics

A pass can finish in three states:

- **PASS** — completed all phases with `error_count == initial_errors`.
- **FAIL** — one or more mismatches recorded in any phase.
- **ABORTED** — `stop_requested` observed mid-pass with no errors recorded.
- **UNSUPPORTED** — invalid arena/config (`worker_count == 0`, misaligned
  arena, etc.).

`nxmt_runner_abort_status` returns FAIL over ABORTED whenever there are any
errors, so a stop request never hides a real fault.

The debug-only `inject_mismatch` flag flips bit `0x100` of the first word of
the worker's slice after writing, so the verify pass guarantees a mismatch
without faulty hardware. This path drives the FAIL path in
`tests/test_runner.c`.

## What This Doesn't Catch

- **System-only memory pools**: outside the addressable arena.
- **Cell coupling beyond the patterns above**: e.g., complex 3D coupling in
  some DRAM topologies isn't directly probed.
- **Transient CPU faults**: only memory mismatches surface as errors. CPU
  errors that don't corrupt the arena are invisible. Extreme mode's
  `pressure_checksum` is informational, not a verification signal.
- **Non-memory hardware**: GPU, DMA, controller queues, and peripherals are
  not exercised.
- **Loader heap reserve**: the first `nxmt_runtime_heap_reserve` bytes of
  the override heap are owned by libnx/newlib at runtime (4 KiB-aligned,
  16–32 MiB depending on heap size) and are not in the test arena.

## Source Map

| Concern                          | File                          |
| -------------------------------- | ----------------------------- |
| Expected-value definitions       | `source/core/patterns.c`      |
| Mode → phase tables, pass driver | `source/core/runner.c`        |
| Chunked NEON/scalar paths        | `source/core/runner.c`        |
| Extreme CPU pressure             | `source/core/runner.c`        |
| Worker partitioning              | `source/core/arena.c`         |
| Error capture and merge          | `source/core/report.c`        |
| Determinism tests                | `tests/test_patterns.c`       |
| End-to-end runner tests          | `tests/test_runner.c`         |
| Partitioning and arena tests     | `tests/test_arena.c`          |
