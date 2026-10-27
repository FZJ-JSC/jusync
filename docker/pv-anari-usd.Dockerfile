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
# or directly:
#   docker build -f docker/pv-anari-usd.Dockerfile \
#                -t pv-anari-usd:6.0.6-jusync ../ANARI-USD
ARG BASE_IMAGE=staticxg7/pv-anari-usd:6.0.6

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

FROM ${BASE_IMAGE}

# JUSYNC fix: UsdBridge::SetNoClipRefs() interpreted an empty-but-present
# reference array as "delete every child". A transient empty commit (view or
# pipeline re-registration) therefore pruned every prim under vtk_world and
# freed their geometry caches via AtRemoveRef, leaving the published USD stage
# with only the root layer, camera and lights - no geometry for the broker, and
# hence nothing for the UE client to spawn.
COPY --from=device-build /build/libanari_library_usd.so /usr/local/lib/libanari_library_usd.so

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
