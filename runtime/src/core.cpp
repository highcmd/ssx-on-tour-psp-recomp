// Memory, logging, indirect-call dispatch and the HLE import registry.
#include <sys/mman.h>
#include <unistd.h>
#include <array>
#include <chrono>
#include <utility>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "hle.h"
#include "kernel.h"

u8 *g_mem;
CPUState *currentMIPS;

// ---------------------------------------------------------------- logging
static const char *s_chan_names[LOG_COUNT] = {"rt", "kernel", "thread", "io", "display", "ge", "audio", "misc", "unimpl"};
static bool s_chan_on[LOG_COUNT];
static FILE *s_logfile;

void rt_log_init() {
	const char *f = getenv("SSX_LOG");
	for (int i = 0; i < LOG_COUNT; i++)
		s_chan_on[i] = !f || strstr(f, s_chan_names[i]) || !strcmp(f, "all");
	s_chan_on[LOG_RT] = s_chan_on[LOG_UNIMPL] = true;
	s_logfile = fopen("ssx.log", "w");
}

bool rt_log_enabled(LogChan ch) { return s_chan_on[ch]; }

static void vlog(const char *chan, const char *fmt, va_list ap) {
	char buf[1024];
	vsnprintf(buf, sizeof(buf), fmt, ap);
	int tid = kernel_current_thread_id();
	fprintf(stderr, "[%9.3f %s t%d] %s\n", rt_time_us() / 1e6, chan, tid, buf);
	if (s_logfile) {
		fprintf(s_logfile, "[%9.3f %s t%d] %s\n", rt_time_us() / 1e6, chan, tid, buf);
		fflush(s_logfile);
	}
}

void rt_log(LogChan ch, const char *fmt, ...) {
	if (!s_chan_on[ch]) return;
	va_list ap;
	va_start(ap, fmt);
	vlog(s_chan_names[ch], fmt, ap);
	va_end(ap);
}

void rt_log_error(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vlog("ERROR", fmt, ap);
	va_end(ap);
}

// Microseconds since start. Note: on this machine the TSC is not synchronised
// across cores (the kernel uses HPET), so rdtsc cannot be used as a clock.
u64 rt_time_us() {
	static auto t0 = std::chrono::steady_clock::now();
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
}

std::string gstr(u32 a) {
	if (!a || !gvalid(a)) return "(null)";
	const char *p = (const char *)gptr(a);
	return std::string(p, strnlen(p, 1024));
}

// ---------------------------------------------------------------- memory
void memory_init() {
	// Reserve the whole 512 MB guest window; pages are committed lazily by the OS.
	void *p = mmap(nullptr, 0x20000000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap guest memory");
		exit(1);
	}
	g_mem = (u8 *)p;
}

bool memory_load_image(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	size_t n = fread(gptr(g_image_base), 1, g_image_size, f);
	fclose(f);
	rt_log(LOG_RT, "image %s: %zu bytes at %08x (entry %08x)", path, n, g_image_base, g_image_entry);
	return n > 0;
}

// ---------------------------------------------------------------- dispatch
static std::unordered_map<u32, RecompFunc> s_func_by_addr;
static std::unordered_map<u32, int> s_import_by_stub;

void dispatch_init() {
	s_func_by_addr.reserve(g_recomp_func_count * 2);
	for (size_t i = 0; i < g_recomp_func_count; i++)
		s_func_by_addr[g_recomp_funcs[i].start] = g_recomp_funcs[i].fn;
	for (size_t i = 0; i < g_import_count; i++)
		s_import_by_stub[g_import_list[i].stub] = (int)i;
}

static const RecompEntry *find_containing(u32 a) {
	size_t lo = 0, hi = g_recomp_func_count;
	while (lo < hi) {
		size_t m = (lo + hi) / 2;
		if (g_recomp_funcs[m].start <= a) lo = m + 1; else hi = m;
	}
	if (lo == 0) return nullptr;
	const RecompEntry *e = &g_recomp_funcs[lo - 1];
	return a < e->end ? e : nullptr;
}

void rt_call(CPUState *c, u32 addr) {
	addr &= 0x3FFFFFFF;
	auto it = s_func_by_addr.find(addr);
	if (it != s_func_by_addr.end()) {
		it->second(c);
		return;
	}
	auto im = s_import_by_stub.find(addr);
	if (im != s_import_by_stub.end()) {
		g_imports[im->second](c);
		return;
	}
	const RecompEntry *e = find_containing(addr);
	if (e)
		rt_log_error("rt_call: %08x is inside function %08x (not an entry), ra=%08x", addr, e->start, c->r[31]);
	else
		rt_log_error("rt_call: no code at %08x, ra=%08x", addr, c->r[31]);
	rt_unhandled(c, addr, "indirect call to unknown address");
}

bool rt_is_function(u32 addr) { return s_func_by_addr.count(addr & 0x3FFFFFFF) != 0; }

void rt_unhandled(CPUState *c, u32 pc, const char *what) {
	rt_log_error("FATAL: %s at %08x (ra=%08x sp=%08x)", what, pc, c->r[31], c->r[29]);
	kernel_dump_threads();
	fflush(stderr);
	_exit(2);
}

void rt_break(CPUState *c, u32 pc) {
	rt_log_error("break at %08x (ra=%08x)", pc, c->r[31]);
}

void rt_halt(CPUState *c, u32 pc) {
	rt_log(LOG_RT, "guest halted (infinite loop) at %08x; thread exits", pc);
	kernel_exit_current_thread(0);
}

void rt_syscall(CPUState *c, u32 pc, u32 op) {
	rt_log_error("raw syscall %08x at %08x", op, pc);
}

// ---------------------------------------------------------------- HLE registry
struct HLEKey {
	std::string lib, name;
	bool operator<(const HLEKey &o) const { return lib != o.lib ? lib < o.lib : name < o.name; }
};
static std::map<HLEKey, HLEFunc> &registry() {
	static std::map<HLEKey, HLEFunc> r;
	return r;
}

HLEReg::HLEReg(const char *lib, const char *name, HLEFunc fn) { registry()[{lib, name}] = fn; }

// One trampoline per import so an unimplemented call can say who it is.
static int s_unimpl_index;
static int s_unimpl_count[256];
template <int N>
static void unimpl_tramp(CPUState *c) {
	const ImportEntry &e = g_import_list[N];
	if (s_unimpl_count[N]++ < 5)
		rt_log(LOG_UNIMPL, "%s::%s(%08x, %08x, %08x, %08x) ra=%08x -> 0", e.lib, e.name, c->r[4], c->r[5], c->r[6], c->r[7], c->r[31]);
	c->r[2] = 0;
}
template <int... Is>
static constexpr std::array<HLEFunc, sizeof...(Is)> make_tramps(std::integer_sequence<int, Is...>) {
	return {unimpl_tramp<Is>...};
}

// Every implemented HLE call is wrapped so that pending interrupts get a chance
// to run right after it (see kernel_poll_interrupts).
void kernel_poll_interrupts();
static HLEFunc s_real[256];
template <int N>
static void hle_wrap(CPUState *c) {
	s_real[N](c);
	kernel_poll_interrupts();
}
template <int... Is>
static constexpr std::array<HLEFunc, sizeof...(Is)> make_wraps(std::integer_sequence<int, Is...>) {
	return {hle_wrap<Is>...};
}

void hle_init_imports() {
	static const auto wraps = make_wraps(std::make_integer_sequence<int, 256>{});
	static const auto tramps = make_tramps(std::make_integer_sequence<int, 256>{});
	int implemented = 0;
	for (size_t i = 0; i < g_import_count; i++) {
		const ImportEntry &e = g_import_list[i];
		auto it = registry().find({e.lib, e.name});
		if (it != registry().end()) {
			s_real[i] = it->second;
			g_imports[i] = i < wraps.size() ? wraps[i] : it->second;
			implemented++;
		} else {
			g_imports[i] = i < tramps.size() ? tramps[i] : tramps[0];
		}
	}
	(void)s_unimpl_index;
	rt_log(LOG_RT, "HLE: %d of %zu imports implemented", implemented, g_import_count);
}

void install_hle_by_name(const char *lib, const char *name, HLEFunc fn) { registry()[{lib, name}] = fn; }
