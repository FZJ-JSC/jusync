#!/bin/sh
# Launch ParaView 6.1 rendering through the ANARI 'usd' device library.
#
# Run inside the pv-anari-usd image. Stop the bigCloud streamer FIRST so the
# broker port 5556 is free (docker stop is not needed):
#   docker exec <c> pkill -f anariBigCloudUsd
#
# Environment comes from anari-env.sh, the single source of truth shared with
# the VNC autostart, jusync-preset and interactive shells. It forces
# ANARI_LIBRARY=usd, points the loader at the container-native device copy,
# disables the device GC and enables RawGeo time-sample streaming. Everything
# it sets except ANARI_LIBRARY itself is a ${VAR:-default}, so a caller (or a
# preset.env) can still override fps/stride/curves.
set -e

# Resolve our own directory: the script is installed at /opt/pv61/ (canonical),
# /opt/sender/ (legacy) and may be run straight from a checkout.
SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ENV_FILE=""
for candidate in "$SELF_DIR/anari-env.sh" /opt/pv61/anari-env.sh \
                 /usr/local/bin/anari-env.sh; do
    if [ -f "$candidate" ]; then
        ENV_FILE="$candidate"
        break
    fi
done
if [ -n "$ENV_FILE" ]; then
    . "$ENV_FILE"
else
    echo "[start-paraview-anari] WARNING: anari-env.sh not found - falling back to inline defaults" >&2
    export ANARI_LIBRARY=usd
    export ANARI_LIBRARY_PATH=/usr/local/lib
    export LIBGL_ALWAYS_SOFTWARE=1
    export ANARI_USD_DISABLE_GC=1
    export JUSYNC_RAWGEO=1
fi

export DISPLAY="${DISPLAY:-:1}"

# ---------------------------------------------------------------------------
# Animation clock: derive it from the dataset that is actually being opened
# ---------------------------------------------------------------------------
# The device stamps samples t0 + n*JUSYNC_ANIM_DT (ParaView never tells it the
# pipeline time), so a hard-coded value puts Unreal on a different timeline
# than the ParaView toolbar whenever the dataset step differs. Read the step
# from the dataset's own index (.pvd / .vtk.series) instead.
# Set JUSYNC_ANIM_DT_LOCK=1 to keep an explicit value.
if [ -z "${JUSYNC_ANIM_DT_LOCK:-}" ]; then
    for _arg in "$@"; do
        case "$_arg" in -*) continue ;; esac
        [ -f "$_arg" ] || continue
        case "$_arg" in *.pvsm) continue ;; esac
        if _info=$(jusync-anim-dt "$_arg" 2>/dev/null) && [ -n "$_info" ]; then
            _dt=$(printf '%s\n' "$_info" | cut -d' ' -f1)
            _n=$(printf '%s\n' "$_info" | cut -d' ' -f2)
            if [ -n "$_dt" ] && [ "$_dt" != "0" ]; then
                JUSYNC_ANIM_DT="$_dt"
                JUSYNC_ANIM_FPS=$(awk -v dt="$_dt" \
                    'BEGIN{f=1/dt; if(f>30)f=30; if(f<0.05)f=0.05; printf "%.6g", f}')
                JUSYNC_ANIM_PUBLISH_HZ="$JUSYNC_ANIM_FPS"
                case "$_n" in
                    ''|*[!0-9]*) _n=64 ;;
                esac
                if [ "$_n" -lt 2 ]; then _n=2; fi
                if [ "$_n" -gt 512 ]; then _n=512; fi
                JUSYNC_ANIM_MAXSAMPLES="$_n"
                export JUSYNC_ANIM_DT JUSYNC_ANIM_FPS \
                       JUSYNC_ANIM_PUBLISH_HZ JUSYNC_ANIM_MAXSAMPLES
                echo "[start-paraview-anari] anim clock from $(basename "$_arg"):" \
                     "dt=$JUSYNC_ANIM_DT fps=$JUSYNC_ANIM_FPS" \
                     "publish=$JUSYNC_ANIM_PUBLISH_HZ samples=$JUSYNC_ANIM_MAXSAMPLES" >&2
            fi
        fi

        # Index the geometry of every frame. The device matches the payload it
        # is handed against this table and stamps the dataset's own time, so
        # Unreal stays on the ParaView timeline even when playback skips
        # frames or the user scrubs to a frame that was never streamed before.
        _idx="/tmp/jusync-frames-$(basename "$_arg").idx"
        if command -v jusync-frame-index >/dev/null 2>&1 && \
           jusync-frame-index "$_arg" "$_idx" 2>/dev/null; then
            JUSYNC_ANIM_FRAME_INDEX="$_idx"
            export JUSYNC_ANIM_FRAME_INDEX
            echo "[start-paraview-anari] frame index: $_idx ($(grep -vc '^#' "$_idx") frames)" >&2
        fi
        break
    done
fi

# PVDIR may come from the caller (or from the paraview wrapper, which prefers a
# live bind mount). Fall back to our own tree, then to the baked one.
if [ -z "${PVDIR:-}" ] || [ ! -x "${PVDIR}/bin/paraview" ]; then
    if [ -x "$SELF_DIR/bin/paraview" ]; then
        PVDIR="$SELF_DIR"
    else
        PVDIR=/opt/pv61
    fi
fi
export PVDIR

# The ANARI mapper node asks VTK for vtkDataObject::DATA_TIME_STEP() on the
# actor's property keys, a key nothing in VTK or ParaView ever fills, so the
# device had to guess the dataset time from the payload. libpvtime feeds the
# animation time (vtkPVView::SetViewTime) into exactly that read, so each
# geometry reaches the device stamped with the time ParaView is showing.
_pvtime="${PVDIR}/lib/libpvtime.so"
if [ -f "$_pvtime" ]; then
    export LD_PRELOAD="$_pvtime${LD_PRELOAD:+:$LD_PRELOAD}"
fi

# A ParaView that is SIGKILLed (jusync-preset switching datasets, or a crash)
# leaves Position/Size/Splitters in ParaView.ini. The next start restores that
# degenerate main window and never applies or shows the source: the view stays
# empty and the device receives no geometry at all. Presets pass their dataset
# on the command line, so the saved window state is not worth keeping.
rm -f "${HOME}/.config/ParaView/ParaView.ini"

# Auto-driver: when present it builds the pipeline, scrubs all timesteps
# (visibility-forced commits) and keeps rendering for the ZMQ pump. This
# ParaView is built without Python support, so the hook only fires when the
# caller deliberately installed pvpython support.
SCRIPT_ARG=""
if [ -f "${JUSYNC_CAPTURE_SCRIPT:-/tmp/jusync_capture.py}" ]; then
    SCRIPT_ARG="--script ${JUSYNC_CAPTURE_SCRIPT:-/tmp/jusync_capture.py}"
fi

# Keep a session log. The desktop icons start this without a terminal, and the
# device's [JusyncAnim] lines are the only way to tell whether the stream is
# being published (and with which times). JUSYNC_SESSION_LOG=- restores stderr.
_jslog="${JUSYNC_SESSION_LOG-/tmp/paraview-session.log}"
if [ "$_jslog" != "-" ]; then
    mkdir -p "$(dirname "$_jslog")" 2>/dev/null || true
    exec "$PVDIR/bin/paraview" $SCRIPT_ARG "$@" >>"$_jslog" 2>&1
fi

exec "$PVDIR/bin/paraview" $SCRIPT_ARG "$@"
