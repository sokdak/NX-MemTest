#pragma once

#include "nxmt/types.h"

uint64_t nxmt_make_seed(uint32_t high, uint32_t low);
uint64_t nxmt_mix64(uint64_t value);
uint64_t nxmt_expected_value(uint64_t seed, NxmtPhase phase, uint64_t pass, uint64_t offset);
uint64_t nxmt_next_offset(uint64_t seed, uint64_t pass, uint64_t index, uint64_t arena_words);
