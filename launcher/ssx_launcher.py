#!/usr/bin/env python3
"""SSX On Tour launcher: turn your UMD dump into the PC game, set it up, play.

The launcher holds no game data. "Choose ISO" runs the same pipeline as
setup.sh / setup.bat (verify -> recompile -> build) on the user's own dump, the
settings tabs write ssx.ini, and Play starts the game.

Run from a source checkout (python3 launcher/ssx_launcher.py) it works in the
checkout. Packaged (PyInstaller, see ssx_launcher.spec) it carries the source
tree and works in a per-user folder (%LOCALAPPDATA%\\SSX-Recomp on Windows).
"""
import json, os, pathlib, platform, queue, shutil, subprocess, sys, threading, traceback
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

APP_VERSION = "0.2.2"
BUILD_VERSION = "0.2.1"   # bump when runtime/ or tools/ change, so packaged users rebuild the game
FROZEN = getattr(sys, "frozen", False)
IS_WIN = os.name == "nt"
HERE = pathlib.Path(__file__).resolve().parent

# ----------------------------------------------------------------- language

TEXT = {
    "en": {
        "title": "SSX On Tour — PC launcher",
        "tab_game": "Game", "tab_display": "Display", "tab_controls": "Controls",
        "play": "PLAY", "running": "Running…",
        "status_none": "Not installed yet: choose your SSX On Tour (USA) ISO to build the game.",
        "status_ready": "Ready to play.",
        "status_stale": "The launcher was updated: rebuild the game once (about a minute).",
        "status_busy": "Working… this takes a minute or two.",
        "iso": "Game image (ISO)", "iso_none": "(none selected)",
        "choose_iso": "Choose ISO…", "rebuild": "Rebuild",
        "open_folder": "Open game folder", "open_saves": "Open saves folder",
        "log": "Details",
        "iso_types": "PSP game image", "all_files": "All files",
        "legal": "Use a dump of a UMD you own. The game is built on this computer from your copy; "
                 "nothing from the game is downloaded or shared.",
        "resolution": "Window size", "desktop": "Desktop", "fullscreen": "Fullscreen",
        "fps": "Frame rate (races)", "fps30": "30 (original)", "fps60": "60", "fps120": "120",
        "vsync": "VSync (no tearing; caps the frame rate at the monitor refresh rate)",
        "scale": "Internal resolution", "scale_auto": "Auto (match the window)",
        "filter": "Scaling", "filter_linear": "Smooth", "filter_nearest": "Sharp pixels",
        "renderer": "Renderer", "renderer_gl": "GPU (OpenGL)", "renderer_sw": "CPU (slow, reference)",
        "monitor": "Monitor: {w}×{h}{hz}",
        "refresh": "Refresh rate", "refresh_desktop": "Desktop (borderless)",
        "fps_hint": "The screen refreshes at {hz} Hz: with VSync on, the game runs at most {hz} fps.",
        "psp": "PSP button", "key": "Keyboard", "key2": "Keyboard (2nd)", "pad": "Gamepad", "pad2": "Gamepad (2nd)",
        "press_key": "Press a key… (Esc = none)", "none": "—",
        "deadzone": "Left stick dead zone", "rstick": "Right stick also works as the D-pad",
        "defaults": "Reset to defaults",
        "stick_note": "The left stick of a gamepad is always the PSP analog stick.",
        "bad_key": "This key can't be used.",
        "build_failed": "Building the game failed:\n\n{err}\n\nThe details are in the log on the Game tab.",
        "game_failed": "The game stopped with an error (exit code {code}).\n\nLast lines of ssx.log:\n\n{tail}",
        "missing_linux": "Some build tools are missing. Install them first, for example on Debian/Ubuntu:\n\n{cmd}",
        "done": "The game is ready. Press PLAY!",
    },
    "pl": {
        "title": "SSX On Tour — launcher PC",
        "tab_game": "Gra", "tab_display": "Obraz", "tab_controls": "Sterowanie",
        "play": "GRAJ", "running": "Gra działa…",
        "status_none": "Gra nie jest jeszcze zainstalowana: wybierz ISO SSX On Tour (USA), aby ją zbudować.",
        "status_ready": "Gotowe do gry.",
        "status_stale": "Launcher został zaktualizowany: przebuduj grę (około minuty).",
        "status_busy": "Pracuję… to potrwa minutę lub dwie.",
        "iso": "Obraz gry (ISO)", "iso_none": "(nie wybrano)",
        "choose_iso": "Wybierz ISO…", "rebuild": "Przebuduj",
        "open_folder": "Otwórz folder gry", "open_saves": "Otwórz folder zapisów",
        "log": "Szczegóły",
        "iso_types": "Obraz gry PSP", "all_files": "Wszystkie pliki",
        "legal": "Użyj zrzutu własnej płyty UMD. Gra jest budowana na tym komputerze z Twojej kopii; "
                 "nic z gry nie jest pobierane ani udostępniane.",
        "resolution": "Rozmiar okna", "desktop": "Pulpit", "fullscreen": "Pełny ekran",
        "fps": "Klatki na sekundę (wyścigi)", "fps30": "30 (oryginał)", "fps60": "60", "fps120": "120",
        "vsync": "VSync (bez rozrywania obrazu; ogranicza fps do odświeżania monitora)",
        "scale": "Rozdzielczość wewnętrzna", "scale_auto": "Auto (dopasuj do okna)",
        "filter": "Skalowanie", "filter_linear": "Gładkie", "filter_nearest": "Ostre piksele",
        "renderer": "Renderer", "renderer_gl": "GPU (OpenGL)", "renderer_sw": "CPU (wolny, referencyjny)",
        "monitor": "Monitor: {w}×{h}{hz}",
        "refresh": "Odświeżanie", "refresh_desktop": "Pulpit (bez ramki)",
        "fps_hint": "Ekran odświeża {hz} Hz: z włączonym VSync gra działa najwyżej w {hz} fps.",
        "psp": "Przycisk PSP", "key": "Klawiatura", "key2": "Klawiatura (2.)", "pad": "Pad", "pad2": "Pad (2.)",
        "press_key": "Naciśnij klawisz… (Esc = brak)", "none": "—",
        "deadzone": "Martwa strefa lewej gałki", "rstick": "Prawa gałka działa też jako krzyżak",
        "defaults": "Przywróć domyślne",
        "stick_note": "Lewa gałka pada to zawsze analog PSP.",
        "bad_key": "Tego klawisza nie można użyć.",
        "build_failed": "Budowanie gry nie powiodło się:\n\n{err}\n\nSzczegóły są w logu na karcie Gra.",
        "game_failed": "Gra zakończyła się błędem (kod {code}).\n\nOstatnie linie ssx.log:\n\n{tail}",
        "missing_linux": "Brakuje narzędzi do budowania. Zainstaluj je najpierw, np. na Debianie/Ubuntu:\n\n{cmd}",
        "done": "Gra jest gotowa. Naciśnij GRAJ!",
    },
}


LANGS = {"en": "English", "pl": "Polski"}
L = TEXT["en"]   # English unless the user picks another language (saved in launcher.json)

# ----------------------------------------------------------------- inputs

# (ssx.ini name, label, default keys, default pad buttons); must match hle_display.cpp
INPUTS = [
    ("up", "D-pad ↑", ["Up"], ["dpup"]),
    ("down", "D-pad ↓", ["Down"], ["dpdown"]),
    ("left", "D-pad ←", ["Left"], ["dpleft"]),
    ("right", "D-pad →", ["Right"], ["dpright"]),
    ("cross", "✕  Cross", ["Z"], ["a"]),
    ("circle", "○  Circle", ["X"], ["b"]),
    ("square", "□  Square", ["A"], ["x"]),
    ("triangle", "△  Triangle", ["S"], ["y"]),
    ("l", "L", ["Q"], ["leftshoulder", "lefttrigger"]),
    ("r", "R", ["E"], ["rightshoulder", "righttrigger"]),
    ("start", "Start", ["Return"], ["start"]),
    ("select", "Select", ["Backspace", "Right Shift"], ["back"]),
    ("analog_up", "Analog ↑", ["I"], None),
    ("analog_down", "Analog ↓", ["K"], None),
    ("analog_left", "Analog ←", ["J"], None),
    ("analog_right", "Analog →", ["L"], None),
]

PAD_BUTTONS = [
    ("", None), ("a", "A"), ("b", "B"), ("x", "X"), ("y", "Y"),
    ("leftshoulder", "LB"), ("rightshoulder", "RB"), ("lefttrigger", "LT"), ("righttrigger", "RT"),
    ("back", "Back / View"), ("start", "Start / Menu"), ("guide", "Guide"),
    ("leftstick", "L3 (left stick)"), ("rightstick", "R3 (right stick)"),
    ("dpup", "D-pad ↑"), ("dpdown", "D-pad ↓"), ("dpleft", "D-pad ←"), ("dpright", "D-pad →"),
]

# Tk keysym -> SDL scancode name (what ssx.ini expects)
TK2SDL = {
    "Return": "Return", "KP_Enter": "Keypad Enter", "space": "Space", "BackSpace": "Backspace",
    "Tab": "Tab", "Shift_L": "Left Shift", "Shift_R": "Right Shift", "Control_L": "Left Ctrl",
    "Control_R": "Right Ctrl", "Alt_L": "Left Alt", "Alt_R": "Right Alt", "Up": "Up", "Down": "Down",
    "Left": "Left", "Right": "Right", "Home": "Home", "End": "End", "Prior": "PageUp", "Next": "PageDown",
    "Insert": "Insert", "Delete": "Delete", "minus": "-", "equal": "=", "bracketleft": "[",
    "bracketright": "]", "semicolon": ";", "apostrophe": "'", "grave": "`", "period": ".", "slash": "/",
    "backslash": "\\", "KP_Add": "Keypad +", "KP_Subtract": "Keypad -", "KP_Multiply": "Keypad *",
    "KP_Divide": "Keypad /", "KP_Decimal": "Keypad .", "Caps_Lock": "CapsLock",
}
for _i in range(10):
    TK2SDL[f"KP_{_i}"] = f"Keypad {_i}"
for _i in range(1, 13):
    if _i != 11:   # F11 toggles fullscreen in the game
        TK2SDL[f"F{_i}"] = f"F{_i}"


def tk_to_sdl(keysym):
    if len(keysym) == 1 and keysym.isalnum():
        return keysym.upper()
    return TK2SDL.get(keysym)


RESOLUTIONS = ["960x544", "1280x720", "1366x768", "1440x816", "1600x900", "1920x1080", "1920x1088",
               "2560x1440", "3840x2160"]

# ----------------------------------------------------------------- paths


def data_dir():
    if IS_WIN:
        base = pathlib.Path(os.environ.get("LOCALAPPDATA") or pathlib.Path.home() / "AppData/Local")
        return base / "SSX-Recomp"
    base = pathlib.Path(os.environ.get("XDG_DATA_HOME") or pathlib.Path.home() / ".local/share")
    return base / "ssx-recomp"


def prepare_workspace():
    """The source tree the pipeline works in: the checkout, or (packaged) a per-user copy."""
    if not FROZEN:
        return HERE.parent
    import hashlib
    src = pathlib.Path(sys._MEIPASS) / "src"
    h = hashlib.sha1()
    for f in sorted(src.rglob("*")):
        if f.is_file():
            h.update(f.relative_to(src).as_posix().encode() + b"\0" + f.read_bytes())
    ws = data_dir() / "workspace"
    stamp = ws / ".source-hash"
    if not stamp.exists() or stamp.read_text().strip() != h.hexdigest():
        shutil.copytree(src, ws, dirs_exist_ok=True)   # build outputs stay; ninja rebuilds what changed
        stamp.write_text(h.hexdigest())
    return ws


class Paths:
    def __init__(self, ws):
        self.ws = ws
        if IS_WIN:
            self.game_dir = ws / "dist/ssx-windows-x64"
            self.exe = self.game_dir / "ssx.exe"
            self.cwd = self.game_dir
        else:
            self.exe = ws / "build/cmake/ssx"
            self.cwd = ws / "run"
            self.game_dir = self.cwd
        self.ini = self.cwd / "ssx.ini"
        self.saves = self.cwd / "ms0/PSP/SAVEDATA"
        self.state = (data_dir() if FROZEN else ws / "run") / "launcher.json"
        self.built_stamp = ws / "build/.launcher-built"


# ----------------------------------------------------------------- ssx.ini


def read_ini(path):
    out = {}
    try:
        for line in path.read_text(encoding="utf-8").splitlines():
            line = line.split("#", 1)[0]
            if "=" in line:
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip()
    except OSError:
        pass
    return out


def write_ini(path, values):
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = ["# Written by the SSX launcher; all settings: docs/OPTIONS.md"]
    lines += [f"{k} = {v}" for k, v in values.items() if v != ""]
    tmp = path.with_suffix(".tmp")
    tmp.write_text("\n".join(lines) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def monitor_info(root):
    """(width, height, refresh Hz or None, {"WxH": {Hz, ...}}) of the primary monitor."""
    w, h = root.winfo_screenwidth(), root.winfo_screenheight()
    hz = None
    modes = {}
    if IS_WIN:
        try:
            import ctypes
            from ctypes import wintypes

            class DEVMODEW(ctypes.Structure):
                _fields_ = [("dmDeviceName", wintypes.WCHAR * 32), ("dmSpecVersion", wintypes.WORD),
                            ("dmDriverVersion", wintypes.WORD), ("dmSize", wintypes.WORD),
                            ("dmDriverExtra", wintypes.WORD), ("dmFields", wintypes.DWORD),
                            ("dmPosition", ctypes.c_long * 2), ("dmDisplayOrientation", wintypes.DWORD),
                            ("dmDisplayFixedOutput", wintypes.DWORD), ("dmColor", ctypes.c_short),
                            ("dmDuplex", ctypes.c_short), ("dmYResolution", ctypes.c_short),
                            ("dmTTOption", ctypes.c_short), ("dmCollate", ctypes.c_short),
                            ("dmFormName", wintypes.WCHAR * 32), ("dmLogPixels", wintypes.WORD),
                            ("dmBitsPerPel", wintypes.DWORD), ("dmPelsWidth", wintypes.DWORD),
                            ("dmPelsHeight", wintypes.DWORD), ("dmDisplayFlags", wintypes.DWORD),
                            ("dmDisplayFrequency", wintypes.DWORD)]
            dm = DEVMODEW()
            dm.dmSize = ctypes.sizeof(DEVMODEW)
            enum = ctypes.windll.user32.EnumDisplaySettingsW
            if enum(None, -1, ctypes.byref(dm)):   # ENUM_CURRENT_SETTINGS
                w, h, hz = dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency or None
            i = 0
            while enum(None, i, ctypes.byref(dm)):
                if dm.dmBitsPerPel == 32 and dm.dmDisplayFrequency > 1:
                    modes.setdefault(f"{dm.dmPelsWidth}x{dm.dmPelsHeight}", set()).add(dm.dmDisplayFrequency)
                i += 1
        except Exception:
            pass
    else:
        # winfo_screen* spans all monitors on X11; take the primary (or first) output instead.
        try:
            import re
            out = subprocess.run(["xrandr", "--current"], capture_output=True, text=True, timeout=2).stdout
            outputs, cur = [], None
            for line in out.splitlines():
                m = re.match(r"\S+ connected (primary )?(\d+)x(\d+)\+", line)
                if m:
                    cur = [bool(m.group(1)), int(m.group(2)), int(m.group(3)), None, {}]
                    outputs.append(cur)
                elif not line.startswith(" "):
                    cur = None
                elif cur:
                    toks = line.split()
                    if toks and re.match(r"\d+x\d+i?$", toks[0]) and not toks[0].endswith("i"):
                        for t in toks[1:]:
                            try:
                                r = round(float(t.strip("*+")))
                            except ValueError:
                                continue
                            cur[4].setdefault(toks[0], set()).add(r)
                            if "*" in t:
                                cur[3] = r
            if outputs:
                best = next((o for o in outputs if o[0]), outputs[0])
                w, h, hz, modes = best[1], best[2], best[3], best[4]
        except Exception:
            pass
    return w, h, hz, modes


def open_path(p):
    p.mkdir(parents=True, exist_ok=True)
    if IS_WIN:
        os.startfile(p)
    elif platform.system() == "Darwin":
        subprocess.Popen(["open", p])
    else:
        subprocess.Popen(["xdg-open", p])


# ----------------------------------------------------------------- the window


class Launcher:
    def __init__(self, root, build_iso=None, quit_after=False):
        self.root = root
        self.ws = prepare_workspace()
        self.p = Paths(self.ws)
        sys.path.insert(0, str(self.ws / "tools"))
        import setup_from_iso
        self.setup = setup_from_iso
        self.q = queue.Queue()
        self.busy = False
        self.game = None
        self.state = self.load_state()
        self.set_lang(self.state.get("lang", "en"))
        self.mon = monitor_info(root)

        root.title(L["title"])
        root.geometry("900x780")
        root.minsize(860, 700)
        self.apply_theme()
        self.build_ui()
        self.load_settings()
        self.refresh_status()
        root.after(100, self.poll)
        root.protocol("WM_DELETE_WINDOW", self.on_close)
        self.quit_after = quit_after
        if build_iso:
            root.after(300, lambda: self.start_build(build_iso))

    # --- language
    def set_lang(self, code):
        global L
        self.lang = code if code in TEXT else "en"
        L = TEXT[self.lang]

    def change_lang(self):
        code = next(k for k, v in LANGS.items() if v == self.lang_var.get())
        if code == self.lang:
            return
        self.save_settings()
        self.state["lang"] = code
        self.save_state()
        log, tab = self.log.get("1.0", "end-1c"), self.nb.index("current")
        self.set_lang(code)
        for w in self.root.winfo_children():
            w.destroy()
        self.root.title(L["title"])
        self.build_ui()
        self.nb.select(tab)
        self.load_settings()
        if log:
            self.append_log(log.rstrip("\n"))
        self.refresh_status()

    # --- state
    def load_state(self):
        try:
            return json.loads(self.p.state.read_text())
        except (OSError, ValueError):
            return {}

    def save_state(self):
        self.p.state.parent.mkdir(parents=True, exist_ok=True)
        self.p.state.write_text(json.dumps(self.state, indent=1))

    def installed(self):
        return self.p.exe.exists() and (IS_WIN and (self.p.game_dir / "image.bin").exists()
                                        or not IS_WIN and (self.ws / "build/gen/image.bin").exists())

    def stale(self):
        if not FROZEN:   # a checkout is rebuilt by the developer (setup.sh) when sources change
            return False
        try:
            return self.p.built_stamp.read_text().strip() != BUILD_VERSION
        except OSError:
            return True

    # --- look
    def apply_theme(self):
        try:
            import sv_ttk
            sv_ttk.set_theme("dark")
        except Exception:
            ttk.Style().theme_use("clam")
        st = ttk.Style()
        st.configure("Play.TButton", font=("TkDefaultFont", 16, "bold"), padding=(30, 12))
        st.configure("Title.TLabel", font=("TkDefaultFont", 18, "bold"))
        st.configure("Status.TLabel", font=("TkDefaultFont", 11))

    def build_ui(self):
        r = self.root
        top = ttk.Frame(r, padding=(16, 12, 16, 4))
        top.pack(fill="x")
        ttk.Label(top, text="SSX On Tour", style="Title.TLabel").pack(side="left")
        ttk.Label(top, text=f"  PC · v{APP_VERSION}").pack(side="left", pady=(8, 0))

        self.lang_var = tk.StringVar(value=LANGS[self.lang])
        self.lang_cb = ttk.Combobox(top, textvariable=self.lang_var, values=list(LANGS.values()),
                                    state="readonly", width=10)
        self.lang_cb.pack(side="right", pady=(6, 0))
        self.lang_cb.bind("<<ComboboxSelected>>", lambda e: self.change_lang())

        self.nb = nb = ttk.Notebook(r)
        nb.pack(fill="both", expand=True, padx=16, pady=8)
        self.tab_game(nb)
        self.tab_display(nb)
        self.tab_controls(nb)
        nb.select(int(os.environ.get("SSX_LAUNCHER_TAB", "0")))   # for screenshots/tests

        bottom = ttk.Frame(r, padding=(16, 4, 16, 14))
        bottom.pack(fill="x")
        self.status = ttk.Label(bottom, style="Status.TLabel", wraplength=600)
        self.status.pack(side="left", fill="x", expand=True)
        self.play_btn = ttk.Button(bottom, text=L["play"], style="Play.TButton", command=self.play)
        self.play_btn.pack(side="right")
        try:
            self.play_btn.configure(style="Accent.TButton")
            ttk.Style().configure("Accent.TButton", font=("TkDefaultFont", 16, "bold"), padding=(30, 12))
        except tk.TclError:
            pass

    def tab_game(self, nb):
        f = ttk.Frame(nb, padding=16)
        nb.add(f, text=L["tab_game"])
        box = ttk.LabelFrame(f, text=L["iso"], padding=12)
        box.pack(fill="x")
        self.iso_var = tk.StringVar(value=self.state.get("iso") or L["iso_none"])
        ttk.Label(box, textvariable=self.iso_var, wraplength=640).pack(anchor="w")
        row = ttk.Frame(box)
        row.pack(fill="x", pady=(10, 0))
        self.choose_btn = ttk.Button(row, text=L["choose_iso"], command=self.choose_iso)
        self.choose_btn.pack(side="left")
        self.rebuild_btn = ttk.Button(row, text=L["rebuild"], command=self.rebuild)
        self.rebuild_btn.pack(side="left", padx=8)
        ttk.Button(row, text=L["open_saves"], command=lambda: open_path(self.p.saves)).pack(side="right")
        ttk.Button(row, text=L["open_folder"], command=lambda: open_path(self.p.game_dir)).pack(side="right", padx=8)
        legal = ttk.Label(f, text=L["legal"], foreground="#9a9a9a")
        legal.pack(fill="x", pady=(10, 6))
        legal.bind("<Configure>", lambda e: legal.configure(wraplength=e.width - 4))

        self.progress = ttk.Progressbar(f, mode="indeterminate")
        lf = ttk.LabelFrame(f, text=L["log"], padding=6)
        lf.pack(fill="both", expand=True, side="bottom")
        self.progress_anchor = lf
        self.log = tk.Text(lf, height=10, wrap="none", font=("TkFixedFont", 9), relief="flat",
                           background="#1c1c1c", foreground="#d0d0d0", insertbackground="#d0d0d0")
        sb = ttk.Scrollbar(lf, command=self.log.yview)
        self.log.configure(yscrollcommand=sb.set, state="disabled")
        sb.pack(side="right", fill="y")
        self.log.pack(fill="both", expand=True)

    def tab_display(self, nb):
        f = ttk.Frame(nb, padding=16)
        nb.add(f, text=L["tab_display"])
        f.columnconfigure(1, weight=1)
        w, h, hz, _ = self.mon
        ttk.Label(f, text=L["monitor"].format(w=w, h=h, hz=f" @ {hz} Hz" if hz else "")).grid(
            row=0, column=0, columnspan=2, sticky="w", pady=(0, 10))

        self.res_var = tk.StringVar()
        self.desktop_label = f"{L['desktop']} ({w}x{h})"
        res = set(r for r in RESOLUTIONS if int(r.split("x")[0]) <= w and int(r.split("x")[1]) <= h)
        res |= {r for r in self.mon[3] if int(r.split("x")[0]) >= 800 and int(r.split("x")[1]) >= 450}
        res = sorted(res, key=lambda r: tuple(int(x) for x in r.split("x")))
        ttk.Label(f, text=L["resolution"]).grid(row=1, column=0, sticky="w", pady=4)
        rc = ttk.Combobox(f, textvariable=self.res_var, values=[self.desktop_label] + res, state="readonly", width=24)
        rc.grid(row=1, column=1, sticky="w")
        rc.bind("<<ComboboxSelected>>", lambda e: self.update_refresh())
        fr = ttk.Frame(f)
        fr.grid(row=2, column=1, sticky="w", pady=4)
        self.fs_var = tk.BooleanVar()
        ttk.Checkbutton(fr, text=L["fullscreen"], variable=self.fs_var, command=self.update_refresh).pack(side="left")
        ttk.Label(fr, text="   " + L["refresh"]).pack(side="left")
        self.refresh_var = tk.StringVar()
        self.refresh_cb = ttk.Combobox(fr, textvariable=self.refresh_var, state="readonly", width=22)
        self.refresh_cb.pack(side="left", padx=6)
        self.refresh_cb.bind("<<ComboboxSelected>>", lambda e: self.update_hint())

        ttk.Label(f, text=L["fps"]).grid(row=3, column=0, sticky="w", pady=(12, 4))
        fr = ttk.Frame(f)
        fr.grid(row=3, column=1, sticky="w", pady=(12, 4))
        self.fps_var = tk.StringVar()
        for v in ("30", "60", "120"):
            ttk.Radiobutton(fr, text=L["fps" + v], value=v, variable=self.fps_var,
                            command=self.update_hint).pack(side="left", padx=(0, 14))
        self.vsync_var = tk.BooleanVar()
        ttk.Checkbutton(f, text=L["vsync"], variable=self.vsync_var, command=self.update_hint).grid(
            row=4, column=1, sticky="w", pady=4)
        self.hint = ttk.Label(f, foreground="#e0b050", wraplength=520)
        self.hint.grid(row=5, column=1, sticky="w")

        ttk.Label(f, text=L["scale"]).grid(row=6, column=0, sticky="w", pady=(12, 4))
        self.scale_choices = [L["scale_auto"]] + [f"{n}× ({480 * n}×{272 * n})" for n in range(1, 7)]
        self.scale_var = tk.StringVar()
        ttk.Combobox(f, textvariable=self.scale_var, values=self.scale_choices, state="readonly",
                     width=24).grid(row=6, column=1, sticky="w", pady=(12, 4))

        ttk.Label(f, text=L["filter"]).grid(row=7, column=0, sticky="w", pady=4)
        fr = ttk.Frame(f)
        fr.grid(row=7, column=1, sticky="w")
        self.filter_var = tk.StringVar()
        ttk.Radiobutton(fr, text=L["filter_linear"], value="linear", variable=self.filter_var).pack(side="left", padx=(0, 14))
        ttk.Radiobutton(fr, text=L["filter_nearest"], value="nearest", variable=self.filter_var).pack(side="left")

        ttk.Label(f, text=L["renderer"]).grid(row=8, column=0, sticky="w", pady=4)
        fr = ttk.Frame(f)
        fr.grid(row=8, column=1, sticky="w")
        self.renderer_var = tk.StringVar()
        ttk.Radiobutton(fr, text=L["renderer_gl"], value="gl", variable=self.renderer_var).pack(side="left", padx=(0, 14))
        ttk.Radiobutton(fr, text=L["renderer_sw"], value="software", variable=self.renderer_var).pack(side="left")

    def tab_controls(self, nb):
        outer = ttk.Frame(nb, padding=(16, 10))
        nb.add(outer, text=L["tab_controls"])
        f = ttk.Frame(outer)
        f.pack(fill="x")
        for c, t in enumerate([L["psp"], L["key"], L["key2"], L["pad"], L["pad2"]]):
            ttk.Label(f, text=t, font=("TkDefaultFont", 10, "bold")).grid(row=0, column=c, sticky="w", padx=4, pady=(0, 4))
        self.key_vars, self.pad_vars = {}, {}
        pad_labels = [L["none"] if lbl is None else lbl for _, lbl in PAD_BUTTONS]
        for r, (name, label, _, pads) in enumerate(INPUTS, start=1):
            ttk.Label(f, text=label).grid(row=r, column=0, sticky="w", padx=4, pady=1)
            for i in range(2):
                v = tk.StringVar()
                self.key_vars[(name, i)] = v
                b = ttk.Button(f, textvariable=v, width=13)
                b.configure(command=lambda b=b, v=v: self.capture_key(b, v))
                b.grid(row=r, column=1 + i, sticky="we", padx=4, pady=1)
            for i in range(2):
                v = tk.StringVar()
                self.pad_vars[(name, i)] = v
                cb = ttk.Combobox(f, textvariable=v, values=pad_labels, state="readonly", width=15)
                if pads is None:
                    cb.configure(state="disabled")
                cb.grid(row=r, column=3 + i, sticky="we", padx=4, pady=1)
        g = ttk.Frame(outer)
        g.pack(fill="x", pady=(10, 0))
        ttk.Label(g, text=L["deadzone"]).pack(side="left")
        self.dz_var = tk.IntVar()
        ttk.Scale(g, from_=0, to=60, variable=self.dz_var, length=200,
                  command=lambda _=None: self.dz_label.configure(text=f"{self.dz_var.get()} %")).pack(side="left", padx=8)
        self.dz_label = ttk.Label(g, width=6)
        self.dz_label.pack(side="left")
        ttk.Button(g, text=L["defaults"], command=self.reset_controls).pack(side="right")
        self.rstick_var = tk.BooleanVar()
        ttk.Checkbutton(outer, text=L["rstick"], variable=self.rstick_var).pack(anchor="w", pady=(6, 0))
        ttk.Label(outer, text=L["stick_note"], foreground="#9a9a9a").pack(anchor="w", pady=(4, 0))

    # --- controls helpers
    def pad_label(self, sdl):
        for n, lbl in PAD_BUTTONS:
            if n == sdl:
                return L["none"] if lbl is None else lbl
        return L["none"]

    def pad_name(self, label):
        for n, lbl in PAD_BUTTONS:
            if (L["none"] if lbl is None else lbl) == label:
                return n
        return ""

    def capture_key(self, button, var):
        old = var.get()
        var.set(L["press_key"])
        button.focus_set()

        def on_key(e):
            self.root.unbind("<KeyPress>")
            if e.keysym == "Escape":
                var.set(L["none"])
                return "break"
            name = tk_to_sdl(e.keysym)
            if not name:
                var.set(old)
                messagebox.showwarning(L["title"], L["bad_key"])
            else:
                var.set(name)
            return "break"
        self.root.bind("<KeyPress>", on_key)

    def reset_controls(self):
        for name, _, keys, pads in INPUTS:
            for i in range(2):
                self.key_vars[(name, i)].set(keys[i] if i < len(keys) else L["none"])
                self.pad_vars[(name, i)].set(self.pad_label(pads[i]) if pads and i < len(pads) else L["none"])
        self.dz_raw = 8000
        self.dz_var.set(24)
        self.dz_label.configure(text="24 %")
        self.rstick_var.set(False)

    # --- settings <-> ssx.ini
    def load_settings(self):
        ini = read_ini(self.p.ini)
        first = not self.p.ini.exists()
        w, h, hz, _ = self.mon
        res = ini.get("resolution", f"{w}x{h}" if first else "960x544")
        self.res_var.set(self.desktop_label if res == f"{w}x{h}" else res)
        self.fs_var.set(ini.get("fullscreen", "true" if first else "false") in ("1", "true", "on", "yes"))
        r = ini.get("refresh", "0")
        self.refresh_var.set(f"{r} Hz" if r.isdigit() and int(r) > 0 else L["refresh_desktop"])
        self.fps_var.set(ini.get("fps", "60" if first else "30"))
        self.vsync_var.set(ini.get("vsync", "true" if first else "false") in ("1", "true", "on", "yes"))
        try:
            n = int(ini.get("render_scale", "0"))
        except ValueError:
            n = 0
        self.scale_var.set(self.scale_choices[n if 0 <= n <= 6 else 0])
        self.filter_var.set(ini.get("filter", "linear"))
        self.renderer_var.set(ini.get("renderer", "gl"))
        self.reset_controls()
        for name, _, _, pads in INPUTS:
            if f"key_{name}" in ini:
                keys = [k.strip() for k in ini[f"key_{name}"].split(",") if k.strip()]
                for i in range(2):
                    self.key_vars[(name, i)].set(keys[i] if i < len(keys) else L["none"])
            if pads is not None and f"pad_{name}" in ini:
                ps = [k.strip() for k in ini[f"pad_{name}"].split(",") if k.strip()]
                for i in range(2):
                    self.pad_vars[(name, i)].set(self.pad_label(ps[i]) if i < len(ps) else L["none"])
        try:
            dz = int(ini.get("deadzone", "8000"))
        except ValueError:
            dz = 8000
        self.dz_raw = dz   # kept exactly unless the slider is moved
        self.dz_var.set(round(dz * 100 / 32767))
        self.dz_label.configure(text=f"{self.dz_var.get()} %")
        self.rstick_var.set(ini.get("right_stick_dpad", "false") in ("1", "true", "on", "yes"))
        self.update_refresh()

    def save_settings(self):
        ini = read_ini(self.p.ini)   # keep keys the launcher doesn't know (pad_mapping, …)
        res = self.current_res()
        b = lambda v: "true" if v else "false"
        ini.update({
            "fps": self.fps_var.get(), "resolution": res, "fullscreen": b(self.fs_var.get()),
            "refresh": str(self.refresh_hz()),
            "vsync": b(self.vsync_var.get()), "render_scale": str(self.scale_choices.index(self.scale_var.get())),
            "filter": self.filter_var.get(), "renderer": self.renderer_var.get(),
            "deadzone": str(self.dz_raw if self.dz_var.get() == round(self.dz_raw * 100 / 32767)
                            else round(self.dz_var.get() * 32767 / 100)),
            "right_stick_dpad": b(self.rstick_var.get()),
        })
        for name, _, _, pads in INPUTS:
            keys = [self.key_vars[(name, i)].get() for i in range(2)]
            ini[f"key_{name}"] = ", ".join(k for k in keys if k and k != L["none"] and k != L["press_key"]) or "none"
            if pads is not None:
                ps = [self.pad_name(self.pad_vars[(name, i)].get()) for i in range(2)]
                ini[f"pad_{name}"] = ", ".join(p for p in ps if p) or "none"
        if self.state.get("iso"):
            ini["iso"] = self.state["iso"]
        write_ini(self.p.ini, ini)

    def current_res(self):
        res = self.res_var.get()
        return f"{self.mon[0]}x{self.mon[1]}" if res.startswith(L["desktop"]) else res

    def refresh_hz(self):
        v = self.refresh_var.get()
        return int(v.split()[0]) if v and v[0].isdigit() else 0

    def update_refresh(self):
        """Refresh rates the monitor offers at the chosen size; only with fullscreen."""
        rates = sorted(self.mon[3].get(self.current_res(), set()), reverse=True)
        values = [L["refresh_desktop"]] + [f"{r} Hz" for r in rates]
        self.refresh_cb.configure(values=values, state="readonly" if self.fs_var.get() else "disabled")
        if self.refresh_var.get() not in values:
            want = self.refresh_hz()
            near = min(rates, key=lambda r: abs(r - want)) if want and rates else None
            self.refresh_var.set(f"{near} Hz" if near else L["refresh_desktop"])
        self.update_hint()

    def update_hint(self):
        hz = self.refresh_hz() if self.fs_var.get() and self.refresh_hz() else self.mon[2]
        if hz and self.vsync_var.get() and int(self.fps_var.get() or 30) > hz:
            self.hint.configure(text=L["fps_hint"].format(hz=hz))
        else:
            self.hint.configure(text="")

    # --- status
    def refresh_status(self):
        if self.busy:
            text = L["status_busy"]
        elif not self.installed():
            text = L["status_none"]
        elif self.stale():
            text = L["status_stale"]
        else:
            text = L["status_ready"]
        self.status.configure(text=text)
        ready = self.installed() and not self.stale() and not self.busy and self.game is None
        self.play_btn.configure(state="normal" if ready else "disabled",
                                text=L["running"] if self.game else L["play"])
        for b in (self.choose_btn,):
            b.configure(state="disabled" if self.busy else "normal")
        self.lang_cb.configure(state="disabled" if self.busy else "readonly")
        self.rebuild_btn.configure(state="normal" if self.state.get("iso") and not self.busy else "disabled")

    # --- log
    def append_log(self, line):
        self.log.configure(state="normal")
        self.log.insert("end", line + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def poll(self):
        try:
            while True:
                kind, val = self.q.get_nowait()
                if kind == "log":
                    self.append_log(val)
                elif kind == "done":
                    self.build_finished(val)
                elif kind == "game_exit":
                    self.game_finished(val)
        except queue.Empty:
            pass
        self.root.after(100, self.poll)

    # --- build
    def choose_iso(self):
        path = filedialog.askopenfilename(
            title=L["choose_iso"],
            filetypes=[(L["iso_types"], "*.iso *.ISO *.zip *.ZIP"), (L["all_files"], "*")])
        if path:
            self.start_build(path)

    def rebuild(self):
        if self.state.get("iso"):
            self.start_build(self.state["iso"])

    def missing_linux_tools(self):
        if IS_WIN:
            return False
        missing = [t for t in ("clang++", "cmake", "ninja", "pkg-config") if not shutil.which(t)]
        if not missing:
            r = subprocess.run(["pkg-config", "--exists", "sdl2", "libavcodec", "libavutil", "libswscale"])
            if r.returncode:
                missing.append("libs")
        if missing:
            cmd = ("sudo apt install clang cmake ninja-build pkg-config libsdl2-dev "
                   "libavcodec-dev libavutil-dev libswscale-dev")
            messagebox.showerror(L["title"], L["missing_linux"].format(cmd=cmd))
            return True
        return False

    def start_build(self, iso):
        if self.busy or self.missing_linux_tools():
            return
        self.busy = True
        self.progress.pack(fill="x", pady=(6, 6), before=self.progress_anchor)
        self.progress.start(12)
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")
        self.refresh_status()

        def work():
            self.setup.log = lambda m: self.q.put(("log", m))
            try:
                result = self.setup.setup(iso)
                self.q.put(("done", (True, iso, result)))
            except self.setup.SetupError as e:
                self.q.put(("done", (False, iso, str(e))))
            except Exception as e:   # show anything unexpected instead of dying silently
                self.q.put(("log", traceback.format_exc()))
                self.q.put(("done", (False, iso, f"{type(e).__name__}: {e}")))
        threading.Thread(target=work, daemon=True).start()

    def build_finished(self, val):
        ok, iso, info = val
        self.busy = False
        self.progress.stop()
        self.progress.pack_forget()
        if ok:
            self.state["iso"] = str(pathlib.Path(iso).resolve())
            self.save_state()
            self.iso_var.set(self.state["iso"])
            self.p.built_stamp.parent.mkdir(parents=True, exist_ok=True)
            self.p.built_stamp.write_text(BUILD_VERSION)
            self.save_settings()
            self.append_log("")
            self.append_log(L["done"])
        else:
            self.append_log(f"error: {info}")
            if not self.quit_after:
                messagebox.showerror(L["title"], L["build_failed"].format(err=info))
        self.refresh_status()
        if self.quit_after:
            data_dir().mkdir(parents=True, exist_ok=True)
            (data_dir() / "last-build.log").write_text(self.log.get("1.0", "end"), encoding="utf-8")
            self.root.after(200, self.on_close)

    # --- play
    def play(self):
        if self.game or not self.installed():
            return
        self.save_settings()
        self.p.cwd.mkdir(parents=True, exist_ok=True)
        flags = 0x08000000 if IS_WIN else 0
        self.game = subprocess.Popen([str(self.p.exe)], cwd=str(self.p.cwd), creationflags=flags)
        self.refresh_status()
        self.root.iconify()

        def wait(proc):
            self.q.put(("game_exit", proc.wait()))
        threading.Thread(target=wait, args=(self.game,), daemon=True).start()

    def game_finished(self, code):
        self.game = None
        self.root.deiconify()
        self.refresh_status()
        if code not in (0, None) and code != -2:   # -2: Ctrl+C on Linux
            try:
                tail = "\n".join((self.p.cwd / "ssx.log").read_text(errors="replace").splitlines()[-12:])
            except OSError:
                tail = "(no ssx.log)"
            messagebox.showerror(L["title"], L["game_failed"].format(code=code, tail=tail))

    def on_close(self):
        try:
            self.save_settings()
        except OSError:
            pass
        self.root.destroy()


def main():
    if IS_WIN:
        try:   # crisp text on high-DPI screens
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except Exception:
            pass
    import argparse
    if sys.stdout is None:   # windowed build: no console
        sys.stdout = sys.stderr = open(os.devnull, "w")
    ap = argparse.ArgumentParser(description="SSX On Tour PC launcher")
    ap.add_argument("--self-test", action="store_true",
                    help="check that the bundled pipeline loads, then exit (used by CI)")
    ap.add_argument("--build", metavar="ISO", help="build the game from this dump right away")
    ap.add_argument("--quit-after-build", action="store_true",
                    help="close when the build ends (log in the data folder: last-build.log)")
    a = ap.parse_args()
    if a.self_test:
        ws = prepare_workspace()
        sys.path.insert(0, str(ws / "tools"))
        import py_compile
        import rabbitizer  # noqa: F401
        import setup_from_iso, win_deps  # noqa: F401
        for t in ("analyze.py", "recomp.py", "psp_prx.py"):
            py_compile.compile(str(ws / "tools" / t), doraise=True)
        sys.exit(0)
    root = tk.Tk()
    Launcher(root, a.build, a.quit_after_build)
    root.mainloop()


if __name__ == "__main__":
    main()
