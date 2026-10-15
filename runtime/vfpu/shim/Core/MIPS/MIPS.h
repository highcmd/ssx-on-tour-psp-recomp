#pragma once
// Compat replacement for PPSSPP's Core/MIPS/MIPS.h: only what the VFPU interpreter needs.
#include "Common/CommonTypes.h"
#include "Core/Opcode.h"
#include "cpu_state.h"
typedef Memory::Opcode MIPSOpcode;
enum { VFPU_CTRL_SPREFIX, VFPU_CTRL_TPREFIX, VFPU_CTRL_DPREFIX, VFPU_CTRL_CC, VFPU_CTRL_INF4,
       VFPU_CTRL_RSV5, VFPU_CTRL_RSV6, VFPU_CTRL_REV, VFPU_CTRL_RCX0, VFPU_CTRL_RCX1, VFPU_CTRL_RCX2,
       VFPU_CTRL_RCX3, VFPU_CTRL_RCX4, VFPU_CTRL_RCX5, VFPU_CTRL_RCX6, VFPU_CTRL_RCX7, VFPU_CTRL_MAX };
enum VCondition { VC_FL, VC_EQ, VC_LT, VC_LE, VC_TR, VC_NE, VC_GE, VC_GT, VC_EZ, VC_EN, VC_EI, VC_ES,
                  VC_NZ, VC_NN, VC_NI, VC_NS };
typedef CPUState MIPSState;
extern CPUState *currentMIPS;
#define mipsr4k (*currentMIPS)
extern u8 voffset[128];
extern u8 fromvoffset[128];
extern const float cst_constants[32];
