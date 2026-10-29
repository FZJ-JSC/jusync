#!/bin/sh
# Register the generated particle example as a ParaView desktop preset.
#
# Run this INSIDE the pv-desktop container:
#
#   docker exec pv-desktop sh /work/examples/paraview-pathlines/install-preset.sh
#
# It only touches the container's writable layer (/opt/jusync/presets and the
# desktop entries), so re-run it after the container is recreated. The dataset
# itself lives in the repo and reaches the container through the /work mount.
set -eu

ROOT=${1:-/work/examples/paraview-pathlines}
NAME=${2:-pathlines}
SRC="$ROOT/particles"

[ -f "$SRC/$NAME.pvsm" ] || {
    echo "install-preset: $SRC/$NAME.pvsm not found" >&2
    echo "run make_particle_example.py first" >&2
    exit 1
}

ln -sfn "$SRC" "/opt/jusync/presets/$NAME"

ENTRY="/usr/share/applications/jusync-preset-$NAME.desktop"
cat > "$ENTRY" <<EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=ParaView preset: $NAME
Comment=Open the $NAME sample on the ANARI usd device
Exec=/usr/local/bin/jusync-preset $NAME
Icon=paraview
Terminal=false
Categories=Graphics;Science;
EOF
chmod +x "$ENTRY"
if [ -d /root/Desktop ]; then
    cp "$ENTRY" "/root/Desktop/jusync-preset-$NAME.desktop"
    chmod +x "/root/Desktop/jusync-preset-$NAME.desktop"
fi

echo "installed preset '$NAME' -> $SRC"
jusync-preset | sed 's/^/  /' || true
