#!/bin/sh
# One-step setup: ./setup.sh /path/to/your/SSX-On-Tour-USA.iso (or .zip)
# Creates a Python venv with the recompiler's dependencies, then generates and
# builds everything from your own UMD dump (see README.md).
set -e
cd "$(dirname "$0")"
if [ $# -lt 1 ]; then
	echo "usage: ./setup.sh <your ULUS-10042 dump: .iso or .zip> [--no-build] [--force] [-j N]" >&2
	exit 1
fi
if [ ! -x .venv/bin/python ]; then
	if command -v uv >/dev/null 2>&1; then
		uv venv -q .venv && uv pip install -q --python .venv/bin/python -r requirements.txt
	else
		python3 -m venv .venv && .venv/bin/pip install -q -r requirements.txt
	fi
fi
exec .venv/bin/python tools/setup_from_iso.py "$@"
