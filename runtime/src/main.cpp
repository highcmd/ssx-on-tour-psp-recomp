// SSX On Tour — statically recompiled PSP game, native runtime entry point.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <unistd.h>

#include "kernel.h"
#include "settings.h"

void rt_log_init();
void memory_init();
bool memory_load_image(const char *path);
void dispatch_init();
void display_init(bool headless);
void kernel_reserve_image();
extern std::string g_umd_root, g_ms_root, g_assetDir, g_iso_path;

static void usage() {
	fprintf(stderr,
	        "usage: ssx [--umd <extracted UMD dir>] [--image <image.bin>] [--iso <umd.iso>] [--assets <dir>] [--ms <memory stick dir>] [--headless]\n%s"
	        "  keys: arrows=d-pad, Z=cross X=circle A=square S=triangle Q/E=L/R Enter=start Backspace=select IJKL=analog, F11=fullscreen\n"
	        "  gamepad (Xbox layout): A=cross B=circle X=square Y=triangle, LB/LT=L, RB/RT=R, Start, Back=select, left stick/d-pad\n",
	        settings_usage());
}

static void on_term(int) {
	rt_log(LOG_RT, "SIGTERM: thread dump");
	kernel_dump_threads();
	void ge_dump_stats();
	ge_dump_stats();
	extern u64 g_hle_calls[256];
	for (size_t i = 0; i < g_import_count && i < 256; i++)
		if (g_hle_calls[i] > 20000) rt_log(LOG_RT, "  HLE %s: %llu calls", g_import_list[i].name, (unsigned long long)g_hle_calls[i]);
	_exit(3);
}

static void on_int(int) { _exit(0); }   // the window goes away with the process

int main(int argc, char **argv) {
	signal(SIGTERM, on_term);
	signal(SIGINT, on_int);
	signal(SIGHUP, on_int);
	std::string image = SSX_DEFAULT_IMAGE;
	bool headless = false;
	g_umd_root = SSX_DEFAULT_UMD;
	g_assetDir = SSX_DEFAULT_ASSETS;
	g_iso_path = SSX_DEFAULT_ISO;
	settings_load("ssx.ini");
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (settings_parse_arg(argc, argv, i)) continue;
		auto next = [&]() -> std::string { if (i + 1 >= argc) { usage(); exit(1); } return argv[++i]; };
		if (a == "--umd") g_umd_root = next();
		else if (a == "--image") image = next();
		else if (a == "--iso") g_iso_path = next();
		else if (a == "--assets") g_assetDir = next();
		else if (a == "--ms") g_ms_root = next();
		else if (a == "--headless") headless = true;
		else { usage(); return a == "--help" ? 0 : 1; }
	}

	rt_log_init();
	memory_init();
	if (!memory_load_image(image.c_str())) {
		fprintf(stderr, "cannot load %s\n", image.c_str());
		return 1;
	}
	dispatch_init();
	hle_init_imports();
	kernel_reserve_image();
	{ extern int g_render_scale; g_render_scale = settings_render_scale(); }
	display_init(headless);
	void audio_init(bool headless);
	audio_init(headless);
	kernel_init();
	kernel_start_module(g_image_entry);
	kernel_run();
	rt_log(LOG_RT, "exit");
	void platform_shutdown();
	platform_shutdown();
	_exit(0);
}
