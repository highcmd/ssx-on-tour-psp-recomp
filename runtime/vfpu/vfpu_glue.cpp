// Glue that PPSSPP's MIPS.cpp / VFS normally provide for the ported VFPU code.
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include "Core/MIPS/MIPS.h"
#include "Core/System.h"
#include "Common/File/VFS/VFS.h"

u8 voffset[128];
u8 fromvoffset[128];

const float cst_constants[32] = {
	0,
	std::numeric_limits<float>::max(),
	sqrtf(2.0f),
	sqrtf(0.5f),
	2.0f / sqrtf((float)M_PI),
	2.0f / (float)M_PI,
	1.0f / (float)M_PI,
	(float)M_PI / 4,
	(float)M_PI / 2,
	(float)M_PI,
	(float)M_E,
	(float)M_LOG2E,
	(float)M_LOG10E,
	(float)M_LN2,
	(float)M_LN10,
	2 * (float)M_PI,
	(float)M_PI / 6,
	log10f(2.0f),
	logf(10.0f) / logf(2.0f),
	sqrtf(3.0f) / 2.0f,
};

// Storage order 0YYMMMXX -> 0MMMXXYY, as in PPSSPP (matrices contiguous, columns contiguous).
static struct VOffsetInit {
	VOffsetInit() {
		int i = 0;
		for (int m = 0; m < 8; m++)
			for (int y = 0; y < 4; y++)
				for (int x = 0; x < 4; x++)
					voffset[m * 4 + x * 32 + y] = i++;
		for (int k = 0; k < 128; k++)
			fromvoffset[voffset[k]] = k;
	}
} s_voffsetInit;

CoreParameterShim &PSP_CoreParameter() {
	static CoreParameterShim p;
	return p;
}

std::string g_assetDir = "assets";
VFSShim g_VFS;
uint8_t *VFSShim::ReadFile(const char *name, size_t *size) {
	std::string path = g_assetDir + "/" + name;
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) { *size = 0; return nullptr; }
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *buf = new uint8_t[*size];
	if (fread(buf, 1, *size, f) != *size) { delete[] buf; buf = nullptr; *size = 0; }
	fclose(f);
	return buf;
}
