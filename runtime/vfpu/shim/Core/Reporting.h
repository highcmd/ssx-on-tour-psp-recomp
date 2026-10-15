#pragma once
#include <cstdio>
namespace Log { enum { CPU, JIT, HLE }; }
void rt_log_error(const char *fmt, ...);
#define ERROR_LOG(cat, ...) rt_log_error(__VA_ARGS__)
#define ERROR_LOG_REPORT(cat, ...) rt_log_error(__VA_ARGS__)
#define ERROR_LOG_REPORT_ONCE(n, cat, ...) rt_log_error(__VA_ARGS__)
#define WARN_LOG(cat, ...) rt_log_error(__VA_ARGS__)
#define _dbg_assert_msg_(cond, ...) do { if (!(cond)) rt_log_error(__VA_ARGS__); } while (0)
#define _dbg_assert_(cond) do {} while (0)
#define VERBOSE_LOG(cat, ...) do {} while (0)
#define INFO_LOG(cat, ...) do {} while (0)
#define _assert_msg_(cond, ...) do { if (!(cond)) rt_log_error(__VA_ARGS__); } while (0)
#define PPSSPP_ARCH(x) PPSSPP_ARCH_##x
#define PPSSPP_ARCH_SSE2 1
#define PPSSPP_ARCH_AMD64 1
#define PPSSPP_ARCH_X86 0
#define PPSSPP_ARCH_ARM_NEON 0
#define PPSSPP_ARCH_ARM64_NEON 0
