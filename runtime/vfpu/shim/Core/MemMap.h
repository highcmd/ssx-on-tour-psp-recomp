#pragma once
#include <cstring>
#include "Common/CommonTypes.h"
#include "Core/Opcode.h"
extern u8 *g_mem;   // host base; guest addr & 0x1FFFFFFF indexes it
namespace Memory {
inline u8 *Ptr(u32 a) { return g_mem + (a & 0x1FFFFFFF); }
inline u32 Read_U32(u32 a) { u32 v; memcpy(&v, Ptr(a), 4); return v; }
inline void Write_U32(u32 v, u32 a) { memcpy(Ptr(a), &v, 4); }
inline float Read_Float(u32 a) { float v; memcpy(&v, Ptr(a), 4); return v; }
inline void Write_Float(float v, u32 a) { memcpy(Ptr(a), &v, 4); }
inline const u8 *GetPointerRange(u32 a, u32) { return Ptr(a); }
inline u8 *GetPointerWriteRange(u32 a, u32) { return Ptr(a); }
}
