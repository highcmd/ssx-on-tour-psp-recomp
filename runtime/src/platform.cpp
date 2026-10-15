#include "platform.h"

#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>

void *platform_alloc_lazy(size_t size) {
	// Committed pages are zero-filled on first touch, like an anonymous mapping.
	return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
void platform_free(void *p, size_t) { VirtualFree(p, 0, MEM_RELEASE); }

struct Fiber { void *h; void (*fn)(); };

static void WINAPI fiber_entry(void *arg) { ((Fiber *)arg)->fn(); }

Fiber *fiber_init_thread() {
	Fiber *f = new Fiber{nullptr, nullptr};
	f->h = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
	if (!f->h) { fprintf(stderr, "ConvertThreadToFiberEx failed (%lu)\n", GetLastError()); exit(1); }
	return f;
}

Fiber *fiber_create(void (*fn)(), size_t stackSize) {
	Fiber *f = new Fiber{nullptr, fn};
	f->h = CreateFiberEx(64 << 10, stackSize, FIBER_FLAG_FLOAT_SWITCH, fiber_entry, f);
	if (!f->h) { fprintf(stderr, "CreateFiberEx failed (%lu)\n", GetLastError()); exit(1); }
	return f;
}

void fiber_destroy(Fiber *f) {
	if (!f) return;
	DeleteFiber(f->h);
	delete f;
}

void fiber_switch(Fiber *, Fiber *to) { SwitchToFiber(to->h); }

void platform_set_thread_name(const char *name) {
	// SetThreadDescription exists from Windows 10 1607; look it up so older systems still start.
	using Fn = HRESULT(WINAPI *)(HANDLE, PCWSTR);
	static Fn fn = (Fn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription");
	if (!fn) return;
	wchar_t w[64];
	MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 64);
	fn(GetCurrentThread(), w);
}

void platform_localtime(time_t t, struct tm *out) { localtime_s(out, &t); }

std::string platform_exe_dir() {
	wchar_t w[MAX_PATH * 4];
	DWORD n = GetModuleFileNameW(nullptr, w, MAX_PATH * 4);
	char u[MAX_PATH * 12];
	int m = WideCharToMultiByte(CP_UTF8, 0, w, (int)n, u, sizeof(u) - 1, nullptr, nullptr);
	std::string s(u, m > 0 ? m : 0);
	size_t slash = s.find_last_of("\\/");
	return slash == std::string::npos ? std::string() : s.substr(0, slash + 1);
}

void platform_init() {
	timeBeginPeriod(1);   // 1 ms sleeps and waits instead of the default 15.6 ms
	SetConsoleOutputCP(CP_UTF8);
}

#else
#include <pthread.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

void *platform_alloc_lazy(size_t size) {
	void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	return p == MAP_FAILED ? nullptr : p;
}
void platform_free(void *p, size_t size) { munmap(p, size); }

struct Fiber {
	ucontext_t uc;
	void *stack = nullptr;
	size_t stackSize = 0;
};

Fiber *fiber_init_thread() { return new Fiber(); }

Fiber *fiber_create(void (*fn)(), size_t stackSize) {
	Fiber *f = new Fiber();
	f->stack = mmap(nullptr, stackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	if (f->stack == MAP_FAILED) { perror("mmap fiber stack"); exit(1); }
	f->stackSize = stackSize;
	getcontext(&f->uc);
	f->uc.uc_stack.ss_sp = f->stack;
	f->uc.uc_stack.ss_size = stackSize;
	f->uc.uc_link = nullptr;   // fn never returns
	makecontext(&f->uc, fn, 0);
	return f;
}

void fiber_destroy(Fiber *f) {
	if (!f) return;
	if (f->stack) munmap(f->stack, f->stackSize);
	delete f;
}

void fiber_switch(Fiber *from, Fiber *to) { swapcontext(&from->uc, &to->uc); }

void platform_set_thread_name(const char *name) { pthread_setname_np(pthread_self(), name); }

void platform_localtime(time_t t, struct tm *out) { localtime_r(&t, out); }

std::string platform_exe_dir() {
	char buf[4096];
	ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (n <= 0) return std::string();
	std::string s(buf, n);
	return s.substr(0, s.rfind('/') + 1);
}

void platform_init() {}
#endif
