#!/usr/bin/env python3
"""Small animated particle dataset for testing JUSYNC pathlines in Unreal.

Writes a PVD time series of binary VTP point clouds - the same wire contract
the shipped streaming presets use (see make_presets.py in the pv-anari-usd
image):

  * one vertex per particle, same order in every frame
  * a stable float32 ``particle_id`` per particle, so UE can join the frames
    into tracks (JUSYNC's default IdAttribute is ``particle_id``)
  * a stable per-particle scalar ``attribute0`` in [0, 1], so the tracks can
    be coloured / range-filtered (the default ScalarAttribute)
  * timesteps that close a loop, so the animation can run forever

Geometry: eight tilted rings of particles. Each ring orbits once over the whole
series while every particle also spins three times around its ring, so each
track is a coiled helix and the whole set reads as eight springs sweeping
through the volume. Tracks are never dropped or re-ordered, which is what makes
the dataset a good pathline test: 256 tracks, 100 nodes each.

Run inside the pv-anari-usd container:

    python3 examples/paraview-pathlines/make_particle_example.py \
        --out /work/examples/paraview-pathlines/particles --name pathlines

Only numpy is needed; no vtk python module (the image's ParaView has no Python
support anyway).
"""

from __future__ import annotations

import argparse
import math
import os
import struct

import numpy as np

# ---------------------------------------------------------------------------
# Dataset shape (all overridable on the command line)
# ---------------------------------------------------------------------------

GROUPS = 8          # concentric tilted rings
PER_GROUP = 32      # particles per ring  -> 256 tracks total
FRAMES = 100        # 100 samples x 0.05 = 5 s loop
DT = 0.05

# Radius of a particle's own little circle around its ring position.
PARTICLE_RADIUS = 0.9
# Radius of the first ring and the growth per ring.
RING_RADIUS = 5.0
RING_RADIUS_STEP = 1.2
# Vertical bob (three cycles per lap) and ring tilt range in degrees.
BOB_AMPLITUDE = 1.6
TILT_MIN_DEG = 12.0
TILT_STEP_DEG = 6.0


# ---------------------------------------------------------------------------
# VTK XML appended-raw helpers
#
# Layout copied from the image's make_presets.py: every array is stored as a
# UInt32 byte count followed by the raw little-endian payload, and the byte
# offsets in the XML are relative to the first byte after the '_' marker.
# ---------------------------------------------------------------------------


def append_array(blob: bytearray, array: np.ndarray) -> int:
    array = np.ascontiguousarray(array)
    offset = len(blob)
    blob += struct.pack("<I", array.nbytes)
    blob += array.tobytes(order="C")
    return offset


def write_pvd(path: str, entries: list[tuple[str, float]]) -> None:
    lines = [
        '<?xml version="1.0"?>',
        '<VTKFile type="Collection" version="0.1" byte_order="LittleEndian">',
        "  <Collection>",
    ]
    for rel, t in entries:
        lines.append(
            f'    <DataSet timestep="{t:.6f}" group="" part="0" file="{rel}"/>')
    lines += ["  </Collection>", "</VTKFile>", ""]
    with open(path, "w", encoding="ascii", newline="\n") as handle:
        handle.write("\n".join(lines))


def write_points_vtp(path: str, points, particle_id, attribute0) -> None:
    """Binary VTP with one vertex cell per point (the pathline particle contract)."""
    points = np.ascontiguousarray(points, dtype="<f4")
    n = points.shape[0]
    connectivity = np.arange(n, dtype="<i4")
    offsets = np.arange(1, n + 1, dtype="<i4")
    particle_id = np.ascontiguousarray(particle_id, dtype="<f4")
    attribute0 = np.ascontiguousarray(attribute0, dtype="<f4")

    blob = bytearray()
    off_points = append_array(blob, points)
    off_conn = append_array(blob, connectivity)
    off_offs = append_array(blob, offsets)
    off_pid = append_array(blob, particle_id)
    off_attr = append_array(blob, attribute0)

    xml = f'''<?xml version="1.0"?>
<VTKFile type="PolyData" version="0.1" byte_order="LittleEndian" header_type="UInt32">
  <PolyData>
    <Piece NumberOfPoints="{n}" NumberOfVerts="{n}" NumberOfLines="0" NumberOfStrips="0" NumberOfPolys="0">
      <PointData Scalars="attribute0">
        <DataArray type="Float32" Name="particle_id" NumberOfComponents="1" format="appended" offset="{off_pid}"/>
        <DataArray type="Float32" Name="attribute0" NumberOfComponents="1" format="appended" offset="{off_attr}"/>
      </PointData>
      <Points>
        <DataArray type="Float32" Name="Points" NumberOfComponents="3" format="appended" offset="{off_points}"/>
      </Points>
      <Verts>
        <DataArray type="Int32" Name="connectivity" format="appended" offset="{off_conn}"/>
        <DataArray type="Int32" Name="offsets" format="appended" offset="{off_offs}"/>
      </Verts>
    </Piece>
  </PolyData>
  <AppendedData encoding="raw">
_'''.encode("ascii")
    with open(path, "wb") as handle:
        handle.write(xml)
        handle.write(blob)
        handle.write(b"\n  </AppendedData>\n</VTKFile>\n")


# ---------------------------------------------------------------------------
# ParaView state file (.pvsm)
#
# Same minimal, verified layout as make_presets.py: PVDReader + a Points
# representation + a camera fitted to the bounds. jusync-preset opens the .pvd
# itself (a saved state carries no animation scene), so this file is what makes
# the dataset show up in the desktop / preset list.
# ---------------------------------------------------------------------------

PRESET_VIEW_SIZE = (1280, 800)


def _camera_for(bounds, angle=30.0):
    lo, hi = np.asarray(bounds[0], float), np.asarray(bounds[1], float)
    center = 0.5 * (lo + hi)
    radius = max(1e-6, 0.5 * float(np.linalg.norm(hi - lo)))
    dist = 3.2 * radius
    direction = np.asarray([0.62, -0.72, 0.32])
    direction /= np.linalg.norm(direction)
    pos = center + direction * dist
    clip = (max(0.05, dist - 2.2 * radius), dist + 2.2 * radius)
    return center, pos, clip, angle


def write_pvsm(path, data_file, bounds, point_size=3.0,
               color_array="attribute0",
               view_size=PRESET_VIEW_SIZE, background=(0.32, 0.34, 0.43)):
    center, pos, clip, angle = _camera_for(bounds)

    # Blue -> light grey -> yellow, readable on the ParaView background and on
    # the dark UE scene.
    rgb = [0.0, 0.231, 0.298, 0.753,
           0.5, 0.865, 0.865, 0.865,
           1.0, 0.706, 0.016, 0.149]
    rgb_xml = "\n".join(
        f'        <Element index="{i}" value="{v:g}"/>' for i, v in enumerate(rgb))

    xml = f'''<?xml version="1.0"?>
<ParaView>
  <ServerManagerState version="6.1.0">
    <Proxy group="sources" type="PVDReader" id="1001" servers="1">
      <Property name="FileName" id="1001.FileName" number_of_elements="1">
        <Element index="0" value="{data_file}"/>
      </Property>
    </Proxy>
    <Proxy group="representations" type="GeometryRepresentation" id="1002" servers="21">
      <Property name="Input" id="1002.Input" number_of_elements="1">
        <Proxy value="1001" output_port="0"/>
      </Property>
      <Property name="Visibility" id="1002.Visibility" number_of_elements="1">
        <Element index="0" value="1"/>
      </Property>
      <Property name="Representation" id="1002.Representation" number_of_elements="1">
        <Element index="0" value="Points"/>
      </Property>
      <Property name="PointSize" id="1002.PointSize" number_of_elements="1">
        <Element index="0" value="{point_size:g}"/>
      </Property>
      <Property name="RenderPointsAsSpheres" id="1002.RenderPointsAsSpheres" number_of_elements="1">
        <Element index="0" value="0"/>
      </Property>
      <Property name="DiffuseColor" id="1002.DiffuseColor" number_of_elements="3">
        <Element index="0" value="0.9"/>
        <Element index="1" value="0.9"/>
        <Element index="2" value="0.9"/>
      </Property>
      <Property name="ColorArrayName" id="1002.ColorArrayName" number_of_elements="5">
        <Element index="0" value=""/>
        <Element index="1" value=""/>
        <Element index="2" value=""/>
        <Element index="3" value="0"/>
        <Element index="4" value="{color_array}"/>
      </Property>
      <Property name="MapScalars" id="1002.MapScalars" number_of_elements="1">
        <Element index="0" value="1"/>
      </Property>
      <Property name="InterpolateScalarsBeforeMapping" id="1002.InterpolateScalarsBeforeMapping" number_of_elements="1">
        <Element index="0" value="1"/>
      </Property>
      <Property name="LookupTable" id="1002.LookupTable" number_of_elements="1">
        <Proxy value="1005"/>
      </Property>
    </Proxy>
    <Proxy group="lookup_tables" type="PVLookupTable" id="1005" servers="21">
      <Property name="RGBPoints" id="1005.RGBPoints" number_of_elements="12">
{rgb_xml}
      </Property>
      <Property name="ScalarRangeInitialized" id="1005.ScalarRangeInitialized" number_of_elements="1">
        <Element index="0" value="1"/>
      </Property>
      <Property name="IndexedLookup" id="1005.IndexedLookup" number_of_elements="1">
        <Element index="0" value="0"/>
      </Property>
      <Property name="NanColor" id="1005.NanColor" number_of_elements="3">
        <Element index="0" value="1"/>
        <Element index="1" value="1"/>
        <Element index="2" value="0"/>
      </Property>
      <Property name="NumberOfTableValues" id="1005.NumberOfTableValues" number_of_elements="1">
        <Element index="0" value="256"/>
      </Property>
      <Property name="RescaleTransferFunctionToDataRangeOverTime" id="1005.RescaleTransferFunctionToDataRangeOverTime" number_of_elements="1">
        <Element index="0" value="0"/>
      </Property>
    </Proxy>
    <Proxy group="views" type="RenderView" id="1003" servers="21">
      <Property name="Representations" id="1003.Representations" number_of_elements="1">
        <Proxy value="1002"/>
      </Property>
      <Property name="Background" id="1003.Background" number_of_elements="3">
        <Element index="0" value="{background[0]:g}"/>
        <Element index="1" value="{background[1]:g}"/>
        <Element index="2" value="{background[2]:g}"/>
      </Property>
      <Property name="CameraPosition" id="1003.CameraPosition" number_of_elements="3">
        <Element index="0" value="{pos[0]:g}"/>
        <Element index="1" value="{pos[1]:g}"/>
        <Element index="2" value="{pos[2]:g}"/>
      </Property>
      <Property name="CameraFocalPoint" id="1003.CameraFocalPoint" number_of_elements="3">
        <Element index="0" value="{center[0]:g}"/>
        <Element index="1" value="{center[1]:g}"/>
        <Element index="2" value="{center[2]:g}"/>
      </Property>
      <Property name="CameraViewUp" id="1003.CameraViewUp" number_of_elements="3">
        <Element index="0" value="0"/>
        <Element index="1" value="0"/>
        <Element index="2" value="1"/>
      </Property>
      <Property name="CameraViewAngle" id="1003.CameraViewAngle" number_of_elements="1">
        <Element index="0" value="{angle:g}"/>
      </Property>
      <Property name="CameraParallelProjection" id="1003.CameraParallelProjection" number_of_elements="1">
        <Element index="0" value="0"/>
      </Property>
      <Property name="CameraClippingRange" id="1003.CameraClippingRange" number_of_elements="2">
        <Element index="0" value="{clip[0]:g}"/>
        <Element index="1" value="{clip[1]:g}"/>
      </Property>
      <Property name="ViewSize" id="1003.ViewSize" number_of_elements="2">
        <Element index="0" value="{view_size[0]}"/>
        <Element index="1" value="{view_size[1]}"/>
      </Property>
    </Proxy>
    <Proxy group="misc" type="ViewLayout" id="1004" servers="20">
      <Property name="PreviewMode" id="1004.PreviewMode" number_of_elements="2">
        <Element index="0" value="0"/>
        <Element index="1" value="0"/>
      </Property>
      <Property name="SeparatorColor" id="1004.SeparatorColor" number_of_elements="3">
        <Element index="0" value="0.937"/>
        <Element index="1" value="0.922"/>
        <Element index="2" value="0.906"/>
      </Property>
      <Property name="SeparatorWidth" id="1004.SeparatorWidth" number_of_elements="1">
        <Element index="0" value="1"/>
      </Property>
      <Layout number_of_elements="1">
        <Item direction="0" fraction="0.5" view="1003"/>
      </Layout>
    </Proxy>
    <ProxyCollection name="sources">
      <Item id="1001" name="{os.path.basename(data_file)}"/>
    </ProxyCollection>
    <ProxyCollection name="representations">
      <Item id="1002" name="GeometryRepresentation1"/>
    </ProxyCollection>
    <ProxyCollection name="views">
      <Item id="1003" name="RenderView1"/>
    </ProxyCollection>
    <ProxyCollection name="layouts">
      <Item id="1004" name="Layout #1"/>
    </ProxyCollection>
  </ServerManagerState>
</ParaView>
'''
    with open(path, "w", encoding="ascii", newline="\n") as handle:
        handle.write(xml)


# ---------------------------------------------------------------------------
# Motion
# ---------------------------------------------------------------------------


def particle_positions(phase: float, groups: int, per_group: int) -> np.ndarray:
    """Positions for one frame; ``phase`` is the lap angle in radians."""
    out = np.empty((groups * per_group, 3), dtype=np.float64)
    index = np.arange(per_group, dtype=np.float64)
    spin = 2.0 * math.pi * index / per_group
    row = 0
    for g in range(groups):
        ring = RING_RADIUS + RING_RADIUS_STEP * g
        a = phase + 2.0 * math.pi * g / groups
        tilt = math.radians(TILT_MIN_DEG + TILT_STEP_DEG * g)
        bob = BOB_AMPLITUDE * math.sin(3.0 * phase + 0.7 * g)

        # Three spins per lap: each track is a coiled helix around the ring.
        b = spin + 3.0 * phase
        radial = ring + PARTICLE_RADIUS * np.cos(b)
        x = radial * math.cos(a)
        y = radial * math.sin(a)
        z = bob + PARTICLE_RADIUS * np.sin(b)

        # Tilt each ring's plane about X so the eight springs interleave.
        ct, st = math.cos(tilt), math.sin(tilt)
        out[row:row + per_group, 0] = x
        out[row:row + per_group, 1] = y * ct - z * st
        out[row:row + per_group, 2] = y * st + z * ct
        row += per_group
    return out


def particle_scalars(groups: int, per_group: int) -> np.ndarray:
    """Stable [0, 1] scalar: half from the ring, half from the position in it.

    Ring-major ordering means a range filter (say 0.4-0.6) keeps a contiguous
    band of tubes, which is a quick way to check the UE range/hue filters.
    """
    g = np.repeat(np.arange(groups, dtype=np.float64), per_group)
    k = np.tile(np.arange(per_group, dtype=np.float64), groups)
    ring_part = g / max(1.0, groups - 1.0)
    slot_part = k / max(1.0, per_group - 1.0)
    return 0.5 * ring_part + 0.5 * slot_part


# ---------------------------------------------------------------------------


def generate(out_dir: str, name: str, groups: int, per_group: int,
             frames: int, dt: float) -> dict:
    points_dir = os.path.join(out_dir, "points")
    os.makedirs(points_dir, exist_ok=True)

    total = groups * per_group
    particle_id = np.arange(total, dtype="<f4")
    attribute0 = particle_scalars(groups, per_group)

    entries: list[tuple[str, float]] = []
    lo = np.full(3, np.inf)
    hi = np.full(3, -np.inf)
    for frame in range(frames):
        phase = 2.0 * math.pi * frame / frames
        pts = particle_positions(phase, groups, per_group)
        lo = np.minimum(lo, pts.min(axis=0))
        hi = np.maximum(hi, pts.max(axis=0))
        rel = f"points/particle_{frame:04d}.vtp"
        write_points_vtp(os.path.join(out_dir, rel), pts, particle_id, attribute0)
        entries.append((rel, frame * dt))

    pvd = os.path.join(out_dir, "particles.pvd")
    write_pvd(pvd, entries)

    pvsm = os.path.join(out_dir, f"{name}.pvsm")
    write_pvsm(pvsm, pvd, bounds=(lo, hi), point_size=3.0)

    # start-paraview-anari.sh re-derives dt/fps/samples from the .pvd, but a
    # preset is expected to ship its own env (and jusync-preset --env prints it).
    fps = 1.0 / dt if dt > 0 else 20.0
    with open(os.path.join(out_dir, "preset.env"), "w",
              encoding="ascii", newline="\n") as handle:
        handle.write("\n".join([
            "JUSYNC_RAWGEO_STRIDE=1",
            f"JUSYNC_ANIM_DT={dt:g}",
            f"JUSYNC_ANIM_FPS={fps:g}",
            f"JUSYNC_ANIM_PUBLISH_HZ={min(fps, 30.0):g}",
            f"JUSYNC_ANIM_MAXSAMPLES={frames}",
        ]) + "\n")

    with open(os.path.join(out_dir, "preset.meta"), "w",
              encoding="ascii", newline="\n") as handle:
        handle.write(f"name={name}\nstate={pvsm}\ndata={pvd}\n")

    return dict(total=total, frames=frames, lo=lo, hi=hi, pvd=pvd,
                pvsm=pvsm, points_dir=points_dir)


def main() -> None:
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(here, "particles"),
                    help="output directory (default: ./particles)")
    ap.add_argument("--name", default="pathlines",
                    help="preset name; <out>/<name>.pvsm is written (default: pathlines)")
    ap.add_argument("--groups", type=int, default=GROUPS)
    ap.add_argument("--per-group", type=int, default=PER_GROUP)
    ap.add_argument("--frames", type=int, default=FRAMES)
    ap.add_argument("--dt", type=float, default=DT)
    args = ap.parse_args()

    info = generate(os.path.abspath(args.out), args.name, args.groups,
                    args.per_group, args.frames, args.dt)
    lo, hi = info["lo"], info["hi"]
    print(f"wrote {info['total']} particles x {info['frames']} frames "
          f"({info['frames'] * args.dt:g} s loop) to {args.out}")
    print(f"bounds  [{lo[0]:.2f} {lo[1]:.2f} {lo[2]:.2f}] .. "
          f"[{hi[0]:.2f} {hi[1]:.2f} {hi[2]:.2f}]")
    print(f"open    {info['pvd']}")


if __name__ == "__main__":
    main()
