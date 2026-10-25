#!/bin/sh
# Runs a command (wine ...) on a virtual X display of its own (Xvfb), away from the desktop: the test runs' windows
# - Wine's own, a demo's - no longer appear on the screen and take the focus (a tiling window manager gives every
# new window the focus). The GPU work is offscreen and needs no real display; the GPU is the same one.
# RVK_DEMO_VISIBLE=1, or no xvfb-run installed: run as it is, on the desktop.
# Usage: tools/wine-hidden.sh wine program.exe [args...]
if [ -z "$RVK_DEMO_VISIBLE" ] && command -v xvfb-run >/dev/null 2>&1; then
    # (Not the Wayland compositor either: Wine would draw there first.)
    exec env -u WAYLAND_DISPLAY xvfb-run -a -s "-screen 0 1280x720x24" "$@"
fi
exec "$@"
