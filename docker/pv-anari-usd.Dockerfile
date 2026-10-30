# pv-desktop runtime image with the JUSYNC-patched ANARI-USD device.
#
# The runtime image (staticxg7/pv-anari-usd:6.0.6) ships a prebuilt
# libanari_library_usd.so and no compiler, so the device is rebuilt here in a
# builder stage that uses the *same* base image: identical glibc, libstdc++,
# Python, /opt/usd (OpenUSD) and /usr/local (ANARI SDK), which keeps the ABI
# compatible with the rest of the runtime.
#
# Build context is the ANARI-USD source tree, e.g. from the jusync repo:
#   docker compose build pv-desktop
# or directly (both extra contexts are required - the launcher scripts and the
# pathline example generator live in the jusync repo, not in ANARI-USD):
#   docker build -f docker/pv-anari-usd.Dockerfile \
#                --build-context scripts=./docker \
#                --build-context presets=./examples/paraview-pathlines \
#                -t staticxg7/pv-anari-usd:6.0.8 ../ANARI-USD
ARG BASE_IMAGE=staticxg7/pv-anari-usd:6.0.6
ARG DESKTOP_BASE=staticxg7/pv-anari-usd:6.0.7

FROM ${BASE_IMAGE} AS device-build

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build git pkg-config python3-dev \
      libopenmpi-dev libzmq3-dev libssl-dev libtbb-dev cppzmq-dev \
 && rm -rf /var/lib/apt/lists/*

COPY . /src

RUN cmake -S /src -B /build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DANARI_ROOT_DIR=/usr/local \
      -DCMAKE_PREFIX_PATH="/opt/usd;/usr/local" \
      -DUSD_DEVICE_BUILD_VERSION_EXPLICIT=1 \
 && cmake --build /build -j"$(nproc)" --target anari_library_usd

# LD_PRELOAD bridge that hands ParaView's animation time to the ANARI mapper
# node (see docker/pvtime.cpp). Built here because the runtime image has no
# compiler, against the same glibc as the runtime.
COPY --from=scripts pvtime.cpp /pvtime/pvtime.cpp
RUN g++ -O2 -shared -fPIC -o /build/libpvtime.so /pvtime/pvtime.cpp -ldl

FROM ${BASE_IMAGE}

# JUSYNC fix: UsdBridge::SetNoClipRefs() interpreted an empty-but-present
# reference array as "delete every child". A transient empty commit (view or
# pipeline re-registration) therefore pruned every prim under vtk_world and
# freed their geometry caches via AtRemoveRef, leaving the published USD stage
# with only the root layer, camera and lights - no geometry for the broker, and
# hence nothing for the UE client to spawn.
COPY --from=device-build /build/libanari_library_usd.so /usr/local/lib/libanari_library_usd.so

# ParaView-side half of the same fix: the ANARI mapper node reads
# vtkDataObject::DATA_TIME_STEP() from the actor's property keys, which nothing
# ever fills. libpvtime (LD_PRELOADed by start-paraview-anari.sh) answers that
# read with vtkPVView::SetViewTime(), i.e. the time in ParaView's Time field.
COPY --from=device-build /build/libpvtime.so /opt/pv61/lib/libpvtime.so

# JUSYNC fix: a preset used to load its .pvsm state, and those states carry no
# animation scene, so ParaView kept a single time value and the stream repeated
# t=0 forever. jusync-preset now opens the dataset itself, the launcher derives
# JUSYNC_ANIM_DT from that dataset's timesteps, and jusync-frame-index records
# each frame's point hash so the device stamps the dataset's OWN time (exact
# match even after skipped frames or scrubbing).
COPY --from=scripts start-paraview-anari.sh /opt/pv61/start-paraview-anari.sh
COPY --from=scripts jusync-preset /usr/local/bin/jusync-preset
COPY --from=scripts jusync-anim-dt /usr/local/bin/jusync-anim-dt
COPY --from=scripts jusync-frame-index /usr/local/bin/jusync-frame-index
RUN chmod +x /opt/pv61/start-paraview-anari.sh /usr/local/bin/jusync-preset \
      /usr/local/bin/jusync-anim-dt /usr/local/bin/jusync-frame-index \
 && if [ -d /opt/sender ]; then cp /opt/pv61/start-paraview-anari.sh /opt/sender/start-paraview-anari.sh; fi

# Small animated point cloud for the UE pathline tracker: 256 particles, 100
# frames, dt = 0.05 (5 s loop), stable particle_id + attribute0 (see
# examples/paraview-pathlines). Generated at build time from the repo
# generator, so no binary data has to be committed; the script is installed
# alongside make_presets.py so variants can be generated inside the image.
COPY --from=presets make_particle_example.py /opt/jusync/make_particle_example.py
RUN python3 /opt/jusync/make_particle_example.py \
      --out /opt/jusync/presets/pathlines --name pathlines

# One-click desktop entry, same convention as the presets shipped by the base
# image (which knows nothing about pathlines).
RUN printf '%s\n' \
      '[Desktop Entry]' \
      'Type=Application' \
      'Version=1.0' \
      'Name=ParaView preset: pathlines' \
      'Comment=Open the pathlines sample on the ANARI usd device' \
      'Exec=/usr/local/bin/jusync-preset pathlines' \
      'Icon=paraview' \
      'Terminal=false' \
      'Categories=Graphics;Science;' \
      > /usr/share/applications/jusync-preset-pathlines.desktop \
 && chmod +x /usr/share/applications/jusync-preset-pathlines.desktop \
 && cp /usr/share/applications/jusync-preset-pathlines.desktop /root/Desktop/ \
 && chmod +x /root/Desktop/jusync-preset-pathlines.desktop

# The base image's labels still say 6.0.6 / anim-player v12; this image builds
# the device from source (module v15) and adds the pathline preset.
LABEL org.opencontainers.image.title="pv-anari-usd:6.0.7" \
      org.opencontainers.image.version="6.0.7" \
      org.opencontainers.image.description="ParaView 6.1 + ANARI-USD RawGeo v3 device (anim-player v15, JUSYNC fixes) + VNC desktop with one-click presets (streamlines, large-cloud, fast-morph, mesh, pathlines)"

# 6.0.8: desktop layer on the published 6.0.7, so desktop edits do not rebuild
# the ANARI device (the stages above become unreachable and are skipped).
FROM ${DESKTOP_BASE}

# Two commands on purpose: this is the exact pair used for the live desktop, and
# splitting them keeps the resolved dependency set the same.
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
      tint2 picom feh imagemagick fonts-inter \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq librsvg2-bin \
 && rm -rf /var/lib/apt/lists/*

COPY --from=scripts desktop/jusync-xstartup /usr/local/bin/jusync-xstartup
COPY --from=scripts desktop/tint2rc /root/.config/tint2/tint2rc
COPY --from=scripts desktop/picom.conf /root/.config/picom.conf
COPY --from=scripts desktop/fluxbox-init /root/.fluxbox/init
COPY --from=scripts desktop/fluxbox-menu /root/.fluxbox/menu
COPY --from=scripts desktop/jusync.png /usr/share/backgrounds/jusync.png
COPY --from=scripts desktop/paraview.png /usr/share/icons/hicolor/64x64/apps/paraview.png
COPY --from=scripts desktop/start-paraview.sh /root/Desktop/start-paraview.sh
COPY --from=scripts desktop/start-paraview.desktop /root/Desktop/start-paraview.desktop
COPY --from=scripts desktop/xterm.desktop /usr/share/applications/xterm.desktop

RUN chmod +x /usr/local/bin/jusync-xstartup /root/Desktop/start-paraview.sh \
      /root/Desktop/start-paraview.desktop \
 && ln -sf /root/Desktop/start-paraview.sh /usr/local/bin/start-paraview

# ParaView must not autostart; the panel/desktop launcher is the only way in.
ENV JUSYNC_AUTOSTART_PV=0

LABEL org.opencontainers.image.title="pv-anari-usd:6.0.8" \
      org.opencontainers.image.version="6.0.8" \
      org.opencontainers.image.description="ParaView 6.1 + ANARI-USD RawGeo v3 device (anim-player v15, JUSYNC fixes) + beautified VNC desktop (JSC wallpaper, tint2 panel, picom, fluxbox) with one-click presets (streamlines, large-cloud, fast-morph, mesh, pathlines); ParaView starts from the launcher, not at boot"
