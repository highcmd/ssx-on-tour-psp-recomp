// SysMem partitions, utils, stdio, module manager, power, misc small modules.
#include <sys/time.h>
#include <ctime>
#include <map>

#include "kernel.h"

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
