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
