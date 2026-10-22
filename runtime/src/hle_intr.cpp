// Sub-interrupt handlers (InterruptManager). Only the VBlank interrupt (30) fires: its
// enabled handlers run as guest interrupts at every vblank, with a0 = sub-interrupt
// number and a1 = the registered argument (as PPSSPP's SubIntrHandler does).
#include <map>

#include "hle.h"
#include "kernel.h"

namespace {
constexpr u32 PSP_VBLANK_INT = 30;
constexpr u32 ERR_ILLEGAL_INTRCODE = 0x80020065;
constexpr u32 ERR_FOUND_HANDLER = 0x80020067;
constexpr u32 ERR_NOTFOUND_HANDLER = 0x80020068;

struct SubIntr {
	u32 handler, arg;
	bool enabled;
};
std::map<std::pair<u32, u32>, SubIntr> s_handlers;
}  // namespace

void intr_on_vblank() {
	for (auto &[key, h] : s_handlers) {
		if (key.first != PSP_VBLANK_INT || !h.enabled || !h.handler) continue;
		u32 sub = key.second, fn = h.handler, arg = h.arg;
		kernel_post_interrupt([sub, fn, arg] { kernel_call_interrupt(fn, sub, arg, 0); });
	}
}

HLE(InterruptManager, sceKernelRegisterSubIntrHandler) {
	u32 intno = ARG(0), sub = ARG(1);
	if (intno >= 67) { RET(ERR_ILLEGAL_INTRCODE); return; }
	auto key = std::make_pair(intno, sub);
	if (s_handlers.count(key)) { RET(ERR_FOUND_HANDLER); return; }
	s_handlers[key] = {ARG(2), ARG(3), false};
	LOGK("register sub-interrupt %u/%u handler %08x arg %08x", intno, sub, ARG(2), ARG(3));
	RET(0);
}

HLE(InterruptManager, sceKernelReleaseSubIntrHandler) {
	RET(s_handlers.erase({ARG(0), ARG(1)}) ? 0 : ERR_NOTFOUND_HANDLER);
}

HLE(InterruptManager, sceKernelEnableSubIntr) {
	auto it = s_handlers.find({ARG(0), ARG(1)});
	if (it == s_handlers.end()) { RET(ERR_NOTFOUND_HANDLER); return; }
	it->second.enabled = true;
	RET(0);
}

HLE(InterruptManager, sceKernelDisableSubIntr) {
	auto it = s_handlers.find({ARG(0), ARG(1)});
	if (it == s_handlers.end()) { RET(ERR_NOTFOUND_HANDLER); return; }
	it->second.enabled = false;
	RET(0);
}
