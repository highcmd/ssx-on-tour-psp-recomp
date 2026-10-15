#pragma once
// HLE (high-level emulation) of the PSP system libraries the game imports.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

#include "recomp.h"

// PSP calling convention (EABI): args in a0-a3, t0-t3 (r4..r11), result in v0/v1.
#define ARG(i) (c->r[4 + (i)])
#define RET(v) (c->r[2] = (u32)(v))
#define RET64(v) do { u64 _v = (u64)(v); c->r[2] = (u32)_v; c->r[3] = (u32)(_v >> 32); } while (0)

struct HLEReg {
	HLEReg(const char *lib, const char *name, HLEFunc fn);
};

// Defines and registers an HLE function:  HLE(ThreadManForUser, sceKernelDelayThread) { ... }
#define HLE(lib, name)                                                   \
	static void hle_##name(CPUState *c);                                 \
	static HLEReg hle_reg_##name(#lib, #name, hle_##name);              \
	static void hle_##name(CPUState *c)

// Logging. Channels can be filtered with SSX_LOG=kernel,io,... (all by default).
enum LogChan { LOG_RT, LOG_KERNEL, LOG_THREAD, LOG_IO, LOG_DISPLAY, LOG_GE, LOG_AUDIO, LOG_MISC, LOG_UNIMPL, LOG_COUNT };
void rt_log(LogChan ch, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void rt_log_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
bool rt_log_enabled(LogChan ch);
#define LOGK(...) rt_log(LOG_KERNEL, __VA_ARGS__)
#define LOGT(...) rt_log(LOG_THREAD, __VA_ARGS__)
#define LOGIO(...) rt_log(LOG_IO, __VA_ARGS__)

// Guest memory helpers for HLE code.
static inline u8 *gptr(u32 a) { return GADDR(a); }
static inline bool gvalid(u32 a) {
	a &= 0x3FFFFFFF;
	return (a >= 0x08000000 && a < 0x0A000000) || (a >= 0x04000000 && a < 0x04200000) ||
	       (a >= 0x00010000 && a < 0x00014000);
}
std::string gstr(u32 a);
void hle_init_imports();
void install_hle_by_name(const char *lib, const char *name, HLEFunc fn);

// Host time in microseconds since start.
u64 rt_time_us();

// Error codes (public: PPSSPP Core/HLE/ErrorCodes.h)
enum : u32 {
	SCE_KERNEL_ERROR_ILLEGAL_PRIORITY = 0x80020193,
	SCE_KERNEL_ERROR_UNKNOWN_THID = 0x80020198,
	SCE_KERNEL_ERROR_UNKNOWN_SEMID = 0x80020199,
	SCE_KERNEL_ERROR_UNKNOWN_FPLID = 0x8002019A,
	SCE_KERNEL_ERROR_UNKNOWN_CBID = 0x800201A1,
	SCE_KERNEL_ERROR_UNKNOWN_ALMID = 0x800201A3,
	SCE_KERNEL_ERROR_SEMA_ZERO = 0x800201AD,
	SCE_KERNEL_ERROR_SEMA_OVF = 0x800201AE,
	SCE_KERNEL_ERROR_WAIT_TIMEOUT = 0x800201A8,
	SCE_KERNEL_ERROR_WAIT_DELETE = 0x800201B5,
	SCE_KERNEL_ERROR_NOT_DORMANT = 0x800201A4,
	SCE_KERNEL_ERROR_DORMANT = 0x800201A2,
	SCE_KERNEL_ERROR_NO_MEMORY = 0x80020190,
	SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT = 0x800200D2,
	SCE_KERNEL_ERROR_NO_TIMER = 0x800201A9,
	SCE_ERROR_ERRNO_FILE_NOT_FOUND = 0x80010002,
	SCE_ERROR_ERRNO_BAD_FD = 0x80010009,
};
