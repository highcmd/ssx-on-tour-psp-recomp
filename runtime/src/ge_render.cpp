// GE renderer. First milestone: no rasterisation; only clears the framebuffer
// target on FINISH so something is visible and counts what the game draws.
#include <cstring>

#include "kernel.h"

static u64 s_prims;

void ge_exec_cmd(u32 cmd, u32 op, u32 pc) {
	(void)pc;
	(void)op;
	switch (cmd) {
	case 0x04:  // PRIM
	case 0x05:  // BEZIER
	case 0x06:  // SPLINE
		s_prims++;
		break;
	default:
		break;
	}
}

u64 ge_prim_count() { return s_prims; }
