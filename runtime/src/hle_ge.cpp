// sceGe_user: display lists run on their own host thread, like the PSP's GE runs
// in parallel with the CPU. The CPU side enqueues lists / moves stall addresses
// and returns at once; SIGNAL/FINISH callbacks and list completion come back to
// the scheduler as interrupts (kernel_post_interrupt). DrawSync/ListSync really
// wait. Control-flow commands are handled here; everything else goes to
// ge_exec_cmd() (the renderer, which only ever runs on the GE thread).
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <pthread.h>
#include <vector>

#include "kernel.h"

void ge_exec_cmd(u32 cmd, u32 op, u32 pc);  // renderer (ge_render.cpp)
void ge_flush();                            // draw everything queued so far (waits)
void ge_submit();                           // start rasterising what is queued (no wait)

struct GeCallback {
	u32 signalFunc, signalArg, finishFunc, finishArg;
};
static std::map<int, GeCallback> s_ge_cbs;   // scheduler side only
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
	u64 startedAt;
};

// Shared between the CPU (scheduler) side and the GE thread.
static std::mutex s_mtx;
static std::condition_variable s_cv;
static std::deque<DisplayList> s_queue;  // front = executing
static int s_next_list = 1;
static bool s_busy;                      // GE thread is executing commands
static u32 s_ge_cmds[256];
u64 g_ge_list_count, g_ge_cmd_count;
u64 g_ge_list_us, g_ge_list_max_us;
u32 g_ge_base, g_ge_offset;   // BASE / OFFSETADDR of the list being executed (for VADDR/IADDR)

u32 ge_get_cmd(int i) { return s_ge_cmds[i & 0xFF]; }

static bool trace_on() {
	static double at = getenv("SSX_VBTRACE_AT") ? atof(getenv("SSX_VBTRACE_AT")) : -1;
	static int n;
	return at >= 0 && rt_time_us() / 1e6 >= at && n++ < 200;
}
// ---------------------------------------------------------------- GE thread
static void post_callback(int cbid, u32 arg, bool finish) {
	kernel_post_interrupt([=] {
		auto it = s_ge_cbs.find(cbid);
		if (it == s_ge_cbs.end()) return;
		u32 f = finish ? it->second.finishFunc : it->second.signalFunc;
		u32 a = finish ? it->second.finishArg : it->second.signalArg;
		if (f) kernel_call_interrupt(f, arg, a, 0);
	});
}

static void post_list_state_changed(int id) {
	kernel_post_interrupt([id] {
		kernel_wake_where(WAIT_GE, (u32)id, 0);   // ListSync on this id
		kernel_wake_where(WAIT_GE, 0, 0);         // DrawSync (re-checks itself)
	});
}

static u32 ge_addr(const DisplayList &l, u32 op) { return ((l.base & 0x0F000000) | (op & 0x00FFFFFF)) + l.offset; }

// Execute until the list ends or hits its stall address. Returns true when done.
static bool run_list(DisplayList &l, std::unique_lock<std::mutex> &lk) {
	while (!l.done) {
		if (l.stall && (l.pc & 0x0FFFFFFF) == (l.stall & 0x0FFFFFFF)) return false;
		lk.unlock();
		u32 op = LW(l.pc);
		u32 cmd = op >> 24;
		u32 pc = l.pc;
		l.pc += 4;
		g_ge_cmd_count++;
		s_ge_cmds[cmd] = op;
		g_ge_base = l.base;
		g_ge_offset = l.offset;
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
			ge_flush();
			l.done = true;
			break;
		case 0x0E:  // SIGNAL
			ge_flush();
			post_callback(l.cbid, op & 0xFFFF, false);
			break;
		case 0x0F:  // FINISH
			{ extern u64 g_frames_finished; g_frames_finished++; }
			if (trace_on()) rt_log(LOG_GE, "FINISH (GE thread) t=%.2f", rt_time_us() / 1e3);
			ge_exec_cmd(cmd, op, pc);
			ge_flush();
			post_callback(l.cbid, op & 0xFFFF, true);
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
		lk.lock();
	}
	return true;
}

static void ge_thread_main() {
	pthread_setname_np(pthread_self(), "ge");
	std::unique_lock<std::mutex> lk(s_mtx);
	while (true) {
		s_cv.wait(lk, [] {
			if (s_queue.empty()) return false;
			const DisplayList &l = s_queue.front();
			return !l.stall || (l.pc & 0x0FFFFFFF) != (l.stall & 0x0FFFFFFF);
		});
		s_busy = true;
		DisplayList &l = s_queue.front();
		if (!l.startedAt) l.startedAt = rt_time_us();
		bool done = run_list(l, lk);   // lock held on return
		int id = l.id;
		if (done) {
			extern u64 g_ge_list_us, g_ge_list_max_us;
			u64 d = rt_time_us() - l.startedAt;
			g_ge_list_us += d;
			g_ge_list_max_us = std::max(g_ge_list_max_us, d);
			s_queue.pop_front();
		}
		else { lk.unlock(); ge_submit(); lk.lock(); }   // stalled: keep the workers busy meanwhile
		s_busy = false;
		lk.unlock();
		post_list_state_changed(id);
		lk.lock();
	}
}

static void ensure_thread() {
	static bool started;
	if (started) return;
	started = true;
	std::thread(ge_thread_main).detach();
}

// ---------------------------------------------------------------- CPU side
static bool list_pending(int id) {
	for (auto &l : s_queue) if (l.id == id) return true;
	return false;
}

// Lists waiting on a stall address that is never moved would block forever on
// real hardware too; DrawSync therefore only waits for lists that can make progress.
static bool ge_drawing() {
	if (s_busy) return true;
	for (auto &l : s_queue)
		if (!l.stall || (l.pc & 0x0FFFFFFF) != (l.stall & 0x0FFFFFFF)) return true;
	return false;
}

static int enqueue(u32 listAddr, u32 stall, int cbid, bool head) {
	ensure_thread();
	if (trace_on()) rt_log(LOG_GE, "EnQueue %08x stall %08x t=%.2f", listAddr, stall, rt_time_us() / 1e3);
	std::lock_guard<std::mutex> lk(s_mtx);
	DisplayList l{};
	l.id = s_next_list++;
	l.start = l.pc = listAddr & 0x0FFFFFFF;
	l.stall = stall & 0x0FFFFFFF;
	l.cbid = cbid;
	g_ge_list_count++;
	if (head && !s_queue.empty() && !s_busy) s_queue.push_front(l);
	else if (head && s_queue.size() > 1) s_queue.insert(s_queue.begin() + 1, l);
	else s_queue.push_back(l);
	s_cv.notify_one();
	return l.id;
}

HLE(sceGe_user, sceGeListEnQueue) { RET(enqueue(ARG(0), ARG(1), (int)ARG(2), false)); }
HLE(sceGe_user, sceGeListEnQueueHead) { RET(enqueue(ARG(0), ARG(1), (int)ARG(2), true)); }

HLE(sceGe_user, sceGeListUpdateStallAddr) {
	std::lock_guard<std::mutex> lk(s_mtx);
	for (auto &l : s_queue)
		if (l.id == (int)ARG(0)) l.stall = ARG(1) & 0x0FFFFFFF;
	s_cv.notify_one();
	RET(0);
}

HLE(sceGe_user, sceGeListSync) {
	int id = (int)ARG(0);
	int mode = (int)ARG(1);
	if (trace_on()) rt_log(LOG_GE, "ListSync(%d, %d) ra=%08x t=%.2f", id, mode, c->r[31], rt_time_us() / 1e3);
	while (true) {
		bool pending;
		{
			std::lock_guard<std::mutex> lk(s_mtx);
			pending = list_pending(id);
		}
		if (!pending) { RET(0); return; }          // completed
		if (mode == 1) { RET(2); return; }         // peek: drawing
		kernel_wait(WAIT_GE, (u32)id, 0, false);
	}
}

HLE(sceGe_user, sceGeDrawSync) {
	int mode = (int)ARG(0);
	if (trace_on()) rt_log(LOG_GE, "DrawSync(%d) ra=%08x t=%.2f", mode, c->r[31], rt_time_us() / 1e3);
	while (true) {
		bool drawing;
		{
			std::lock_guard<std::mutex> lk(s_mtx);
			drawing = ge_drawing();
		}
		if (!drawing) { RET(0); return; }
		if (mode == 1) { RET(2); return; }
		kernel_wait(WAIT_GE, 0, 0, false);
	}
}

// Wait (from the CPU side) until the GE thread is idle.
static void ge_idle_wait(CPUState *c) {
	(void)c;
	while (true) {
		{
			std::lock_guard<std::mutex> lk(s_mtx);
			if (!ge_drawing()) return;
		}
		kernel_wait(WAIT_GE, 0, 0, false);
	}
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
HLE(sceGe_user, sceGeGetCmd) { ge_idle_wait(c); RET(s_ge_cmds[ARG(0) & 0xFF]); }

HLE(sceGe_user, sceGeSaveContext) {
	ge_idle_wait(c);
	for (int i = 0; i < 256; i++) SW(ARG(0) + 4 * i, s_ge_cmds[i]);
	RET(0);
}
HLE(sceGe_user, sceGeRestoreContext) {
	ge_idle_wait(c);
	// Replay the state commands into the renderer (no draws / control flow).
	for (int i = 0; i < 256; i++) {
		u32 op = LW(ARG(0) + 4 * i);
		s_ge_cmds[i] = op;
		bool control = i <= 0x0F || i == 0x10 || i == 0x13 || i == 0x14 || i == 0xC4 || i == 0xEA ||
		               (i >= 0x2A && i <= 0x2B) || (i >= 0x3A && i <= 0x41);
		if (!control && (op >> 24) == (u32)i) ge_exec_cmd(i, op, 0);
	}
	RET(0);
}
