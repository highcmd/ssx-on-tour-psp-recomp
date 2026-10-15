// SSX On Tour — statically recompiled PSP game, native runtime entry point.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <filesystem>
#include <string>
#include <system_error>

#include "kernel.h"
#include "platform.h"
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
	        "usage: ssx [--iso <umd.iso>] [--umd <extracted UMD dir>] [--image <image.bin>] [--assets <dir>] [--ms <memory stick dir>] [--headless]\n%s"
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
#ifdef SIGHUP
	signal(SIGHUP, on_int);
#endif
	platform_init();
	bool headless = false;
#ifdef SSX_PORTABLE
	// Self-contained folder: ssx.exe, image.bin, assets/ side by side. It is also
	// the working directory, so ssx.ini, ssx.log and the saves (ms0/) live there.
	const std::string base = platform_exe_dir();
	{
		std::error_code ec;
		if (!base.empty()) std::filesystem::current_path(u8p(base), ec);
	}
	std::string image = base + "image.bin";
	g_assetDir = base + "assets";
	g_iso_path = "";
#else
	std::string image = SSX_DEFAULT_IMAGE;
	g_assetDir = SSX_DEFAULT_ASSETS;
	g_iso_path = SSX_DEFAULT_ISO;
#endif
	settings_load("ssx.ini");
	if (!g_settings.isoPath.empty()) g_iso_path = g_settings.isoPath;
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

	if (g_iso_path.empty() || !std::filesystem::exists(u8p(g_iso_path))) {
		// Look for the image next to the executable, then in the working directory.
		std::string found;
		for (std::string dir : {platform_exe_dir(), std::string("./")}) {
			std::error_code ec;
			for (std::filesystem::directory_iterator it(u8p(dir.empty() ? "." : dir), ec), e;
			     !ec && it != e && found.empty(); it.increment(ec)) {
				std::string ext = u8s(it->path().extension());
				for (auto &ch : ext) ch = (char)tolower((unsigned char)ch);
				if (ext == ".iso") found = u8s(it->path());
			}
			if (!found.empty()) break;
		}
		if (!found.empty()) g_iso_path = found;
	}
	if (g_umd_root.empty() && (g_iso_path.empty() || !std::filesystem::exists(u8p(g_iso_path)))) {
		fprintf(stderr, "No UMD image found. Put your SSX On Tour (ULUS-10042) .iso next to ssx, "
		                "set 'iso = <path>' in ssx.ini, or pass --iso <path>.\n");
		return 1;
	}

	rt_log_init();
	rt_log(LOG_RT, "UMD: %s", g_umd_root.empty() ? g_iso_path.c_str() : g_umd_root.c_str());
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
