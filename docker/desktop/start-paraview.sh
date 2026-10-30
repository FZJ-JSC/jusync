#!/bin/sh
# Manual ParaView start for the VNC desktop (autostart is off).
# No state file by default: a .pvsm builds the colour-map editor, which crashes.
#
#   start-paraview              empty session
#   start-paraview <file|args>  passed through to the packaged launcher
set -eu
export DISPLAY="${DISPLAY:-:1}"

exec /opt/pv61/start-paraview-anari.sh "$@"
