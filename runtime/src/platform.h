#pragma once
// The few OS services that differ between Linux and Windows: guest memory,
// fibers for guest threads, thread names, 64-bit file offsets, local time and
// the executable's directory.
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>

#ifdef _WIN32
#define fseeko _fseeki64
#define ftello _ftelli64
#endif

// Reserve size bytes of zeroed read/write memory, committed lazily by the OS.
void *platform_alloc_lazy(size_t size);
void platform_free(void *p, size_t size);

// Cooperative fibers. The thread that calls fiber_init_thread() becomes a fiber
// itself (the scheduler); fiber_create() makes a new one that starts in fn().
struct Fiber;
Fiber *fiber_init_thread();
Fiber *fiber_create(void (*fn)(), size_t stackSize);
void fiber_destroy(Fiber *f);                 // never the running fiber
void fiber_switch(Fiber *from, Fiber *to);    // from = the running fiber

void platform_set_thread_name(const char *name);
void platform_localtime(time_t t, struct tm *out);
std::string platform_exe_dir();               // with a trailing separator
void platform_init();                         // timer resolution etc.

// UTF-8 std::string <-> filesystem path (C++20 made u8path/u8string char8_t-based).
inline std::filesystem::path u8p(const std::string &s) { return std::filesystem::path((const char8_t *)s.c_str()); }
inline std::string u8s(const std::filesystem::path &p) {
	auto u = p.u8string();
	return std::string(u.begin(), u.end());
}
