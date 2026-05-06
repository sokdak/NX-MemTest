#pragma once

#include "nxmt/arena.h"

typedef struct NxmtInput {
    bool a;
    bool x;
    bool y;
    bool plus;
} NxmtInput;

typedef struct NxmtPlatformMemory {
    void *override_heap_addr;
    uint64_t override_heap_size;
    uint64_t physical_pools_total;
    uint64_t process_total_memory;
    uint64_t effective_total_memory;
    uint64_t switch_total_memory;
    NxmtMemorySource effective_total_source;
    bool has_heap_override;
    bool has_physical_pools_total;
    bool has_process_total_memory;
    bool has_effective_total;
    bool has_switch_total;
    bool extended_memory_detected;
} NxmtPlatformMemory;

void nxmt_platform_get_memory(NxmtPlatformMemory *out);
uint64_t nxmt_platform_seed64(void);
uint64_t nxmt_platform_ticks_ms(void);
void nxmt_platform_console_init(void);
void nxmt_platform_console_exit(void);
void nxmt_platform_print(const char *fmt, ...);
NxmtInput nxmt_platform_read_input(void);
bool nxmt_platform_should_quit(void);
bool nxmt_platform_write_report(const char *text);
