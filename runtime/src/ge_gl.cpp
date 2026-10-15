// OpenGL 3.3 core backend for the GE (see ge_gl.h).
//
// Threads / contexts (all objects shared):
//   GE thread      - context bound to a hidden 1x1 window; draws into FBOs.
//   present thread - context bound to the game window; shows the render target
//                    the game displays (sceDisplaySetFrameBuf), letterboxed.
// Cross-context ordering: the GE thread puts a fence after each frame
// (FINISH) for every target it drew into; the present thread glWaitSync()s it.
//
// Orientation: PSP row y is GL row y (row 0 at the bottom of the GL texture),
// so scissor rects and texture rows need no flipping; only presentation flips.
#include "ge_gl.h"

#include "platform.h"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gl_funcs.h"
#include "hle.h"

extern int g_render_scale;

namespace {

// ------------------------------------------------------------------ shared state
struct ColorTarget {
	GLuint tex = 0;
	u32 fmt = 3, stride = 512;
	GLsync fence = nullptr;
	u64 lastDrawUs = 0;
	bool drawnSinceFence = false;
};
std::mutex g_tmtx;
std::map<u32, ColorTarget> g_color;      // by VRAM address (0x04xxxxxx)
std::atomic<u64> g_frameSerial{0};

SDL_Window *g_win, *g_geWin;
SDL_GLContext g_presentCtx, g_geCtx;
bool g_active, g_headless, g_vsync, g_linear, g_stencilExport;
int TW = 512, TH = 512;

// ------------------------------------------------------------------ helpers
GLuint compile(GLenum type, const std::string &src) {
	GLuint s = glCreateShader(type);
	const char *p = src.c_str();
	glShaderSource(s, 1, &p, nullptr);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[4096];
		glGetShaderInfoLog(s, sizeof(log), nullptr, log);
		rt_log_error("GL shader compile failed:\n%s", log);
	}
	return s;
}

GLuint link(const std::string &vs, const std::string &fs) {
	GLuint p = glCreateProgram();
	glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
	glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
	glLinkProgram(p);
	GLint ok = 0;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[4096];
		glGetProgramInfoLog(p, sizeof(log), nullptr, log);
		rt_log_error("GL program link failed:\n%s", log);
	}
	return p;
}

inline u32 c565(u16 x) { return 0xFF000000u | (((x >> 11) * 255 / 31) << 16) | ((((x >> 5) & 63) * 255 / 63) << 8) | ((x & 31) * 255 / 31); }
inline u32 c5551(u16 x) { return ((x >> 15) ? 0xFF000000u : 0) | ((((x >> 10) & 31) * 255 / 31) << 16) | ((((x >> 5) & 31) * 255 / 31) << 8) | ((x & 31) * 255 / 31); }
inline u32 c4444(u16 x) { return ((x >> 12) * 17u << 24) | (((x >> 8) & 15) * 17u << 16) | (((x >> 4) & 15) * 17u << 8) | ((x & 15) * 17u); }

// Decode a PSP framebuffer in VRAM into RGBA8 rows (w x h).
void decode_vram(u32 addr, u32 stride, u32 fmt, int w, int h, std::vector<u32> &out) {
	out.assign((size_t)w * h, 0xFF000000u);
	u32 bpp = fmt == 3 ? 4 : 2;
	u32 base = addr & 0x1FFFFF;
	for (int y = 0; y < h; y++) {
		if (base + (size_t)(y + 1) * stride * bpp > 0x200000) break;
		const u8 *row = gptr(0x04000000 | (base + y * stride * bpp));
		for (int x = 0; x < w && x < (int)stride; x++) {
			u32 c;
			if (bpp == 4) memcpy(&c, row + x * 4, 4);
			else {
				u16 p;
				memcpy(&p, row + x * 2, 2);
				c = fmt == 0 ? c565(p) : fmt == 1 ? c5551(p) : c4444(p);
			}
			out[(size_t)y * w + x] = c;
		}
	}
}

// ================================================================== GE thread side
struct GeGL {
	bool inited = false, failed = false;
	GLuint prog = 0, vao = 0, vbo = 0;
	GLint uMode, uMode2, uAlpha, uTexEnv, uFogColor, uFix, uZRange, uUVScale, uTex;
	GLuint samplers[8];
	std::map<u32, GLuint> depth;            // z buffer address -> D24S8 texture
	std::map<u64, GLuint> fbos;             // (colorTex << 32 | depthTex) -> FBO
	GLuint uploadTex = 0, uploadFbo = 0;
	size_t vboSize = 0;
} G;

const char *GE_VS = R"(#version 330 core
layout(location = 0) in vec4 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
layout(location = 3) in float aFog;
out vec2 vUV;
out vec4 vColor;
out float vFog;
void main() { gl_Position = aPos; vUV = aUV; vColor = aColor; vFog = aFog; }
)";

const char *GE_FS = R"(
in vec2 vUV;
in vec4 vColor;
in float vFog;
uniform sampler2D uTex;
uniform ivec4 uMode;    // clear, texture, texture function, use texture alpha
uniform ivec4 uMode2;   // colour doubling, fog, alpha test func (-1 off), blend pre-multiply mode
uniform ivec4 uAlpha;   // alpha ref, alpha mask, stencil export (0 off, 1 8-bit, 2 1-bit, 3 4-bit), z range test
uniform vec3 uTexEnv;
uniform vec3 uFogColor;
uniform vec3 uFix;
uniform vec2 uZRange;
uniform vec2 uUVScale;
out vec4 fragColor;

bool cmpf(int f, int a, int b) {
	if (f == 0) return false;
	if (f == 1) return true;
	if (f == 2) return a == b;
	if (f == 3) return a != b;
	if (f == 4) return a < b;
	if (f == 5) return a <= b;
	if (f == 6) return a > b;
	return a >= b;
}

void main() {
	vec4 c = vColor;
	if (uMode.x == 0) {
		if (uMode.y != 0) {
			vec4 t = texture(uTex, vUV * uUVScale);
			float dbl = uMode2.x != 0 ? 2.0 : 1.0;
			bool rgba = uMode.w != 0;
			int f = uMode.z;
			if (f == 0) { c.rgb = c.rgb * t.rgb * dbl; if (rgba) c.a *= t.a; }
			else if (f == 1) { c.rgb = (rgba ? mix(c.rgb, t.rgb, t.a) : t.rgb) * dbl; }
			else if (f == 2) { c.rgb = mix(c.rgb, uTexEnv, t.rgb) * dbl; if (rgba) c.a *= t.a; }
			else if (f == 3) { c.rgb = t.rgb * dbl; if (rgba) c.a = t.a; }
			else { c.rgb = c.rgb + t.rgb * dbl; if (rgba) c.a *= t.a; }
			c = clamp(c, 0.0, 1.0);
		}
		if (uMode2.y != 0) c.rgb = mix(uFogColor, c.rgb, vFog);
		if (uMode2.z >= 0) {
			int a = int(c.a * 255.0 + 0.5);
			if (!cmpf(uMode2.z, a & uAlpha.y, uAlpha.x & uAlpha.y)) discard;
		}
		if (uAlpha.w != 0) {
			float z = gl_FragCoord.z * 65535.0;
			if (z < uZRange.x - 0.5 || z > uZRange.y + 0.5) discard;
		}
		int bp = uMode2.w;
		if (bp == 1) c.rgb = min(c.rgb * 2.0 * c.a, vec3(1.0));
		else if (bp == 2) c.rgb *= 1.0 - min(2.0 * c.a, 1.0);
		else if (bp == 3) c.rgb *= uFix;
		else if (bp == 4) c.rgb = min(c.rgb * 2.0, vec3(1.0));
	}
	fragColor = c;
#ifdef STENCIL_EXPORT
	if (uAlpha.z != 0) {
		int s = int(c.a * 255.0 + 0.5);
		if (uAlpha.z == 2) s = s >= 128 ? 255 : 0;
		else if (uAlpha.z == 3) s = (s >> 4) * 17;
		gl_FragStencilRefARB = s;
	}
#endif
}
)";

bool has_extension(const char *name) {
	GLint n = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &n);
	for (GLint i = 0; i < n; i++)
		if (!strcmp((const char *)glGetStringi(GL_EXTENSIONS, i), name)) return true;
	return false;
}

bool ge_init() {
	if (G.inited) return !G.failed;
	G.inited = true;
	if (SDL_GL_MakeCurrent(g_geWin, g_geCtx) != 0) {
		rt_log_error("GL: GE thread MakeCurrent failed: %s", SDL_GetError());
		G.failed = true;
		return false;
	}
	g_stencilExport = has_extension("GL_ARB_shader_stencil_export");
	std::string fs = std::string("#version 330 core\n") +
	                 (g_stencilExport ? "#extension GL_ARB_shader_stencil_export : require\n#define STENCIL_EXPORT 1\n" : "") + GE_FS;
	G.prog = link(GE_VS, fs);
	G.uMode = glGetUniformLocation(G.prog, "uMode");
	G.uMode2 = glGetUniformLocation(G.prog, "uMode2");
	G.uAlpha = glGetUniformLocation(G.prog, "uAlpha");
	G.uTexEnv = glGetUniformLocation(G.prog, "uTexEnv");
	G.uFogColor = glGetUniformLocation(G.prog, "uFogColor");
	G.uFix = glGetUniformLocation(G.prog, "uFix");
	G.uZRange = glGetUniformLocation(G.prog, "uZRange");
	G.uUVScale = glGetUniformLocation(G.prog, "uUVScale");
	G.uTex = glGetUniformLocation(G.prog, "uTex");
	glUseProgram(G.prog);
	glUniform1i(G.uTex, 0);

	glGenVertexArrays(1, &G.vao);
	glBindVertexArray(G.vao);
	glGenBuffers(1, &G.vbo);
	glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(GLVert), (void *)offsetof(GLVert, x));
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GLVert), (void *)offsetof(GLVert, u));
	glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GLVert), (void *)offsetof(GLVert, color));
	glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(GLVert), (void *)offsetof(GLVert, fog));
	for (int i = 0; i < 4; i++) glEnableVertexAttribArray(i);

	glGenSamplers(8, G.samplers);
	for (int i = 0; i < 8; i++) {
		bool lin = i & 1, cu = i & 2, cv = i & 4;
		glSamplerParameteri(G.samplers[i], GL_TEXTURE_MIN_FILTER, lin ? GL_LINEAR : GL_NEAREST);
		glSamplerParameteri(G.samplers[i], GL_TEXTURE_MAG_FILTER, lin ? GL_LINEAR : GL_NEAREST);
		glSamplerParameteri(G.samplers[i], GL_TEXTURE_WRAP_S, cu ? GL_CLAMP_TO_EDGE : GL_REPEAT);
		glSamplerParameteri(G.samplers[i], GL_TEXTURE_WRAP_T, cv ? GL_CLAMP_TO_EDGE : GL_REPEAT);
	}
	glGenTextures(1, &G.uploadTex);
	glBindTexture(GL_TEXTURE_2D, G.uploadTex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 512, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glGenFramebuffers(1, &G.uploadFbo);
	glBindFramebuffer(GL_FRAMEBUFFER, G.uploadFbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, G.uploadTex, 0);
	glEnable(GL_DEPTH_CLAMP);
	glDisable(GL_CULL_FACE);
	glDisable(GL_DITHER);
	rt_log(LOG_GE, "GL GE thread ready: %s, stencil export %s, targets %dx%d", glGetString(GL_RENDERER),
	       g_stencilExport ? "yes" : "no", TW, TH);
	return true;
}

GLuint fbo_for(GLuint colorTex, GLuint depthTex) {
	u64 key = ((u64)colorTex << 32) | depthTex;
	auto it = G.fbos.find(key);
	if (it != G.fbos.end()) return it->second;
	GLuint f;
	glGenFramebuffers(1, &f);
	glBindFramebuffer(GL_FRAMEBUFFER, f);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
	if (depthTex) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depthTex, 0);
	GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (st != GL_FRAMEBUFFER_COMPLETE) rt_log_error("GL: incomplete FBO %x", st);
	G.fbos[key] = f;
	return f;
}

// Colour target for a VRAM address; created on first use from the VRAM contents.
GLuint color_target(u32 addr, u32 stride, u32 fmt) {
	GLuint tex;
	bool created = false;
	{
		std::lock_guard<std::mutex> lk(g_tmtx);
		ColorTarget &t = g_color[addr & 0x0FFFFFFF];
		if (!t.tex) {
			glGenTextures(1, &t.tex);
			glBindTexture(GL_TEXTURE_2D, t.tex);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, TW, TH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			created = true;
		}
		t.fmt = fmt;
		t.stride = stride;
		t.lastDrawUs = rt_time_us();
		t.drawnSinceFence = true;
		tex = t.tex;
	}
	if (created) {
		// start from what VRAM holds (nearest upscale)
		std::vector<u32> px;
		decode_vram(addr, stride, fmt, 512, 512, px);
		glBindTexture(GL_TEXTURE_2D, G.uploadTex);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 512, 512, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
		GLuint dst = fbo_for(tex, 0);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, G.uploadFbo);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst);
		glDisable(GL_SCISSOR_TEST);
		glBlitFramebuffer(0, 0, 512, 512, 0, 0, TW, TH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		rt_log(LOG_GE, "GL: new render target %08x stride %u fmt %u", addr, stride, fmt);
	}
	return tex;
}

GLuint depth_target(u32 addr) {
	auto it = G.depth.find(addr);
	if (it != G.depth.end()) return it->second;
	GLuint t;
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, TW, TH, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	G.depth[addr] = t;
	return t;
}

const GLenum kCmp[8] = {GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL, GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL};
const GLenum kStencilOp[8] = {GL_KEEP, GL_ZERO, GL_REPLACE, GL_INVERT, GL_INCR, GL_DECR, GL_KEEP, GL_KEEP};

// PSP blend factors -> GL, with shader pre-multiplication for the doubled ones.
void setup_blend(const GLDrawState &st, int &pre, float *fixOut) {
	pre = 0;
	if (!st.blend || st.clear) { glDisable(GL_BLEND); return; }
	glEnable(GL_BLEND);
	bool constUsed = false;
	u32 constColor = 0;
	auto fixval = [](u32 c) { return c & 0xFFFFFF; };
	GLenum sf, df;
	switch (st.bSrc) {
	case 0: sf = GL_DST_COLOR; break;
	case 1: sf = GL_ONE_MINUS_DST_COLOR; break;
	case 2: sf = GL_SRC_ALPHA; break;
	case 3: sf = GL_ONE_MINUS_SRC_ALPHA; break;
	case 4: sf = GL_DST_ALPHA; break;
	case 5: sf = GL_ONE_MINUS_DST_ALPHA; break;
	case 6: sf = GL_ONE; pre = 1; break;               // 2 * src alpha
	case 7: sf = GL_ONE; pre = 2; break;               // 1 - 2 * src alpha
	case 8: sf = GL_DST_ALPHA; pre = 4; break;         // 2 * dst alpha
	case 9: sf = GL_ONE_MINUS_DST_ALPHA; break;        // approximation
	default:
		if (fixval(st.fixA) == 0xFFFFFF) sf = GL_ONE;
		else if (fixval(st.fixA) == 0) sf = GL_ZERO;
		else { sf = GL_CONSTANT_COLOR; constUsed = true; constColor = st.fixA; }
		break;
	}
	switch (st.bDst) {
	case 0: df = GL_SRC_COLOR; break;
	case 1: df = GL_ONE_MINUS_SRC_COLOR; break;
	case 2: case 6: df = GL_SRC_ALPHA; break;
	case 3: case 7: df = GL_ONE_MINUS_SRC_ALPHA; break;
	case 4: case 8: df = GL_DST_ALPHA; break;
	case 5: case 9: df = GL_ONE_MINUS_DST_ALPHA; break;
	default: {
		u32 fb = fixval(st.fixB);
		if (fb == 0xFFFFFF) df = GL_ONE;
		else if (fb == 0) df = GL_ZERO;
		else if (!constUsed) { df = GL_CONSTANT_COLOR; constUsed = true; constColor = st.fixB; }
		else if (fb == (~constColor & 0xFFFFFF) || ((fb + constColor) & 0xFFFFFF) == 0xFFFFFF ||
		         (((fb >> 0) & 255) + ((constColor >> 0) & 255) >= 254 && ((fb >> 0) & 255) + ((constColor >> 0) & 255) <= 256)) {
			df = GL_ONE_MINUS_CONSTANT_COLOR;
		} else {
			// both factors are distinct constants: fold the source one into the shader
			pre = 3;
			sf = GL_ONE;
			for (int k = 0; k < 3; k++) fixOut[k] = ((st.fixA >> (8 * k)) & 255) / 255.0f;
			constColor = st.fixB;
			df = GL_CONSTANT_COLOR;
		}
		break;
	}
	}
	if (constUsed || pre == 3)
		glBlendColor((constColor & 255) / 255.0f, ((constColor >> 8) & 255) / 255.0f, ((constColor >> 16) & 255) / 255.0f, 1.0f);
	static const GLenum eqs[8] = {GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX, GL_MAX, GL_FUNC_ADD, GL_FUNC_ADD};
	glBlendEquationSeparate(eqs[st.bEq & 7], GL_FUNC_ADD);
	// PSP: the alpha channel is not blended; it receives the source alpha (or the stencil result)
	glBlendFuncSeparate(sf, df, GL_ONE, GL_ZERO);
}

}  // namespace

// ================================================================== GE thread API
bool gl_backend_active() { return g_active; }
int gl_target_width() { return TW; }

bool gl_find_color_target(u32 addr, u32 *base, u32 *stride, u32 *fmt) {
	std::lock_guard<std::mutex> lk(g_tmtx);
	addr &= 0x0FFFFFFF;
	for (auto &[b, t] : g_color) {
		u32 bytes = t.stride * (t.fmt == 3 ? 4 : 2) * 512;
		if (t.tex && addr >= b && addr < b + bytes) {
			*base = b; *stride = t.stride; *fmt = t.fmt;
			return true;
		}
	}
	return false;
}

bool gl_has_color_target(u32 addr) {
	std::lock_guard<std::mutex> lk(g_tmtx);
	auto it = g_color.find(addr & 0x0FFFFFFF);
	return it != g_color.end() && it->second.tex;
}

void gl_ge_draw(const GLDrawState &st, const GLVert *v, int count) {
	if (count <= 0 || !ge_init()) return;
	bool fbHasAlpha = st.fbFmt != 0;
	GLuint ct = color_target(st.fbAddr, st.fbStride, st.fbFmt);
	GLuint dt = depth_target(st.zbAddr);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo_for(ct, dt));
	glViewport(0, 0, TW, TH);
	glEnable(GL_SCISSOR_TEST);
	glScissor(st.sx0, st.sy0, std::max(0, st.sx1 - st.sx0 + 1), std::max(0, st.sy1 - st.sy0 + 1));
	glUseProgram(G.prog);
	glBindVertexArray(G.vao);

	int pre = 0;
	float fix[3] = {1, 1, 1};
	int stencilExport = 0;
	int exportKind = st.fbFmt == 1 ? 2 : st.fbFmt == 2 ? 3 : 1;
	u32 m = st.writeMask;
	bool aWrite = fbHasAlpha && (m >> 24) != 0xFF;
	if (st.clear) {
		glDisable(GL_BLEND);
		bool cc = st.clearColor, ca = st.clearAlpha && fbHasAlpha;
		glColorMask(cc, cc, cc, ca);
		if (st.clearDepth) { glEnable(GL_DEPTH_TEST); glDepthFunc(GL_ALWAYS); glDepthMask(GL_TRUE); }
		else { glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); }
		if (ca && g_stencilExport) {
			glEnable(GL_STENCIL_TEST);
			glStencilFunc(GL_ALWAYS, 0, 0xFF);
			glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
			glStencilMask(0xFF);
			stencilExport = exportKind;
		} else {
			glDisable(GL_STENCIL_TEST);
			glStencilMask(0);
		}
	} else {
		setup_blend(st, pre, fix);
		glColorMask((m & 0xFF) != 0xFF, ((m >> 8) & 0xFF) != 0xFF, ((m >> 16) & 0xFF) != 0xFF, aWrite);
		if (st.depthTest) {
			glEnable(GL_DEPTH_TEST);
			glDepthFunc(kCmp[st.zFunc & 7]);
			glDepthMask(st.zWrite ? GL_TRUE : GL_FALSE);
		} else {
			glDisable(GL_DEPTH_TEST);   // the PSP does not write Z without the depth test
			glDepthMask(GL_FALSE);
		}
		// The PSP keeps stencil in the framebuffer alpha: mirror alpha writes into GL stencil.
		u32 aMask = fbHasAlpha ? (~(m >> 24) & 0xFF) : 0;
		if (st.stencil && fbHasAlpha) {
			glEnable(GL_STENCIL_TEST);
			glStencilFunc(kCmp[st.stFunc & 7], st.stRef & 0xFF, st.stMask & 0xFF);
			glStencilOp(kStencilOp[st.sFail & 7], kStencilOp[st.sZFail & 7], kStencilOp[st.sZPass & 7]);
			glStencilMask(aMask);
		} else if (aWrite && g_stencilExport) {
			glEnable(GL_STENCIL_TEST);
			glStencilFunc(GL_ALWAYS, 0, 0xFF);
			glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
			glStencilMask(aMask);
			stencilExport = exportKind;
		} else {
			glDisable(GL_STENCIL_TEST);
			glStencilMask(0);
		}
	}

	// texture
	float uvsx = 1, uvsy = 1;
	bool texOn = st.tex && !st.clear;
	if (texOn) {
		glActiveTexture(GL_TEXTURE0);
		GLuint tex = 0;
		if (st.fboTexAddr) {
			std::lock_guard<std::mutex> lk(g_tmtx);
			auto it = g_color.find(st.fboTexAddr);
			if (it != g_color.end()) tex = it->second.tex;
			uvsx = st.texW / 512.0f;
			uvsy = st.texH / 512.0f;
		} else if (st.texRef) {
			GLTexRef &r = *st.texRef;
			if (!r.id) glGenTextures(1, &r.id);
			glBindTexture(GL_TEXTURE_2D, r.id);
			if (r.version != st.texVersion) {
				glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, st.texW, st.texH, 0, GL_RGBA, GL_UNSIGNED_BYTE, st.texPixels);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
				r.version = st.texVersion;
			}
			tex = r.id;
		}
		glBindTexture(GL_TEXTURE_2D, tex);
		glBindSampler(0, G.samplers[(st.texLinear ? 1 : 0) | (st.clampU || st.fboTexAddr ? 2 : 0) | (st.clampV || st.fboTexAddr ? 4 : 0)]);
		if (!tex) texOn = false;
	}

	glUniform4i(G.uMode, st.clear ? 1 : 0, texOn ? 1 : 0, st.texFunc, st.texRGBA ? 1 : 0);
	glUniform4i(G.uMode2, st.colorDouble ? 1 : 0, st.fog ? 1 : 0, st.alphaTest && !st.clear ? (st.atFunc & 7) : -1, pre);
	glUniform4i(G.uAlpha, st.atRef & 0xFF, st.atMask & 0xFF, stencilExport, st.rangeTest ? 1 : 0);
	glUniform3f(G.uTexEnv, (st.texEnv & 255) / 255.0f, ((st.texEnv >> 8) & 255) / 255.0f, ((st.texEnv >> 16) & 255) / 255.0f);
	glUniform3f(G.uFogColor, (st.fogColor & 255) / 255.0f, ((st.fogColor >> 8) & 255) / 255.0f, ((st.fogColor >> 16) & 255) / 255.0f);
	glUniform3f(G.uFix, fix[0], fix[1], fix[2]);
	glUniform2f(G.uZRange, (float)st.minZ, (float)st.maxZ);
	glUniform2f(G.uUVScale, uvsx, uvsy);

	glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
	size_t bytes = (size_t)count * sizeof(GLVert);
	glBufferData(GL_ARRAY_BUFFER, bytes, v, GL_STREAM_DRAW);
	glDrawArrays(GL_TRIANGLES, 0, count);
}

void gl_ge_frame_done() {
	if (!G.inited || G.failed) return;
	{
		std::lock_guard<std::mutex> lk(g_tmtx);
		for (auto &[addr, t] : g_color) {
			if (!t.drawnSinceFence) continue;
			if (t.fence) glDeleteSync(t.fence);
			t.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
			t.drawnSinceFence = false;
		}
	}
	glFlush();
	g_frameSerial++;
}

// ================================================================== present thread
namespace {
struct PresentReq {
	u32 fb = 0, stride = 512, fmt = 3;
};
std::mutex g_pmtx;
std::condition_variable g_pcv;
PresentReq g_req;
bool g_reqPending;
std::string g_shotPath;

const char *PR_VS = R"(#version 330 core
uniform vec4 uRect;   // u0 v0 u1 v1 (v0 = PSP top row)
out vec2 vUV;
void main() {
	vec2 p = vec2((gl_VertexID & 1) != 0 ? 1.0 : -1.0, (gl_VertexID & 2) != 0 ? 1.0 : -1.0);
	vUV = vec2(mix(uRect.x, uRect.z, (p.x + 1.0) * 0.5), mix(uRect.w, uRect.y, (p.y + 1.0) * 0.5));
	gl_Position = vec4(p, 0.0, 1.0);
}
)";
const char *PR_FS = R"(#version 330 core
in vec2 vUV;
uniform sampler2D uTex;
out vec4 fragColor;
void main() { fragColor = vec4(texture(uTex, vUV).rgb, 1.0); }
)";

void write_ppm(const std::string &path, const std::vector<u32> &px, int w, int h) {
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	std::vector<u8> row((size_t)w * 3);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			u32 c = px[(size_t)y * w + x];
			row[x * 3] = c & 255; row[x * 3 + 1] = (c >> 8) & 255; row[x * 3 + 2] = (c >> 16) & 255;
		}
		fwrite(row.data(), 1, row.size(), f);
	}
	fclose(f);
}

void present_thread() {
	platform_set_thread_name("present");
	if (SDL_GL_MakeCurrent(g_win, g_presentCtx) != 0) {
		rt_log_error("GL: present MakeCurrent failed: %s", SDL_GetError());
		return;
	}
	SDL_GL_SetSwapInterval(g_vsync ? 1 : 0);
	GLuint prog = link(PR_VS, PR_FS);
	GLint uRect = glGetUniformLocation(prog, "uRect");
	glUseProgram(prog);
	glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
	GLuint vao;
	glGenVertexArrays(1, &vao);
	GLuint sampler;
	glGenSamplers(1, &sampler);
	glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, g_linear ? GL_LINEAR : GL_NEAREST);
	glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, g_linear ? GL_LINEAR : GL_NEAREST);
	glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	GLuint vramTex;
	glGenTextures(1, &vramTex);
	glBindTexture(GL_TEXTURE_2D, vramTex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 512, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	GLuint readFbo;
	glGenFramebuffers(1, &readFbo);
	std::vector<u32> px;

	u64 lastSerial = ~0ull, lastPresentUs = 0;
	u32 lastFb = ~0u;
	while (true) {
		PresentReq req;
		std::string shot;
		{
			std::unique_lock<std::mutex> lk(g_pmtx);
			g_pcv.wait(lk, [] { return g_reqPending; });
			g_reqPending = false;
			req = g_req;
			shot.swap(g_shotPath);
		}
		u64 serial = g_frameSerial.load(), now = rt_time_us();
		if (shot.empty() && serial == lastSerial && req.fb == lastFb && now - lastPresentUs < 50000) continue;

		GLuint tex = 0;
		int srcW = 480, srcH = 272, texW = 512, texH = 512;
		{
			std::lock_guard<std::mutex> lk(g_tmtx);
			auto it = g_color.find(req.fb & 0x0FFFFFFF);
			if (it != g_color.end() && it->second.tex && now - it->second.lastDrawUs < 500000) {
				tex = it->second.tex;
				if (it->second.fence) glWaitSync(it->second.fence, 0, GL_TIMEOUT_IGNORED);
				srcW = 480 * g_render_scale;
				srcH = 272 * g_render_scale;
				texW = TW;
				texH = TH;
			}
		}
		if (!tex) {   // nothing rendered by the GE here: show VRAM as the CPU left it
			decode_vram(req.fb, req.stride ? req.stride : 512, req.fmt, 480, 272, px);
			glBindTexture(GL_TEXTURE_2D, vramTex);
			glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 480, 272, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
			tex = vramTex;
		}
		if (!g_headless) {
			int dw = 0, dh = 0;
			SDL_GL_GetDrawableSize(g_win, &dw, &dh);
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glDisable(GL_SCISSOR_TEST);
			glDisable(GL_BLEND);
			glDisable(GL_DEPTH_TEST);
			glDisable(GL_STENCIL_TEST);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			glViewport(0, 0, dw, dh);
			glClearColor(0, 0, 0, 1);
			glClear(GL_COLOR_BUFFER_BIT);
			// letterbox to 480:272
			int vw = dw, vh = dw * 272 / 480;
			if (vh > dh) { vh = dh; vw = dh * 480 / 272; }
			glViewport((dw - vw) / 2, (dh - vh) / 2, vw, vh);
			glUseProgram(prog);
			glBindVertexArray(vao);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, tex);
			glBindSampler(0, sampler);
			glUniform4f(uRect, 0.0f, 0.0f, (float)srcW / texW, (float)srcH / texH);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			SDL_GL_SwapWindow(g_win);
		}
		if (!shot.empty()) {
			glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
			glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
			px.assign((size_t)srcW * srcH, 0);
			glPixelStorei(GL_PACK_ALIGNMENT, 4);
			glReadPixels(0, 0, srcW, srcH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
			glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
			write_ppm(shot, px, srcW, srcH);   // GL row 0 = PSP top row
		}
		lastSerial = serial;
		lastFb = req.fb;
		lastPresentUs = now;
	}
}
}  // namespace

bool gl_display_init(SDL_Window *win, bool headless, bool vsync, bool linear) {
	g_win = win;
	g_headless = headless;
	g_vsync = vsync;
	g_linear = linear;
	g_presentCtx = SDL_GL_CreateContext(win);
	if (!g_presentCtx) {
		rt_log_error("GL: cannot create context: %s", SDL_GetError());
		return false;
	}
	if (!gl_load_functions()) return false;
	rt_log(LOG_DISPLAY, "OpenGL: %s / %s", glGetString(GL_VERSION), glGetString(GL_RENDERER));
	g_geWin = SDL_CreateWindow("ssx-ge", 0, 0, 1, 1, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
	SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
	g_geCtx = SDL_GL_CreateContext(g_geWin ? g_geWin : win);
	SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
	if (!g_geWin || !g_geCtx) {
		rt_log_error("GL: cannot create the GE context: %s", SDL_GetError());
		return false;
	}
	SDL_GL_MakeCurrent(win, nullptr);
	TW = TH = 512 * g_render_scale;
	g_active = true;
	std::thread(present_thread).detach();
	return true;
}

void gl_display_present(u32 fbAddr, u32 stride, u32 fmt) {
	{
		std::lock_guard<std::mutex> lk(g_pmtx);
		g_req.fb = fbAddr;
		g_req.stride = stride;
		g_req.fmt = fmt;
		g_reqPending = true;
	}
	g_pcv.notify_one();
}

void gl_display_screenshot(const char *path) {
	std::lock_guard<std::mutex> lk(g_pmtx);
	g_shotPath = path;
	g_reqPending = true;
	g_pcv.notify_one();
}
