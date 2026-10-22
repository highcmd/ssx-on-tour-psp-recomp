#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "hle.h"
#include <SDL2/SDL.h>
#include <algorithm>

Settings g_settings;

static bool parse_res(const std::string &v, int &w, int &h) {
	return sscanf(v.c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0;
}

static bool parse_bool(const std::string &v) { return v == "1" || v == "true" || v == "on" || v == "yes"; }

static bool apply_setting(const std::string &k, const std::string &v) {
	Settings &s = g_settings;
	if (k == "fps") {
		int f = atoi(v.c_str());
		if (f != 30 && f != 60 && f != 120) { fprintf(stderr, "fps must be 30, 60 or 120\n"); return false; }
		s.fps = f;
	} else if (k == "resolution" || k == "res") {
		if (!parse_res(v, s.width, s.height)) { fprintf(stderr, "resolution must be WxH, e.g. 1920x1088\n"); return false; }
	} else if (k == "scale") {
		int n = atoi(v.c_str());
		if (n < 1) return false;
		s.width = 480 * n; s.height = 272 * n;
	} else if (k == "renderer") {
		if (v != "gl" && v != "software") { fprintf(stderr, "renderer must be gl or software\n"); return false; }
		s.renderer = v;
	} else if (k == "render_scale") {
		int n = atoi(v.c_str());
		if (n < 0 || n > 6) { fprintf(stderr, "render_scale must be 0 (auto) .. 6\n"); return false; }
		s.renderScale = n;
	} else if (k == "fullscreen") s.fullscreen = parse_bool(v);
	else if (k == "refresh") {
		int r = atoi(v.c_str());
		if (r < 0 || r > 1000) { fprintf(stderr, "refresh must be 0 (desktop) or a rate in Hz\n"); return false; }
		s.refresh = r;
	}
	else if (k == "filter") s.linear = v != "nearest";
	else if (k == "vsync") s.vsync = parse_bool(v);
	else if (k == "deadzone") s.deadzone = std::max(0, std::min(32767, atoi(v.c_str())));
	else if (k == "right_stick_dpad") s.rightStickAsDpad = parse_bool(v);
	else if (k == "pad_mapping") s.padMapping = v;
	else if (k == "iso") s.isoPath = v;
	else if (k.rfind("key_", 0) == 0 || k.rfind("pad_", 0) == 0) s.bindings[k] = v;
	else return false;
	return true;
}

void settings_load(const char *path) {
	std::ifstream f(path);
	if (!f) return;
	std::string line;
	while (std::getline(f, line)) {
		size_t h = line.find('#');
		if (h != std::string::npos) line.resize(h);
		size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		auto trim = [](std::string x) {
			size_t a = x.find_first_not_of(" \t\r"), b = x.find_last_not_of(" \t\r");
			return a == std::string::npos ? std::string() : x.substr(a, b - a + 1);
		};
		std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
		if (!apply_setting(k, v)) fprintf(stderr, "%s: ignoring '%s'\n", path, line.c_str());
	}
}

bool settings_parse_arg(int argc, char **argv, int &i) {
	std::string a = argv[i];
	if (a.rfind("--", 0) != 0) return false;
	std::string k = a.substr(2);
	for (auto &ch : k) if (ch == '-') ch = '_';
	if (k == "fullscreen" || k == "vsync" || k == "right_stick_dpad") return apply_setting(k, "1");
	if (k == "windowed") { g_settings.fullscreen = false; return true; }
	if (k == "fps" || k == "renderer" || k == "render_scale" || k == "res" || k == "resolution" || k == "scale" || k == "filter" || k == "deadzone" || k == "pad_mapping" || k == "refresh") {
		if (i + 1 >= argc) return false;
		return apply_setting(k, argv[++i]);
	}
	return false;
}

int settings_render_scale() {
	if (g_settings.renderScale > 0) return g_settings.renderScale;
	int h = g_settings.height;
	if (g_settings.fullscreen && g_settings.refresh == 0) {   // borderless: the desktop's size
		SDL_DisplayMode m;
		if (SDL_GetDesktopDisplayMode(0, &m) == 0) h = m.h;
	}
	return std::max(1, std::min(6, (h + 136) / 272));   // nearest multiple of 272
}

const char *settings_usage() {
	return "  --renderer gl|software  GPU (OpenGL 3.3, default) or the CPU reference renderer\n"
	       "  --render-scale N         internal resolution 480x272 x N, 1..6 (0/default: match the window height, e.g. 1088 -> 4)\n"
	       "  --fps 30|60|120          game rate (30 = original; 60/120 raise the engine's tick)\n"
	       "  --res WxH | --scale N    window size (default 960x544); --fullscreen / --windowed\n"
	       "  --refresh HZ             with --fullscreen: switch the monitor to --res at HZ (0 = keep the desktop mode)\n"
	       "  --filter linear|nearest  scaling filter for the final picture; --vsync\n"
	       "  --deadzone N             gamepad stick deadzone 0..32767 (default 8000); --right-stick-dpad\n"
	       "  --pad-mapping STR        extra SDL GameControllerDB mapping line for an unknown pad\n"
	       "  every setting can also go in ./ssx.ini as key = value (fps, resolution, scale, renderer, render_scale,\n"
	       "  fullscreen, refresh, filter, vsync, deadzone, right_stick_dpad, pad_mapping, iso); flags override the file. See docs/OPTIONS.md\n";
}
