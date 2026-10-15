// Threads, scheduling, waits, semaphores, FPLs, callbacks, alarms.
// Each guest thread runs on its own host fiber (platform.h) with its own CPUState.
// Only one guest thread runs at a time; switches happen inside HLE calls, like
// the PSP's cooperative-at-syscall priority scheduler (interrupts run between).
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "kernel.h"
#include "platform.h"

static u32 VBLANK_US = 16683;   // 59.94 Hz; 120 Hz when the game runs above 60
void kernel_set_vblank_hz(double hz) { VBLANK_US = (u32)(1e6 / hz); }

struct Callback {
	int id;
	std::string name;
	int tid;
	u32 func, common;
	u32 notifyCount = 0, notifyArg = 0;
};

struct Thread {
	int id;
	std::string name;
	u32 entry;
	int prio, initPrio;
	u32 attr;
	u32 stackSize, stackAddr;
	u32 gp;
	enum Status { RUNNING = 1, READY = 2, WAITING = 4, SUSPENDED = 8, DORMANT = 16, DEAD = 32 } status;
	WaitType wait = WAIT_NONE;
	u32 waitId = 0;
	u64 waitDeadline = 0;
	bool waitCB = false;
	u32 waitResult = 0;
	u32 waitTimeoutPtr = 0;
	u32 waitArg = 0;        // e.g. sema count requested
	u32 waitOutPtr = 0;     // e.g. FPL output pointer
	u64 readySeq = 0;
	u32 wakeupCount = 0;
	u32 exitStatus = 0x800201A4;
	CPUState ctx;
	Fiber *fiber = nullptr;
	u32 argSize = 0, argPtr = 0;
	bool pendingCallbacks = false;
};

static const size_t HOST_STACK = 4 << 20;
static std::map<int, std::unique_ptr<Thread>> s_threads;
static std::map<int, Callback> s_callbacks;
static Thread *s_cur;
static Fiber *s_sched_fiber;
static int s_next_uid = 0x100;
static u64 s_ready_seq;
static bool s_exit;
static std::vector<Fiber *> s_dead_fibers;

std::map<int, std::pair<std::string, std::pair<int, int>>> s_semas_dump();

int kernel_new_uid() { return s_next_uid++; }

int kernel_current_thread_id() { return s_cur ? s_cur->id : 0; }
CPUState *kernel_current_ctx() { return s_cur ? &s_cur->ctx : currentMIPS; }
// Quit at once: the GE/raster/audio threads are detached and the game's
// threads live on fibers, so an orderly unwind would only hang.
void kernel_request_exit() {
	s_exit = true;
	rt_log(LOG_RT, "exit requested");
	void platform_shutdown();
	platform_shutdown();
	_exit(0);
}
bool kernel_exit_requested() { return s_exit; }

static Thread *get_thread(int id) {
	if (id == 0 && s_cur) return s_cur;
	auto it = s_threads.find(id);
	return it == s_threads.end() ? nullptr : it->second.get();
}

void kernel_dump_threads() {
	for (auto &[id, t] : s_threads)
		rt_log(LOG_RT, "  thread %d '%s' prio %d status %d wait %d/%x ra=%08x sp=%08x", id, t->name.c_str(),
		       t->prio, t->status, t->wait, t->waitId, t->ctx.r[31], t->ctx.r[29]);
	for (auto &[id, s] : s_semas_dump())
		rt_log(LOG_RT, "  sema %d '%s' count %d/%d", id, s.first.c_str(), s.second.first, s.second.second);
}


// ------------------------------------------------------------- timers
struct Timer {
	u64 when;
	int id;
	std::function<void()> fn;
};
static std::vector<Timer> s_timers;
static int s_next_timer = 1;

int kernel_schedule(u64 when_us, std::function<void()> fn) {
	int id = s_next_timer++;
	s_timers.push_back({when_us, id, std::move(fn)});
	return id;
}
void kernel_unschedule(int id) {
	s_timers.erase(std::remove_if(s_timers.begin(), s_timers.end(), [&](const Timer &t) { return t.id == id; }), s_timers.end());
}

static CPUState s_intr_ctx;
u32 kernel_call_interrupt(u32 func, u32 a0, u32 a1, u32 a2) {
	CPUState *saved = currentMIPS;
	Thread *savedCur = s_cur;
	static u32 intrStack = 0;
	if (!intrStack) intrStack = kernel_alloc(0x10000, 1, 0, "interrupt stack") + 0x10000 - 0x40;
	CPUState &c = s_intr_ctx;
	c.r[4] = a0; c.r[5] = a1; c.r[6] = a2;
	c.r[29] = intrStack;
	c.r[31] = 0;
	c.r[28] = g_image_gp;
	currentMIPS = &c;
	s_cur = nullptr;
	rt_call(&c, func);
	s_cur = savedCur;
	currentMIPS = saved;
	return c.r[2];
}

// ------------------------------------------------------------- scheduling
static void make_ready(Thread *t) {
	t->status = Thread::READY;
	t->readySeq = ++s_ready_seq;
}

static Thread *pick_next() {
	Thread *best = nullptr;
	for (auto &[id, t] : s_threads) {
		if (t->status != Thread::READY) continue;
		if (!best || t->prio < best->prio || (t->prio == best->prio && t->readySeq < best->readySeq)) best = t.get();
	}
	return best;
}

// Switch from the current fiber back to the scheduler.
static void switch_to_scheduler() {
	Thread *t = s_cur;
	fiber_switch(t->fiber, s_sched_fiber);
	// resumed
	s_cur = t;
	currentMIPS = &t->ctx;
}

static void run_callbacks_for(Thread *t);

u32 kernel_wait(WaitType type, u32 id, u64 timeout_us, bool allowCallbacks, u32 timeoutPtr) {
	Thread *t = s_cur;
	if (!t) {
		rt_log_error("kernel_wait(%d) outside of a thread (interrupt context?)", type);
		return SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
	}
	u64 waitStart = rt_time_us();
	t->wait = type;
	t->waitId = id;
	t->waitDeadline = timeout_us ? rt_time_us() + timeout_us : 0;
	t->waitCB = allowCallbacks;
	t->waitTimeoutPtr = timeoutPtr;
	t->waitResult = 0;
	t->status = Thread::WAITING;
	if (allowCallbacks) run_callbacks_for(t);
	while (true) {
		if (t->status == Thread::WAITING || t->status == Thread::READY)
			; // fallthrough
		switch_to_scheduler();
		if (t->pendingCallbacks && t->waitCB && t->wait != WAIT_NONE) {
			run_callbacks_for(t);
			t->status = Thread::WAITING;  // keep waiting after callbacks
			continue;
		}
		break;
	}
	t->wait = WAIT_NONE;
	t->waitCB = false;
	static double traceAt = getenv("SSX_VBTRACE_AT") ? atof(getenv("SSX_VBTRACE_AT")) : -1;
	u64 now = rt_time_us();
	if (traceAt >= 0 && now / 1e6 >= traceAt && ((now - waitStart > 8000 && type != WAIT_VBLANK && type != WAIT_DELAY) || (t->id == 257 && now - waitStart > 500)))
		rt_log(LOG_THREAD, "long wait: '%s' type %d id %x %.1f ms ra=%08x", t->name.c_str(), type, id, (now - waitStart) / 1e3, t->ctx.r[31]);
	return t->waitResult;
}

int kernel_wake_where(WaitType type, u32 id, u32 result, const std::function<bool(int)> &pred) {
	int n = 0;
	for (auto &[tid, t] : s_threads) {
		if (t->status == Thread::WAITING && t->wait == type && t->waitId == id && (!pred || pred(tid))) {
			t->waitResult = result;
			t->wait = WAIT_NONE;
			if (t->waitTimeoutPtr && t->waitDeadline) {
				u64 now = rt_time_us();
				SW(t->waitTimeoutPtr, t->waitDeadline > now ? (u32)(t->waitDeadline - now) : 0);
			}
			make_ready(t.get());
			n++;
		}
	}
	return n;
}

void kernel_maybe_preempt() {
	if (!s_cur) return;
	Thread *n = pick_next();
	if (n && n->prio < s_cur->prio) {
		make_ready(s_cur);
		s_cur->readySeq = 0;  // preempted thread goes to the front of its priority
		switch_to_scheduler();
	}
}

// Called after every HLE function: if a timer/interrupt is due (or the GE
// posted something), let the scheduler run it now, as an interrupt would on
// the PSP, instead of waiting until the current thread blocks.
static u64 s_next_due = 0;
static bool posted_pending();
void kernel_poll_interrupts() {
	if (!s_cur) return;
	u64 now = rt_time_us();
	if (now < s_next_due && !posted_pending()) return;
	make_ready(s_cur);
	s_cur->readySeq = 0;   // resumes first among equals, like after an interrupt
	switch_to_scheduler();
}

void kernel_yield() {
	if (!s_cur) return;
	make_ready(s_cur);
	switch_to_scheduler();
}

// Events posted from other host threads (the GE thread) run here, in order.
static std::mutex s_post_mtx;
static std::condition_variable s_post_cv;
static std::vector<std::function<void()>> s_posted;

void kernel_post_interrupt(std::function<void()> fn) {
	{
		std::lock_guard<std::mutex> lk(s_post_mtx);
		s_posted.push_back(std::move(fn));
	}
	s_post_cv.notify_one();
}

static bool posted_pending() {
	std::lock_guard<std::mutex> lk(s_post_mtx);
	return !s_posted.empty();
}

static void run_posted() {
	std::vector<std::function<void()>> todo;
	{
		std::lock_guard<std::mutex> lk(s_post_mtx);
		todo.swap(s_posted);
	}
	for (auto &f : todo) f();
}

// Sleep until `us` passes or something is posted.
static void idle_wait(u64 us) {
	std::unique_lock<std::mutex> lk(s_post_mtx);
	if (!s_posted.empty()) return;
	s_post_cv.wait_for(lk, std::chrono::microseconds(us));
}

static void process_timers() {
	run_posted();
	u64 now = rt_time_us();
	// thread wait timeouts
	for (auto &[id, t] : s_threads) {
		if (t->status == Thread::WAITING && t->waitDeadline && t->waitDeadline <= now) {
			t->waitResult = t->wait == WAIT_DELAY ? 0 : SCE_KERNEL_ERROR_WAIT_TIMEOUT;
			if (t->waitTimeoutPtr) SW(t->waitTimeoutPtr, 0);
			t->wait = WAIT_NONE;
			make_ready(t.get());
		}
	}
	// generic timers (may add more timers while running)
	for (size_t i = 0; i < s_timers.size();) {
		if (s_timers[i].when <= now) {
			auto fn = std::move(s_timers[i].fn);
			s_timers.erase(s_timers.begin() + i);
			fn();
			i = 0;
		} else {
			i++;
		}
	}
}

static u64 next_deadline();
static void update_next_due() { s_next_due = next_deadline(); }
static u64 next_deadline() {
	u64 d = ~0ull;
	for (auto &[id, t] : s_threads)
		if (t->status == Thread::WAITING && t->waitDeadline) d = std::min(d, t->waitDeadline);
	for (auto &tm : s_timers) d = std::min(d, tm.when);
	return d;
}

static void fiber_main() {
	Thread *t = s_cur;
	CPUState *c = &t->ctx;
	rt_call(c, t->entry);
	kernel_exit_current_thread(c->r[2]);
}

static void reset_context(Thread *t) {
	memset(&t->ctx, 0, sizeof(t->ctx));
	CPUState &c = t->ctx;
	for (int i = 0; i < 128; i++) c.vi[i] = 0x7F800001;  // NaN, like the PSP
	c.vfpuCtrl[0] = 0xE4;  // S prefix: identity
	c.vfpuCtrl[1] = 0xE4;  // T prefix
	c.vfpuCtrl[2] = 0;     // D prefix
	c.vfpuCtrl[3] = 0x3F;  // CC
	c.vfpuCtrl[7] = 0x7772CEAB;  // REV (PPSSPP default)
	c.vfpuCtrl[8] = 0x3F800001;  // RCX0..7: PRNG state (PPSSPP defaults)
	c.vfpuCtrl[9] = 0x3F800002;
	c.vfpuCtrl[10] = 0x3F800004;
	c.vfpuCtrl[11] = 0x3F800008;
	c.vfpuCtrl[12] = 0x3F800000;
	c.vfpuCtrl[13] = 0x3F800000;
	c.vfpuCtrl[14] = 0x3F800000;
	c.vfpuCtrl[15] = 0x3F800000;
	u32 top = t->stackAddr + t->stackSize;
	u32 k0 = top - 0x100;
	memset(gptr(k0), 0, 0x100);
	SW(k0 + 0xC0, t->id);
	SW(k0 + 0xF8, 0xFFFFFFFF);
	SW(k0 + 0xFC, 0xFFFFFFFF);
	c.r[26] = k0;
	c.r[29] = k0;
	c.r[28] = t->gp;
	c.r[31] = 0;
	c.fcr31 = 0x00000E00;
}

// (Re)start a thread from its entry point on a fresh fiber. The old one (if the
// thread ran before) is dormant, so it can go at once.
static void start_fiber(Thread *t) {
	fiber_destroy(t->fiber);
	t->fiber = fiber_create(fiber_main, HOST_STACK);
}

int kernel_create_thread(const std::string &name, u32 entry, int prio, u32 stackSize, u32 attr) {
	auto t = std::make_unique<Thread>();
	t->id = kernel_new_uid();
	t->name = name;
	t->entry = entry;
	t->prio = t->initPrio = prio;
	t->attr = attr;
	t->stackSize = (std::max<u32>(stackSize, 0x200) + 0xFF) & ~0xFFu;
	t->stackAddr = kernel_alloc(t->stackSize, 1, 0, ("stack/" + name).c_str());
	if (!t->stackAddr) return SCE_KERNEL_ERROR_NO_MEMORY;
	if (!(attr & 0x00100000))  // !NO_FILLSTACK
		memset(gptr(t->stackAddr), 0xFF, t->stackSize);
	t->gp = g_image_gp;
	t->status = Thread::DORMANT;
	int id = t->id;
	s_threads[id] = std::move(t);
	LOGT("create thread %d '%s' entry %08x prio %d stack %x attr %x", id, name.c_str(), entry, prio, stackSize, attr);
	return id;
}

int kernel_start_thread(int id, u32 argSize, u32 argPtr) {
	Thread *t = get_thread(id);
	if (!t) return SCE_KERNEL_ERROR_UNKNOWN_THID;
	if (t->status != Thread::DORMANT) return SCE_KERNEL_ERROR_NOT_DORMANT;
	reset_context(t);
	CPUState &c = t->ctx;
	if (argPtr && argSize) {
		c.r[29] -= (argSize + 0xF) & ~0xFu;
		memmove(gptr(c.r[29]), gptr(argPtr), argSize);
		c.r[4] = argSize;
		c.r[5] = c.r[29];
	} else {
		c.r[4] = argSize;
		c.r[5] = 0;
	}
	c.r[29] -= 64;
	start_fiber(t);
	make_ready(t);
	LOGT("start thread %d '%s' args %u@%08x", id, t->name.c_str(), argSize, argPtr);
	return 0;
}

void kernel_exit_current_thread(u32 code) {
	Thread *t = s_cur;
	LOGT("thread %d '%s' exits with %08x", t->id, t->name.c_str(), code);
	t->exitStatus = code;
	t->status = Thread::DORMANT;
	t->wait = WAIT_NONE;
	kernel_wake_where(WAIT_THREADEND, t->id, code);
	fiber_switch(t->fiber, s_sched_fiber);  // never resumed; StartThread makes a new fiber
	abort();
}

// ------------------------------------------------------------- callbacks
static void run_callbacks_for(Thread *t) {
	t->pendingCallbacks = false;
	for (auto it = s_callbacks.begin(); it != s_callbacks.end();) {
		Callback &cb = it->second;
		if (cb.tid != t->id || !cb.notifyCount) { ++it; continue; }
		u32 count = cb.notifyCount, arg = cb.notifyArg;
		cb.notifyCount = 0;
		cb.notifyArg = 0;
		CPUState saved = t->ctx;
		CPUState &c = t->ctx;
		c.r[4] = count; c.r[5] = arg; c.r[6] = cb.common;
		c.r[29] -= 64;
		LOGT("run callback %d '%s' func %08x (%u, %08x)", cb.id, cb.name.c_str(), cb.func, count, arg);
		rt_call(&c, cb.func);
		u32 ret = c.r[2];
		t->ctx = saved;
		if (ret) it = s_callbacks.erase(it); else ++it;
	}
}

void kernel_notify_callback(int cbid, u32 arg) {
	auto it = s_callbacks.find(cbid);
	if (it == s_callbacks.end()) return;
	it->second.notifyCount++;
	it->second.notifyArg = arg;
	Thread *t = get_thread(it->second.tid);
	if (t) {
		t->pendingCallbacks = true;
		if (t->status == Thread::WAITING && t->waitCB) make_ready(t);
	}
}

// ------------------------------------------------------------- scheduler loop
void kernel_start_module(u32 entry) {
	// The PSP runs module_start in a fresh thread; SDK crt0 then creates "user_main".
	int id = kernel_create_thread("module_start", entry, 0x20, 0x40000, 0x80000000);
	kernel_start_thread(id, 0, 0);
}

static void vblank_tick(u64 when) {
	display_on_vblank();
	kernel_wake_where(WAIT_VBLANK, 0, 0);
	kernel_schedule(when + VBLANK_US, [when] { vblank_tick(when + VBLANK_US); });
}

void kernel_init() {
	u64 now = rt_time_us();
	kernel_schedule(now + VBLANK_US, [now] { vblank_tick(now + VBLANK_US); });
}

static u64 s_idle_since;
u64 g_prof_guest, g_prof_ge, g_prof_flush;
void kernel_run() {
	s_sched_fiber = fiber_init_thread();
	while (!s_exit) {
		process_timers();
		for (Fiber *f : s_dead_fibers) fiber_destroy(f);
		s_dead_fibers.clear();
		Thread *t = pick_next();
		if (!t) {
			display_pump_events();
			u64 d = next_deadline(), now = rt_time_us();
			if (d == ~0ull) {
				rt_log_error("deadlock: no thread ready and nothing scheduled");
				kernel_dump_threads();
				return;
			}
			static u64 lastReport;
			if (!s_idle_since) s_idle_since = now;
			if (now - s_idle_since > 2000000 && now - lastReport > 2000000) {
				lastReport = now;
				rt_log(LOG_RT, "idle %.1fs: next deadline in %lld us, %zu timers", (now - s_idle_since) / 1e6, (long long)(d - now), s_timers.size());
				kernel_dump_threads();
			}
			if (d > now) idle_wait(std::min<u64>(d - now, 2000));
			continue;
		}
		s_idle_since = 0;
		s_cur = t;
		currentMIPS = &t->ctx;
		t->status = Thread::RUNNING;
		update_next_due();
		u64 t0 = rt_time_us();
		fiber_switch(s_sched_fiber, t->fiber);
		g_prof_guest += rt_time_us() - t0;
		s_cur = nullptr;
	}
}

// =============================================================== HLE: threads
static u32 timeout_from_ptr(u32 p) { return p ? LW(p) : 0; }

HLE(ThreadManForUser, sceKernelCreateThread) {
	int prio = (int)ARG(2);
	if (prio < 8 || prio > 0x77) { RET(SCE_KERNEL_ERROR_ILLEGAL_PRIORITY); return; }
	RET(kernel_create_thread(gstr(ARG(0)), ARG(1), prio, ARG(3), ARG(4)));
}

HLE(ThreadManForUser, sceKernelStartThread) {
	RET(kernel_start_thread((int)ARG(0), ARG(1), ARG(2)));
	kernel_maybe_preempt();
}

HLE(ThreadManForUser, sceKernelExitThread) { kernel_exit_current_thread(ARG(0)); }

HLE(ThreadManForUser, sceKernelExitDeleteThread) {
	s_cur->attr |= 0x40000000;  // mark for deletion
	kernel_exit_current_thread(ARG(0));
}

static int terminate(int id, bool del) {
	Thread *t = get_thread(id);
	if (!t || t == s_cur) return SCE_KERNEL_ERROR_UNKNOWN_THID;
	LOGT("terminate thread %d '%s'%s", t->id, t->name.c_str(), del ? " (delete)" : "");
	t->status = Thread::DORMANT;
	t->wait = WAIT_NONE;
	t->exitStatus = 0x800201AC;
	kernel_wake_where(WAIT_THREADEND, t->id, t->exitStatus);
	if (del) {
		if (t->fiber) s_dead_fibers.push_back(t->fiber);
		kernel_free(t->stackAddr);
		s_threads.erase(t->id);
	}
	return 0;
}

HLE(ThreadManForUser, sceKernelTerminateThread) { RET(terminate((int)ARG(0), false)); }
HLE(ThreadManForUser, sceKernelTerminateDeleteThread) { RET(terminate((int)ARG(0), true)); }

HLE(ThreadManForUser, sceKernelDeleteThread) {
	Thread *t = get_thread((int)ARG(0));
	if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
	if (t->status != Thread::DORMANT) { RET(SCE_KERNEL_ERROR_NOT_DORMANT); return; }
	if (t->fiber) s_dead_fibers.push_back(t->fiber);
	kernel_free(t->stackAddr);
	s_threads.erase(t->id);
	RET(0);
}

HLE(ThreadManForUser, sceKernelGetThreadId) { RET(s_cur ? s_cur->id : 0); }
HLE(ThreadManForUser, sceKernelGetThreadCurrentPriority) { RET(s_cur ? s_cur->prio : 0); }

HLE(ThreadManForUser, sceKernelChangeThreadPriority) {
	Thread *t = get_thread((int)ARG(0));
	if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
	int p = (int)ARG(1);
	if (p == 0) p = s_cur ? s_cur->prio : t->prio;
	t->prio = p;
	RET(0);
	kernel_yield();
}

HLE(ThreadManForUser, sceKernelRotateThreadReadyQueue) {
	RET(0);
	kernel_yield();
}

HLE(ThreadManForUser, sceKernelDelayThread) {
	u32 us = std::max<u32>(ARG(0), 200);
	kernel_wait(WAIT_DELAY, 0, us, false);
	RET(0);
}

HLE(ThreadManForUser, sceKernelDelayThreadCB) {
	u32 us = std::max<u32>(ARG(0), 200);
	kernel_wait(WAIT_DELAY, 0, us, true);
	RET(0);
}

HLE(ThreadManForUser, sceKernelSleepThread) {
	if (s_cur->wakeupCount) { s_cur->wakeupCount--; RET(0); return; }
	RET(kernel_wait(WAIT_SLEEP, 0, 0, false));
}

HLE(ThreadManForUser, sceKernelWakeupThread) {
	Thread *t = get_thread((int)ARG(0));
	if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
	if (t->status == Thread::WAITING && t->wait == WAIT_SLEEP) {
		t->wait = WAIT_NONE;
		t->waitResult = 0;
		make_ready(t);
	} else {
		t->wakeupCount++;
	}
	RET(0);
	kernel_maybe_preempt();
}

HLE(ThreadManForUser, sceKernelSuspendThread) {
	Thread *t = get_thread((int)ARG(0));
	if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
	rt_log(LOG_THREAD, "suspend thread %d (approximated as no-op)", t->id);
	RET(0);
}

HLE(ThreadManForUser, sceKernelWaitThreadEnd) {
	Thread *t = get_thread((int)ARG(0));
	if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
	if (t->status == Thread::DORMANT) { RET(t->exitStatus); return; }
	RET(kernel_wait(WAIT_THREADEND, t->id, timeout_from_ptr(ARG(1)), false, ARG(1)));
}

HLE(ThreadManForUser, sceKernelReferThreadStatus) {
	Thread *t = get_thread((int)ARG(0));
	if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
	u32 p = ARG(1);
	u32 size = LW(p);
	u8 buf[0x68] = {};
	auto put = [&](int off, u32 v) { memcpy(buf + off, &v, 4); };
	put(0, 0x68);
	strncpy((char *)buf + 4, t->name.c_str(), 31);
	put(0x24, t->attr);
	put(0x28, t == s_cur ? 1 : (u32)t->status);
	put(0x2C, t->entry);
	put(0x30, t->stackAddr);
	put(0x34, t->stackSize);
	put(0x38, t->gp);
	put(0x3C, t->initPrio);
	put(0x40, t->prio);
	put(0x44, t->wait);
	put(0x48, t->waitId);
	put(0x4C, t->wakeupCount);
	put(0x50, t->exitStatus);
	memcpy(gptr(p), buf, std::min<u32>(size ? size : 0x68, 0x68));
	RET(0);
}

HLE(ThreadManForUser, sceKernelReferThreadRunStatus) {
	Thread *t = get_thread((int)ARG(0));
	if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
	u32 p = ARG(1);
	SW(p + 4, t == s_cur ? 1 : (u32)t->status);
	SW(p + 8, t->prio);
	SW(p + 12, t->wait);
	SW(p + 16, t->waitId);
	RET(0);
}

// ---------------------------------------------------------------- callbacks
HLE(ThreadManForUser, sceKernelCreateCallback) {
	Callback cb;
	cb.id = kernel_new_uid();
	cb.name = gstr(ARG(0));
	cb.func = ARG(1);
	cb.common = ARG(2);
	cb.tid = s_cur ? s_cur->id : 0;
	s_callbacks[cb.id] = cb;
	LOGT("create callback %d '%s' func %08x", cb.id, cb.name.c_str(), cb.func);
	RET(cb.id);
}

HLE(ThreadManForUser, sceKernelDeleteCallback) {
	RET(s_callbacks.erase((int)ARG(0)) ? 0 : SCE_KERNEL_ERROR_UNKNOWN_CBID);
}

HLE(ThreadManForUser, sceKernelCheckCallback) {
	bool any = false;
	for (auto &[id, cb] : s_callbacks)
		if (cb.tid == s_cur->id && cb.notifyCount) any = true;
	if (any) run_callbacks_for(s_cur);
	RET(any ? 1 : 0);
}

// ---------------------------------------------------------------- semaphores
struct Sema {
	std::string name;
	u32 attr;
	int count, max;
};
static std::map<int, Sema> s_semas;
std::map<int, std::pair<std::string, std::pair<int, int>>> s_semas_dump() {
	std::map<int, std::pair<std::string, std::pair<int, int>>> r;
	for (auto &[id, s] : s_semas) r[id] = {s.name, {s.count, s.max}};
	return r;
}

HLE(ThreadManForUser, sceKernelCreateSema) {
	int id = kernel_new_uid();
	s_semas[id] = {gstr(ARG(0)), ARG(1), (int)ARG(2), (int)ARG(3)};
	LOGT("create sema %d '%s' init %d max %d", id, s_semas[id].name.c_str(), (int)ARG(2), (int)ARG(3));
	RET(id);
}

HLE(ThreadManForUser, sceKernelDeleteSema) {
	int id = (int)ARG(0);
	if (!s_semas.erase(id)) { RET(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
	kernel_wake_where(WAIT_SEMA, id, SCE_KERNEL_ERROR_WAIT_DELETE);
	RET(0);
	kernel_maybe_preempt();
}

static void sema_wake_waiters(int id, Sema &s) {
	// Wake waiters in priority order while the count satisfies them (FIFO attr ignored).
	while (true) {
		Thread *best = nullptr;
		for (auto &[tid, t] : s_threads)
			if (t->status == Thread::WAITING && t->wait == WAIT_SEMA && t->waitId == (u32)id && (int)t->waitArg <= s.count &&
			    (!best || t->prio < best->prio))
				best = t.get();
		if (!best) break;
		s.count -= best->waitArg;
		best->waitResult = 0;
		best->wait = WAIT_NONE;
		make_ready(best);
	}
}

HLE(ThreadManForUser, sceKernelSignalSema) {
	int id = (int)ARG(0), n = (int)ARG(1);
	auto it = s_semas.find(id);
	if (it == s_semas.end()) { RET(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
	Sema &s = it->second;
	if (s.count + n > s.max) { RET(SCE_KERNEL_ERROR_SEMA_OVF); return; }
	s.count += n;
	sema_wake_waiters(id, s);
	RET(0);
	kernel_maybe_preempt();
}

HLE(ThreadManForUser, sceKernelWaitSema) {
	int id = (int)ARG(0), n = (int)ARG(1);
	auto it = s_semas.find(id);
	if (it == s_semas.end()) { RET(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
	if (n <= 0) { RET(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT); return; }
	if (it->second.count >= n) {
		it->second.count -= n;
		RET(0);
		return;
	}
	s_cur->waitArg = n;
	RET(kernel_wait(WAIT_SEMA, id, timeout_from_ptr(ARG(2)), false, ARG(2)));
}

HLE(ThreadManForUser, sceKernelPollSema) {
	int id = (int)ARG(0), n = (int)ARG(1);
	auto it = s_semas.find(id);
	if (it == s_semas.end()) { RET(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
	if (it->second.count >= n) { it->second.count -= n; RET(0); }
	else RET(SCE_KERNEL_ERROR_SEMA_ZERO);
}

// ---------------------------------------------------------------- FPL
struct Fpl {
	std::string name;
	u32 base, blockSize, numBlocks;
	std::vector<bool> used;
};
static std::map<int, Fpl> s_fpls;

HLE(ThreadManForUser, sceKernelCreateFpl) {
	u32 bs = (ARG(3) + 3) & ~3u, n = ARG(4);
	u32 base = kernel_alloc(bs * n, 0, 0, gstr(ARG(0)).c_str());
	if (!base) { RET(SCE_KERNEL_ERROR_NO_MEMORY); return; }
	int id = kernel_new_uid();
	s_fpls[id] = {gstr(ARG(0)), base, bs, n, std::vector<bool>(n)};
	LOGK("create fpl %d '%s' %u x %u at %08x", id, s_fpls[id].name.c_str(), n, bs, base);
	RET(id);
}

static bool fpl_try(Fpl &f, u32 outPtr) {
	for (u32 i = 0; i < f.numBlocks; i++)
		if (!f.used[i]) {
			f.used[i] = true;
			SW(outPtr, f.base + i * f.blockSize);
			return true;
		}
	return false;
}

HLE(ThreadManForUser, sceKernelAllocateFpl) {
	auto it = s_fpls.find((int)ARG(0));
	if (it == s_fpls.end()) { RET(SCE_KERNEL_ERROR_UNKNOWN_FPLID); return; }
	if (fpl_try(it->second, ARG(1))) { RET(0); return; }
	rt_log(LOG_KERNEL, "fpl %d exhausted; waiting", (int)ARG(0));
	s_cur->waitOutPtr = ARG(1);
	RET(kernel_wait(WAIT_FPL, ARG(0), timeout_from_ptr(ARG(2)), false, ARG(2)));
}

// ---------------------------------------------------------------- alarms
struct Alarm {
	u32 handler, common;
	int timer;
};
static std::map<int, Alarm> s_alarms;

static void alarm_fire(int id, u64 when) {
	auto it = s_alarms.find(id);
	if (it == s_alarms.end()) return;
	static double traceAt = getenv("SSX_VBTRACE_AT") ? atof(getenv("SSX_VBTRACE_AT")) : -1;
	static int n;
	u64 now0 = rt_time_us();
	u32 next = kernel_call_interrupt(it->second.handler, it->second.common, 0, 0);
	if (traceAt >= 0 && now0 / 1e6 >= traceAt && n++ < 30)
		rt_log(LOG_THREAD, "alarm %d fired at %.2f ms (scheduled %.2f) -> next %u", id, now0 / 1e3, when / 1e3, next);
	it = s_alarms.find(id);
	if (it == s_alarms.end()) return;
	if (next) {
		// period counts from the scheduled time, not from when we got round to it
		u64 at = std::max<u64>(when + next, rt_time_us() > when + 4 * (u64)next ? rt_time_us() : 0);
		it->second.timer = kernel_schedule(at, [id, at] { alarm_fire(id, at); });
	} else {
		s_alarms.erase(it);
	}
}

HLE(ThreadManForUser, sceKernelSetAlarm) {
	int id = kernel_new_uid();
	s_alarms[id] = {ARG(1), ARG(2), 0};
	u64 at = rt_time_us() + ARG(0);
	s_alarms[id].timer = kernel_schedule(at, [id, at] { alarm_fire(id, at); });
	LOGT("SetAlarm(%u us, handler %08x, common %08x) -> %d", ARG(0), ARG(1), ARG(2), id);
	RET(id);
}

HLE(ThreadManForUser, sceKernelCancelAlarm) {
	auto it = s_alarms.find((int)ARG(0));
	if (it == s_alarms.end()) { RET(SCE_KERNEL_ERROR_UNKNOWN_ALMID); return; }
	kernel_unschedule(it->second.timer);
	s_alarms.erase(it);
	RET(0);
}

// ---------------------------------------------------------------- time
HLE(ThreadManForUser, sceKernelGetSystemTime) {
	u64 t = rt_time_us();
	SW(ARG(0), (u32)t);
	SW(ARG(0) + 4, (u32)(t >> 32));
	RET(0);
}
HLE(ThreadManForUser, sceKernelGetSystemTimeWide) { RET64(rt_time_us()); }
