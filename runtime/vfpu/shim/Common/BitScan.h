#pragma once
#include <cstdint>
inline uint32_t clz32_nonzero(uint32_t v) { return __builtin_clz(v); }
inline uint32_t clz32(uint32_t v) { return v ? __builtin_clz(v) : 32; }
