#pragma once
#include <cstring>
#include "Common/CommonTypes.h"
constexpr int32_t SignExtend16ToS32(uint32_t v) { return (int32_t)(int16_t)(uint16_t)v; }
constexpr uint32_t SignExtend16ToU32(uint32_t v) { return (uint32_t)SignExtend16ToS32(v); }
