// sceDisplay + sceCtrl on SDL2. The PSP framebuffer lives in guest VRAM
// (0x04000000); every vblank we convert it to RGBA and present it.
#include <SDL2/SDL.h>
#include <cstring>
#include <string>
#include <vector>

#include "kernel.h"

static SDL_Window *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture *s_tex;
static SDL_GameController *s_pad;
static u32 s_vcount;
static u32 s_fb_addr, s_fb_width = 512, s_fb_fmt = 3;
static u32 s_buttons;
static u8 s_lx = 128, s_ly = 128;
static u32 s_rgba[480 * 272];
static int s_scale = 2;
static bool s_headless;

void display_init(bool headless) {
	s_headless = headless;
	if (headless) return;
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
		rt_log_error("SDL_Init: %s", SDL_GetError());
		s_headless = true;
		return;
	}
	if (const char *s = getenv("SSX_SCALE")) s_scale = atoi(s) > 0 ? atoi(s) : 2;
	s_win = SDL_CreateWindow("SSX On Tour (recompiled)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 480 * s_scale,
	                         272 * s_scale, SDL_WINDOW_RESIZABLE);
	s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED);
	SDL_RenderSetLogicalSize(s_ren, 480, 272);
	s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 480, 272);
	for (int i = 0; i < SDL_NumJoysticks(); i++)
		if (SDL_IsGameController(i)) { s_pad = SDL_GameControllerOpen(i); break; }
}

u32 display_vcount() { return s_vcount; }

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

static void convert_fb() {
	u32 bpp = s_fb_fmt == 3 ? 4 : 2;
	const u8 *src = gptr(s_fb_addr);
	for (int y = 0; y < 272; y++) {
		const u8 *row = src + (size_t)y * s_fb_width * bpp;
		u32 *dst = s_rgba + y * 480;
		if (bpp == 4) {
			for (int x = 0; x < 480; x++) { u32 p; memcpy(&p, row + x * 4, 4); dst[x] = p | 0xFF000000u; }
		} else {
			for (int x = 0; x < 480; x++) { u16 p; memcpy(&p, row + x * 2, 2); dst[x] = conv(s_fb_fmt, p); }
		}
	}
}

static void write_ppm(const char *path) {
	FILE *f = fopen(path, "wb");
	if (!f) return;
	fprintf(f, "P6\n480 272\n255\n");
	for (int i = 0; i < 480 * 272; i++) {
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
	if (s_headless && !wantShot) return;
	convert_fb();
	if (wantShot) {
		char name[64];
		snprintf(name, sizeof(name), "shot_%03d.ppm", shot++);
		write_ppm(name);
		next += every;
	}
	if (s_headless) return;
	SDL_UpdateTexture(s_tex, nullptr, s_rgba, 480 * 4);
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

static void update_buttons() {
	if (s_headless) { s_buttons = scripted_buttons(); return; }
	const Uint8 *k = SDL_GetKeyboardState(nullptr);
	u32 b = scripted_buttons();
	if (k[SDL_SCANCODE_UP]) b |= 0x10;
	if (k[SDL_SCANCODE_RIGHT]) b |= 0x20;
	if (k[SDL_SCANCODE_DOWN]) b |= 0x40;
	if (k[SDL_SCANCODE_LEFT]) b |= 0x80;
	if (k[SDL_SCANCODE_RETURN]) b |= 0x8;        // start
	if (k[SDL_SCANCODE_BACKSPACE] || k[SDL_SCANCODE_RSHIFT]) b |= 0x1;  // select
	if (k[SDL_SCANCODE_Q]) b |= 0x100;           // L
	if (k[SDL_SCANCODE_E]) b |= 0x200;           // R
	if (k[SDL_SCANCODE_S]) b |= 0x1000;          // triangle
	if (k[SDL_SCANCODE_X]) b |= 0x2000;          // circle
	if (k[SDL_SCANCODE_Z]) b |= 0x4000;          // cross
	if (k[SDL_SCANCODE_A]) b |= 0x8000;          // square
	int lx = 128, ly = 128;
	if (k[SDL_SCANCODE_J]) lx = 0;
	if (k[SDL_SCANCODE_L]) lx = 255;
	if (k[SDL_SCANCODE_I]) ly = 0;
	if (k[SDL_SCANCODE_K]) ly = 255;
	if (s_pad) {
		auto btn = [&](SDL_GameControllerButton sb, u32 pb) { if (SDL_GameControllerGetButton(s_pad, sb)) b |= pb; };
		btn(SDL_CONTROLLER_BUTTON_DPAD_UP, 0x10);
		btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x20);
		btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x40);
		btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x80);
		btn(SDL_CONTROLLER_BUTTON_START, 0x8);
		btn(SDL_CONTROLLER_BUTTON_BACK, 0x1);
		btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0x100);
		btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0x200);
		btn(SDL_CONTROLLER_BUTTON_Y, 0x1000);
		btn(SDL_CONTROLLER_BUTTON_B, 0x2000);
		btn(SDL_CONTROLLER_BUTTON_A, 0x4000);
		btn(SDL_CONTROLLER_BUTTON_X, 0x8000);
		int ax = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_LEFTX);
		int ay = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_LEFTY);
		if (abs(ax) > 4000) lx = (ax + 32768) >> 8;
		if (abs(ay) > 4000) ly = (ay + 32768) >> 8;
		if (SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) b |= 0x100;
		if (SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) b |= 0x200;
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
		if (e.type == SDL_CONTROLLERDEVICEADDED && !s_pad) s_pad = SDL_GameControllerOpen(e.cdevice.which);
	}
	update_buttons();
}

void display_on_vblank() {
	s_vcount++;
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

HLE(sceDisplay, sceDisplayGetVcount) { RET(s_vcount); }

HLE(sceDisplay, sceDisplayWaitVblankStart) {
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
