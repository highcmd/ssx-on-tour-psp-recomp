#pragma once
// User settings: ssx.ini (key = value) in the working directory, overridden by CLI flags.
#include <string>

struct Settings {
	int fps = 30;              // 30 = original pacing, 60 / 120 = raised game rate (time-based engine)
	int width = 960;           // window size (output resolution)
	int height = 544;
	bool fullscreen = false;   // borderless desktop fullscreen
	bool linear = true;        // scaling filter: linear (smooth) or nearest (sharp pixels)
	bool vsync = false;        // host vsync on present
	int deadzone = 8000;       // gamepad stick deadzone (0..32767)
	bool rightStickAsDpad = false;
	std::string padMapping;    // optional SDL_GameControllerDB line
};

extern Settings g_settings;
void settings_load(const char *path);
// Returns false on an unknown option; i is advanced past consumed arguments.
bool settings_parse_arg(int argc, char **argv, int &i);
const char *settings_usage();
