// Hooks on specific SSX On Tour functions (see data/hooks.json).
#include "kernel.h"
#include "settings.h"

void kernel_set_vblank_hz(double hz);

// F_0886E774(timer object, hz, ...): sets the engine's update rate. It stores hz
// at 0x08B214C4, programs the game alarm to 1/hz and derives dt = 1/hz, which
// the simulation uses — so raising hz gives more frames at the same game speed.
// The game asks for 60 Hz in menus and 30 Hz (20 when struggling) in races.
void hook_SetGameRate(CPUState *c, RecompFunc orig) {
	u32 req = c->r[5];
	u32 hz = req;
	int target = g_settings.fps;
	if (target > 30 && req <= 30) hz = (u32)target;   // races: lift to the chosen rate
	// The flip routine always waits two vblanks per frame (30 fps max on a PSP),
	// so a raised rate also needs a faster virtual vblank. At 2 x hz the flip
	// sometimes ends just after the next tick and a frame is lost; at 4 x hz it
	// always fits inside one tick (the tick, not the vblank, paces frames). Menus and
	// movies keep the real 59.94 Hz.
	kernel_set_vblank_hz(hz != req ? 4.0 * hz : 59.94);   // 4x: the 2-vblank flip then takes half a tick
	if (hz != req) rt_log(LOG_MISC, "game rate %u Hz requested -> %u Hz", req, hz);
	c->r[5] = hz;
	orig(c);
}
