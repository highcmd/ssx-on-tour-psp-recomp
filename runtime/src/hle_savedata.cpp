// sceUtilitySavedata: real saves on the host, in <ms root>/PSP/SAVEDATA/<game><save>/
// (DATA file, PARAM.SFO, optional ICON0/ICON1/PIC1/SND0), like a PSP memory stick.
// The system's save/load dialogs are not drawn: list modes pick the slot the
// way the dialog's initial focus would, and the operation completes at once.
// Structure layout and result codes follow PPSSPP Core/Dialog/SavedataParam
// (public, GPL). Data is stored unencrypted (not interchangeable with a PSP yet).
#include <chrono>
#include <filesystem>
#include <system_error>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "kernel.h"
#include "platform.h"

namespace fs = std::filesystem;

extern std::string g_ms_root;

namespace {

enum : u32 {
	LOAD_NO_DATA = 0x80110307,
	LOAD_FILE_NOT_FOUND = 0x80110309,
	RW_NO_DATA = 0x80110327,
	RW_FILE_NOT_FOUND = 0x80110329,
	SAVE_ACCESS_ERROR = 0x80110385,
	DELETE_NO_DATA = 0x80110347,
	SIZES_NO_DATA = 0x801103C7,
	SAVEDATA_TYPE_ERROR = 0x80110300,
};

// SceUtilitySavedataParam offsets
enum : u32 {
	P_SIZE = 0x00, P_RESULT = 0x1C, P_MODE = 0x30, P_BIND = 0x34, P_OVERWRITE = 0x38,
	P_GAME = 0x3C, P_SAVE = 0x4C, P_SAVELIST = 0x60, P_FILE = 0x64,
	P_DATABUF = 0x74, P_DATABUFSIZE = 0x78, P_DATASIZE = 0x7C,
	P_SFO_TITLE = 0x80, P_SFO_SAVETITLE = 0x100, P_SFO_DETAIL = 0x180, P_SFO_PARENTAL = 0x580,
	P_ICON0 = 0x584, P_ICON1 = 0x594, P_PIC1 = 0x5A4, P_SND0 = 0x5B4,
	P_FOCUS = 0x5C8, P_MSFREE = 0x5D0, P_MSDATA = 0x5D4, P_UTILDATA = 0x5D8,
	P_IDLIST = 0x5F4,
};

const char *kModeNames[] = {"AUTOLOAD", "AUTOSAVE", "LOAD", "SAVE", "LISTLOAD", "LISTSAVE", "LISTDELETE", "LISTALLDELETE",
                            "SIZES", "AUTODELETE", "DELETE", "LIST", "FILES", "MAKEDATASECURE", "MAKEDATA", "READDATASECURE",
                            "READDATA", "WRITEDATASECURE", "WRITEDATA", "ERASESECURE", "ERASE", "DELETEDATA", "GETSIZE"};

std::string fixed_str(u32 addr, size_t n) {
	const char *p = (const char *)gptr(addr);
	return std::string(p, strnlen(p, n));
}

void put_str(u32 addr, const std::string &s, size_t n) {
	char *p = (char *)gptr(addr);
	memset(p, 0, n);
	memcpy(p, s.data(), std::min(s.size(), n - 1));
}

std::string save_root() { return g_ms_root + "/PSP/SAVEDATA/"; }

bool is_dir(const std::string &p) {
	std::error_code ec;
	return fs::is_directory(u8p(p), ec);
}

time_t mtime_of(const std::string &p) {
	std::error_code ec;
	auto ft = fs::last_write_time(u8p(p), ec);
	if (ec) return 0;
	return std::chrono::system_clock::to_time_t(std::chrono::time_point_cast<std::chrono::system_clock::duration>(
	    ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now()));
}

void mkdirs(const std::string &path) {
	std::error_code ec;
	fs::create_directories(u8p(path), ec);
}

// Names of the entries in a directory (empty if it doesn't exist).
static std::vector<std::string> list_dir(const std::string &dir) {
	std::vector<std::string> out;
	std::error_code ec;
	for (fs::directory_iterator it(u8p(dir), ec), e; !ec && it != e; it.increment(ec))
		out.push_back(u8s(it->path().filename()));
	return out;
}

bool read_file(const std::string &path, std::vector<u8> &out) {
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	out.resize(n > 0 ? n : 0);
	bool ok = out.empty() || fread(out.data(), 1, out.size(), f) == out.size();
	fclose(f);
	return ok;
}

bool write_file(const std::string &path, const void *data, size_t n) {
	std::string tmp = path + ".tmp";
	FILE *f = fopen(tmp.c_str(), "wb");
	if (!f) return false;
	bool ok = n == 0 || fwrite(data, 1, n, f) == n;
	ok = fclose(f) == 0 && ok;
	std::error_code ec;
	if (ok) fs::rename(u8p(tmp), u8p(path), ec);   // replaces: never leave a half-written save
	return ok && !ec;
}

// ---------------------------------------------------------------- PARAM.SFO
struct SfoEntry {
	std::string key;
	u16 fmt;                  // 0x0004 binary, 0x0204 utf8 string, 0x0404 int32
	std::vector<u8> data;
	u32 maxLen;
};

std::vector<u8> build_sfo(std::vector<SfoEntry> entries) {
	std::sort(entries.begin(), entries.end(), [](const SfoEntry &a, const SfoEntry &b) { return a.key < b.key; });
	u32 n = (u32)entries.size();
	std::vector<u8> keys, data;
	std::vector<u8> index(n * 16);
	for (u32 i = 0; i < n; i++) {
		const SfoEntry &e = entries[i];
		u16 koff = (u16)keys.size();
		keys.insert(keys.end(), e.key.begin(), e.key.end());
		keys.push_back(0);
		u32 doff = (u32)data.size();
		u32 len = (u32)e.data.size();
		std::vector<u8> d = e.data;
		d.resize(e.maxLen, 0);
		data.insert(data.end(), d.begin(), d.end());
		u8 *ix = &index[i * 16];
		memcpy(ix + 0, &koff, 2);
		memcpy(ix + 2, &e.fmt, 2);
		memcpy(ix + 4, &len, 4);
		memcpy(ix + 8, &e.maxLen, 4);
		memcpy(ix + 12, &doff, 4);
	}
	while (keys.size() % 4) keys.push_back(0);
	u32 keyStart = 20 + n * 16, dataStart = keyStart + (u32)keys.size();
	std::vector<u8> out(20);
	memcpy(out.data(), "\0PSF", 4);
	u32 ver = 0x00000101;
	memcpy(&out[4], &ver, 4);
	memcpy(&out[8], &keyStart, 4);
	memcpy(&out[12], &dataStart, 4);
	memcpy(&out[16], &n, 4);
	out.insert(out.end(), index.begin(), index.end());
	out.insert(out.end(), keys.begin(), keys.end());
	out.insert(out.end(), data.begin(), data.end());
	return out;
}

SfoEntry sfo_str(const std::string &key, const std::string &v, u32 maxLen) {
	SfoEntry e{key, 0x0204, std::vector<u8>(v.begin(), v.end()), maxLen};
	e.data.push_back(0);
	if (e.data.size() > maxLen) { e.data.resize(maxLen); e.data.back() = 0; }
	return e;
}

SfoEntry sfo_int(const std::string &key, u32 v) {
	SfoEntry e{key, 0x0404, std::vector<u8>(4), 4};
	memcpy(e.data.data(), &v, 4);
	return e;
}

SfoEntry sfo_bin(const std::string &key, u32 len) { return SfoEntry{key, 0x0004, std::vector<u8>(len, 0), len}; }

bool parse_sfo(const std::vector<u8> &s, std::string &title, std::string &saveTitle, std::string &detail, u32 &parental) {
	if (s.size() < 20 || memcmp(s.data(), "\0PSF", 4)) return false;
	u32 keyStart, dataStart, n;
	memcpy(&keyStart, &s[8], 4);
	memcpy(&dataStart, &s[12], 4);
	memcpy(&n, &s[16], 4);
	for (u32 i = 0; i < n && 20 + i * 16 + 16 <= s.size(); i++) {
		u16 koff, fmt;
		u32 len, doff;
		memcpy(&koff, &s[20 + i * 16], 2);
		memcpy(&fmt, &s[20 + i * 16 + 2], 2);
		memcpy(&len, &s[20 + i * 16 + 4], 4);
		memcpy(&doff, &s[20 + i * 16 + 12], 4);
		if (keyStart + koff >= s.size() || dataStart + doff + len > s.size()) continue;
		std::string key((const char *)&s[keyStart + koff]);
		const u8 *d = &s[dataStart + doff];
		std::string str((const char *)d, strnlen((const char *)d, len));
		if (key == "TITLE") title = str;
		else if (key == "SAVEDATA_TITLE") saveTitle = str;
		else if (key == "SAVEDATA_DETAIL") detail = str;
		else if (key == "PARENTAL_LEVEL" && len >= 4) memcpy(&parental, d, 4);
	}
	return true;
}

// ---------------------------------------------------------------- operations
std::string dir_for(u32 p, const std::string &saveName) {
	return save_root() + fixed_str(p + P_GAME, 13) + saveName;
}

std::vector<std::string> save_name_list(u32 p) {
	std::vector<std::string> names;
	u32 list = LW(p + P_SAVELIST);
	if (!list || !gvalid(list)) return names;
	for (int i = 0; i < 100; i++) {
		std::string n = fixed_str(list + i * 20, 20);
		if (n.empty()) break;
		names.push_back(n);
	}
	return names;
}

// Slot a list dialog would end up on (no UI: we take its initial focus).
std::string pick_slot(u32 p, int mode) {
	std::string base = fixed_str(p + P_SAVE, 20);
	std::vector<std::string> names = save_name_list(p);
	if (names.empty()) return base;
	bool load = mode == 4 || mode == 6;
	std::vector<std::string> existing;
	for (auto &n : names) if (is_dir(dir_for(p, n))) existing.push_back(n);
	auto in = [](const std::vector<std::string> &v, const std::string &s) { return std::find(v.begin(), v.end(), s) != v.end(); };
	auto latest = [&]() {
		std::string best;
		time_t bt = 0;
		for (auto &n : existing) {
			time_t t = mtime_of(dir_for(p, n));
			if (best.empty() || t > bt) { best = n; bt = t; }
		}
		return best;
	};
	u32 focus = LW(p + P_FOCUS);
	if (load) {
		if (existing.empty()) return "";
		if (in(existing, base)) return base;
		return latest();
	}
	if (focus == 7 || focus == 8) {   // first / last empty slot
		for (size_t k = 0; k < names.size(); k++) {
			const std::string &n = focus == 7 ? names[k] : names[names.size() - 1 - k];
			if (!in(existing, n)) return n;
		}
	}
	if (focus == 3 && !existing.empty()) return latest();
	if (in(names, base)) return base;
	// No usable focus (SSX passes name "000" with slots 001..005): never overwrite another
	// save silently - take the first empty slot, or the least recently written one if all are used.
	for (auto &n : names)
		if (!in(existing, n)) return n;
	std::string oldest;
	time_t ot = 0;
	for (auto &n : existing) {
		time_t t = mtime_of(dir_for(p, n));
		if (oldest.empty() || t < ot) { oldest = n; ot = t; }
	}
	return oldest.empty() ? names[0] : oldest;
}

u32 do_save(u32 p, const std::string &saveName) {
	std::string dir = dir_for(p, saveName);
	mkdirs(dir);
	if (!is_dir(dir)) return SAVE_ACCESS_ERROR;
	std::string file = fixed_str(p + P_FILE, 13);
	u32 buf = LW(p + P_DATABUF), size = LW(p + P_DATASIZE);
	if (!file.empty()) {
		if (size && !gvalid(buf)) return SAVE_ACCESS_ERROR;
		if (!write_file(dir + "/" + file, size ? gptr(buf) : nullptr, size)) return SAVE_ACCESS_ERROR;
	}
	std::vector<SfoEntry> sfo = {
		sfo_str("CATEGORY", "MS", 4),
		sfo_int("PARENTAL_LEVEL", gptr(p + P_SFO_PARENTAL)[0]),
		sfo_str("SAVEDATA_DETAIL", fixed_str(p + P_SFO_DETAIL, 0x400), 0x400),
		sfo_str("SAVEDATA_DIRECTORY", fixed_str(p + P_GAME, 13) + saveName, 64),
		sfo_bin("SAVEDATA_FILE_LIST", 0xC60),
		sfo_bin("SAVEDATA_PARAMS", 0x80),
		sfo_str("SAVEDATA_TITLE", fixed_str(p + P_SFO_SAVETITLE, 0x80), 0x80),
		sfo_str("TITLE", fixed_str(p + P_SFO_TITLE, 0x80), 0x80),
	};
	// list the data file in SAVEDATA_FILE_LIST like the PSP does (name only; no hash: unencrypted)
	if (!file.empty()) memcpy(sfo[4].data.data(), file.data(), std::min<size_t>(file.size(), 12));
	std::vector<u8> bin = build_sfo(sfo);
	if (!write_file(dir + "/PARAM.SFO", bin.data(), bin.size())) return SAVE_ACCESS_ERROR;
	static const struct { u32 off; const char *name; } extras[] = {
		{P_ICON0, "ICON0.PNG"}, {P_ICON1, "ICON1.PMF"}, {P_PIC1, "PIC1.PNG"}, {P_SND0, "SND0.AT3"}};
	for (auto &x : extras) {
		u32 b = LW(p + x.off), sz = LW(p + x.off + 8);
		if (!b || !sz || !gvalid(b)) continue;
		const u8 *d = gptr(b);
		// games often pass the buffer size; keep a PNG only up to its IEND chunk
		if (sz > 8 && !memcmp(d, "\x89PNG", 4))
			for (u32 k = 8; k + 12 <= sz; k++)
				if (!memcmp(d + k, "IEND", 4)) { sz = k + 8; break; }
		write_file(dir + "/" + x.name, d, sz);
	}
	rt_log(LOG_MISC, "  saved %s/%s (%u bytes)", dir.c_str(), file.c_str(), size);
	return 0;
}

u32 do_load(u32 p, const std::string &saveName, bool rw) {
	std::string dir = dir_for(p, saveName);
	if (saveName.empty() || !is_dir(dir)) return rw ? RW_NO_DATA : LOAD_NO_DATA;
	std::string file = fixed_str(p + P_FILE, 13);
	std::vector<u8> data;
	if (!file.empty() && !read_file(dir + "/" + file, data)) return rw ? RW_FILE_NOT_FOUND : LOAD_FILE_NOT_FOUND;
	SW(p + P_DATASIZE, 0);
	u32 buf = LW(p + P_DATABUF), cap = LW(p + P_DATABUFSIZE);
	u32 n = std::min<u32>((u32)data.size(), cap);
	if (n && gvalid(buf)) memcpy(gptr(buf), data.data(), n);
	SW(p + P_DATASIZE, n);
	std::vector<u8> sfo;
	if (read_file(dir + "/PARAM.SFO", sfo)) {
		std::string title, saveTitle, detail;
		u32 parental = 0;
		if (parse_sfo(sfo, title, saveTitle, detail, parental)) {
			put_str(p + P_SFO_TITLE, title, 0x80);
			put_str(p + P_SFO_SAVETITLE, saveTitle, 0x80);
			put_str(p + P_SFO_DETAIL, detail, 0x400);
			gptr(p + P_SFO_PARENTAL)[0] = (u8)parental;
		}
	}
	SW(p + P_BIND, 1021);
	static const struct { u32 off; const char *name; } extras[] = {
		{P_ICON0, "ICON0.PNG"}, {P_ICON1, "ICON1.PMF"}, {P_PIC1, "PIC1.PNG"}, {P_SND0, "SND0.AT3"}};
	for (auto &x : extras) {
		u32 b = LW(p + x.off), bsz = LW(p + x.off + 4);
		std::vector<u8> f;
		if (b && bsz && gvalid(b) && read_file(dir + "/" + x.name, f)) {
			u32 k = std::min<u32>((u32)f.size(), bsz);
			memcpy(gptr(b), f.data(), k);
			SW(p + x.off + 8, k);
		}
	}
	rt_log(LOG_MISC, "  loaded %s/%s (%u bytes)", dir.c_str(), file.c_str(), n);
	return 0;
}

bool do_delete(u32 p, const std::string &saveName) {
	std::string dir = dir_for(p, saveName);
	if (saveName.empty() || !is_dir(dir)) return false;
	std::error_code ec;
	fs::remove_all(u8p(dir), ec);
	return !ec;
}

u32 dir_size(const std::string &dir, u32 cluster, u32 *clusters) {
	u32 total = 0;
	for (const std::string &n : list_dir(dir)) {
		std::error_code ec;
		u64 sz = fs::file_size(u8p(dir + "/" + n), ec);
		if (ec) continue;
		total += (u32)sz;
		*clusters += ((u32)sz + cluster - 1) / cluster;
	}
	return total;
}

std::string kb_text(u32 kb) {
	char b[16];
	if (kb < 1024) snprintf(b, sizeof(b), "%u KB", kb);
	else snprintf(b, sizeof(b), "%u MB", kb / 1024);
	return b;
}

u32 do_sizes(u32 p) {
	const u32 cluster = 0x8000;
	u32 ret = 0;
	u32 msFree = LW(p + P_MSFREE), msData = LW(p + P_MSDATA), util = LW(p + P_UTILDATA);
	if (msFree && gvalid(msFree)) {
		const u32 freeClusters = 0x8000;   // 1 GB free
		SW(msFree + 0, cluster);
		SW(msFree + 4, freeClusters);
		SW(msFree + 8, freeClusters * (cluster / 1024));
		put_str(msFree + 12, kb_text(freeClusters * (cluster / 1024)), 8);
	}
	if (msData && gvalid(msData)) {
		std::string game = fixed_str(msData, 13), save = fixed_str(msData + 16, 20);
		std::string dir = save_root() + game + (save == "<>" ? "" : save);
		u32 info = msData + 36, used = 0;
		if (is_dir(dir)) {
			dir_size(dir, cluster, &used);
			u32 kb = used * (cluster / 1024);
			SW(info + 0, used);
			SW(info + 4, kb);
			put_str(info + 8, kb_text(kb), 8);
			SW(info + 16, kb);
			put_str(info + 20, kb_text(kb), 8);
		} else {
			for (int k = 0; k < 28; k += 4) SW(info + k, 0);
			ret = SIZES_NO_DATA;
		}
	}
	if (util && gvalid(util)) {
		u32 clusters = 2 + (fixed_str(p + P_FILE, 13).empty() ? 0 : (LW(p + P_DATASIZE) + cluster - 1) / cluster);
		for (u32 off : {P_ICON0, P_ICON1, P_PIC1, P_SND0}) clusters += (LW(p + off + 8) + cluster - 1) / cluster;
		u32 kb = clusters * (cluster / 1024);
		SW(util + 0, clusters);
		SW(util + 4, kb);
		put_str(util + 8, kb_text(kb), 8);
		SW(util + 16, kb);
		put_str(util + 20, kb_text(kb), 8);
	}
	return ret;
}

// LIST (V3 struct only): directories matching game+save pattern ('*' wildcard).
void do_list(u32 p) {
	u32 idList = LW(p + P_IDLIST);
	if (LW(p + P_SIZE) < 0x600 || !idList || !gvalid(idList)) return;
	u32 maxCount = LW(idList), entries = LW(idList + 8), count = 0;
	std::string pattern = fixed_str(p + P_GAME, 13) + fixed_str(p + P_SAVE, 20);
	auto match = [](const std::string &pat, const std::string &s) {
		size_t star = pat.find('*');
		if (star == std::string::npos) return pat == s;
		return s.compare(0, star, pat, 0, star) == 0;
	};
	{
		for (const std::string &n : list_dir(save_root())) {
			if (count >= maxCount || !gvalid(entries)) break;
			if (!match(pattern, n)) continue;
			u32 ent = entries + count * 72;   // mode, 3 x ScePspDateTime (16), name[20]
			memset(gptr(ent), 0, 72);
			SW(ent, 0x11FF);
			std::string game = fixed_str(p + P_GAME, 13);
			put_str(ent + 52, n.size() > game.size() ? n.substr(game.size()) : std::string(), 20);
			count++;
		}
	}
	SW(idList + 4, count);
}

// ---------------------------------------------------------------- dialog state machine
struct Dialog {
	int status = 0;   // 0 none, 1 init, 2 running, 3 finished, 4 shutdown
	u32 param = 0;
} s_dlg;

u32 run_request(u32 p) {
	u32 mode = LW(p + P_MODE);
	std::string slot;
	switch (mode) {
	case 0: case 2: return do_load(p, fixed_str(p + P_SAVE, 20), false);                    // AUTOLOAD, LOAD
	case 1: case 3: return do_save(p, fixed_str(p + P_SAVE, 20));                           // AUTOSAVE, SAVE
	case 4:                                                                                   // LISTLOAD
		slot = pick_slot(p, 4);
		if (slot.empty()) return LOAD_NO_DATA;
		put_str(p + P_SAVE, slot, 20);
		return do_load(p, slot, false);
	case 5:                                                                                   // LISTSAVE
		slot = pick_slot(p, 5);
		put_str(p + P_SAVE, slot, 20);
		return do_save(p, slot);
	case 6: {                                                                                 // LISTDELETE
		// Without the system list we only delete an unambiguous save; otherwise "cancelled".
		std::string base = fixed_str(p + P_SAVE, 20);
		std::vector<std::string> existing;
		for (auto &n : save_name_list(p)) if (is_dir(dir_for(p, n))) existing.push_back(n);
		if (std::find(existing.begin(), existing.end(), base) != existing.end()) slot = base;
		else if (existing.size() == 1) slot = existing[0];
		else return 1;   // SCE_UTILITY_DIALOG_RESULT_CANCEL
		put_str(p + P_SAVE, slot, 20);
		return do_delete(p, slot) ? 0 : DELETE_NO_DATA;
	}
	case 7: return 0;                                                                        // LISTALLDELETE (user cancels)
	case 8: return do_sizes(p);                                                               // SIZES
	case 9: case 10: return do_delete(p, fixed_str(p + P_SAVE, 20)) ? 0 : DELETE_NO_DATA;   // AUTODELETE, DELETE
	case 11: do_list(p); return 0;                                                            // LIST
	case 13: case 14: case 17: case 18: return do_save(p, fixed_str(p + P_SAVE, 20));      // MAKEDATA*, WRITEDATA*
	case 15: case 16: return do_load(p, fixed_str(p + P_SAVE, 20), true);                   // READDATA*
	case 19: case 20: case 21: return do_delete(p, fixed_str(p + P_SAVE, 20)) ? 0 : RW_NO_DATA;
	default:
		rt_log(LOG_UNIMPL, "savedata mode %u not implemented", mode);
		return SAVEDATA_TYPE_ERROR;
	}
}

}  // namespace

HLE(sceUtility, sceUtilitySavedataInitStart) {
	u32 p = ARG(0);
	if (s_dlg.status != 0 && s_dlg.status != 4) { RET(0x80110001); return; }   // already running
	u32 mode = LW(p + P_MODE);
	rt_log(LOG_MISC, "Savedata %s: game '%s' save '%s' file '%s' (struct %x)", mode < 23 ? kModeNames[mode] : "?",
	       fixed_str(p + P_GAME, 13).c_str(), fixed_str(p + P_SAVE, 20).c_str(), fixed_str(p + P_FILE, 13).c_str(), LW(p + P_SIZE));
	if (mode == 4 || mode == 5 || mode == 6) {
		std::string names;
		for (auto &n : save_name_list(p)) names += n + " ";
		rt_log(LOG_MISC, "  focus %u, slots: %s", LW(p + P_FOCUS), names.c_str());
	}
	u32 r = run_request(p);
	SW(p + P_RESULT, r);
	if (r) rt_log(LOG_MISC, "  -> result %08x", r);
	s_dlg.status = 1;
	s_dlg.param = p;
	RET(0);
}
HLE(sceUtility, sceUtilitySavedataGetStatus) {
	int s = s_dlg.status;
	if (s_dlg.status == 1) s_dlg.status = 2;
	else if (s_dlg.status == 4) s_dlg.status = 0;
	RET(s);
}
HLE(sceUtility, sceUtilitySavedataUpdate) { if (s_dlg.status == 2) s_dlg.status = 3; RET(0); }
HLE(sceUtility, sceUtilitySavedataShutdownStart) { if (s_dlg.status == 3) s_dlg.status = 4; RET(0); }
