// SysMem partitions, utils, stdio, module manager, power, misc small modules.
#include <sys/time.h>
#include <ctime>
#include <map>

#include "kernel.h"
#include "platform.h"

int kernel_new_uid();

// ------------------------------------------------------------- user partition
// PSP-1000 layout: user partition 0x08800000-0x0A000000 (24 MB). The game image is
// loaded at its bottom (0x08804000); everything else comes from this allocator.
struct Block {
	u32 addr, size;
	std::string name;
};
static std::map<u32, Block> s_blocks;  // addr -> block
static const u32 USER_LO = 0x08800000, USER_HI = 0x0A000000;

static bool range_free(u32 a, u32 size) {
	for (auto &[addr, b] : s_blocks)
		if (a < b.addr + b.size && b.addr < a + size) return false;
	return a >= USER_LO && a + size <= USER_HI;
}

u32 kernel_alloc(u32 size, int type, u32 want, const char *name, u32 align) {
	size = (size + 0xFF) & ~0xFFu;
	if (!size) size = 0x100;
	u32 found = 0;
	if (type == 2) {  // fixed address
		if (range_free(want, size)) found = want;
	} else if (type == 0 || type == 3) {  // low
		u32 a = USER_LO;
		for (auto &[addr, b] : s_blocks) {
			u32 ca = (a + align - 1) & ~(align - 1);
			if (ca + size <= b.addr) break;
			a = std::max(a, b.addr + b.size);
		}
		a = (a + align - 1) & ~(align - 1);
		if (a + size <= USER_HI && range_free(a, size)) found = a;
	} else {  // high
		u32 top = USER_HI;
		for (auto it = s_blocks.rbegin(); it != s_blocks.rend(); ++it) {
			u32 ca = (top - size) & ~(align - 1);
			if (ca >= it->second.addr + it->second.size) break;
			top = std::min(top, it->second.addr);
		}
		u32 a = (top - size) & ~(align - 1);
		if (a >= USER_LO && range_free(a, size)) found = a;
	}
	if (!found) {
		rt_log(LOG_KERNEL, "alloc '%s' %x type %d FAILED (max free %x)", name, size, type, kernel_max_free());
		return 0;
	}
	s_blocks[found] = {found, size, name};
	rt_log(LOG_KERNEL, "alloc '%s' %x type %d -> %08x", name, size, type, found);
	return found;
}

void kernel_free(u32 addr) { s_blocks.erase(addr); }

u32 kernel_max_free() {
	u32 best = 0, a = USER_LO;
	for (auto &[addr, b] : s_blocks) {
		if (b.addr > a) best = std::max(best, b.addr - a);
		a = std::max(a, b.addr + b.size);
	}
	return std::max(best, USER_HI - a);
}

u32 kernel_total_free() {
	u32 used = 0;
	for (auto &[addr, b] : s_blocks) used += b.size;
	return USER_HI - USER_LO - used;
}

void kernel_reserve_image() {
	s_blocks[USER_LO] = {USER_LO, (g_image_base - USER_LO) + ((g_image_size + 0xFF) & ~0xFFu), "game image"};
}

struct PartBlock {
	u32 addr;
};
static std::map<int, PartBlock> s_partblocks;

HLE(SysMemUserForUser, sceKernelAllocPartitionMemory) {
	int part = (int)ARG(0);
	std::string name = gstr(ARG(1));
	int type = (int)ARG(2);
	u32 size = ARG(3), addr = ARG(4);
	u32 a = kernel_alloc(size, type, addr, name.c_str(), type == 3 || type == 4 ? addr : 256);
	(void)part;
	if (!a) { RET(SCE_KERNEL_ERROR_NO_MEMORY); return; }
	int id = kernel_new_uid();
	s_partblocks[id] = {a};
	RET(id);
}

HLE(SysMemUserForUser, sceKernelFreePartitionMemory) {
	auto it = s_partblocks.find((int)ARG(0));
	if (it == s_partblocks.end()) { RET(0x800200CB); return; }
	kernel_free(it->second.addr);
	s_partblocks.erase(it);
	RET(0);
}

HLE(SysMemUserForUser, sceKernelGetBlockHeadAddr) {
	auto it = s_partblocks.find((int)ARG(0));
	RET(it == s_partblocks.end() ? 0 : it->second.addr);
}

HLE(SysMemUserForUser, sceKernelMaxFreeMemSize) { RET(kernel_max_free()); }
HLE(SysMemUserForUser, sceKernelTotalFreeMemSize) { RET(kernel_total_free()); }

// ------------------------------------------------------------- volatile memory (0x08400000, 4 MB)
static bool s_volatile_locked;
HLE(sceSuspendForUser, sceKernelVolatileMemLock) {
	if (s_volatile_locked) { RET(0x802B0200); return; }
	s_volatile_locked = true;
	if (ARG(1)) SW(ARG(1), 0x08400000);
	if (ARG(2)) SW(ARG(2), 0x00400000);
	RET(0);
}
HLE(sceSuspendForUser, sceKernelVolatileMemTryLock) {
	if (s_volatile_locked) { RET(0x802B0200); return; }
	s_volatile_locked = true;
	if (ARG(1)) SW(ARG(1), 0x08400000);
	if (ARG(2)) SW(ARG(2), 0x00400000);
	RET(0);
}
HLE(sceSuspendForUser, sceKernelVolatileMemUnlock) { s_volatile_locked = false; RET(0); }
HLE(sceSuspendForUser, sceKernelPowerLock) { RET(0); }
HLE(sceSuspendForUser, sceKernelPowerUnlock) { RET(0); }
HLE(sceSuspendForUser, sceKernelPowerTick) { RET(0); }

// ------------------------------------------------------------- utils
HLE(UtilsForUser, sceKernelDcacheWritebackAll) { RET(0); }
HLE(UtilsForUser, sceKernelDcacheWritebackInvalidateAll) { RET(0); }
HLE(UtilsForUser, sceKernelDcacheInvalidateRange) { RET(0); }
HLE(UtilsForUser, sceKernelLibcClock) { RET((u32)rt_time_us()); }
HLE(UtilsForUser, sceKernelLibcTime) {
	u32 t = (u32)time(nullptr);
	if (ARG(0)) SW(ARG(0), t);
	RET(t);
}
HLE(UtilsForUser, sceKernelLibcGettimeofday) {
	struct timeval tv;
	gettimeofday(&tv, nullptr);
	if (ARG(0)) { SW(ARG(0), (u32)tv.tv_sec); SW(ARG(0) + 4, (u32)tv.tv_usec); }
	RET(0);
}

HLE(sceRtc, sceRtcGetCurrentTick) {
	// ticks are microseconds since 0001-01-01
	struct timeval tv;
	gettimeofday(&tv, nullptr);
	u64 t = (u64)tv.tv_sec * 1000000ull + tv.tv_usec + 62135596800000000ull;
	SW(ARG(0), (u32)t);
	SW(ARG(0) + 4, (u32)(t >> 32));
	RET(0);
}

// ScePspDateTime: u16 year, month, day, hour, minute, second; u32 microsecond
static void put_datetime(u32 p, const struct tm &t, u32 usec) {
	SH(p, (u16)(t.tm_year + 1900)); SH(p + 2, (u16)(t.tm_mon + 1)); SH(p + 4, (u16)t.tm_mday);
	SH(p + 6, (u16)t.tm_hour); SH(p + 8, (u16)t.tm_min); SH(p + 10, (u16)t.tm_sec);
	SW(p + 12, usec);
}
static void current_clock(u32 p, bool local, int tzMinutes) {
	struct timeval tv;
	gettimeofday(&tv, nullptr);
	time_t secs = tv.tv_sec + (time_t)tzMinutes * 60;
	struct tm t;
	if (local) platform_localtime(secs, &t); else t = *std::gmtime(&secs);
	put_datetime(p, t, (u32)tv.tv_usec);
}
HLE(sceRtc, sceRtcGetCurrentClock) { current_clock(ARG(0), false, (int)ARG(1)); RET(0); }
HLE(sceRtc, sceRtcGetCurrentClockLocalTime) { current_clock(ARG(0), true, 0); RET(0); }

// datetime -> ticks (microseconds since 0001-01-01)
HLE(sceRtc, sceRtcGetTick) {
	u32 p = ARG(0);
	int y = LH(p), mo = LH(p + 2), d = LH(p + 4);
	// days from 0001-01-01 (proleptic Gregorian), via the civil-from-days inverse
	y -= mo <= 2;
	int era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = (unsigned)(y - era * 400);
	unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	s64 days = (s64)era * 146097 + doe - 719468 + 719162;   // 1970-01-01 is day 719162
	u64 t = ((u64)days * 86400 + LH(p + 6) * 3600 + LH(p + 8) * 60 + LH(p + 10)) * 1000000ull + LW(p + 12);
	SW(ARG(1), (u32)t);
	SW(ARG(1) + 4, (u32)(t >> 32));
	RET(0);
}
HLE(sceRtc, sceRtcCompareTick) {
	u64 a = LW(ARG(0)) | ((u64)LW(ARG(0) + 4) << 32), b = LW(ARG(1)) | ((u64)LW(ARG(1) + 4) << 32);
	RET(a < b ? -1 : a > b ? 1 : 0);
}

HLE(UtilsForUser, sceKernelIcacheInvalidateAll) { RET(0); }   // overlays: see rt_call
HLE(scePspNpDrm_user, sceNpDrmSetLicenseeKey) { RET(0); }
HLE(scePspNpDrm_user, sceNpDrmEdataSetupKey) { RET(0); }

// ------------------------------------------------------------- stdio
HLE(StdioForUser, sceKernelStdin) { RET(0); }
HLE(StdioForUser, sceKernelStdout) { RET(1); }
HLE(StdioForUser, sceKernelStderr) { RET(2); }

// ------------------------------------------------------------- modules
// The game loads Sony's own PRX libraries (sc_sascore.prx, videocodec.prx, ...).
// Their functions are HLE'd, so loading just hands out an id.
HLE(ModuleMgrForUser, sceKernelLoadModuleByID) {
	int id = kernel_new_uid();
	LOGK("LoadModuleByID(fd %d) -> %d", (int)ARG(0), id);
	RET(id);
}
HLE(ModuleMgrForUser, sceKernelStartModule) {
	LOGK("StartModule(%d)", (int)ARG(0));
	if (ARG(3)) SW(ARG(3), 0);
	RET(ARG(0));
}
HLE(ModuleMgrForUser, sceKernelStopModule) { RET(0); }
HLE(ModuleMgrForUser, sceKernelUnloadModule) { RET(0); }
HLE(ModuleMgrForUser, sceKernelSelfStopUnloadModule) {
	LOGK("SelfStopUnloadModule");
	kernel_exit_current_thread(0);
}

// ------------------------------------------------------------- exit / power
static int s_exit_cb;
HLE(LoadExecForUser, sceKernelRegisterExitCallback) { s_exit_cb = (int)ARG(0); RET(0); }
HLE(LoadExecForUser, sceKernelExitGame) {
	rt_log(LOG_RT, "sceKernelExitGame");
	kernel_request_exit();
	kernel_wait(WAIT_HLE, 0xDEAD, 0, false);
}
HLE(scePower, scePowerRegisterCallback) { RET(0); }

HLE(sceOpenPSID, sceOpenPSIDGetOpenPSID) {
	for (int i = 0; i < 16; i++) SB(ARG(0) + i, (u8)(0x10 + i));
	RET(0);
}

// ------------------------------------------------------------- UMD
static int s_umd_cb;
HLE(sceUmdUser, sceUmdActivate) { RET(0); if (s_umd_cb) kernel_notify_callback(s_umd_cb, 0x32); }
HLE(sceUmdUser, sceUmdCheckMedium) { RET(1); }
HLE(sceUmdUser, sceUmdGetDriveStat) { RET(0x32); }  // PRESENT | READY | READABLE
HLE(sceUmdUser, sceUmdGetErrorStat) { RET(0); }
HLE(sceUmdUser, sceUmdRegisterUMDCallBack) { s_umd_cb = (int)ARG(0); RET(0); }
HLE(sceUmdUser, sceUmdUnRegisterUMDCallBack) { s_umd_cb = 0; RET(0); }
