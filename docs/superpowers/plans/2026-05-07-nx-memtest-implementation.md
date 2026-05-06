# NX-MemTest Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an NRO-only Nintendo Switch memory stress and verification app with host-testable core logic and libnx platform integration.

**Architecture:** Keep memory algorithms in portable C modules that can be tested on the host. Keep Switch-specific heap override, SVC memory queries, input, console UI, threading, and SD logging behind a small platform layer. The first working version ships Quick Check, Memory Load, and Extreme modes with deterministic seeds, progress metrics, first-error capture, and plain text logs.

**Tech Stack:** C11, devkitPro/devkitA64/libnx for NRO builds, CMake/CTest for host unit tests, PowerShell helper scripts for toolchain checks on Windows.

---

## File Structure

- `.gitignore`: build artifacts and generated NRO files.
- `README.md`: build, run, and full-memory/title override usage notes.
- `Makefile`: devkitPro/libnx NRO build.
- `CMakeLists.txt`: host unit test build.
- `scripts/check-toolchain.ps1`: reports missing host compiler and devkitPro tools.
- `include/nxmt/types.h`: shared enums, constants, fixed-width structs.
- `include/nxmt/patterns.h`: deterministic seed and expected-value API.
- `include/nxmt/arena.h`: arena alignment, block split, progress calculations.
- `include/nxmt/report.h`: first-error and aggregate error API.
- `include/nxmt/runner.h`: session config, stats, and portable runner API.
- `include/nxmt/platform.h`: platform interface consumed by app code.
- `source/core/patterns.c`: portable pattern generation.
- `source/core/arena.c`: portable arena/progress helpers.
- `source/core/report.c`: portable error aggregation.
- `source/core/runner.c`: portable single-thread phase engine used by host tests and NX workers.
- `source/nx/platform_nx.c`: libnx heap override, memory info, time, input, logging, and worker wrappers.
- `source/nx/main.c`: NRO menu, mode selection, live UI, and session lifecycle.
- `tests/test_patterns.c`: host tests for deterministic pattern generation.
- `tests/test_arena.c`: host tests for alignment, coverage, and progress math.
- `tests/test_report.c`: host tests for first-error capture and aggregation.
- `tests/test_runner.c`: host tests for pass execution and injected mismatch behavior.

---

### Task 1: Repository Scaffold And Toolchain Check

**Files:**
- Create: `.gitignore`
- Create: `README.md`
- Create: `scripts/check-toolchain.ps1`
- Create: `CMakeLists.txt`
- Create: `tests/test_smoke.c`

- [ ] **Step 1: Write the failing smoke test**

Create `tests/test_smoke.c`:

```c
#include "nxmt/types.h"

int main(void) {
    return NXMT_WORD_BYTES == 8 ? 0 : 1;
}
```

- [ ] **Step 2: Add host build scaffold**

Create `.gitignore`:

```gitignore
build/
*.elf
*.nro
*.nacp
*.map
*.o
*.d
.vs/
.vscode/
```

Create `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(NXMemTestHost C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

enable_testing()

add_library(nxmt_core
    source/core/patterns.c
)
target_include_directories(nxmt_core PUBLIC include)

add_executable(test_smoke tests/test_smoke.c)
target_link_libraries(test_smoke PRIVATE nxmt_core)
add_test(NAME test_smoke COMMAND test_smoke)
```

Create `scripts/check-toolchain.ps1`:

```powershell
$ErrorActionPreference = "Stop"

function Test-Command($Name) {
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $cmd) {
        Write-Host "missing: $Name"
        return $false
    }
    Write-Host "found:   $Name -> $($cmd.Source)"
    return $true
}

$ok = $true
$ok = (Test-Command "cmake") -and $ok
$ok = ((Test-Command "gcc") -or (Test-Command "clang") -or (Test-Command "cl")) -and $ok
$ok = (Test-Command "make") -and $ok
$ok = (Test-Command "aarch64-none-elf-gcc") -and $ok
$ok = (Test-Command "nxlink") -and $ok

if (-not $env:DEVKITPRO) {
    Write-Host "missing: DEVKITPRO environment variable"
    $ok = $false
} else {
    Write-Host "found:   DEVKITPRO -> $env:DEVKITPRO"
}

if (-not $ok) {
    exit 1
}
exit 0
```

Create `README.md`:

```markdown
# NX-MemTest

NX-MemTest is an NRO-only Nintendo Switch homebrew app for full-memory RAM and
memory-controller stress testing.

Run hbmenu through title override/full-memory mode before launching the NRO.
Applet mode has a much smaller heap and is only suitable for Quick Check.

## Host Tests

```powershell
cmake -S . -B build/host
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

## Switch Build

Install devkitPro with devkitA64 and libnx, then run:

```powershell
make
```

The NRO is produced at `NX-MemTest.nro`.
```

- [ ] **Step 3: Run smoke test to verify it fails before core headers exist**

Run:

```powershell
cmake -S . -B build/host
```

Expected on a machine with CMake and a C compiler: configure fails because `include/nxmt/types.h` and `source/core/patterns.c` do not exist.

Expected on the current machine before toolchain install: CMake or compiler discovery fails. This is acceptable for this step because the repository is being scaffolded before toolchain installation.

- [ ] **Step 4: Commit scaffold**

```powershell
git add .gitignore README.md CMakeLists.txt scripts/check-toolchain.ps1 tests/test_smoke.c
git commit -m "chore: add project scaffold"
```

---

### Task 2: Core Types And Pattern Generator

**Files:**
- Create: `include/nxmt/types.h`
- Create: `include/nxmt/patterns.h`
- Create: `source/core/patterns.c`
- Create: `tests/test_patterns.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing pattern tests**

Create `tests/test_patterns.c`:

```c
#include <stdint.h>
#include "nxmt/patterns.h"

static int expect_u64(uint64_t got, uint64_t want) {
    return got == want ? 0 : 1;
}

int main(void) {
    uint64_t seed = nxmt_make_seed(0x12345678u, 0x9abcdef0u);
    int failed = 0;

    failed |= expect_u64(seed, 0x123456789abcdef0ull);
    failed |= expect_u64(nxmt_mix64(0), 0xe220a8397b1dcdafull);
    failed |= expect_u64(nxmt_mix64(1), 0x910a2dec89025cc1ull);

    uint64_t a = nxmt_expected_value(seed, NXMT_PHASE_ADDRESS, 2, 0x40);
    uint64_t b = nxmt_expected_value(seed, NXMT_PHASE_ADDRESS, 2, 0x40);
    uint64_t c = nxmt_expected_value(seed, NXMT_PHASE_ADDRESS, 2, 0x48);
    failed |= a == b ? 0 : 1;
    failed |= a != c ? 0 : 1;

    failed |= expect_u64(nxmt_expected_value(seed, NXMT_PHASE_FIXED_A, 0, 0), 0xaaaaaaaaaaaaaaaaull);
    failed |= expect_u64(nxmt_expected_value(seed, NXMT_PHASE_FIXED_5, 0, 0), 0x5555555555555555ull);
    failed |= expect_u64(nxmt_expected_value(seed, NXMT_PHASE_CHECKER, 0, 0), 0xaaaaaaaaaaaaaaaaull);
    failed |= expect_u64(nxmt_expected_value(seed, NXMT_PHASE_CHECKER, 0, 8), 0x5555555555555555ull);

    return failed;
}
```

Modify `CMakeLists.txt` by adding:

```cmake
add_executable(test_patterns tests/test_patterns.c)
target_link_libraries(test_patterns PRIVATE nxmt_core)
add_test(NAME test_patterns COMMAND test_patterns)
```

- [ ] **Step 2: Run pattern tests to verify failure**

Run:

```powershell
cmake -S . -B build/host
cmake --build build/host
ctest --test-dir build/host --output-on-failure -R test_patterns
```

Expected: build fails because `nxmt/patterns.h` is not present.

- [ ] **Step 3: Add core types and pattern implementation**

Create `include/nxmt/types.h`:

```c
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NXMT_WORD_BYTES 8u
#define NXMT_CACHE_LINE_BYTES 64u
#define NXMT_PAGE_BYTES 4096u
#define NXMT_MIB_BYTES (1024ull * 1024ull)
#define NXMT_GIB_BYTES (1024ull * 1024ull * 1024ull)

typedef enum NxmtMode {
    NXMT_MODE_QUICK = 0,
    NXMT_MODE_MEMORY_LOAD = 1,
    NXMT_MODE_EXTREME = 2
} NxmtMode;

typedef enum NxmtPhase {
    NXMT_PHASE_FIXED_A = 0,
    NXMT_PHASE_FIXED_5 = 1,
    NXMT_PHASE_CHECKER = 2,
    NXMT_PHASE_ADDRESS = 3,
    NXMT_PHASE_RANDOM = 4,
    NXMT_PHASE_WALKING = 5
} NxmtPhase;

typedef enum NxmtStatus {
    NXMT_STATUS_PASS = 0,
    NXMT_STATUS_FAIL = 1,
    NXMT_STATUS_ABORTED = 2,
    NXMT_STATUS_UNSUPPORTED = 3
} NxmtStatus;
```

Create `include/nxmt/patterns.h`:

```c
#pragma once

#include "nxmt/types.h"

uint64_t nxmt_make_seed(uint32_t high, uint32_t low);
uint64_t nxmt_mix64(uint64_t value);
uint64_t nxmt_expected_value(uint64_t seed, NxmtPhase phase, uint64_t pass, uint64_t offset);
uint64_t nxmt_next_offset(uint64_t seed, uint64_t pass, uint64_t index, uint64_t arena_words);
```

Create `source/core/patterns.c`:

```c
#include "nxmt/patterns.h"

static uint64_t nxmt_rotl64(uint64_t value, unsigned shift) {
    return (value << shift) | (value >> (64u - shift));
}

uint64_t nxmt_make_seed(uint32_t high, uint32_t low) {
    return ((uint64_t)high << 32) | (uint64_t)low;
}

uint64_t nxmt_mix64(uint64_t value) {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

uint64_t nxmt_expected_value(uint64_t seed, NxmtPhase phase, uint64_t pass, uint64_t offset) {
    uint64_t word_index = offset / NXMT_WORD_BYTES;

    switch (phase) {
    case NXMT_PHASE_FIXED_A:
        return 0xaaaaaaaaaaaaaaaaull;
    case NXMT_PHASE_FIXED_5:
        return 0x5555555555555555ull;
    case NXMT_PHASE_CHECKER:
        return (word_index & 1u) ? 0x5555555555555555ull : 0xaaaaaaaaaaaaaaaaull;
    case NXMT_PHASE_ADDRESS:
        return nxmt_mix64(seed ^ nxmt_rotl64(offset, 17) ^ (pass * 0x100000001b3ull));
    case NXMT_PHASE_RANDOM:
        return nxmt_mix64(seed ^ nxmt_mix64(pass) ^ nxmt_mix64(word_index));
    case NXMT_PHASE_WALKING: {
        unsigned bit = (unsigned)((word_index + pass) & 63u);
        return 1ull << bit;
    }
    }

    return nxmt_mix64(seed ^ offset ^ pass);
}

uint64_t nxmt_next_offset(uint64_t seed, uint64_t pass, uint64_t index, uint64_t arena_words) {
    if (arena_words == 0) {
        return 0;
    }
    uint64_t mixed = nxmt_mix64(seed ^ nxmt_mix64(pass) ^ (index * 0xd6e8feb86659fd93ull));
    return (mixed % arena_words) * NXMT_WORD_BYTES;
}
```

- [ ] **Step 4: Run tests to verify pass**

Run:

```powershell
cmake -S . -B build/host
cmake --build build/host
ctest --test-dir build/host --output-on-failure -R "test_smoke|test_patterns"
```

Expected with host toolchain installed: both tests pass.

- [ ] **Step 5: Commit pattern core**

```powershell
git add include/nxmt/types.h include/nxmt/patterns.h source/core/patterns.c tests/test_patterns.c CMakeLists.txt
git commit -m "feat: add deterministic memory patterns"
```

---

### Task 3: Arena Alignment And Progress Metrics

**Files:**
- Create: `include/nxmt/arena.h`
- Create: `source/core/arena.c`
- Create: `tests/test_arena.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing arena tests**

Create `tests/test_arena.c`:

```c
#include <stdint.h>
#include "nxmt/arena.h"

static int expect_u64(uint64_t got, uint64_t want) {
    return got == want ? 0 : 1;
}

int main(void) {
    NxmtArena arena = nxmt_arena_from_range((void*)0x1003u, 0x5005u);
    int failed = 0;

    failed |= expect_u64((uint64_t)(uintptr_t)arena.base, 0x2000u);
    failed |= expect_u64(arena.size, 0x4000u);
    failed |= expect_u64(arena.words, 0x800u);

    failed |= nxmt_arena_is_large_enough(&arena, 0x4000u) ? 0 : 1;
    failed |= nxmt_arena_is_large_enough(&arena, 0x8000u) ? 1 : 0;

    failed |= expect_u64(nxmt_percent_milli(50, 200), 25000u);
    failed |= expect_u64(nxmt_percent_milli(0, 0), 0u);
    failed |= expect_u64(nxmt_split_block_start(1000, 4, 2), 500u);
    failed |= expect_u64(nxmt_split_block_size(1000, 4, 2), 250u);
    failed |= expect_u64(nxmt_split_block_size(1001, 4, 3), 251u);

    return failed;
}
```

Modify `CMakeLists.txt` by adding:

```cmake
target_sources(nxmt_core PRIVATE source/core/arena.c)

add_executable(test_arena tests/test_arena.c)
target_link_libraries(test_arena PRIVATE nxmt_core)
add_test(NAME test_arena COMMAND test_arena)
```

- [ ] **Step 2: Run arena tests to verify failure**

Run:

```powershell
cmake -S . -B build/host
cmake --build build/host
ctest --test-dir build/host --output-on-failure -R test_arena
```

Expected: build fails because `nxmt/arena.h` is not present.

- [ ] **Step 3: Add arena implementation**

Create `include/nxmt/arena.h`:

```c
#pragma once

#include "nxmt/types.h"

typedef struct NxmtArena {
    uint8_t *base;
    uint64_t size;
    uint64_t words;
} NxmtArena;

NxmtArena nxmt_arena_from_range(void *base, uint64_t size);
bool nxmt_arena_is_large_enough(const NxmtArena *arena, uint64_t minimum_bytes);
uint64_t nxmt_percent_milli(uint64_t numerator, uint64_t denominator);
uint64_t nxmt_split_block_start(uint64_t total_words, uint32_t worker_count, uint32_t worker_index);
uint64_t nxmt_split_block_size(uint64_t total_words, uint32_t worker_count, uint32_t worker_index);
```

Create `source/core/arena.c`:

```c
#include "nxmt/arena.h"

static uint64_t align_up_u64(uint64_t value, uint64_t align) {
    return (value + align - 1u) & ~(align - 1u);
}

static uint64_t align_down_u64(uint64_t value, uint64_t align) {
    return value & ~(align - 1u);
}

NxmtArena nxmt_arena_from_range(void *base, uint64_t size) {
    uint64_t raw_base = (uint64_t)(uintptr_t)base;
    uint64_t aligned_base = align_up_u64(raw_base, NXMT_PAGE_BYTES);
    uint64_t raw_end = raw_base + size;
    uint64_t aligned_end = align_down_u64(raw_end, NXMT_PAGE_BYTES);

    NxmtArena arena;
    if (aligned_end <= aligned_base) {
        arena.base = (uint8_t*)aligned_base;
        arena.size = 0;
        arena.words = 0;
        return arena;
    }

    arena.base = (uint8_t*)aligned_base;
    arena.size = aligned_end - aligned_base;
    arena.words = arena.size / NXMT_WORD_BYTES;
    return arena;
}

bool nxmt_arena_is_large_enough(const NxmtArena *arena, uint64_t minimum_bytes) {
    return arena != 0 && arena->size >= minimum_bytes;
}

uint64_t nxmt_percent_milli(uint64_t numerator, uint64_t denominator) {
    if (denominator == 0) {
        return 0;
    }
    return (numerator * 100000u) / denominator;
}

uint64_t nxmt_split_block_start(uint64_t total_words, uint32_t worker_count, uint32_t worker_index) {
    if (worker_count == 0 || worker_index >= worker_count) {
        return 0;
    }
    return (total_words * worker_index) / worker_count;
}

uint64_t nxmt_split_block_size(uint64_t total_words, uint32_t worker_count, uint32_t worker_index) {
    if (worker_count == 0 || worker_index >= worker_count) {
        return 0;
    }
    uint64_t start = nxmt_split_block_start(total_words, worker_count, worker_index);
    uint64_t end = nxmt_split_block_start(total_words, worker_count, worker_index + 1u);
    return end - start;
}
```

- [ ] **Step 4: Run arena tests to verify pass**

Run:

```powershell
cmake --build build/host
ctest --test-dir build/host --output-on-failure -R "test_arena|test_smoke"
```

Expected with host toolchain installed: tests pass.

- [ ] **Step 5: Commit arena metrics**

```powershell
git add include/nxmt/arena.h source/core/arena.c tests/test_arena.c CMakeLists.txt
git commit -m "feat: add arena alignment and metrics"
```

---

### Task 4: Error Report Aggregation

**Files:**
- Create: `include/nxmt/report.h`
- Create: `source/core/report.c`
- Create: `tests/test_report.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing report tests**

Create `tests/test_report.c`:

```c
#include "nxmt/report.h"

int main(void) {
    NxmtReport report;
    nxmt_report_init(&report);

    int failed = 0;
    failed |= report.error_count == 0 ? 0 : 1;
    failed |= report.has_first_error ? 1 : 0;

    nxmt_report_record_error(&report, NXMT_MODE_MEMORY_LOAD, NXMT_PHASE_RANDOM, 7, 3, 2, 0x80, 0x1111, 0x1011);
    failed |= report.error_count == 1 ? 0 : 1;
    failed |= report.has_first_error ? 0 : 1;
    failed |= report.first.offset == 0x80 ? 0 : 1;
    failed |= report.first.xor_diff == 0x0100 ? 0 : 1;

    nxmt_report_record_error(&report, NXMT_MODE_EXTREME, NXMT_PHASE_ADDRESS, 8, 4, 1, 0x100, 0x2222, 0x3333);
    failed |= report.error_count == 2 ? 0 : 1;
    failed |= report.first.mode == NXMT_MODE_MEMORY_LOAD ? 0 : 1;
    failed |= report.min_error_offset == 0x80 ? 0 : 1;
    failed |= report.max_error_offset == 0x100 ? 0 : 1;

    return failed;
}
```

Modify `CMakeLists.txt` by adding:

```cmake
target_sources(nxmt_core PRIVATE source/core/report.c)

add_executable(test_report tests/test_report.c)
target_link_libraries(test_report PRIVATE nxmt_core)
add_test(NAME test_report COMMAND test_report)
```

- [ ] **Step 2: Run report tests to verify failure**

Run:

```powershell
cmake -S . -B build/host
cmake --build build/host
ctest --test-dir build/host --output-on-failure -R test_report
```

Expected: build fails because `nxmt/report.h` is not present.

- [ ] **Step 3: Add report implementation**

Create `include/nxmt/report.h`:

```c
#pragma once

#include "nxmt/types.h"

typedef struct NxmtError {
    NxmtMode mode;
    NxmtPhase phase;
    uint64_t seed;
    uint64_t pass;
    uint32_t worker_id;
    uint64_t offset;
    uint64_t expected;
    uint64_t actual;
    uint64_t xor_diff;
} NxmtError;

typedef struct NxmtReport {
    bool has_first_error;
    NxmtError first;
    uint64_t error_count;
    uint64_t min_error_offset;
    uint64_t max_error_offset;
    uint64_t bit_diff_or;
} NxmtReport;

void nxmt_report_init(NxmtReport *report);
void nxmt_report_record_error(
    NxmtReport *report,
    NxmtMode mode,
    NxmtPhase phase,
    uint64_t seed,
    uint64_t pass,
    uint32_t worker_id,
    uint64_t offset,
    uint64_t expected,
    uint64_t actual);
```

Create `source/core/report.c`:

```c
#include "nxmt/report.h"

void nxmt_report_init(NxmtReport *report) {
    report->has_first_error = false;
    report->error_count = 0;
    report->min_error_offset = 0;
    report->max_error_offset = 0;
    report->bit_diff_or = 0;
}

void nxmt_report_record_error(
    NxmtReport *report,
    NxmtMode mode,
    NxmtPhase phase,
    uint64_t seed,
    uint64_t pass,
    uint32_t worker_id,
    uint64_t offset,
    uint64_t expected,
    uint64_t actual) {
    uint64_t diff = expected ^ actual;

    if (!report->has_first_error) {
        report->has_first_error = true;
        report->first.mode = mode;
        report->first.phase = phase;
        report->first.seed = seed;
        report->first.pass = pass;
        report->first.worker_id = worker_id;
        report->first.offset = offset;
        report->first.expected = expected;
        report->first.actual = actual;
        report->first.xor_diff = diff;
        report->min_error_offset = offset;
        report->max_error_offset = offset;
    } else {
        if (offset < report->min_error_offset) {
            report->min_error_offset = offset;
        }
        if (offset > report->max_error_offset) {
            report->max_error_offset = offset;
        }
    }

    report->error_count += 1u;
    report->bit_diff_or |= diff;
}
```

- [ ] **Step 4: Run report tests to verify pass**

Run:

```powershell
cmake --build build/host
ctest --test-dir build/host --output-on-failure -R "test_report|test_patterns|test_arena"
```

Expected with host toolchain installed: tests pass.

- [ ] **Step 5: Commit report aggregation**

```powershell
git add include/nxmt/report.h source/core/report.c tests/test_report.c CMakeLists.txt
git commit -m "feat: add memory error reports"
```

---

### Task 5: Portable Runner And Injected Failure Test

**Files:**
- Create: `include/nxmt/runner.h`
- Create: `source/core/runner.c`
- Create: `tests/test_runner.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing runner tests**

Create `tests/test_runner.c`:

```c
#include <stdint.h>
#include <string.h>
#include "nxmt/arena.h"
#include "nxmt/runner.h"

int main(void) {
    uint8_t buffer[12288];
    memset(buffer, 0, sizeof(buffer));

    NxmtArena arena = nxmt_arena_from_range(buffer, sizeof(buffer));
    NxmtReport report;
    NxmtRunStats stats;
    NxmtRunConfig config;

    config.mode = NXMT_MODE_QUICK;
    config.seed = 0x1234;
    config.pass = 0;
    config.worker_id = 0;
    config.worker_count = 1;
    config.inject_mismatch = false;

    nxmt_report_init(&report);
    NxmtStatus status = nxmt_runner_run_pass(&arena, &config, &report, &stats);

    int failed = 0;
    failed |= status == NXMT_STATUS_PASS ? 0 : 1;
    failed |= report.error_count == 0 ? 0 : 1;
    failed |= stats.bytes_verified == arena.size * 3u ? 0 : 1;

    config.inject_mismatch = true;
    nxmt_report_init(&report);
    status = nxmt_runner_run_pass(&arena, &config, &report, &stats);
    failed |= status == NXMT_STATUS_FAIL ? 0 : 1;
    failed |= report.error_count >= 1 ? 0 : 1;

    return failed;
}
```

Modify `CMakeLists.txt` by adding:

```cmake
target_sources(nxmt_core PRIVATE source/core/runner.c)

add_executable(test_runner tests/test_runner.c)
target_link_libraries(test_runner PRIVATE nxmt_core)
add_test(NAME test_runner COMMAND test_runner)
```

- [ ] **Step 2: Run runner tests to verify failure**

Run:

```powershell
cmake -S . -B build/host
cmake --build build/host
ctest --test-dir build/host --output-on-failure -R test_runner
```

Expected: build fails because `nxmt/runner.h` is not present.

- [ ] **Step 3: Add runner implementation**

Create `include/nxmt/runner.h`:

```c
#pragma once

#include "nxmt/arena.h"
#include "nxmt/patterns.h"
#include "nxmt/report.h"

typedef struct NxmtRunConfig {
    NxmtMode mode;
    uint64_t seed;
    uint64_t pass;
    uint32_t worker_id;
    uint32_t worker_count;
    bool inject_mismatch;
} NxmtRunConfig;

typedef struct NxmtRunStats {
    uint64_t bytes_written;
    uint64_t bytes_verified;
    NxmtPhase current_phase;
} NxmtRunStats;

NxmtStatus nxmt_runner_run_pass(
    const NxmtArena *arena,
    const NxmtRunConfig *config,
    NxmtReport *report,
    NxmtRunStats *stats);
```

Create `source/core/runner.c`:

```c
#include "nxmt/runner.h"

static const NxmtPhase quick_phases[] = {
    NXMT_PHASE_FIXED_A,
    NXMT_PHASE_ADDRESS,
    NXMT_PHASE_RANDOM
};

static const NxmtPhase memory_load_phases[] = {
    NXMT_PHASE_FIXED_A,
    NXMT_PHASE_FIXED_5,
    NXMT_PHASE_CHECKER,
    NXMT_PHASE_ADDRESS,
    NXMT_PHASE_RANDOM,
    NXMT_PHASE_WALKING
};

static uint32_t nxmt_phase_count_for_mode(NxmtMode mode) {
    if (mode == NXMT_MODE_QUICK) {
        return (uint32_t)(sizeof(quick_phases) / sizeof(quick_phases[0]));
    }
    return (uint32_t)(sizeof(memory_load_phases) / sizeof(memory_load_phases[0]));
}

static NxmtPhase nxmt_phase_for_mode(NxmtMode mode, uint32_t index) {
    if (mode == NXMT_MODE_QUICK) {
        return quick_phases[index];
    }
    return memory_load_phases[index];
}

static void nxmt_write_phase(uint64_t *words, uint64_t start, uint64_t count, NxmtPhase phase, const NxmtRunConfig *config) {
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t word_index = start + i;
        uint64_t offset = word_index * NXMT_WORD_BYTES;
        words[word_index] = nxmt_expected_value(config->seed, phase, config->pass, offset);
    }

    if (config->inject_mismatch && count > 0) {
        words[start] ^= 0x100u;
    }
}

static void nxmt_verify_phase(
    uint64_t *words,
    uint64_t start,
    uint64_t count,
    NxmtPhase phase,
    const NxmtRunConfig *config,
    NxmtReport *report) {
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t word_index = start + i;
        uint64_t offset = word_index * NXMT_WORD_BYTES;
        uint64_t expected = nxmt_expected_value(config->seed, phase, config->pass, offset);
        uint64_t actual = words[word_index];
        if (actual != expected) {
            nxmt_report_record_error(report, config->mode, phase, config->seed, config->pass, config->worker_id, offset, expected, actual);
        }
    }
}

NxmtStatus nxmt_runner_run_pass(
    const NxmtArena *arena,
    const NxmtRunConfig *config,
    NxmtReport *report,
    NxmtRunStats *stats) {
    if (arena == 0 || arena->base == 0 || arena->words == 0 || config == 0 || report == 0 || stats == 0) {
        return NXMT_STATUS_UNSUPPORTED;
    }

    stats->bytes_written = 0;
    stats->bytes_verified = 0;
    stats->current_phase = NXMT_PHASE_FIXED_A;

    uint64_t start = nxmt_split_block_start(arena->words, config->worker_count, config->worker_id);
    uint64_t count = nxmt_split_block_size(arena->words, config->worker_count, config->worker_id);
    uint64_t *words = (uint64_t*)arena->base;
    uint32_t phase_count = nxmt_phase_count_for_mode(config->mode);

    for (uint32_t p = 0; p < phase_count; ++p) {
        NxmtPhase phase = nxmt_phase_for_mode(config->mode, p);
        stats->current_phase = phase;
        nxmt_write_phase(words, start, count, phase, config);
        stats->bytes_written += count * NXMT_WORD_BYTES;
        nxmt_verify_phase(words, start, count, phase, config, report);
        stats->bytes_verified += count * NXMT_WORD_BYTES;
    }

    return report->error_count == 0 ? NXMT_STATUS_PASS : NXMT_STATUS_FAIL;
}
```

- [ ] **Step 4: Run full host test suite**

Run:

```powershell
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

Expected with host toolchain installed: all host tests pass.

- [ ] **Step 5: Commit portable runner**

```powershell
git add include/nxmt/runner.h source/core/runner.c tests/test_runner.c CMakeLists.txt
git commit -m "feat: add portable memory test runner"
```

---

### Task 6: libnx Platform Layer And NRO Build

**Files:**
- Create: `include/nxmt/platform.h`
- Create: `source/nx/platform_nx.c`
- Create: `source/nx/main.c`
- Create: `Makefile`

- [ ] **Step 1: Add Switch platform API**

Create `include/nxmt/platform.h`:

```c
#pragma once

#include "nxmt/arena.h"

typedef struct NxmtPlatformMemory {
    void *override_heap_addr;
    uint64_t override_heap_size;
    uint64_t switch_total_memory;
    bool has_heap_override;
    bool has_switch_total;
} NxmtPlatformMemory;

void nxmt_platform_get_memory(NxmtPlatformMemory *out);
uint64_t nxmt_platform_seed64(void);
uint64_t nxmt_platform_ticks_ms(void);
void nxmt_platform_console_init(void);
void nxmt_platform_console_exit(void);
void nxmt_platform_print(const char *fmt, ...);
bool nxmt_platform_should_quit(void);
bool nxmt_platform_write_report(const char *text);
```

- [ ] **Step 2: Add libnx implementation with internal runtime heap**

Create `source/nx/platform_nx.c`:

```c
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include "nxmt/platform.h"

static unsigned char g_internal_heap[4 * 1024 * 1024];

void __libnx_initheap(void) {
    extern char *fake_heap_start;
    extern char *fake_heap_end;
    fake_heap_start = (char*)g_internal_heap;
    fake_heap_end = (char*)g_internal_heap + sizeof(g_internal_heap);
}

void nxmt_platform_get_memory(NxmtPlatformMemory *out) {
    memset(out, 0, sizeof(*out));
    out->has_heap_override = envHasHeapOverride();
    if (out->has_heap_override) {
        out->override_heap_addr = envGetHeapOverrideAddr();
        out->override_heap_size = envGetHeapOverrideSize();
    }

    uint64_t total = 0;
    uint64_t pool = 0;
    Result rc0 = svcGetSystemInfo(&pool, SystemInfoType_TotalPhysicalMemorySize, INVALID_HANDLE, PhysicalMemorySystemInfo_Application);
    if (R_SUCCEEDED(rc0)) {
        total += pool;
        svcGetSystemInfo(&pool, SystemInfoType_TotalPhysicalMemorySize, INVALID_HANDLE, PhysicalMemorySystemInfo_Applet);
        total += pool;
        svcGetSystemInfo(&pool, SystemInfoType_TotalPhysicalMemorySize, INVALID_HANDLE, PhysicalMemorySystemInfo_System);
        total += pool;
        svcGetSystemInfo(&pool, SystemInfoType_TotalPhysicalMemorySize, INVALID_HANDLE, PhysicalMemorySystemInfo_SystemUnsafe);
        total += pool;
        out->switch_total_memory = total;
        out->has_switch_total = total != 0;
    }
}

uint64_t nxmt_platform_seed64(void) {
    uint64_t seed_pair[2] = {0, 0};
    if (envHasRandomSeed()) {
        envGetRandomSeed(seed_pair);
        return seed_pair[0] ^ seed_pair[1];
    }
    return armGetSystemTick();
}

uint64_t nxmt_platform_ticks_ms(void) {
    return armGetSystemTick() / 19200ull;
}

void nxmt_platform_console_init(void) {
    consoleInit(NULL);
}

void nxmt_platform_console_exit(void) {
    consoleExit(NULL);
}

void nxmt_platform_print(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    consoleUpdate(NULL);
}

bool nxmt_platform_should_quit(void) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    padUpdate(&pad);
    uint64_t down = padGetButtonsDown(&pad);
    return (down & HidNpadButton_Plus) != 0;
}

bool nxmt_platform_write_report(const char *text) {
    fsdevCreateFile("sdmc:/switch/NX-MemTest/logs/latest.txt", 0, 0);
    FILE *f = fopen("sdmc:/switch/NX-MemTest/logs/latest.txt", "wb");
    if (!f) {
        return false;
    }
    size_t len = strlen(text);
    bool ok = fwrite(text, 1, len, f) == len;
    fclose(f);
    return ok;
}
```

- [ ] **Step 3: Add minimal NRO main**

Create `source/nx/main.c`:

```c
#include <stdio.h>
#include "nxmt/arena.h"
#include "nxmt/platform.h"
#include "nxmt/runner.h"

static void print_size_line(const char *label, uint64_t bytes) {
    nxmt_platform_print("%s: %llu MiB\n", label, (unsigned long long)(bytes / NXMT_MIB_BYTES));
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    nxmt_platform_console_init();

    NxmtPlatformMemory memory;
    nxmt_platform_get_memory(&memory);

    nxmt_platform_print("NX-MemTest\n");
    nxmt_platform_print("NRO full-memory stress test\n\n");

    if (!memory.has_heap_override) {
        nxmt_platform_print("No OverrideHeap detected.\n");
        nxmt_platform_print("Run hbmenu through title override/full-memory mode.\n");
        nxmt_platform_print("Press PLUS to exit.\n");
        while (appletMainLoop() && !nxmt_platform_should_quit()) {
        }
        nxmt_platform_console_exit();
        return 0;
    }

    NxmtArena arena = nxmt_arena_from_range(memory.override_heap_addr, memory.override_heap_size);
    print_size_line("Test Arena", arena.size);
    if (memory.has_switch_total) {
        print_size_line("Switch Total", memory.switch_total_memory);
        nxmt_platform_print("Physical Coverage: %llu.%03llu%%\n",
            (unsigned long long)(nxmt_percent_milli(arena.size, memory.switch_total_memory) / 1000ull),
            (unsigned long long)(nxmt_percent_milli(arena.size, memory.switch_total_memory) % 1000ull));
    }

    NxmtReport report;
    NxmtRunStats stats;
    NxmtRunConfig config;
    nxmt_report_init(&report);

    config.mode = NXMT_MODE_QUICK;
    config.seed = nxmt_platform_seed64();
    config.pass = 0;
    config.worker_id = 0;
    config.worker_count = 1;
    config.inject_mismatch = false;

    nxmt_platform_print("\nRunning Quick Check pass...\n");
    uint64_t started = nxmt_platform_ticks_ms();
    NxmtStatus status = nxmt_runner_run_pass(&arena, &config, &report, &stats);
    uint64_t elapsed = nxmt_platform_ticks_ms() - started;

    nxmt_platform_print("System Stress Pass: 100.000%%\n");
    nxmt_platform_print("Verified Arena: 100.000%% of %llu MiB\n", (unsigned long long)(arena.size / NXMT_MIB_BYTES));
    nxmt_platform_print("Tested: %llu MiB\n", (unsigned long long)(stats.bytes_verified / NXMT_MIB_BYTES));
    nxmt_platform_print("Elapsed: %llu ms\n", (unsigned long long)elapsed);
    nxmt_platform_print("Errors: %llu\n", (unsigned long long)report.error_count);
    nxmt_platform_print("Status: %s\n", status == NXMT_STATUS_PASS ? "PASS" : "FAIL");
    nxmt_platform_print("\nPress PLUS to exit.\n");

    while (appletMainLoop() && !nxmt_platform_should_quit()) {
    }

    nxmt_platform_console_exit();
    return status == NXMT_STATUS_PASS ? 0 : 1;
}
```

- [ ] **Step 4: Add devkitPro Makefile**

Create `Makefile`:

```make
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to devkitpro>")
endif

include $(DEVKITPRO)/libnx/switch_rules

TARGET := NX-MemTest
BUILD := build/nx
SOURCES := source/core source/nx
INCLUDES := include
APP_TITLE := NX-MemTest
APP_AUTHOR := Codex
APP_VERSION := 0.1.0

CFLAGS := -g -Wall -Wextra -O2 -ffunction-sections -fdata-sections
CFLAGS += $(ARCH) $(DEFINES)
CFLAGS += $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir))
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS := -g $(ARCH)
LDFLAGS = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS := -lnx

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
OFILES := $(CFILES:.c=.o)

.PHONY: all clean

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -rf $(BUILD) $(TARGET).elf $(TARGET).nro $(TARGET).nacp $(TARGET).map

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).nro: $(OUTPUT).elf

$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
```

- [ ] **Step 5: Run Switch build check**

Run:

```powershell
pwsh -File scripts/check-toolchain.ps1
make
```

Expected with devkitPro/devkitA64/libnx installed: `NX-MemTest.nro` is produced.

Expected on the current machine before installing devkitPro: the check script prints missing toolchain entries and exits non-zero.

- [ ] **Step 6: Commit NX platform build**

```powershell
git add include/nxmt/platform.h source/nx/platform_nx.c source/nx/main.c Makefile
git commit -m "feat: add libnx NRO shell"
```

---

### Task 7: Console Modes, Progress Display, And Plain Text Logs

**Files:**
- Modify: `source/nx/main.c`
- Modify: `source/nx/platform_nx.c`
- Modify: `include/nxmt/platform.h`

- [ ] **Step 1: Add mode selection and report formatting**

Replace `source/nx/main.c` with:

```c
#include <stdio.h>
#include <string.h>
#include "nxmt/arena.h"
#include "nxmt/platform.h"
#include "nxmt/runner.h"

static const char *mode_name(NxmtMode mode) {
    switch (mode) {
    case NXMT_MODE_QUICK: return "Quick Check";
    case NXMT_MODE_MEMORY_LOAD: return "Memory Load";
    case NXMT_MODE_EXTREME: return "Extreme";
    }
    return "Unknown";
}

static void print_percent(const char *label, uint64_t milli) {
    nxmt_platform_print("%s: %llu.%03llu%%\n",
        label,
        (unsigned long long)(milli / 1000ull),
        (unsigned long long)(milli % 1000ull));
}

static bool choose_mode(uint64_t arena_size, NxmtMode *out_mode) {
    nxmt_platform_print("Select mode:\n");
    nxmt_platform_print("A: Quick Check\n");
    nxmt_platform_print("X: Memory Load\n");
    nxmt_platform_print("Y: Extreme\n");
    nxmt_platform_print("PLUS: Exit\n");

    while (appletMainLoop()) {
        NxmtInput input = nxmt_platform_read_input();
        if (input.plus) {
            return false;
        }
        if (input.a) {
            *out_mode = NXMT_MODE_QUICK;
            return true;
        }
        if (input.x && arena_size >= 256ull * NXMT_MIB_BYTES) {
            *out_mode = NXMT_MODE_MEMORY_LOAD;
            return true;
        }
        if (input.y && arena_size >= 512ull * NXMT_MIB_BYTES) {
            *out_mode = NXMT_MODE_EXTREME;
            return true;
        }
    }
    return false;
}

static uint32_t worker_count_for_mode(NxmtMode mode) {
    return mode == NXMT_MODE_EXTREME ? 3u : 1u;
}

static void format_report(char *out, size_t out_size, NxmtMode mode, uint64_t seed, const NxmtArena *arena, const NxmtPlatformMemory *memory, const NxmtReport *report, const NxmtRunStats *stats, NxmtStatus status, uint64_t elapsed_ms) {
    snprintf(out, out_size,
        "NX-MemTest Report\n"
        "Mode: %s\n"
        "Seed: 0x%016llx\n"
        "Status: %s\n"
        "Test Arena MiB: %llu\n"
        "Switch Total MiB: %llu\n"
        "Physical Coverage MilliPercent: %llu\n"
        "Bytes Verified: %llu\n"
        "Elapsed ms: %llu\n"
        "Errors: %llu\n"
        "First Error Offset: 0x%llx\n"
        "First Error Expected: 0x%016llx\n"
        "First Error Actual: 0x%016llx\n"
        "First Error XorDiff: 0x%016llx\n",
        mode_name(mode),
        (unsigned long long)seed,
        status == NXMT_STATUS_PASS ? "PASS" : "FAIL",
        (unsigned long long)(arena->size / NXMT_MIB_BYTES),
        (unsigned long long)(memory->switch_total_memory / NXMT_MIB_BYTES),
        (unsigned long long)nxmt_percent_milli(arena->size, memory->switch_total_memory),
        (unsigned long long)stats->bytes_verified,
        (unsigned long long)elapsed_ms,
        (unsigned long long)report->error_count,
        (unsigned long long)(report->has_first_error ? report->first.offset : 0),
        (unsigned long long)(report->has_first_error ? report->first.expected : 0),
        (unsigned long long)(report->has_first_error ? report->first.actual : 0),
        (unsigned long long)(report->has_first_error ? report->first.xor_diff : 0));
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    nxmt_platform_console_init();

    NxmtPlatformMemory memory;
    nxmt_platform_get_memory(&memory);

    nxmt_platform_print("NX-MemTest\n\n");
    if (!memory.has_heap_override) {
        nxmt_platform_print("No OverrideHeap detected.\n");
        nxmt_platform_print("Run hbmenu through title override/full-memory mode.\n");
        nxmt_platform_print("Press PLUS to exit.\n");
        while (appletMainLoop() && !nxmt_platform_read_input().plus) {
        }
        nxmt_platform_console_exit();
        return 0;
    }

    NxmtArena arena = nxmt_arena_from_range(memory.override_heap_addr, memory.override_heap_size);
    nxmt_platform_print("Test Arena: %llu MiB\n", (unsigned long long)(arena.size / NXMT_MIB_BYTES));
    if (memory.has_switch_total) {
        nxmt_platform_print("Switch Total: %llu MiB\n", (unsigned long long)(memory.switch_total_memory / NXMT_MIB_BYTES));
        print_percent("Physical Coverage", nxmt_percent_milli(arena.size, memory.switch_total_memory));
    }

    NxmtMode mode = NXMT_MODE_QUICK;
    if (!choose_mode(arena.size, &mode)) {
        nxmt_platform_console_exit();
        return 0;
    }
    uint64_t seed = nxmt_platform_seed64();
    uint32_t workers = worker_count_for_mode(mode);

    NxmtReport report;
    NxmtRunStats total_stats;
    nxmt_report_init(&report);
    memset(&total_stats, 0, sizeof(total_stats));

    uint64_t started = nxmt_platform_ticks_ms();
    NxmtStatus status = NXMT_STATUS_PASS;

    for (uint32_t worker = 0; worker < workers; ++worker) {
        NxmtRunConfig config;
        NxmtRunStats stats;
        config.mode = mode;
        config.seed = seed;
        config.pass = 0;
        config.worker_id = worker;
        config.worker_count = workers;
        config.inject_mismatch = false;
        status = nxmt_runner_run_pass(&arena, &config, &report, &stats);
        total_stats.bytes_written += stats.bytes_written;
        total_stats.bytes_verified += stats.bytes_verified;
        if (status == NXMT_STATUS_FAIL) {
            break;
        }
    }

    uint64_t elapsed = nxmt_platform_ticks_ms() - started;
    print_percent("System Stress Pass", 100000u);
    uint64_t verified_for_progress = total_stats.bytes_verified;
    if (verified_for_progress > arena.size) {
        verified_for_progress = arena.size;
    }
    print_percent("Verified Arena", nxmt_percent_milli(verified_for_progress, arena.size));
    nxmt_platform_print("Mode: %s\n", mode_name(mode));
    nxmt_platform_print("Workers: %u\n", workers);
    nxmt_platform_print("Tested: %llu MiB\n", (unsigned long long)(total_stats.bytes_verified / NXMT_MIB_BYTES));
    nxmt_platform_print("Elapsed: %llu ms\n", (unsigned long long)elapsed);
    nxmt_platform_print("Errors: %llu\n", (unsigned long long)report.error_count);
    nxmt_platform_print("Status: %s\n", status == NXMT_STATUS_PASS ? "PASS" : "FAIL");

    char report_text[2048];
    format_report(report_text, sizeof(report_text), mode, seed, &arena, &memory, &report, &total_stats, status, elapsed);
    bool log_ok = nxmt_platform_write_report(report_text);
    nxmt_platform_print("Log: %s\n", log_ok ? "sdmc:/switch/NX-MemTest/logs/latest.txt" : "unavailable");

    nxmt_platform_print("\nPress PLUS to exit.\n");
    while (appletMainLoop() && !nxmt_platform_read_input().plus) {
    }

    nxmt_platform_console_exit();
    return status == NXMT_STATUS_PASS ? 0 : 1;
}
```

- [ ] **Step 2: Add input struct to platform API**

Modify `include/nxmt/platform.h` by adding this before `NxmtPlatformMemory`:

```c
typedef struct NxmtInput {
    bool a;
    bool x;
    bool y;
    bool plus;
} NxmtInput;
```

Add this declaration:

```c
NxmtInput nxmt_platform_read_input(void);
```

- [ ] **Step 3: Replace quit helper with reusable input helper**

Modify `source/nx/platform_nx.c` by replacing `nxmt_platform_should_quit` with:

```c
NxmtInput nxmt_platform_read_input(void) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    padUpdate(&pad);
    uint64_t down = padGetButtonsDown(&pad);

    NxmtInput input;
    input.a = (down & HidNpadButton_A) != 0;
    input.x = (down & HidNpadButton_X) != 0;
    input.y = (down & HidNpadButton_Y) != 0;
    input.plus = (down & HidNpadButton_Plus) != 0;
    return input;
}

bool nxmt_platform_should_quit(void) {
    return nxmt_platform_read_input().plus;
}
```

- [ ] **Step 4: Ensure SD log directory exists**

Modify `nxmt_platform_write_report` in `source/nx/platform_nx.c`:

```c
bool nxmt_platform_write_report(const char *text) {
    mkdir("sdmc:/switch/NX-MemTest", 0777);
    mkdir("sdmc:/switch/NX-MemTest/logs", 0777);

    FILE *f = fopen("sdmc:/switch/NX-MemTest/logs/latest.txt", "wb");
    if (!f) {
        return false;
    }
    size_t len = strlen(text);
    bool ok = fwrite(text, 1, len, f) == len;
    fclose(f);
    return ok;
}
```

Add this include to `source/nx/platform_nx.c`:

```c
#include <sys/stat.h>
```

- [ ] **Step 5: Run host and Switch checks**

Run:

```powershell
cmake --build build/host
ctest --test-dir build/host --output-on-failure
make
```

Expected with full toolchain installed: host tests pass and `NX-MemTest.nro` is produced.

- [ ] **Step 6: Commit UI and logs**

```powershell
git add include/nxmt/platform.h source/nx/platform_nx.c source/nx/main.c
git commit -m "feat: add console modes and logging"
```

---

### Task 8: Extreme Worker Threads And CPU Pressure

**Files:**
- Modify: `include/nxmt/report.h`
- Modify: `source/core/report.c`
- Modify: `tests/test_report.c`
- Modify: `source/core/runner.c`
- Modify: `source/nx/main.c`

- [ ] **Step 1: Add report merge test**

Modify `tests/test_report.c` by adding this block before `return failed;`:

```c
    NxmtReport other;
    nxmt_report_init(&other);
    nxmt_report_record_error(&other, NXMT_MODE_EXTREME, NXMT_PHASE_WALKING, 9, 5, 2, 0x40, 0xf0f0, 0xffff);
    nxmt_report_merge(&report, &other);
    failed |= report.error_count == 3 ? 0 : 1;
    failed |= report.first.mode == NXMT_MODE_MEMORY_LOAD ? 0 : 1;
    failed |= report.min_error_offset == 0x40 ? 0 : 1;
    failed |= report.max_error_offset == 0x100 ? 0 : 1;
```

- [ ] **Step 2: Run report test to verify failure**

Run:

```powershell
cmake --build build/host
ctest --test-dir build/host --output-on-failure -R test_report
```

Expected: build fails because `nxmt_report_merge` is not declared.

- [ ] **Step 3: Add report merge API**

Modify `include/nxmt/report.h` by adding:

```c
void nxmt_report_merge(NxmtReport *dst, const NxmtReport *src);
```

Modify `source/core/report.c` by adding:

```c
void nxmt_report_merge(NxmtReport *dst, const NxmtReport *src) {
    if (src->error_count == 0) {
        return;
    }

    if (!dst->has_first_error && src->has_first_error) {
        dst->has_first_error = true;
        dst->first = src->first;
        dst->min_error_offset = src->min_error_offset;
        dst->max_error_offset = src->max_error_offset;
    } else {
        if (src->min_error_offset < dst->min_error_offset) {
            dst->min_error_offset = src->min_error_offset;
        }
        if (src->max_error_offset > dst->max_error_offset) {
            dst->max_error_offset = src->max_error_offset;
        }
    }

    dst->error_count += src->error_count;
    dst->bit_diff_or |= src->bit_diff_or;
}
```

- [ ] **Step 4: Add CPU pressure in Extreme mode**

Modify `source/core/runner.c` by adding this after the phase arrays:

```c
static volatile uint64_t g_extreme_sink;

static void nxmt_extreme_cpu_pressure(uint64_t value) {
    for (uint32_t i = 0; i < 64u; ++i) {
        value = nxmt_mix64(value + i);
        value ^= value << 13;
        value ^= value >> 7;
        value ^= value << 17;
    }
    g_extreme_sink ^= value;
}
```

Modify `nxmt_write_phase` by adding this after assigning `words[word_index]`:

```c
        if (config->mode == NXMT_MODE_EXTREME) {
            nxmt_extreme_cpu_pressure(words[word_index]);
        }
```

Modify `nxmt_verify_phase` by adding this after reading `actual`:

```c
        if (config->mode == NXMT_MODE_EXTREME) {
            nxmt_extreme_cpu_pressure(actual);
        }
```

- [ ] **Step 5: Run host tests**

Run:

```powershell
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

Expected with host toolchain installed: all host tests pass.

- [ ] **Step 6: Replace sequential NX worker execution with libnx threads**

Modify `source/nx/main.c` by adding this include:

```c
#include <switch.h>
```

Add this block after `worker_count_for_mode`:

```c
typedef struct NxmtWorkerContext {
    const NxmtArena *arena;
    NxmtRunConfig config;
    NxmtReport report;
    NxmtRunStats stats;
    NxmtStatus status;
} NxmtWorkerContext;

static Thread g_worker_threads[3];
static unsigned char g_worker_stacks[3][64 * 1024] __attribute__((aligned(16)));
static NxmtWorkerContext g_worker_contexts[3];

static void nxmt_worker_entry(void *arg) {
    NxmtWorkerContext *ctx = (NxmtWorkerContext*)arg;
    nxmt_report_init(&ctx->report);
    ctx->status = nxmt_runner_run_pass(ctx->arena, &ctx->config, &ctx->report, &ctx->stats);
}
```

Replace the worker loop in `main` with:

```c
    bool threaded = workers > 1u;
    if (threaded) {
        for (uint32_t worker = 0; worker < workers; ++worker) {
            NxmtWorkerContext *ctx = &g_worker_contexts[worker];
            memset(ctx, 0, sizeof(*ctx));
            ctx->arena = &arena;
            ctx->config.mode = mode;
            ctx->config.seed = seed;
            ctx->config.pass = 0;
            ctx->config.worker_id = worker;
            ctx->config.worker_count = workers;
            ctx->config.inject_mismatch = false;
            Result rc = threadCreate(&g_worker_threads[worker], nxmt_worker_entry, ctx, g_worker_stacks[worker], sizeof(g_worker_stacks[worker]), 0x2c, worker);
            if (R_FAILED(rc)) {
                threaded = false;
                break;
            }
        }
    }

    if (threaded) {
        for (uint32_t worker = 0; worker < workers; ++worker) {
            threadStart(&g_worker_threads[worker]);
        }
        for (uint32_t worker = 0; worker < workers; ++worker) {
            threadWaitForExit(&g_worker_threads[worker]);
            threadClose(&g_worker_threads[worker]);
            NxmtWorkerContext *ctx = &g_worker_contexts[worker];
            nxmt_report_merge(&report, &ctx->report);
            total_stats.bytes_written += ctx->stats.bytes_written;
            total_stats.bytes_verified += ctx->stats.bytes_verified;
            if (ctx->status == NXMT_STATUS_FAIL) {
                status = NXMT_STATUS_FAIL;
            }
        }
    } else {
        workers = 1u;
        NxmtRunConfig config;
        NxmtRunStats stats;
        config.mode = mode;
        config.seed = seed;
        config.pass = 0;
        config.worker_id = 0;
        config.worker_count = 1;
        config.inject_mismatch = false;
        status = nxmt_runner_run_pass(&arena, &config, &report, &stats);
        total_stats.bytes_written += stats.bytes_written;
        total_stats.bytes_verified += stats.bytes_verified;
    }
```

- [ ] **Step 7: Run Switch build check**

Run:

```powershell
make
```

Expected with devkitPro/devkitA64/libnx installed: `NX-MemTest.nro` is produced and links successfully.

- [ ] **Step 8: Commit Extreme threading**

```powershell
git add include/nxmt/report.h source/core/report.c tests/test_report.c source/core/runner.c source/nx/main.c
git commit -m "feat: add extreme worker threads"
```

---

### Task 9: Documentation And Final Verification

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Expand run instructions**

Replace `README.md` with:

```markdown
# NX-MemTest

NX-MemTest is an NRO-only Nintendo Switch memory stress and verification app.
It is designed for full-memory homebrew execution through hbmenu title override.

## Modes

- Quick Check: short sanity pass, allowed in small-memory environments.
- Memory Load: maximum arena memory traffic for long-running RAM stability.
- Extreme: memory traffic plus CPU pressure to stress the memory controller,
  thermals, and power stability.

## Progress Labels

- System Stress Pass: reaches 100% when the configured arena completes a pass.
- Verified Arena: bytes directly written, read, and compared.
- Physical Coverage: test arena size divided by Switch total physical memory.

NX-MemTest does not directly verify system-only memory pools. Those regions are
outside the normal NRO addressable arena.

## Host Tests

```powershell
cmake -S . -B build/host
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

## Switch Build

Install devkitPro with devkitA64 and libnx, then run:

```powershell
pwsh -File scripts/check-toolchain.ps1
make
```

The build produces `NX-MemTest.nro`.

## Switch Run

Copy `NX-MemTest.nro` to:

```text
sdmc:/switch/NX-MemTest.nro
```

Launch hbmenu through title override/full-memory mode, then start NX-MemTest.
Applet mode shows a warning and should only be used for Quick Check.

Logs are written to:

```text
sdmc:/switch/NX-MemTest/logs/latest.txt
```
```

- [ ] **Step 2: Run verification commands**

Run:

```powershell
git status --short
pwsh -File scripts/check-toolchain.ps1
cmake -S . -B build/host
cmake --build build/host
ctest --test-dir build/host --output-on-failure
make
```

Expected with all required toolchains installed:

```text
git status --short
```

prints no tracked changes before the README edit and prints `M README.md` after the README edit until committed. The check script exits 0. CTest reports all tests passed. `make` produces `NX-MemTest.nro`.

Expected on the current machine before toolchain installation: `scripts/check-toolchain.ps1` exits non-zero and lists missing host compiler/devkitPro tools. Record this as a verification blocker rather than changing source code.

- [ ] **Step 3: Commit documentation**

```powershell
git add README.md
git commit -m "docs: document build and run workflow"
```

---

## Self-Review Checklist

- Spec coverage: NRO-only scope, direct `OverrideHeap` arena, Quick Check, Memory Load, Extreme worker threads, deterministic seeds, progress labels, first-error reporting, plain text logs, and host tests are covered by Tasks 1-9.
- Placeholder scan: this plan contains no placeholder tokens, incomplete sections, or unnamed files.
- Type consistency: `NxmtMode`, `NxmtPhase`, `NxmtArena`, `NxmtReport`, `NxmtRunConfig`, `NxmtRunStats`, `NxmtPlatformMemory`, and `NxmtInput` are introduced before use.
- Verification risk: the current machine does not expose host C compiler, CMake, make, devkitA64, libnx, or nxlink on PATH. Implementation can still create source files, but build verification requires installing/configuring those tools.
