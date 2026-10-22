// IoFileMgrForUser: disc0:/umd0: -> the UMD image (or an extracted copy with
// --umd), ms0: -> ./ms0 (saves).
#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "kernel.h"
#include "platform.h"

namespace fs = std::filesystem;

std::string g_umd_root;   // extracted UMD tree; empty = read the files from the ISO
std::string g_iso_path;   // raw UMD image: disc0: files, umd0: block access, sce_lbn opens
std::string g_ms_root = "ms0";

struct DirEntry {
	std::string name;
	bool dir = false;
	u64 size = 0;
	time_t mtime = 0;
};

struct OpenFile {
	FILE *f = nullptr;
	bool isDir = false;
	std::vector<DirEntry> entries;   // directories: listed at dopen
	size_t dirPos = 0;
	std::string path;
	bool blockMode = false;   // umd0: device: read/seek in 2048-byte sectors
	bool raw = false;         // byte window [rawStart, rawStart+rawSize) of the ISO
	u64 rawStart = 0, rawSize = 0;
	bool isoFile = false;     // a file of the UMD image (window = the whole file, may be empty)
	s64 asyncResult = 0;
	bool asyncPending = false;
};
static std::map<int, OpenFile> s_files;
static int s_next_fd = 3;

static std::string lower(std::string s) {
	for (auto &ch : s) ch = (char)tolower((unsigned char)ch);
	return s;
}

// ------------------------------------------------------------- ISO 9660 reader
// The UMD's file system, indexed once: lower-case path -> extent. Files on a UMD
// are contiguous, so an open file is a byte window into the image.
struct IsoNode {
	u64 offset = 0, size = 0;
	bool dir = false;
	time_t mtime = 0;
	std::vector<std::string> children;   // original-case names
};
static std::unordered_map<std::string, IsoNode> s_iso;
static bool s_iso_ok;

static time_t iso_time(const u8 *d) {
	struct tm t{};
	t.tm_year = d[0];
	t.tm_mon = d[1] - 1;
	t.tm_mday = d[2];
	t.tm_hour = d[3];
	t.tm_min = d[4];
	t.tm_sec = d[5];
	t.tm_isdst = -1;
	return mktime(&t);
}

static void iso_scan(FILE *f, const std::string &path, u32 lba, u32 size, int depth) {
	if (depth > 16) return;
	std::vector<u8> buf(size);
	if (fseeko(f, (s64)lba * 2048, SEEK_SET) != 0 || fread(buf.data(), 1, size, f) != size) return;
	for (u32 off = 0; off < size;) {
		u8 len = buf[off];
		if (len == 0) { off = (off / 2048 + 1) * 2048; continue; }   // records never cross a sector
		if (off + 33 > size || off + len > size) break;
		const u8 *r = &buf[off];
		u8 nlen = r[32];
		std::string name((const char *)r + 33, nlen);
		off += len;
		if (nlen == 1 && (name[0] == 0 || name[0] == 1)) continue;   // . and ..
		name = name.substr(0, name.find(';'));
		IsoNode n;
		u32 elba, esize;
		memcpy(&elba, r + 2, 4);
		memcpy(&esize, r + 10, 4);
		n.offset = (u64)elba * 2048;
		n.size = esize;
		n.dir = r[25] & 2;
		n.mtime = iso_time(r + 18);
		std::string full = path.empty() ? name : path + "/" + name;
		s_iso[path].children.push_back(name);
		s_iso[lower(full)] = n;
		if (n.dir) iso_scan(f, lower(full), elba, esize, depth + 1);
	}
}

static bool iso_init() {
	static bool done;
	if (done) return s_iso_ok;
	done = true;
	FILE *f = g_iso_path.empty() ? nullptr : fopen(g_iso_path.c_str(), "rb");
	if (!f) {
		rt_log_error("cannot open the UMD image '%s' (--iso)", g_iso_path.c_str());
		return false;
	}
	u8 pvd[2048];
	if (fseeko(f, 16 * 2048, SEEK_SET) == 0 && fread(pvd, 1, 2048, f) == 2048 && pvd[0] == 1 && !memcmp(pvd + 1, "CD001", 5)) {
		u32 lba, size;
		memcpy(&lba, pvd + 156 + 2, 4);
		memcpy(&size, pvd + 156 + 10, 4);
		IsoNode &root = s_iso[""];
		root.dir = true;
		iso_scan(f, "", lba, size, 0);
		s_iso_ok = true;
		rt_log(LOG_IO, "UMD image %s: %zu entries", g_iso_path.c_str(), s_iso.size());
	} else {
		rt_log_error("'%s' is not an ISO 9660 image", g_iso_path.c_str());
	}
	fclose(f);
	return s_iso_ok;
}

static bool umd_from_iso() { return g_umd_root.empty(); }

static const IsoNode *iso_find(const std::string &rel) {
	if (!iso_init()) return nullptr;
	std::string key;
	size_t i = 0;
	while (i <= rel.size()) {   // normalise: drop empty and "." parts
		size_t j = rel.find('/', i);
		if (j == std::string::npos) j = rel.size();
		std::string part = rel.substr(i, j - i);
		i = j + 1;
		if (part.empty() || part == ".") continue;
		key += (key.empty() ? "" : "/") + lower(part);
	}
	auto it = s_iso.find(key);
	return it == s_iso.end() ? nullptr : &it->second;
}

// ------------------------------------------------------------- host paths
// Case-insensitive path resolution (UMD is case-insensitive, an extracted copy may not be).
static std::string resolve_ci(const std::string &root, const std::string &rel) {
	std::string cur = root;
	size_t i = 0;
	std::error_code ec;
	while (i <= rel.size()) {
		size_t j = rel.find('/', i);
		if (j == std::string::npos) j = rel.size();
		std::string part = rel.substr(i, j - i);
		i = j + 1;
		if (part.empty() || part == ".") continue;
		std::string exact = cur + "/" + part;
		if (fs::exists(u8p(exact), ec)) { cur = exact; continue; }
		bool found = false;
		for (fs::directory_iterator it(u8p(cur), ec), e; !ec && it != e; it.increment(ec)) {
			std::string n = u8s(it->path().filename());
			if (lower(n) == lower(part)) { cur += "/" + n; found = true; break; }
		}
		if (!found) cur = exact;
	}
	return cur;
}

static std::string s_cwd = "disc0:/PSP_GAME/USRDIR";

// Split a guest path into (lower-case device, path on the device).
static void split_guest(const std::string &guest, std::string &dev, std::string &rel) {
	std::string p = guest;
	std::replace(p.begin(), p.end(), '\\', '/');
	size_t colon = p.find(':');
	if (colon == std::string::npos) {   // relative to cwd
		size_t c = s_cwd.find(':');
		dev = s_cwd.substr(0, c);
		rel = s_cwd.substr(c + 1) + "/" + p;
	} else {
		dev = p.substr(0, colon);
		rel = p.substr(colon + 1);
	}
	dev = lower(dev);
}

static bool is_umd_dev(const std::string &dev) {
	return dev.rfind("disc", 0) == 0 || dev.rfind("umd", 0) == 0 || dev.rfind("host", 0) == 0;
}

std::string io_host_path(const std::string &guest) {
	std::string dev, rel;
	split_guest(guest, dev, rel);
	if (dev.rfind("ms", 0) == 0 || dev.rfind("fatms", 0) == 0) return g_ms_root + "/" + rel;
	return resolve_ci(g_umd_root.empty() ? std::string(".") : g_umd_root, rel);
}

// The UMD file for a guest path when the UMD comes from the image, else null.
static const IsoNode *umd_iso_node(const std::string &guest) {
	std::string dev, rel;
	split_guest(guest, dev, rel);
	if (!is_umd_dev(dev) || !umd_from_iso()) return nullptr;
	return iso_find(rel);
}
static bool umd_iso_path(const std::string &guest) {
	std::string dev, rel;
	split_guest(guest, dev, rel);
	return is_umd_dev(dev) && umd_from_iso();
}

static void mkdirs_for(const std::string &path) {
	std::error_code ec;
	fs::create_directories(u8p(path).parent_path(), ec);
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
	if (umd_iso_path(g)) {
		const IsoNode *n = umd_iso_node(g);
		FILE *f = n && !n->dir && !(flags & 0x0002) ? fopen(g_iso_path.c_str(), "rb") : nullptr;
		if (!f) {
			LOGIO("open '%s' flags %x -> not found in the UMD image", g.c_str(), flags);
			RET(SCE_ERROR_ERRNO_FILE_NOT_FOUND);
			return;
		}
		fseeko(f, (s64)n->offset, SEEK_SET);
		int fd = s_next_fd++;
		OpenFile &of = s_files[fd];
		of.f = f;
		of.path = g;
		of.raw = true;
		of.rawStart = n->offset;
		of.rawSize = n->size;
		of.isoFile = true;
		LOGIO("open '%s' flags %x -> %d (image)", g.c_str(), flags, fd);
		RET(fd);
		return;
	}
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
		if ((of.rawSize || of.isoFile) && pos + size > of.rawSize) size = pos < of.rawSize ? (u32)(of.rawSize - pos) : 0;
	}
	return (s64)fread(gptr(buf), 1, size, of.f);
}

HLE(IoFileMgrForUser, sceIoRead) {
	s64 r = do_read((int)ARG(0), ARG(1), ARG(2));
	if (!ARG(1) && ARG(2)) {   // a read into NULL: usually a failed allocation in the game
		void rt_log_backtrace(CPUState *, u32);
		rt_log_backtrace(c, 256);
	}
	if (rt_log_enabled(LOG_IO)) LOGIO("read(%d, %08x, %x) -> %llx ra=%08x", (int)ARG(0), ARG(1), ARG(2), (long long)r, c->r[31]);
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
	if (!it->second.f && !it->second.isDir) s_files.erase(it);   // sceIoCloseAsync done: fd is free
	RET(0);
}

// sceIoCloseAsync: close now; the fd stays until its async result (0) is collected.
HLE(IoFileMgrForUser, sceIoCloseAsync) {
	auto it = s_files.find((int)ARG(0));
	if (it == s_files.end() || !it->second.f) { RET(SCE_ERROR_ERRNO_BAD_FD); return; }
	fclose(it->second.f);
	it->second.f = nullptr;
	it->second.asyncResult = 0;
	it->second.asyncPending = true;
	RET(0);
}
HLE(IoFileMgrForUser, sceIoWaitAsync) { async_result(c, true); }
HLE(IoFileMgrForUser, sceIoPollAsync) { async_result(c, false); }
HLE(IoFileMgrForUser, sceIoChangeAsyncPriority) { RET(0); }

HLE(IoFileMgrForUser, sceIoDopen) {
	std::string g = gstr(ARG(0));
	std::vector<DirEntry> list;
	bool ok = false;
	if (umd_iso_path(g)) {
		const IsoNode *n = umd_iso_node(g);
		if (n && n->dir) {
			ok = true;
			std::string dev, rel;
			split_guest(g, dev, rel);
			for (const std::string &c : n->children) {
				const IsoNode *ch = iso_find(rel + "/" + c);
				if (ch) list.push_back({c, ch->dir, ch->size, ch->mtime});
			}
		}
	} else {
		std::error_code ec;
		fs::path dir = u8p(io_host_path(g));
		if (fs::is_directory(dir, ec)) {
			ok = true;
			for (fs::directory_iterator it(dir, ec), e; !ec && it != e; it.increment(ec)) {
				DirEntry d;
				d.name = u8s(it->path().filename());
				d.dir = it->is_directory(ec);
				d.size = d.dir ? 0 : (u64)it->file_size(ec);
				auto ft = it->last_write_time(ec);
				d.mtime = std::chrono::system_clock::to_time_t(
				    std::chrono::time_point_cast<std::chrono::system_clock::duration>(
				        ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now()));
				list.push_back(d);
			}
		}
	}
	if (!ok) { LOGIO("dopen '%s' -> not found", g.c_str()); RET(SCE_ERROR_ERRNO_FILE_NOT_FOUND); return; }
	int fd = s_next_fd++;
	s_files[fd].isDir = true;
	s_files[fd].entries = std::move(list);
	s_files[fd].path = g;
	RET(fd);
}

// ScePspDateTime: year, month, day, hour, minute, second (u16 each), microsecond (u32)
static void put_psp_time(u32 a, time_t t) {
	struct tm tmv;
	platform_localtime(t, &tmv);
	u16 v[6] = {(u16)(tmv.tm_year + 1900), (u16)(tmv.tm_mon + 1), (u16)tmv.tm_mday, (u16)tmv.tm_hour, (u16)tmv.tm_min, (u16)tmv.tm_sec};
	memcpy(gptr(a), v, 12);
	SW(a + 12, 0);
}

HLE(IoFileMgrForUser, sceIoDread) {
	auto it = s_files.find((int)ARG(0));
	if (it == s_files.end() || !it->second.isDir) { RET(SCE_ERROR_ERRNO_BAD_FD); return; }
	OpenFile &of = it->second;
	if (of.dirPos >= of.entries.size()) { RET(0); return; }
	const DirEntry &e = of.entries[of.dirPos++];
	u32 p = ARG(1);
	// SceIoDirent: SceIoStat (mode, attr, size64, ctime, atime, mtime, private[6]) + name[256] + d_private
	memset(gptr(p), 0, 0x58 + 256);
	SW(p + 0, e.dir ? 0x11FF : 0x21FF);
	SW(p + 4, e.dir ? 0x10 : 0x20);
	SW(p + 8, (u32)e.size);
	SW(p + 12, (u32)(e.size >> 32));
	put_psp_time(p + 16, e.mtime);
	put_psp_time(p + 32, e.mtime);
	put_psp_time(p + 48, e.mtime);
	strncpy((char *)gptr(p + 0x58), e.name.c_str(), 255);
	RET(1);
}

HLE(IoFileMgrForUser, sceIoDclose) {
	auto it = s_files.find((int)ARG(0));
	if (it == s_files.end() || !it->second.isDir) { RET(SCE_ERROR_ERRNO_BAD_FD); return; }
	s_files.erase(it);
	RET(0);
}

HLE(IoFileMgrForUser, sceIoRename) {
	std::error_code ec;
	fs::rename(u8p(io_host_path(gstr(ARG(0)))), u8p(io_host_path(gstr(ARG(1)))), ec);
	RET(ec ? SCE_ERROR_ERRNO_FILE_NOT_FOUND : 0);
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
