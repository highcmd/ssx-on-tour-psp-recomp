// sceDisplay + sceCtrl on SDL2. The PSP framebuffer lives in guest VRAM
// (0x04000000); every vblank we convert it to RGBA and present it.
#include <SDL2/SDL.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "kernel.h"
#include "ge_gl.h"
#include "settings.h"

static SDL_Window *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture *s_tex;
static u32 s_vcount;
static u32 s_fb_addr, s_fb_width = 512, s_fb_fmt = 3;
static u32 s_buttons;
static u8 s_lx = 128, s_ly = 128;
static std::vector<u32> s_rgba(480 * 272);
static int s_tex_scale = 1;
const u32 *ge_shadow_for_display(u32 fbAddr, u32 *stride, int *scale);
extern int g_render_scale;
static bool s_headless;
static bool s_gl;   // OpenGL renderer + present thread

static std::vector<SDL_GameController *> s_pads;

static void open_pad(int index) {
	if (!SDL_IsGameController(index)) return;
	SDL_GameController *p = SDL_GameControllerOpen(index);
	if (!p) return;
	for (auto *q : s_pads) if (q == p) return;
	s_pads.push_back(p);
	rt_log(LOG_DISPLAY, "gamepad connected: %s", SDL_GameControllerName(p));
}

void display_init(bool headless) {
	s_headless = headless;
	const Settings &st = g_settings;
	bool wantGL = st.renderer == "gl";
	if (headless && !wantGL) {
		s_tex_scale = g_render_scale;
		s_rgba.assign((size_t)480 * 272 * s_tex_scale * s_tex_scale, 0);
		return;
	}
	SDL_SetMainReady();   // our main() is not SDL_main
	SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");   // our own SIGINT handler quits at once
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
		rt_log_error("SDL_Init: %s", SDL_GetError());
		s_headless = true;
		return;
	}
	if (!st.padMapping.empty() && SDL_GameControllerAddMapping(st.padMapping.c_str()) < 0)
		rt_log_error("pad_mapping rejected: %s", SDL_GetError());
	g_render_scale = settings_render_scale();
	u32 flags = SDL_WINDOW_RESIZABLE | (st.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
	if (wantGL) {
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
		SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
		SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
		SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
		flags |= SDL_WINDOW_OPENGL | (headless ? SDL_WINDOW_HIDDEN : 0);
	}
	s_win = SDL_CreateWindow("SSX On Tour (recompiled)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, st.width, st.height, flags);
	if (wantGL && s_win && gl_display_init(s_win, headless, st.vsync, st.linear)) {
		s_gl = true;
	} else {
		if (wantGL) rt_log_error("OpenGL renderer unavailable; falling back to the software renderer");
		if (headless) { s_headless = true; s_tex_scale = g_render_scale; s_rgba.assign((size_t)480 * 272 * s_tex_scale * s_tex_scale, 0); return; }
		SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, st.linear ? "1" : "0");
		s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED | (st.vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
		SDL_RenderSetLogicalSize(s_ren, 480, 272);   // keeps the PSP aspect ratio (letterbox)
		s_tex_scale = g_render_scale;
		s_rgba.assign((size_t)480 * 272 * s_tex_scale * s_tex_scale, 0);
		s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 480 * s_tex_scale, 272 * s_tex_scale);
	}
	for (int i = 0; i < SDL_NumJoysticks(); i++) open_pad(i);
	rt_log(LOG_DISPLAY, "%s renderer, internal resolution %dx%d (render scale %d)", s_gl ? "OpenGL" : "software",
	       480 * g_render_scale, 272 * g_render_scale, g_render_scale);
	rt_log(LOG_DISPLAY, "window %dx%d%s, filter %s, vsync %s, fps mode %d, %zu gamepad(s)", st.width, st.height,
	       st.fullscreen ? " fullscreen" : "", st.linear ? "linear" : "nearest", st.vsync ? "on" : "off", st.fps, s_pads.size());
}

u32 display_vcount() { return s_vcount; }

void platform_shutdown() {
	if (s_win) SDL_Quit();
}

static inline u32 conv(u32 fmt, u32 p) {
	u32 r, g, b;
	switch (fmt) {
	case 0:  // 565
		r = (p & 0x1F) << 3; g = ((p >> 5) & 0x3F) << 2; b = ((p >> 11) & 0x1F) << 3;
		break;
	case 1:  // 5551
		r = (p & 0x1F) << 3; g = ((p >> 5) & 0x1F) << 3; b = ((p >> 10) & 0x1F) << 3;
		break;
	case 2:  // 4444
		r = (p & 0xF) << 4; g = ((p >> 4) & 0xF) << 4; b = ((p >> 8) & 0xF) << 4;
		break;
	default:
		return p | 0xFF000000u;
	}
	return 0xFF000000u | (b << 16) | (g << 8) | r;
}

// Fills s_rgba (480S x 272S) and returns S actually used (1 when VRAM is shown).
static int convert_fb() {
	u32 sstride = 0;
	int sc = 1;
	const u32 *sh = ge_shadow_for_display(s_fb_addr, &sstride, &sc);
	if (sh && sc == s_tex_scale) {
		int W = 480 * sc, H = 272 * sc;
		for (int y = 0; y < H; y++) {
			const u32 *src = sh + (size_t)y * sstride;
			u32 *dst = s_rgba.data() + (size_t)y * W;
			for (int x = 0; x < W; x++) dst[x] = src[x] | 0xFF000000u;
		}
		return sc;
	}
	u32 bpp = s_fb_fmt == 3 ? 4 : 2;
	const u8 *src = gptr(s_fb_addr);
	int S = s_tex_scale, W = 480 * S;
	for (int y = 0; y < 272; y++) {
		const u8 *row = src + (size_t)y * s_fb_width * bpp;
		for (int x = 0; x < 480; x++) {
			u32 c;
			if (bpp == 4) { memcpy(&c, row + x * 4, 4); c |= 0xFF000000u; }
			else { u16 p; memcpy(&p, row + x * 2, 2); c = conv(s_fb_fmt, p); }
			for (int dy = 0; dy < S; dy++)
				for (int dx = 0; dx < S; dx++) s_rgba[(size_t)(y * S + dy) * W + x * S + dx] = c;
		}
	}
	return 1;
}

static void write_ppm(const char *path) {
	FILE *f = fopen(path, "wb");
	if (!f) return;
	int W = 480 * s_tex_scale, H = 272 * s_tex_scale;
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (int i = 0; i < W * H; i++) {
		u8 px[3] = {(u8)(s_rgba[i] & 255), (u8)((s_rgba[i] >> 8) & 255), (u8)((s_rgba[i] >> 16) & 255)};
		fwrite(px, 1, 3, f);
	}
	fclose(f);
}

void display_present() {
	static double every = getenv("SSX_SHOT_EVERY") ? atof(getenv("SSX_SHOT_EVERY")) : 0;
	static double next = every;
	static int shot = 0;
	if (!s_fb_addr) return;
	bool wantShot = every > 0 && rt_time_us() / 1e6 >= next;
	if (s_gl) {
		gl_display_present(s_fb_addr, s_fb_width, s_fb_fmt);
		if (wantShot) {
			char name[64];
			snprintf(name, sizeof(name), "shot_%03d.ppm", shot++);
			gl_display_screenshot(name);
			next += every;
		}
		return;
	}
	if (s_headless && !wantShot) return;
	convert_fb();
	if (wantShot) {
		char name[64];
		snprintf(name, sizeof(name), "shot_%03d.ppm", shot++);
		write_ppm(name);
		next += every;
	}
	if (s_headless) return;
	SDL_UpdateTexture(s_tex, nullptr, s_rgba.data(), 480 * s_tex_scale * 4);
	SDL_RenderClear(s_ren);
	SDL_RenderCopy(s_ren, s_tex, nullptr, nullptr);
	SDL_RenderPresent(s_ren);
}

// Scripted input for headless testing: SSX_INPUT="16=start,20.5=cross+down,30=cross:1.0"
// (time in seconds = buttons joined by '+', optional :duration, default 0.15 s)
struct ScriptedPress { double t, dur; u32 buttons; };
static std::vector<ScriptedPress> s_script;
static void parse_script() {
	static bool done;
	if (done) return;
	done = true;
	const char *e = getenv("SSX_INPUT");
	if (!e) return;
	static const struct { const char *n; u32 b; } names[] = {
		{"select", 0x1}, {"start", 0x8}, {"up", 0x10}, {"right", 0x20}, {"down", 0x40}, {"left", 0x80},
		{"l", 0x100}, {"r", 0x200}, {"triangle", 0x1000}, {"circle", 0x2000}, {"cross", 0x4000}, {"square", 0x8000}};
	std::string str = e;
	size_t i = 0;
	while (i < str.size()) {
		size_t j = str.find(',', i);
		if (j == std::string::npos) j = str.size();
		std::string item = str.substr(i, j - i);
		i = j + 1;
		size_t eq = item.find('=');
		if (eq == std::string::npos) continue;
		ScriptedPress p{atof(item.c_str()), 0.15, 0};
		std::string rest = item.substr(eq + 1);
		size_t colon = rest.find(':');
		if (colon != std::string::npos) { p.dur = atof(rest.c_str() + colon + 1); rest = rest.substr(0, colon); }
		size_t k = 0;
		while (k <= rest.size()) {
			size_t plus = rest.find('+', k);
			if (plus == std::string::npos) plus = rest.size();
			std::string n = rest.substr(k, plus - k);
			for (auto &nm : names) if (n == nm.n) p.buttons |= nm.b;
			k = plus + 1;
		}
		s_script.push_back(p);
	}
}
static u32 scripted_buttons() {
	parse_script();
	double now = rt_time_us() / 1e6;
	u32 b = 0;
	for (auto &p : s_script) if (now >= p.t && now < p.t + p.dur) b |= p.buttons;
	return b;
}

// ------------------------------------------------------------- input bindings
// PSP inputs that can be bound; "analog_*" are keyboard stand-ins for the nub.
struct PspInput { const char *name; u32 bit; const char *keys; const char *pad; };
static const PspInput s_inputs[] = {
	{"up", 0x10, "Up", "dpup"},          {"down", 0x40, "Down", "dpdown"},
	{"left", 0x80, "Left", "dpleft"},    {"right", 0x20, "Right", "dpright"},
	{"cross", 0x4000, "Z", "a"},         {"circle", 0x2000, "X", "b"},
	{"square", 0x8000, "A", "x"},        {"triangle", 0x1000, "S", "y"},
	{"l", 0x100, "Q", "leftshoulder,lefttrigger"},
	{"r", 0x200, "E", "rightshoulder,righttrigger"},
	{"start", 0x8, "Return", "start"},   {"select", 0x1, "Backspace,Right Shift", "back"},
	{"analog_up", 0, "I", ""},           {"analog_down", 0, "K", ""},
	{"analog_left", 0, "J", ""},         {"analog_right", 0, "L", ""},
};
enum { AN_UP = 1 << 0, AN_DOWN = 1 << 1, AN_LEFT = 1 << 2, AN_RIGHT = 1 << 3 };
struct KeyBind { SDL_Scancode sc; u32 bit; u32 analog; };
struct PadBind { int button; int axis; u32 bit; };   // button or trigger axis (-1 = unused)
static std::vector<KeyBind> s_keyBinds;
static std::vector<PadBind> s_padBinds;

static std::vector<std::string> split_list(const std::string &v) {
	std::vector<std::string> out;
	size_t i = 0;
	while (i <= v.size()) {
		size_t j = v.find(',', i);
		if (j == std::string::npos) j = v.size();
		std::string t = v.substr(i, j - i);
		size_t a = t.find_first_not_of(" \t"), z = t.find_last_not_of(" \t");
		if (a != std::string::npos && t.substr(a, z - a + 1) != "none") out.push_back(t.substr(a, z - a + 1));
		i = j + 1;
	}
	return out;
}

static void build_bindings() {
	s_keyBinds.clear();
	s_padBinds.clear();
	const auto &cfg = g_settings.bindings;
	for (int i = 0; i < (int)(sizeof(s_inputs) / sizeof(s_inputs[0])); i++) {
		const PspInput &in = s_inputs[i];
		u32 analog = i >= 12 ? 1u << (i - 12) : 0;
		auto k = cfg.find(std::string("key_") + in.name);
		for (const std::string &n : split_list(k != cfg.end() ? k->second : in.keys)) {
			SDL_Scancode sc = SDL_GetScancodeFromName(n.c_str());
			if (sc == SDL_SCANCODE_UNKNOWN) rt_log_error("ssx.ini key_%s: unknown key '%s'", in.name, n.c_str());
			else s_keyBinds.push_back({sc, in.bit, analog});
		}
		if (analog) continue;
		auto pd = cfg.find(std::string("pad_") + in.name);
		for (const std::string &n : split_list(pd != cfg.end() ? pd->second : in.pad)) {
			if (n == "lefttrigger") { s_padBinds.push_back({-1, SDL_CONTROLLER_AXIS_TRIGGERLEFT, in.bit}); continue; }
			if (n == "righttrigger") { s_padBinds.push_back({-1, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, in.bit}); continue; }
			SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(n.c_str());
			if (b == SDL_CONTROLLER_BUTTON_INVALID) rt_log_error("ssx.ini pad_%s: unknown button '%s'", in.name, n.c_str());
			else s_padBinds.push_back({b, -1, in.bit});
		}
	}
}

static void update_buttons() {
	if (s_headless) { s_buttons = scripted_buttons(); return; }
	static bool bound;
	if (!bound) { build_bindings(); bound = true; }
	const Uint8 *k = SDL_GetKeyboardState(nullptr);
	u32 b = scripted_buttons(), analog = 0;
	for (const KeyBind &kb : s_keyBinds)
		if (k[kb.sc]) { b |= kb.bit; analog |= kb.analog; }
	int lx = 128, ly = 128;
	if (analog & AN_LEFT) lx = 0;
	if (analog & AN_RIGHT) lx = 255;
	if (analog & AN_UP) ly = 0;
	if (analog & AN_DOWN) ly = 255;
	const int dz = g_settings.deadzone;
	for (SDL_GameController *pad : s_pads) {
		for (const PadBind &pb : s_padBinds) {
			bool on = pb.button >= 0 ? SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)pb.button)
			                         : SDL_GameControllerGetAxis(pad, (SDL_GameControllerAxis)pb.axis) > 16000;
			if (on) b |= pb.bit;
		}
		// left stick -> PSP analog nub, rescaled outside the deadzone
		auto axis = [&](SDL_GameControllerAxis a) {
			int v = SDL_GameControllerGetAxis(pad, a);
			if (abs(v) < dz) return 128;
			double n = (abs(v) - dz) / double(32767 - dz) * (v < 0 ? -1 : 1);
			return std::clamp((int)lround(128 + n * 127.5), 0, 255);
		};
		int ax = axis(SDL_CONTROLLER_AXIS_LEFTX), ay = axis(SDL_CONTROLLER_AXIS_LEFTY);
		if (ax != 128) lx = ax;
		if (ay != 128) ly = ay;
		if (g_settings.rightStickAsDpad) {
			int rx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX), ry = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY);
			if (rx < -16000) b |= 0x80;
			if (rx > 16000) b |= 0x20;
			if (ry < -16000) b |= 0x10;
			if (ry > 16000) b |= 0x40;
		}
	}
	s_buttons = b;
	s_lx = (u8)lx;
	s_ly = (u8)ly;
}

void display_pump_events() {
	if (s_headless) { update_buttons(); return; }
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		if (e.type == SDL_QUIT) kernel_request_exit();
		if (e.type == SDL_CONTROLLERDEVICEADDED) open_pad(e.cdevice.which);
		if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
			for (size_t i = 0; i < s_pads.size(); i++)
				if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_pads[i])) == e.cdevice.which) {
					rt_log(LOG_DISPLAY, "gamepad disconnected: %s", SDL_GameControllerName(s_pads[i]));
					SDL_GameControllerClose(s_pads[i]);
					s_pads.erase(s_pads.begin() + i);
					break;
				}
		}
		if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F11) {
			bool fs = SDL_GetWindowFlags(s_win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
			SDL_SetWindowFullscreen(s_win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
		}
	}
	update_buttons();
}

u64 g_frames_finished;   // GE FINISH count (game frames), for the FPS meter
void display_on_vblank() {
	s_vcount++;
	static u64 lastT, lastF;
	static double fpsEvery = getenv("SSX_FPS") ? atof(getenv("SSX_FPS")) : 0;
	u64 now = rt_time_us();
	if (fpsEvery > 0 && now - lastT >= fpsEvery * 1e6) {
		extern u64 g_frames_finished;
		extern u64 g_ge_flushes, g_ge_jobs;
		static u64 lastFl, lastJ;
		u64 fr = g_frames_finished - lastF;
		if (lastT && fr) rt_log(LOG_DISPLAY, "fps %.1f flushes/frame %.1f jobs/frame %.0f", fr * 1e6 / (now - lastT),
		                        double(g_ge_flushes - lastFl) / fr, double(g_ge_jobs - lastJ) / fr);
		lastFl = g_ge_flushes; lastJ = g_ge_jobs;
		extern u64 g_prof_guest, g_prof_ge, g_prof_flush;
		static u64 lg, lge, lfl;
		if (fr) rt_log(LOG_DISPLAY, "  per frame ms: guest-thread time %.1f (of which GE transform %.1f, raster flush %.1f)",
		               (g_prof_guest - lg) / 1e3 / fr, (g_prof_ge - lge) / 1e3 / fr, (g_prof_flush - lfl) / 1e3 / fr);
		lg = g_prof_guest; lge = g_prof_ge; lfl = g_prof_flush;
		extern u64 g_ge_list_us, g_ge_list_max_us, g_ge_list_count;
		static u64 llu, llc;
		if (g_ge_list_count > llc)
			rt_log(LOG_DISPLAY, "  GE list time avg %.1f ms max %.1f ms", (g_ge_list_us - llu) / 1e3 / (g_ge_list_count - llc), g_ge_list_max_us / 1e3);
		llu = g_ge_list_us; llc = g_ge_list_count; g_ge_list_max_us = 0;
		extern std::atomic<u64> g_raster_px, g_raster_ns;
		static u64 lpx, lns;
		u64 px = g_raster_px.load(), ns = g_raster_ns.load();
		if (px > lpx) rt_log(LOG_DISPLAY, "  raster: %.0f px/frame, %.1f ns/px (thread time), %.1f ms worker-time/frame", double(px - lpx) / fr, double(ns - lns) / double(px - lpx), (ns - lns) / 1e6 / fr);
		lpx = px; lns = ns;
		extern u64 g_ge_cmd_count;
		static u64 lcmd;
		rt_log(LOG_DISPLAY, "  GE cmds/frame %.0f", double(g_ge_cmd_count - lcmd) / std::max<u64>(fr, 1));
		lcmd = g_ge_cmd_count;
		extern u64 g_sh_create, g_sh_upload, g_sh_resolve;
		rt_log(LOG_DISPLAY, "  shadows: created %llu uploads %llu resolves %llu", (unsigned long long)g_sh_create, (unsigned long long)g_sh_upload, (unsigned long long)g_sh_resolve);
		extern u64 g_flush_why[8];
		rt_log(LOG_DISPLAY, "  flush reasons: vram-tex %llu tex-redecode %llu blit %llu", (unsigned long long)g_flush_why[1], (unsigned long long)g_flush_why[2], (unsigned long long)g_flush_why[3]);
		lastT = now;
		lastF = g_frames_finished;
	}
	display_pump_events();
	display_present();
}

// ------------------------------------------------------------- HLE
HLE(sceDisplay, sceDisplaySetMode) { RET(0); }

HLE(sceDisplay, sceDisplaySetFrameBuf) {
	if (ARG(0)) {
		s_fb_addr = ARG(0);
		s_fb_width = ARG(1);
		s_fb_fmt = ARG(2);
	}
	static int n;
	if (n++ < 3) rt_log(LOG_DISPLAY, "SetFrameBuf(%08x, %u, fmt %u, sync %u)", ARG(0), ARG(1), ARG(2), ARG(3));
	RET(0);
}

HLE(sceDisplay, sceDisplayGetVcount) {
	static double vbTrace = getenv("SSX_VBTRACE_AT") ? atof(getenv("SSX_VBTRACE_AT")) : -1;
	static int n;
	if (vbTrace >= 0 && rt_time_us() / 1e6 >= vbTrace && n++ < 40) rt_log(LOG_DISPLAY, "GetVcount ra=%08x -> %u", c->r[31], s_vcount);
	RET(s_vcount);
}

HLE(sceDisplay, sceDisplayWaitVblankStart) {
	static double vbTrace = getenv("SSX_VBTRACE_AT") ? atof(getenv("SSX_VBTRACE_AT")) : -1;
	static int vbn;
	if (vbTrace >= 0 && rt_time_us() / 1e6 >= vbTrace && vbn++ < 40) {
		extern u64 g_frames_finished;
		rt_log(LOG_DISPLAY, "WaitVblank ra=%08x vcount=%u frames=%llu t=%.2fms", c->r[31], s_vcount, (unsigned long long)g_frames_finished, rt_time_us() / 1e3);
	}
	kernel_wait(WAIT_VBLANK, 0, 0, false);
	RET(0);
}

HLE(sceCtrl, sceCtrlSetSamplingMode) { RET(0); }

HLE(sceCtrl, sceCtrlReadBufferPositive) {
	u32 p = ARG(0);
	int n = (int)ARG(1);
	static int calls;
	static u32 lastB = ~0u;
	if (calls++ < 3 || s_buttons != lastB) rt_log(LOG_DISPLAY, "CtrlReadBufferPositive(%08x, %d) buttons %04x (call %d)", p, n, s_buttons, calls);
	lastB = s_buttons;
	for (int i = 0; i < n; i++) {
		u32 q = p + i * 16;
		SW(q, (u32)rt_time_us());
		SW(q + 4, s_buttons);
		SB(q + 8, s_lx);
		SB(q + 9, s_ly);
		for (int k = 10; k < 16; k++) SB(q + k, 0);
	}
	RET(n);
}
