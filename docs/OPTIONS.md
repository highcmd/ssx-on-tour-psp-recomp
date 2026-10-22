# Options and configuration

The easiest way to change settings is the **launcher** (`SSXLauncher.exe` on Windows, `./launcher.sh`
on Linux): its Display and Controls tabs write `ssx.ini` for you. This page describes the file
and the command-line flags behind it.

You can set options in two places:

1. **`ssx.ini`**: the configuration file. On Linux it is read from the directory the game starts
   in; `run.sh` always starts it in `run/`, so the file is `run/ssx.ini`. On Windows it is read
   from the folder of `ssx.exe`. Copy [`ssx.ini.example`](../ssx.ini.example) to begin.
2. **Command-line flags**: `./run.sh --fps 60 --fullscreen`. A flag overrides the same key in the file.

In `ssx.ini`, write `key = value`, one per line; `#` starts a comment. On the command line, use
`--key value`; dashes and underscores are interchangeable (`--render-scale` = `--render_scale`).
`./run.sh --help` prints a short summary.

## Settings

| `ssx.ini` key | Flag | Values | Default | What it does |
|---|---|---|---|---|
| `fps` | `--fps N` | `30`, `60`, `120` | `30` | game frame rate, see [Frame rate](#frame-rate) |
| `resolution` (or `res`) | `--res WxH` | e.g. `1920x1080` | `960x544` | window size |
| `scale` | `--scale N` | `1`, `2`, … | none | shorthand for a window of 480·N × 272·N |
| `fullscreen` | `--fullscreen` / `--windowed` | `true`/`false` | `false` | borderless fullscreen at the desktop resolution; F11 toggles it while playing |
| `renderer` | `--renderer R` | `gl`, `software` | `gl` | `gl` = GPU (OpenGL 3.3); `software` = CPU reference renderer |
| `render_scale` | `--render-scale N` | `0`–`6` | `0` (auto) | internal resolution, see [Resolution](#resolution) |
| `filter` | `--filter F` | `linear`, `nearest` | `linear` | filter used to fit the picture into the window |
| `vsync` | `--vsync` | `true`/`false` | `false` | sync presentation to the monitor refresh rate |
| `deadzone` | `--deadzone N` | `0`–`32767` | `8000` | left-stick deadzone; raise it if the rider drifts |
| `right_stick_dpad` | `--right-stick-dpad` | `true`/`false` | `false` | the right stick also works as the D-pad (menus) |
| `pad_mapping` | `--pad-mapping STR` | SDL mapping line | none | adds a mapping for a pad SDL doesn't know, see [Gamepads](#gamepads) |
| `iso` | `--iso FILE` | path | see [Paths](#paths) | your UMD image |
| `key_<button>` | none | key names | see [Remapping](#remapping) | keyboard keys for a PSP button |
| `pad_<button>` | none | button names | see [Remapping](#remapping) | gamepad buttons for a PSP button |

In `ssx.ini`, booleans accept `true`/`1`/`on`/`yes`; anything else counts as false.

Example `run/ssx.ini` for a 1080p monitor:

```ini
fps = 60
resolution = 1920x1080
fullscreen = true
render_scale = 0      # auto -> 4 (1088 px of internal height)
filter = linear
vsync = true
```

### Resolution

The PSP renders at 480×272. The game is drawn at **480·N × 272·N** on the GPU (`render_scale = N`).
This is real rendering at the higher resolution, with sharp edges and detailed distant geometry, not a
stretched 480×272 image. The finished frame is then fitted into the window with its 480:272 aspect
ratio kept (letterboxed when needed) and filtered with `filter`.

| `render_scale` | Internal resolution | Good for |
|---|---|---|
| 1 | 480×272 | original look |
| 2 | 960×544 | |
| 3 | 1440×816 | 900p and smaller screens |
| 4 | 1920×1088 | 1080p |
| 5 | 2400×1360 | 1440p |
| 6 | 2880×1632 | 1440p+ with supersampling, 4K |

`0` (auto) picks the multiple of 272 nearest to the window height: the desktop height in
fullscreen, otherwise the height from `resolution`. For the sharpest picture, use a window height that
is a multiple of 272 (544, 816, 1088, …) or let auto pick, and keep `filter = linear`.

The GPU cost grows with N². On an integrated AMD Radeon ("Phoenix" APU), N = 4 holds 120 fps in races and
N = 6 holds 60. The `software` renderer is much slower (about 60 fps at N = 1 and 18 fps at N = 3) and is
there as a reference.

### Frame rate

On the PSP, the game runs races at 30 fps. Its simulation is time-based (each tick advances by
1/rate seconds), so the port can raise the race rate to 60 or 120 fps. `30` keeps the original
behaviour. Menus are not affected.

The rider's speed, physics and race timer stay correct at every setting; the race timer was
verified 1:1 with the wall clock at 60 and 120. If you see tearing, turn on `vsync`. With
vsync on, a monitor refresh below the chosen rate caps the frame rate.

### Gamepads

Any pad that SDL2 recognises as a game controller should work (Xbox, PlayStation, Switch Pro and most
clones). It can be plugged in before or after the game starts, and several pads at once are fine.
Tested with a wired Xbox 360 pad.

| PSP | Xbox-style pad | Keyboard |
|---|---|---|
| D-pad | D-pad (and the right stick with `right_stick_dpad`) | arrow keys |
| Analog nub | left stick | I / J / K / L |
| ✕ Cross | A | Z |
| ○ Circle | B | X |
| □ Square | X | A |
| △ Triangle | Y | S |
| L | LB or LT | Q |
| R | RB or RT | E |
| Start | Start | Enter |
| Select | Back | Backspace or right Shift |
| none | none | F11 = toggle fullscreen |

Keyboard and pads can be used at the same time.

### Remapping

Every PSP button can be bound to other keys and pad buttons in `ssx.ini`, or in the launcher's
Controls tab. Separate several bindings with commas; `none` removes all of them.

```ini
key_cross = Space, Z
key_select = none
pad_l = leftshoulder
pad_r = rightshoulder, righttrigger
```

| PSP button | `ssx.ini` names |
|---|---|
| D-pad | `up`, `down`, `left`, `right` |
| ✕ ○ □ △ | `cross`, `circle`, `square`, `triangle` |
| shoulders, Start, Select | `l`, `r`, `start`, `select` |
| analog nub, keyboard only | `analog_up`, `analog_down`, `analog_left`, `analog_right` |

- **Keys** use SDL's key names: `A`–`Z`, `0`–`9`, `Return`, `Space`, `Backspace`, `Tab`,
  `Left Shift`, `Right Shift`, `Left Ctrl`, `Right Ctrl`, `Left Alt`, `Right Alt`, `Up`, `Down`, `Left`,
  `Right`, `Keypad 0`–`Keypad 9`, `Keypad Enter`, `F1`–`F12`, and so on. Letters refer to the key's
  position on a US layout.
- **Pad buttons** use SDL's game controller names: `a`, `b`, `x`, `y`, `leftshoulder`, `rightshoulder`,
  `lefttrigger`, `righttrigger`, `back`, `start`, `guide`, `leftstick`, `rightstick`, `dpup`, `dpdown`,
  `dpleft`, `dpright`.
- The left stick is always the analog nub. Unknown names are reported in `ssx.log`.

If your pad isn't recognised (nothing happens, or the buttons are mixed up), create a mapping line
with a tool such as the SDL2 Gamepad Tool or
[SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) and put it in `ssx.ini`:

```ini
pad_mapping = 03000000xxxxxxxxxxxxxxxxxxxxxxxx,My Pad,a:b0,b:b1,x:b2,y:b3,back:b6,start:b7,leftshoulder:b4,rightshoulder:b5,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,leftx:a0,lefty:a1,lefttrigger:a2,righttrigger:a5,platform:Linux,
```

SDL also reads the `SDL_GAMECONTROLLERCONFIG` environment variable, which holds the same kind of line.

## Paths

The game reads all its files straight from your ISO. It finds the ISO in this order:

1. `--iso FILE` on the command line, or `iso = FILE` in `ssx.ini`
2. on Linux, the image given to `setup.sh` (its path is built into the executable)
3. the first `*.iso` next to the executable, then in the working directory

Other paths, which you rarely need:

| Flag | Default | Contents |
|---|---|---|
| `--image FILE` | Linux: `<repo>/build/gen/image.bin`; Windows: next to `ssx.exe` | initial memory image of the recompiled game |
| `--assets DIR` | Linux: `<repo>/runtime/assets`; Windows: `assets\` next to `ssx.exe` | runtime data (VFPU tables) |
| `--ms DIR` | `ms0` in the working directory (`run/ms0`, or next to `ssx.exe` on Windows) | the memory stick (`ms0:`) |
| `--umd DIR` | none | read the game files from an extracted UMD (`setup.sh --extract-all`) instead of the ISO |

### Saves

Saves are stored on the virtual memory stick, in the same layout as on a PSP (Linux: `run/ms0`,
Windows: `ms0\` next to `ssx.exe`):

```
ms0/PSP/SAVEDATA/ULUS10042001/
    DATA.BIN  PARAM.SFO  ICON0.PNG  ICON1.PMF
```

To back up your progress, copy that folder. These saves are **not encrypted**, so they can't yet be
copied to a real PSP or to PPSSPP, and saves from those can't be loaded either.

## Files written at run time

| File (in `run/`, or next to `ssx.exe` on Windows) | Contents |
|---|---|
| `ssx.log` | runtime log; it is overwritten on every start, so keep a copy when reporting a bug |
| `ms0/` | memory stick (saves) |
| `shot_NNN.ppm` | screenshots, only with `SSX_SHOT_EVERY` |

## Debugging environment variables

These are for development. A normal player never needs them.

| Variable | Example | Effect |
|---|---|---|
| `SSX_LOG` | `SSX_LOG=io,ge` | log only these channels: `kernel thread io display ge audio misc` (`rt` and `unimpl` are always on) |
| `SSX_FPS` | `SSX_FPS=2` | log fps and per-frame timings every 2 s |
| `SSX_SHOT_EVERY` | `SSX_SHOT_EVERY=5` | write a screenshot every 5 s |
| `SSX_INPUT` | `SSX_INPUT="16=start,20.5=cross+down,30=cross:1.0"` | scripted button presses: `seconds=buttons[:duration]` (buttons: `cross circle square triangle up down left right l r start select`) |
| `SSX_GETRACE_AT` | `SSX_GETRACE_AT=40` | dump one frame of GE commands at 40 s |
| `SSX_VBTRACE_AT` | `SSX_VBTRACE_AT=40` | timeline of vblanks, GE and waits around 40 s |
| `SSX_GE_THREADS` | `SSX_GE_THREADS=8` | rasteriser threads of the software renderer |

`--headless` runs without a window and without sound (for automated tests together with
`SSX_INPUT` and `SSX_SHOT_EVERY`).
