#!/bin/sh
# Start the recompiled game. Extra arguments are passed through, e.g.:
#   ./run.sh --fps 60 --res 1920x1088 --fullscreen
cd "$(dirname "$0")" && mkdir -p run && cd run && exec ../build/cmake/ssx "$@"
