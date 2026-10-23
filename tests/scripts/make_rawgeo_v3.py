#!/usr/bin/env python3
"""Generate deterministic RawGeo v3 curve fixtures (and verify them back).

The fixtures are the wire format the ANARI-USD sender produces and the UE
client consumes: whole curves packed into a subset lattice, no USD, no zstd.

    python make_rawgeo_v3.py --out DIR --curves 6 --points-per-curve 5 --subsets 4
    python make_rawgeo_v3.py --out DIR --curves 4 --points-per-curve 3 --subsets 2 --rank 0
    python make_rawgeo_v3.py --verify DIR
"""

import argparse
import math
import os
import struct
import sys

MAGIC = 0x3147534A  # "JSG1"
VERSION = 3
TOPOLOGY_POINTS = 0
TOPOLOGY_LINEAR_CURVES = 1

# <I magic, I version, 64s prim_name, Q num_points, d time,
#  I position_components, I color_components, I num_attributes, I attr_uniform_count,
#  Q payload_bytes, Q subset_index, Q subset_count, Q full_num_points,
#  I topology, I curve_count, Q full_curve_count, I width_components, I flags
HEADER = struct.Struct("<II64sQdIIIIQQQQIIQII")
ATTR_DESC = struct.Struct("<32sII")
assert HEADER.size == 160, HEADER.size
assert ATTR_DESC.size == 40, ATTR_DESC.size

PRIM_NAME = "Streamlines"


def fmt_number(value):
    value = float(value)
    if value.is_integer():
        return str(int(value))
    return ("%.9g" % value)


def make_curves(count, points_per_curve):
    """Each curve is a short polyline; colors and widths differ per curve."""
    curves = []
    for i in range(count):
        nodes = []
        for n in range(points_per_curve):
            nodes.append((
                float(i),
                0.1 * i + 0.01 * n,
                0.05 * n,
            ))
        color = (1.0, 0.0, 0.0) if (i % 2 == 0) else (0.0, 0.5, 1.0)
        colors = [color] * points_per_curve
        width = 0.02 + 0.01 * i
        widths = [width] * points_per_curve
        curves.append({"points": nodes, "colors": colors, "widths": widths})
    return curves


def build_payload(curves, subset_index, subset_count, time):
    full_curve_count = len(curves)
    selected = list(range(subset_index, full_curve_count, subset_count))

    lengths = [len(curves[c]["points"]) for c in selected]
    num_points = sum(lengths)
    full_num_points = sum(len(c["points"]) for c in curves)

    points = [p for c in selected for p in curves[c]["points"]]
    colors = [col for c in selected for col in curves[c]["colors"]]
    widths = [w for c in selected for w in curves[c]["widths"]]

    payload = bytearray()
    for p in points:
        payload += struct.pack("<fff", *p)
    for c in colors:
        payload += struct.pack("<fff", *c)
    for n in lengths:
        payload += struct.pack("<I", n)
    for w in widths:
        payload += struct.pack("<f", w)

    header = HEADER.pack(
        MAGIC, VERSION, PRIM_NAME.encode("utf-8"), num_points, float(time),
        3, 3, 0, num_points, len(payload),
        subset_index, subset_count, full_num_points,
        TOPOLOGY_LINEAR_CURVES, len(selected), full_curve_count, 1, 0,
    )
    return header + bytes(payload)


def file_name(time, rank, subset_index, subset_count):
    base = "Wave_Geom__"
    if rank is not None and rank >= 0:
        base += "r%d_" % rank
    base += fmt_number(time)
    if subset_count > 1:
        base += "_s%d" % subset_index
    return base + ".geo"


def generate(args):
    if args.curves <= 0:
        raise SystemExit("--curves must be positive")
    if args.points_per_curve < 2:
        raise SystemExit("--points-per-curve must be >= 2")
    if not 1 <= args.subsets <= 8:
        raise SystemExit("--subsets must be in [1, 8]")

    outdir = os.path.join(args.out, "clips")
    os.makedirs(outdir, exist_ok=True)
    curves = make_curves(args.curves, args.points_per_curve)

    written = 0
    for k in range(args.subsets):
        payload = build_payload(curves, k, args.subsets, args.time)
        path = os.path.join(outdir, file_name(args.time, args.rank, k, args.subsets))
        with open(path, "wb") as f:
            f.write(payload)
        written += 1
    print("wrote %d files to %s" % (written, outdir))


def fail(path, message):
    raise SystemExit("FAIL %s: %s" % (path, message))


def read_payload(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < HEADER.size:
        fail(path, "truncated header (%d bytes)" % len(data))

    (magic, version, raw_name, num_points, time, position_components,
     color_components, num_attributes, attr_uniform_count, payload_bytes,
     subset_index, subset_count, full_num_points, topology, curve_count,
     full_curve_count, width_components, flags) = HEADER.unpack_from(data, 0)

    if magic != MAGIC:
        fail(path, "bad magic 0x%08X" % magic)
    if version != VERSION:
        fail(path, "bad version %d" % version)
    if position_components != 3:
        fail(path, "position_components %d" % position_components)
    if color_components not in (0, 3, 4):
        fail(path, "color_components %d" % color_components)
    if attr_uniform_count != num_points:
        fail(path, "attr_uniform_count %d != num_points %d" % (attr_uniform_count, num_points))
    if flags != 0:
        fail(path, "flags %d" % flags)
    if topology != TOPOLOGY_LINEAR_CURVES:
        fail(path, "topology %d" % topology)
    if width_components not in (0, 1):
        fail(path, "width_components %d" % width_components)
    if subset_count == 0 or subset_index >= subset_count:
        fail(path, "subset %d of %d" % (subset_index, subset_count))
    if full_num_points < num_points:
        fail(path, "full_num_points %d < num_points %d" % (full_num_points, num_points))
    if curve_count > full_curve_count:
        fail(path, "curve_count %d > full_curve_count %d" % (curve_count, full_curve_count))
    expected_curves = int(math.ceil((full_curve_count - subset_index) / float(subset_count)))
    if curve_count != expected_curves:
        fail(path, "curve_count %d != ceil((%d-%d)/%d)=%d"
             % (curve_count, full_curve_count, subset_index, subset_count, expected_curves))

    if len(data) != HEADER.size + payload_bytes:
        fail(path, "file size %d != header %d + payload %d"
             % (len(data), HEADER.size, payload_bytes))

    attr_components = []
    for i in range(num_attributes):
        off = HEADER.size + i * ATTR_DESC.size
        if off + ATTR_DESC.size > len(data):
            fail(path, "attribute descriptor %d out of bounds" % i)
        _name, comps, _pad = ATTR_DESC.unpack_from(data, off)
        if not 1 <= comps <= 4:
            fail(path, "attribute %d components %d" % (i, comps))
        attr_components.append(comps)

    attr_bytes = sum(num_points * c * 4 for c in attr_components)
    expected_payload = (num_points * 3 * 4
                        + num_points * color_components * 4
                        + attr_bytes
                        + curve_count * 4
                        + num_points * width_components * 4)
    if payload_bytes != expected_payload:
        fail(path, "payload_bytes %d != %d" % (payload_bytes, expected_payload))

    off = HEADER.size + num_attributes * ATTR_DESC.size
    off += num_points * 3 * 4
    off += num_points * color_components * 4
    off += attr_bytes
    lengths = list(struct.unpack_from("<%dI" % curve_count, data, off)) if curve_count else []
    if sum(lengths) != num_points:
        fail(path, "sum(curve_vertex_counts) %d != num_points %d" % (sum(lengths), num_points))
    if any(n < 2 for n in lengths):
        fail(path, "a curve has fewer than 2 nodes: %s" % lengths)

    ordinals = [subset_index + i * subset_count for i in range(curve_count)]
    return {
        "path": path,
        "subset_index": subset_index,
        "subset_count": subset_count,
        "full_curve_count": full_curve_count,
        "curve_lengths": lengths,
        "ordinals": ordinals,
    }


def strip_subset_suffix(filename):
    if not filename.endswith(".geo"):
        return filename
    base = filename[:-4]
    idx = base.rfind("_s")
    if idx < 0:
        return filename
    suffix = base[idx + 2:]
    if suffix.isdigit():
        return base[:idx]
    return filename


def verify(directory):
    if not os.path.isdir(directory):
        raise SystemExit("not a directory: %s" % directory)

    files = []
    for root, _dirs, names in os.walk(directory):
        for name in names:
            if name.endswith(".geo"):
                files.append(os.path.join(root, name))
    if not files:
        raise SystemExit("no .geo files under %s" % directory)

    groups = {}
    for path in files:
        info = read_payload(path)
        key = strip_subset_suffix(os.path.basename(path))
        groups.setdefault(key, []).append(info)

    for key, payloads in groups.items():
        subset_count = payloads[0]["subset_count"]
        full_curve_count = payloads[0]["full_curve_count"]
        for p in payloads:
            if p["subset_count"] != subset_count:
                fail(p["path"], "subset_count %d != %d for %s"
                     % (p["subset_count"], subset_count, key))
            if p["full_curve_count"] != full_curve_count:
                fail(p["path"], "full_curve_count %d != %d for %s"
                     % (p["full_curve_count"], full_curve_count, key))
        if len(payloads) != subset_count:
            fail(key, "found %d subsets, expected %d" % (len(payloads), subset_count))
        covered = set()
        for p in payloads:
            if p["subset_index"] in [q["subset_index"] for q in payloads if q is not p]:
                fail(p["path"], "duplicate subset index %d" % p["subset_index"])
            for ordinal in p["ordinals"]:
                if ordinal in covered:
                    fail(p["path"], "curve ordinal %d emitted twice" % ordinal)
                covered.add(ordinal)
        if covered != set(range(full_curve_count)):
            fail(key, "curve ordinals %s do not cover 0..%d"
                 % (sorted(covered), full_curve_count - 1))

    print("OK %d files" % len(files))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", help="output directory (fixtures go in <out>/clips)")
    parser.add_argument("--curves", type=int, default=6)
    parser.add_argument("--points-per-curve", type=int, default=5)
    parser.add_argument("--subsets", type=int, default=4)
    parser.add_argument("--rank", type=int, default=None)
    parser.add_argument("--time", type=float, default=0.0)
    parser.add_argument("--verify", metavar="DIR", help="verify existing fixtures")
    args = parser.parse_args()

    if args.verify:
        verify(args.verify)
        return
    if not args.out:
        parser.error("--out is required unless --verify is used")
    generate(args)


if __name__ == "__main__":
    main()
