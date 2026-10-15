#pragma once
// Guest CPU context. Field names match PPSSPP's MIPSState so the ported VFPU
// interpreter (runtime/vfpu) can use it directly via `currentMIPS`.
#include <cstdint>

struct CPUState {
	uint32_t r[32];
	union {
		float f[32];
		uint32_t fi[32];
		int32_t fs[32];
	};
	union {
		float v[128];
		uint32_t vi[128];
	};
	uint32_t vfpuCtrl[16];
	uint32_t pc, lo, hi;
	uint32_t fcr31;
	uint32_t fpcond;
	uint32_t nextPC;
	int downcount;
	bool inDelaySlot;
	int llBit;
	alignas(16) uint32_t vcmpResult[4];
	float sincostemp[2];

	uint8_t VfpuWriteMask() const { return (vfpuCtrl[2] >> 8) & 0xF; }
	bool VfpuWriteMask(int i) const { return (vfpuCtrl[2] >> (8 + i)) & 1; }
	bool HasDefaultPrefix() const {
		return vfpuCtrl[0] == 0xE4 && vfpuCtrl[1] == 0xE4 && vfpuCtrl[2] == 0;
	}
};
