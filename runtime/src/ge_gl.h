#pragma once
// OpenGL backend for the GE: the CPU side (ge_render.cpp) still decodes,
// transforms, lights and clips; this backend rasterises on the GPU into one
// FBO per PSP render target (at the internal resolution) and presents.
#include <SDL2/SDL.h>
#include <cstdint>

struct GLTexRef {
	unsigned id = 0;
	uint64_t version = ~0ull;
};

struct GLVert {
	float x, y, z, w;   // clip space (already including the render scale)
	float u, v;         // normalised texture coordinates
	uint32_t color;     // 0xAABBGGRR
	float fog;          // 1 = no fog
};

struct GLDrawState {
	uint32_t fbAddr, fbStride, fbFmt, zbAddr;
	int sx0, sy0, sx1, sy1;            // scissor in target pixels (inclusive)
	bool clear, clearColor, clearAlpha, clearDepth;
	bool tex;
	GLTexRef *texRef;                  // decoded texture cache entry
	const uint32_t *texPixels;
	uint32_t texW, texH;
	uint64_t texVersion;
	uint32_t fboTexAddr;               // != 0: sample this render target instead
	bool texLinear, clampU, clampV;
	int texFunc;
	bool texRGBA, colorDouble;
	uint32_t texEnv;
	bool fog;
	uint32_t fogColor;
	bool alphaTest;
	int atFunc, atRef, atMask;
	bool depthTest;
	int zFunc;
	bool zWrite;
	bool rangeTest;
	int minZ, maxZ;
	bool blend;
	int bSrc, bDst, bEq;
	uint32_t fixA, fixB;
	bool stencil;
	int stFunc, stRef, stMask, sFail, sZFail, sZPass;
	uint32_t writeMask;                // bits set = keep destination
};

// --- GE thread
bool gl_backend_active();
void gl_ge_draw(const GLDrawState &st, const GLVert *v, int count);
void gl_ge_frame_done();               // FINISH: publish fences for the display
bool gl_has_color_target(uint32_t vramAddr);
bool gl_find_color_target(uint32_t vramAddr, uint32_t *base, uint32_t *stride, uint32_t *fmt);
int gl_target_width();                 // render target size in pixels (512 * scale)

// --- display side (main thread)
bool gl_display_init(SDL_Window *win, bool headless, bool vsync, bool linear);
void gl_display_present(uint32_t fbAddr, uint32_t stride, uint32_t fmt);
void gl_display_screenshot(const char *path);
