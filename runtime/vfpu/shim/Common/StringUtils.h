#pragma once
#include <string>
#include <cstdio>
#include <cstdarg>
inline std::string StringFromFormat(const char *fmt, ...) {
	char buf[512]; va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a); return buf;
}
