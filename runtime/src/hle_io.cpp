// IoFileMgrForUser: disc0:/umd0: -> extracted UMD tree, ms0: -> ./ms0 (saves).
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <ctime>
#include <cstring>
#include <map>
#include <string>

#include "kernel.h"

std::string g_umd_root = "extracted";
std::string g_iso_path;   // raw UMD image for umd0: block access and sce_lbn opens
std::string g_ms_root = "ms0";

struct OpenFile {
	FILE *f = nullptr;
	DIR *d = nullptr;
	std::string path;
	bool blockMode = false;   // umd0: device: read/seek in 2048-byte sectors
	bool raw = false;         // sce_lbn: byte window [rawStart, rawStart+rawSize) of the ISO
	u64 rawStart = 0, rawSize = 0;
	s64 asyncResult = 0;
	bool asyncPending = false;
};
static std::map<int, OpenFile> s_files;
static int s_next_fd = 3;

// Case-insensitive path resolution (UMD is case-insensitive, our extracted copy is not).
static std::string resolve_ci(const std::string &root, const std::string &rel) {
	std::string cur = root;
	size_t i = 0;
	while (i <= rel.size()) {
		size_t j = rel.find('/', i);
		if (j == std::string::npos) j = rel.size();
		std::string part = rel.substr(i, j - i);
		i = j + 1;
		if (part.empty() || part == ".") continue;
		std::string exact = cur + "/" + part;
		struct stat st;
		if (stat(exact.c_str(), &st) == 0) { cur = exact; continue; }
		DIR *d = opendir(cur.c_str());
		bool found = false;
		if (d) {
			while (dirent *e = readdir(d))
				if (!strcasecmp(e->d_name, part.c_str())) { cur += "/" + std::string(e->d_name); found = true; break; }
			closedir(d);
		}
		if (!found) cur = exact;
	}
	return cur;
}

static std::string s_cwd = "disc0:/PSP_GAME/USRDIR";

std::string io_host_path(const std::string &guest) {
	std::string p = guest;
	std::replace(p.begin(), p.end(), '\\', '/');
	size_t colon = p.find(':');
	std::string dev, rel;
	if (colon == std::string::npos) {
		// relative to cwd
		std::string base = s_cwd;
		colon = base.find(':');
		dev = base.substr(0, colon);
		rel = base.substr(colon + 1) + "/" + p;
	} else {
		dev = p.substr(0, colon);
		rel = p.substr(colon + 1);
	}
	for (auto &ch : dev) ch = tolower(ch);
	if (dev.rfind("disc", 0) == 0 || dev.rfind("umd", 0) == 0 || dev.rfind("host", 0) == 0)
		return resolve_ci(g_umd_root, rel);
	if (dev.rfind("ms", 0) == 0 || dev.rfind("fatms", 0) == 0) {
		std::string full = g_ms_root + "/" + rel;
		return full;
	}
	return g_umd_root + "/" + rel;
}

static void mkdirs_for(const std::string &path) {
	for (size_t i = 1; i < path.size(); i++)
		if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
}

static int open_raw(const std::string &g) {
	// "umd0:" (whole disc, sector units) or "<dev>:/sce_lbn0x<sector>_size0x<bytes>"
	size_t colon = g.find(':');
	std::string dev = g.substr(0, colon), rest = g.substr(colon + 1);
	for (auto &ch : dev) ch = tolower(ch);
	bool umdDev = dev == "umd0" || dev == "umd1" || dev == "umd";
	bool whole = umdDev && (rest.empty() || rest == "/");
	size_t lbn = rest.find("sce_lbn");
	if (!whole && lbn == std::string::npos) return -1;
	if (g_iso_path.empty()) {
		rt_log_error("raw UMD open '%s' but no ISO given (--iso)", g.c_str());
		return (int)SCE_ERROR_ERRNO_FILE_NOT_FOUND;
	}
	FILE *f = fopen(g_iso_path.c_str(), "rb");
	if (!f) return (int)SCE_ERROR_ERRNO_FILE_NOT_FOUND;
	OpenFile of;
	of.f = f;
	of.path = g;
	if (whole) {
		of.blockMode = true;
	} else {
		unsigned long long sector = 0, size = 0;
		sscanf(rest.c_str() + lbn, "sce_lbn%llx_size%llx", &sector, &size);
		of.raw = true;
		of.rawStart = sector * 2048;
		of.rawSize = size;
		of.blockMode = umdDev;
		fseeko(f, of.rawStart, SEEK_SET);
	}
	int fd = s_next_fd++;
	s_files[fd] = of;
	LOGIO("open raw '%s' -> %d (block %d, start %llx size %llx)", g.c_str(), fd, of.blockMode,
	      (unsigned long long)of.rawStart, (unsigned long long)of.rawSize);
	return fd;
}

HLE(IoFileMgrForUser, sceIoOpen) {
	std::string g = gstr(ARG(0));
	u32 flags = ARG(1);
	int rfd = open_raw(g);
	if (rfd != -1) { RET(rfd); return; }
	std::string h = io_host_path(g);
	const char *mode = "rb";
	if (flags & 0x0002) {  // write
		if (flags & 0x0200) mode = (flags & 0x0400) ? "w+b" : "w+b";  // CREAT
		else mode = "r+b";
		if (flags & 0x0100) mode = "a+b";  // APPEND
		mkdirs_for(h);
	}
	FILE *f = fopen(h.c_str(), mode);
	if (!f && (flags & 0x0200)) f = fopen(h.c_str(), "w+b");
	if (!f) {
		LOGIO("open '%s' (%s) flags %x -> not found", g.c_str(), h.c_str(), flags);
		RET(SCE_ERROR_ERRNO_FILE_NOT_FOUND);
		return;
	}
	int fd = s_next_fd++;
	s_files[fd].f = f;
	s_files[fd].path = g;
	LOGIO("open '%s' flags %x -> %d", g.c_str(), flags, fd);
	RET(fd);
}

HLE(IoFileMgrForUser, sceIoClose) {
	auto it = s_files.find((int)ARG(0));
	if (it == s_files.end() || !it->second.f) { RET(SCE_ERROR_ERRNO_BAD_FD); return; }
	fclose(it->second.f);
	s_files.erase(it);
	RET(0);
}

static s64 do_read(int fd, u32 buf, u32 size) {
	auto it = s_files.find(fd);
	if (it == s_files.end() || !it->second.f) return (s32)SCE_ERROR_ERRNO_BAD_FD;
	if (!gvalid(buf)) return 0;
	OpenFile &of = it->second;
	if (of.blockMode)
		return (s64)(fread(gptr(buf), 1, (size_t)size * 2048, of.f) / 2048);
	if (of.raw) {
		u64 pos = ftello(of.f) - of.rawStart;
		if (of.rawSize && pos + size > of.rawSize) size = pos < of.rawSize ? (u32)(of.rawSize - pos) : 0;
	}
	return (s64)fread(gptr(buf), 1, size, of.f);
}

HLE(IoFileMgrForUser, sceIoRead) {
	s64 r = do_read((int)ARG(0), ARG(1), ARG(2));
	if (rt_log_enabled(LOG_IO)) LOGIO("read(%d, %08x, %x) -> %llx", (int)ARG(0), ARG(1), ARG(2), (long long)r);
	RET((u32)r);
}

HLE(IoFileMgrForUser, sceIoWrite) {
	int fd = (int)ARG(0);
	u32 buf = ARG(1), size = ARG(2);
	if (fd == 1 || fd == 2) {
		std::string s((const char *)gptr(buf), size);
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
		rt_log(LOG_MISC, "guest %s: %s", fd == 1 ? "stdout" : "stderr", s.c_str());
		RET(size);
		return;
	}
	auto it = s_files.find(fd);
	if (it == s_files.end() || !it->second.f) { RET(SCE_ERROR_ERRNO_BAD_FD); return; }
	RET((u32)fwrite(gptr(buf), 1, size, it->second.f));
}

HLE(IoFileMgrForUser, sceIoLseek) {
	int fd = (int)ARG(0);
	s64 off = (s64)(((u64)ARG(3) << 32) | ARG(2));  // 64-bit offset in a2:a3 (aligned pair)
	int whence = (int)ARG(4);
	auto it = s_files.find(fd);
	if (it == s_files.end() || !it->second.f) { RET64((s64)(s32)SCE_ERROR_ERRNO_BAD_FD); return; }
	OpenFile &of = it->second;
	if (of.blockMode) off *= 2048;
	if (of.raw && whence == 0) off += of.rawStart;
	if (of.raw && whence == 2) { off += of.rawStart + of.rawSize; whence = 0; }
	fseeko(of.f, off, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END);
	s64 pos = ftello(of.f) - (of.raw ? of.rawStart : 0);
	RET64(of.blockMode ? pos / 2048 : pos);
}

HLE(IoFileMgrForUser, sceIoReadAsync) {
	int fd = (int)ARG(0);
	auto it = s_files.find(fd);
	if (it == s_files.end()) { RET(SCE_ERROR_ERRNO_BAD_FD); return; }
	it->second.asyncResult = do_read(fd, ARG(1), ARG(2));
	it->second.asyncPending = true;
	RET(0);
}

static void async_result(CPUState *c, bool wait) {
	auto it = s_files.find((int)ARG(0));
	if (it == s_files.end()) { RET(SCE_ERROR_ERRNO_BAD_FD); return; }
	if (!it->second.asyncPending) { RET(wait ? 0x80020329 : 1); return; }  // no async op
	SW(ARG(1), (u32)it->second.asyncResult);
	SW(ARG(1) + 4, (u32)(it->second.asyncResult >> 32));
	it->second.asyncPending = false;
	RET(0);
}
HLE(IoFileMgrForUser, sceIoWaitAsync) { async_result(c, true); }
HLE(IoFileMgrForUser, sceIoPollAsync) { async_result(c, false); }
HLE(IoFileMgrForUser, sceIoChangeAsyncPriority) { RET(0); }

HLE(IoFileMgrForUser, sceIoDopen) {
	std::string g = gstr(ARG(0));
	DIR *d = opendir(io_host_path(g).c_str());
	if (!d) { LOGIO("dopen '%s' -> not found", g.c_str()); RET(SCE_ERROR_ERRNO_FILE_NOT_FOUND); return; }
	int fd = s_next_fd++;
	s_files[fd].d = d;
	s_files[fd].path = g;
	RET(fd);
}

// ScePspDateTime: year, month, day, hour, minute, second (u16 each), microsecond (u32)
static void put_psp_time(u32 a, time_t t) {
	struct tm tmv;
	localtime_r(&t, &tmv);
	u16 v[6] = {(u16)(tmv.tm_year + 1900), (u16)(tmv.tm_mon + 1), (u16)tmv.tm_mday, (u16)tmv.tm_hour, (u16)tmv.tm_min, (u16)tmv.tm_sec};
	memcpy(gptr(a), v, 12);
	SW(a + 12, 0);
}

HLE(IoFileMgrForUser, sceIoDread) {
	auto it = s_files.find((int)ARG(0));
	if (it == s_files.end() || !it->second.d) { RET(SCE_ERROR_ERRNO_BAD_FD); return; }
	dirent *e;
	do { e = readdir(it->second.d); } while (e && (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")));
	if (!e) { RET(0); return; }
	u32 p = ARG(1);
	// SceIoDirent: SceIoStat (mode, attr, size64, ctime, atime, mtime, private[6]) + name[256] + d_private
	memset(gptr(p), 0, 0x58 + 256);
	struct stat st{};
	std::string full = io_host_path(it->second.path) + "/" + e->d_name;
	stat(full.c_str(), &st);
	bool dir = S_ISDIR(st.st_mode) || e->d_type == DT_DIR;
	SW(p + 0, dir ? 0x11FF : 0x21FF);
	SW(p + 4, dir ? 0x10 : 0x20);
	SW(p + 8, (u32)st.st_size);
	SW(p + 12, (u32)((u64)st.st_size >> 32));
	put_psp_time(p + 16, st.st_ctime);
	put_psp_time(p + 32, st.st_atime);
	put_psp_time(p + 48, st.st_mtime);
	strncpy((char *)gptr(p + 0x58), e->d_name, 255);
	RET(1);
}

HLE(IoFileMgrForUser, sceIoDclose) {
	auto it = s_files.find((int)ARG(0));
	if (it == s_files.end() || !it->second.d) { RET(SCE_ERROR_ERRNO_BAD_FD); return; }
	closedir(it->second.d);
	s_files.erase(it);
	RET(0);
}

HLE(IoFileMgrForUser, sceIoRename) {
	RET(rename(io_host_path(gstr(ARG(0))).c_str(), io_host_path(gstr(ARG(1))).c_str()) == 0 ? 0 : SCE_ERROR_ERRNO_FILE_NOT_FOUND);
}

HLE(IoFileMgrForUser, sceIoDevctl) {
	std::string dev = gstr(ARG(0));
	u32 cmd = ARG(1), out = ARG(4), outlen = ARG(5);
	if (cmd != 0x02025806)  // the game polls "inserted?" every few frames
		LOGIO("devctl '%s' cmd %08x in %08x/%x out %08x/%x", dev.c_str(), cmd, ARG(2), ARG(3), out, outlen);
	if (dev.rfind("ms", 0) == 0 || dev.rfind("fatms", 0) == 0 || dev.rfind("mscmhc", 0) == 0) {
		switch (cmd) {
		case 0x02425818:  // free space: struct pointer in input
			if (ARG(2) && gvalid(ARG(2))) {
				u32 p = LW(ARG(2));
				SW(p + 0, 0x10000);    // max clusters (2 GB stick)
				SW(p + 4, 0x8000);     // free clusters (1 GB; must not overflow u32 bytes)
				SW(p + 8, 0x8000);
				SW(p + 12, 512);       // sector size
				SW(p + 16, 64);        // sectors per cluster
			}
			RET(0);
			return;
		case 0x02025806:  // inserted? 1 = yes, 2 = no
			if (out) SW(out, 1);
			RET(0);
			return;
		case 0x02025801:  // driver state: 4 = stick inserted, 1 = driver ready only
			if (out) SW(out, 4);
			RET(0);
			return;
		}
	}
	RET(0);
}
