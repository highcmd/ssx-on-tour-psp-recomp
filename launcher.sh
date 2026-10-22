#!/bin/sh
# Start the graphical launcher from a source checkout (Linux).
# Needs Python 3.10+ with Tk (Debian/Ubuntu: sudo apt install python3-tk python3-venv).
set -e
cd "$(dirname "$0")"
if ! python3 -c "import tkinter" 2>/dev/null; then
	echo "The launcher needs Tk for Python: sudo apt install python3-tk (Fedora: python3-tkinter, Arch: tk)" >&2
	exit 1
fi
if [ ! -x .venv/bin/python ]; then
	if command -v uv >/dev/null 2>&1; then
		uv venv -q .venv && uv pip install -q --python .venv/bin/python -r requirements.txt sv-ttk
	else
		python3 -m venv .venv && .venv/bin/pip install -q -r requirements.txt sv-ttk
	fi
fi
.venv/bin/python -c "import sv_ttk" 2>/dev/null || .venv/bin/python -m pip install -q sv-ttk 2>/dev/null || true
exec .venv/bin/python launcher/ssx_launcher.py "$@"
