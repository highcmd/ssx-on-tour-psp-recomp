#pragma once
// PSP kernel emulation: threads (as fibers), scheduling, waits, timers, memory partitions.
#include <functional>
#include <string>

#include "hle.h"

enum WaitType {
	WAIT_NONE = 0,
	WAIT_SLEEP = 1,
	WAIT_DELAY = 2,
	WAIT_SEMA = 3,
	WAIT_EVENTFLAG = 4,
	WAIT_FPL = 10,
	WAIT_THREADEND = 9,
	WAIT_VBLANK = 20,    // not real PSP wait types beyond this point
	WAIT_GE = 21,
	WAIT_AUDIO = 22,
	WAIT_IO = 23,
	WAIT_HLE = 24,
};

void kernel_init();
void kernel_start_module(u32 entry);
void kernel_run();                  // scheduler loop; returns when the game exits
void kernel_request_exit();
bool kernel_exit_requested();

int kernel_current_thread_id();
CPUState *kernel_current_ctx();
void kernel_dump_threads();
[[noreturn]] void kernel_exit_current_thread(u32 code);

// Block the current thread. timeout_us == 0 means no timeout. Returns the wake result.
// `id` identifies the object waited on (sema id, vblank count, ...).
u32 kernel_wait(WaitType type, u32 id, u64 timeout_us, bool allowCallbacks, u32 timeoutPtr = 0);
// Wake every thread waiting on (type, id) for which pred returns true; result goes to v0.
int kernel_wake_where(WaitType type, u32 id, u32 result, const std::function<bool(int tid)> &pred = nullptr);
// Let a higher-priority ready thread run (called after waking someone).
void kernel_maybe_preempt();
// Yield to the scheduler while staying ready (used by polling HLE calls).
void kernel_yield();

// Timed callbacks, executed from the scheduler loop (interrupt context).
int kernel_schedule(u64 when_us, std::function<void()> fn);
void kernel_unschedule(int id);
// Run a guest function in interrupt context (no blocking allowed). Returns v0.
u32 kernel_call_interrupt(u32 func, u32 a0, u32 a1, u32 a2);

// Callbacks
void kernel_notify_callback(int cbid, u32 arg);

// User memory partition
u32 kernel_alloc(u32 size, int type, u32 addr, const char *name, u32 align = 256);
void kernel_free(u32 addr);
u32 kernel_max_free();
u32 kernel_total_free();

// vblank
u32 display_vcount();
void display_on_vblank();
void display_pump_events();
