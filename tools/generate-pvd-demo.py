# Regenerates the animated PVD demo baked into staticxg7/pv-anari-usd >= 6.6
# at /opt/pvd-examples/animated-demo/ (baked tag built via docker commit).
#
# Run inside a throwaway container of that image (it has no pip/vtk baked in):
#   docker run -d --name pvd-gen --entrypoint bash staticxg7/pv-anari-usd:6.6 -c "sleep infinity"
#   docker exec pvd-gen bash -c "apt-get update -qq && apt-get install -y -qq python3-pip && python3 -m pip install --break-system-packages numpy vtk"
#   docker cp tools/generate-pvd-demo.py pvd-gen:/gen.py
#   docker exec pvd-gen python3 -u /gen.py          # writes + validates /out
#   docker cp pvd-gen:/out/. <staging>:/opt/pvd-examples/animated-demo
#   docker commit <staging> staticxg7/pv-anari-usd:<newtag>
#
# Output lands in /out:
#   /out/plasma-torus.pvd   51,200-vertex torus mesh, 30 seamless-loop steps,
#                           traveling harmonic ripples, scalars "energy"
#   /out/storm-points.pvd   320,000 orbiting points, 30 steps, scalars
#                           "speed" (differential band rotation) + "temp"
#
# Everything uses integer-multiple time frequencies so step N-1 flows back to
# step 0 with zero jump (perfect loop). Writes compressed appended-binary .vtp
# via the pip vtk module; run inside the throwaway generator container.
import math
import os

import numpy as np
import vtk
import vtk.util.numpy_support as nps

OUT = "/out"
N_STEPS = 30
N_POINTS = 320_000
TORUS_U = 320
TORUS_V = 160
TAU = 2.0 * math.pi


def n2v(arr, dtype=None):
    a = np.ascontiguousarray(arr)
    return nps.numpy_to_vtk(a, deep=True, array_type=dtype)


def make_polydata(points, scalars, polys=None):
    # NOTE: vtkPoints.SetData stores a raw pointer WITHOUT taking a reference,
    # so the numpy-backed vtk arrays must stay referenced (pd._keepalive) or
    # they are GC'd and the point buffer dies under us (heap corruption).
    keep = []

    pts_arr = n2v(np.ascontiguousarray(points.astype(np.float32)))
    keep.append(pts_arr)
    pts = vtk.vtkPoints()
    pts.SetDataTypeToFloat()
    pts.SetData(pts_arr)
    pd = vtk.vtkPolyData()
    pd.SetPoints(pts)

    if polys is not None:
        legacy, n_cells = polys
        flat = n2v(np.ascontiguousarray(legacy.astype(np.int64)),
                   dtype=vtk.VTK_ID_TYPE)
        keep.append(flat)
        cells = vtk.vtkCellArray()
        cells.SetCells(n_cells, flat)
        pd.SetPolys(cells)

    for name, arr in scalars.items():
        a = n2v(np.ascontiguousarray(arr.astype(np.float32)))
        a.SetName(name)
        a.SetNumberOfComponents(1)
        pd.GetPointData().AddArray(a)
    pd.GetPointData().SetActiveScalars(list(scalars.keys())[0])
    pd._keepalive = keep
    return pd


def write_vtp(path, pd):
    w = vtk.vtkXMLPolyDataWriter()
    w.SetFileName(path)
    w.SetInputData(pd)
    w.SetDataModeToAppended()
    if not w.Write():
        raise RuntimeError("failed to write " + path)


def pvd(path, files):
    lines = ['<?xml version="1.0"?>',
             '<VTKFile type="Collection" version="0.1" byte_order="LittleEndian">',
             '  <Collection>']
    for step, f in enumerate(files):
        lines.append('    <DataSet timestep="%d" group="" part="0" file="%s"/>' % (step, f))
    lines += ['  </Collection>', '</VTKFile>']
    with open(path, "w") as fh:
        fh.write("\n".join(lines) + "\n")


# ---------------------------------------------------------------- mesh ----
def build_torus():
    os.makedirs(os.path.join(OUT, "mesh"), exist_ok=True)
    nu, nv = TORUS_U, TORUS_V
    R, r = 60.0, 18.0
    u = (np.arange(nu) / nu) * TAU
    v = (np.arange(nv) / nv) * TAU
    uu, vv = np.meshgrid(u, v, indexing="ij")
    uu = uu.ravel().astype(np.float64)
    vv = vv.ravel().astype(np.float64)
    nverts = nu * nv

    base = np.empty((nverts, 3), dtype=np.float64)
    cos_v, sin_v = np.cos(vv), np.sin(vv)
    cos_u, sin_u = np.cos(uu), np.sin(uu)
    base[:, 0] = (R + r * cos_v) * cos_u
    base[:, 1] = (R + r * cos_v) * sin_u
    base[:, 2] = r * sin_v
    normal = np.stack([cos_v * cos_u, cos_v * sin_u, sin_v], axis=1)

    # quad grid wrapped on both axes -> 2 triangles per quad
    i = np.repeat(np.arange(nu), nv)
    j = np.tile(np.arange(nv), nu)
    a = i * nv + j
    b = ((i + 1) % nu) * nv + j
    c = ((i + 1) % nu) * nv + (j + 1) % nv
    d = i * nv + (j + 1) % nv
    tri = np.stack([a, b, c, a, c, d], axis=1).reshape(-1, 3).astype(np.int64)
    ntri = tri.shape[0]
    legacy_tris = np.column_stack([np.full(ntri, 3, dtype=np.int64), tri]).ravel()

    files = []
    for s in range(N_STEPS):
        ph = TAU * s / N_STEPS
        disp = (4.0 * np.sin(3.0 * uu - 2.0 * ph)
                + 3.0 * np.sin(5.0 * uu + 4.0 * vv + 4.0 * ph)
                + 2.5 * np.sin(8.0 * vv - 6.0 * ph)
                + 1.5 * np.sin(7.0 * uu - 9.0 * vv + 8.0 * ph))
        pts = base + normal * disp[:, None]
        energy = ((disp + 11.0) / 22.0
                  + 0.15 * np.sin(6.0 * uu + 3.0 * vv + 2.0 * ph))
        energy = np.clip(energy, 0.0, 1.0)

        pd = make_polydata(pts, {"energy": energy},
                           polys=(legacy_tris, ntri))
        normals = vtk.vtkPolyDataNormals()
        normals.SetInputData(pd)
        normals.ComputePointNormalsOn()
        normals.ConsistencyOn()
        normals.SetSplitting(0)
        normals.Update()
        out = normals.GetOutput()
        out.GetPointData().SetActiveScalars("energy")
        name = "plasma_torus_%02d.vtp" % s
        write_vtp(os.path.join(OUT, "mesh", name), out)
        files.append("mesh/" + name)
    pvd(os.path.join(OUT, "plasma-torus.pvd"), files)
    print("MESH_DONE steps=%d verts=%d tris=%d" % (N_STEPS, nverts, ntri))


# --------------------------------------------------------------- points ----
def build_storm():
    os.makedirs(os.path.join(OUT, "points"), exist_ok=True)
    rng = np.random.default_rng(20261016)
    n = N_POINTS
    # clustered radius: sqrt keeps volume density even, min radius avoids the core
    rad = 25.0 + 85.0 * np.sqrt(rng.random(n))
    phi0 = rng.random(n) * TAU
    z0 = rng.normal(0.0, 14.0, n)
    # integer winding count per radial band -> seamless loop despite shear
    band = (1.0 + 3.0 * (1.0 - (rad - 25.0) / 85.0)).round()
    spin = np.where(rng.random(n) < 0.92, 1.0, -1.0)  # a few retrograde motes
    speed = band * rad * 0.01  # tangential speed proxy for coloring

    # single-vertex cells so every proxy path treats it as an explicit point set
    legacy_verts = np.column_stack(
        [np.ones(n, dtype=np.int64), np.arange(n, dtype=np.int64)]).ravel()

    files = []
    for s in range(N_STEPS):
        ph = TAU * s / N_STEPS
        ang = phi0 + spin * band * ph
        rr = rad * (1.0 + 0.06 * np.sin(2.0 * ph + phi0 * 3.0))
        x = rr * np.cos(ang)
        y = rr * np.sin(ang)
        z = (z0 * (1.0 + 0.30 * np.sin(3.0 * ph + phi0 * 2.0))
             + 7.0 * np.sin(ph) * np.cos(2.0 * ang))
        pts = np.stack([x, y, z], axis=1)
        temp = 0.5 + 0.5 * np.sin(2.0 * ph + phi0 * 4.0 + rad * 0.05)

        pd = make_polydata(pts, {"speed": speed, "temp": temp},
                           polys=(legacy_verts, n))
        name = "storm_points_%02d.vtp" % s
        write_vtp(os.path.join(OUT, "points", name), pd)
        files.append("points/" + name)
    pvd(os.path.join(OUT, "storm-points.pvd"), files)
    print("POINTS_DONE steps=%d n=%d" % (N_STEPS, n))


# ------------------------------------------------------------ validate ----
def validate():
    import xml.etree.ElementTree as ET
    total = 0
    for p, sub in (("plasma-torus.pvd", "mesh"), ("storm-points.pvd", "points")):
        root = ET.parse(os.path.join(OUT, p)).getroot()
        ds = root.findall("./Collection/DataSet")
        assert len(ds) == N_STEPS, (p, len(ds))
        counts = []
        for el in ds:
            rd = vtk.vtkXMLPolyDataReader()
            rd.SetFileName(os.path.join(OUT, el.get("file")))
            rd.Update()
            out = rd.GetOutput()
            counts.append((out.GetNumberOfPoints(), out.GetNumberOfCells()))
        assert counts[0] == counts[-1], counts[0]
        first = vtk.vtkXMLPolyDataReader()
        first.SetFileName(os.path.join(OUT, ds[0].get("file")))
        first.Update()
        last = vtk.vtkXMLPolyDataReader()
        last.SetFileName(os.path.join(OUT, ds[N_STEPS - 1].get("file")))
        last.Update()
        d0 = np.array(nps.vtk_to_numpy(first.GetOutput().GetPoints().GetData()))
        d1 = np.array(nps.vtk_to_numpy(last.GetOutput().GetPoints().GetData()))
        moved = float(np.max(np.abs(d1 - d0)))
        assert moved > 1.0, ("last step identical to first?", moved)
        names = [first.GetOutput().GetPointData().GetArrayName(i)
                 for i in range(first.GetOutput().GetPointData().GetNumberOfArrays())]
        print("VALID %s steps=%d pts=%d cells=%d arrays=%s max_move=%.1f"
              % (p, len(ds), counts[0][0], counts[0][1], names, moved))
    for dirpath, _, files in os.walk(OUT):
        total += sum(os.path.getsize(os.path.join(dirpath, f)) for f in files)
    print("TOTAL_SIZE_MB %.1f" % (total / 1e6))


build_torus()
build_storm()
validate()
print("GEN_DONE")
