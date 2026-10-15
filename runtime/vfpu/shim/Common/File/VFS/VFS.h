#pragma once
#include <cstddef>
#include <cstdint>
// Reads a file from runtime/assets; returns new[]'d buffer.
struct VFSShim { uint8_t *ReadFile(const char *name, size_t *size); };
extern VFSShim g_VFS;
