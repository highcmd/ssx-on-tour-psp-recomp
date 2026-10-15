// sceGe_user: display list management. Lists are executed synchronously when
// enqueued (or when the stall address moves). Control-flow commands are handled
// here; everything else goes to ge_exec_cmd() (the renderer).
#include <cstring>
#include <map>
#include <vector>

#include "kernel.h"

void ge_exec_cmd(u32 cmd, u32 op, u32 pc);  // renderer (ge_render.cpp)

struct GeCallback {
	u32 signalFunc, signalArg, finishFunc, finishArg;
};
static std::map<int, GeCallback> s_ge_cbs;
static int s_next_ge_cb = 1;

struct DisplayList {
	int id;
	u32 start, pc, stall;
	int cbid;
	bool done;
	u32 base;      // BASE register (upper address bits)
	u32 offset;    // OFFSETADDR
	u32 stack[32];
	int sp;
	u32 signalArg;
};
static std::map<int, DisplayList> s_lists;
static int s_next_list = 1;
static u32 s_ge_cmds[256];
u64 g_ge_list_count, g_ge_cmd_count;

u32 ge_get_cmd(int i) { return s_ge_cmds[i & 0xFF]; }

static void ge_finish_cb(int cbid, u32 arg, bool finish) {
	auto it = s_ge_cbs.find(cbid);
	if (it == s_ge_cbs.end()) return;
	u32 f = finish ? it->second.finishFunc : it->second.signalFunc;
	u32 a = finish ? it->second.finishArg : it->second.signalArg;
	if (f) kernel_call_interrupt(f, arg, a, 0);
}

static u32 ge_addr(DisplayList &l, u32 op) { return ((l.base & 0x0F000000) | (op & 0x00FFFFFF)) + l.offset; }

static void run_list(DisplayList &l) {
	while (!l.done) {
		if (l.stall && (l.pc & 0x0FFFFFFF) == (l.stall & 0x0FFFFFFF)) return;  // wait for more commands
		u32 op = LW(l.pc);
		u32 cmd = op >> 24;
		u32 pc = l.pc;
		l.pc += 4;
		g_ge_cmd_count++;
		s_ge_cmds[cmd] = op;
		switch (cmd) {
		case 0x08:  // JUMP
			l.pc = ge_addr(l, op) & ~3u;
			break;
		case 0x09:  // BJUMP (conditional on bounding box; we never cull)
			break;
		case 0x0A:  // CALL
			if (l.sp < 32) l.stack[l.sp++] = l.pc, l.stack[l.sp++] = l.offset;
			l.pc = ge_addr(l, op) & ~3u;
			break;
		case 0x0B:  // RET
			if (l.sp >= 2) { l.offset = l.stack[--l.sp]; l.pc = l.stack[--l.sp]; }
			break;
		case 0x0C:  // END
			l.done = true;
			break;
		case 0x0E:  // SIGNAL
			l.signalArg = op & 0xFFFF;
			ge_finish_cb(l.cbid, op & 0xFFFF, false);
			break;
		case 0x0F:  // FINISH
			ge_exec_cmd(cmd, op, pc);
			ge_finish_cb(l.cbid, op & 0xFFFF, true);
			break;
		case 0x10:  // BASE
			l.base = (op & 0x000F0000) << 8;
			break;
		case 0x13:  // OFFSETADDR
			l.offset = op << 8;
			break;
		case 0x14:  // ORIGIN
			l.offset = pc;
			break;
		default:
			ge_exec_cmd(cmd, op, pc);
			break;
		}
	}
}

static int enqueue(u32 listAddr, u32 stall, int cbid, bool head) {
	int id = s_next_list++;
	DisplayList l{};
	l.id = id;
	l.start = l.pc = listAddr & 0x0FFFFFFF;
	l.stall = stall & 0x0FFFFFFF;
	l.cbid = cbid;
	s_lists[id] = l;
	g_ge_list_count++;
	(void)head;
	run_list(s_lists[id]);
	if (s_lists[id].done) s_lists.erase(id);
	return id;
}

HLE(sceGe_user, sceGeListEnQueue) { RET(enqueue(ARG(0), ARG(1), (int)ARG(2), false)); }
HLE(sceGe_user, sceGeListEnQueueHead) { RET(enqueue(ARG(0), ARG(1), (int)ARG(2), true)); }

HLE(sceGe_user, sceGeListUpdateStallAddr) {
	auto it = s_lists.find((int)ARG(0));
	if (it == s_lists.end()) { RET(0); return; }
	it->second.stall = ARG(1) & 0x0FFFFFFF;
	run_list(it->second);
	if (it->second.done) s_lists.erase(it);
	RET(0);
}

HLE(sceGe_user, sceGeListSync) {
	// mode 0 = wait, 1 = peek. Lists still waiting on a stall are "drawing" (2).
	auto it = s_lists.find((int)ARG(0));
	RET(it == s_lists.end() ? 0 : 2);
}

HLE(sceGe_user, sceGeDrawSync) {
	// Lists with a pending stall never finish by themselves; treat them as done.
	RET(0);
}

HLE(sceGe_user, sceGeBreak) { RET(0); }
HLE(sceGe_user, sceGeContinue) { RET(0); }

HLE(sceGe_user, sceGeSetCallback) {
	u32 p = ARG(0);
	int id = s_next_ge_cb++;
	s_ge_cbs[id] = {LW(p), LW(p + 4), LW(p + 8), LW(p + 12)};
	rt_log(LOG_GE, "SetCallback %d signal %08x finish %08x", id, LW(p), LW(p + 8));
	RET(id);
}
HLE(sceGe_user, sceGeUnsetCallback) { s_ge_cbs.erase((int)ARG(0)); RET(0); }

HLE(sceGe_user, sceGeEdramGetAddr) { RET(0x04000000); }
HLE(sceGe_user, sceGeEdramGetSize) { RET(0x00200000); }
HLE(sceGe_user, sceGeEdramSetAddrTranslation) { RET(0x400); }
HLE(sceGe_user, sceGeGetCmd) { RET(s_ge_cmds[ARG(0) & 0xFF]); }

HLE(sceGe_user, sceGeSaveContext) {
	for (int i = 0; i < 256; i++) SW(ARG(0) + 4 * i, s_ge_cmds[i]);
	RET(0);
}
HLE(sceGe_user, sceGeRestoreContext) {
	for (int i = 0; i < 256; i++) s_ge_cmds[i] = LW(ARG(0) + 4 * i);
	RET(0);
}
