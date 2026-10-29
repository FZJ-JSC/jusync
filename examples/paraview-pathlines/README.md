# ParaView pathline example

A tiny animated point cloud that ParaView streams through the ANARI `usd`
device, so the UE plugin's pathline renderer can be exercised end to end
without any HPC job:

* **256 particles, 100 frames, dt = 0.05 s** (5 s loop, ~820 KB on disk)
* every particle keeps a stable `particle_id` in every frame, and a stable
  `attribute0` scalar in `[0, 1]` - exactly the two arrays
  `AJUSYNCFileSpawnerActor` uses to join frames into tracks
* one vertex per particle, binary appended-raw VTP - the same layout the
  image's own presets (`large-cloud`, `fast-morph`, ...) use
* the motion closes on itself, so the loop can run forever and no track ever
  expires

Geometry: eight tilted rings. Each ring orbits once over the whole series while
every particle also spins three times around its ring, so each track is a coiled
helix and the set reads as eight springs sweeping through the volume.

```
make_particle_example.py   generator (numpy only, no vtk python needed)
install-preset.sh          registers particles/ as preset "pathlines"
                           (/work iteration only - 6.0.7 bakes the preset in)
particles/
  particles.pvd            100-entry VTK time series
  points/particle_0000.vtp one point cloud per frame
  pathlines.pvsm           PVDReader + Points + fitted camera
  preset.env / preset.meta settings read by jusync-preset / the launcher
```

## Regenerate

The generator writes regular files under the repo, which the container sees
through its `/work` mount:

```sh
docker exec pv-desktop python3 /work/examples/paraview-pathlines/make_particle_example.py

# other shapes: fewer/more tracks, a different clock
docker exec pv-desktop python3 /work/examples/paraview-pathlines/make_particle_example.py \
    --groups 4 --per-group 16 --frames 60 --dt 0.1
```

The generator also ships in the image as
`/opt/jusync/make_particle_example.py`; writing straight into the preset
directory (`--out /opt/jusync/presets/pathlines`) replaces the baked-in copy
inside that container only.

`--out` and `--name` control where the preset lands; keep `--name` equal to the
directory name (`/opt/jusync/presets/<name>/<name>.pvsm` is the convention).

## Open it in ParaView

Since 6.0.7 the preset is baked into `staticxg7/pv-anari-usd`, so there is no
install step - `pathlines` appears in the preset list next to the image's own
`streamlines` / `large-cloud` / `fast-morph` / `mesh`:

```sh
docker exec pv-desktop jusync-preset              # list presets
docker exec pv-desktop jusync-preset pathlines    # launch it
```

`jusync-preset` restarts ParaView on display `:1` with the dataset and the
ANARI animation clock; watch it at <http://localhost:6080/vnc.html>
(password `jusync`).

The launcher deliberately opens the **data** (the `.pvd`), not the saved state,
so ParaView learns the full 0..4.95 s range - a `.pvsm` restores the pipeline
but not the reader's timestep list. That leaves the reader unapplied, so click
**Apply** once. Switch the representation to *Points* with a point size of ~4
if the default surface rendering makes the particles hard to see, then press
**Play** (enable looping in *View > Animation View* for continuous motion).

Check the stream with:

```sh
docker exec pv-desktop tail -f /tmp/paraview-session.log
```

### Iterating against the live /work copy

`install-preset.sh` registers this checkout's `particles/` instead of the
baked-in copy - use it only while changing the generator, so a regenerated
dataset is picked up without rebuilding the image:

```sh
docker exec pv-desktop sh /work/examples/paraview-pathlines/install-preset.sh
docker exec pv-desktop jusync-preset pathlines
```

It writes to the container's writable layer, so re-run it after
`docker compose up -d --force-recreate`; a fresh container from the same image
shows the baked-in preset again. Opening the `.pvd` by hand (File > Open, e.g.
`/opt/jusync/presets/pathlines/particles.pvd`) works too - the launcher is only
needed for the animation clock and the device environment.

## Settings on the Unreal side

On `AJUSYNCFileSpawnerActor`:

| Setting | Value | Why |
| --- | --- | --- |
| Enable Pathlines | on | off by default |
| Pathline Max Tracks | >= 256 (default 1000) | one track per particle |
| Pathline History Samples | >= 100 (default 128) | a full lap is 100 samples |
| Pathline Track Expiry Samples | 64 (default) | nothing ever drops out |
| Pathline Radius | ~0.5-1.5 | **the default 0.02 is invisible here**: the dataset spans +-14 units and the radius is in the same space |
| Pathline Tube Sides | 4-6 | 4 is enough at this radius |
| Pathline Id Attribute | `particle_id` | default |
| Pathline Scalar Attribute | `attribute0` | default, component 0 |
| Pathline Color By Scalar | on, range 0-1 | `attribute0` spans exactly 0..1 |

Then the filters are easy to eyeball:

* **Range filter 0.4-0.6** keeps the middle rings (`attribute0` is ring-major,
  half its value from the ring index, half from the position within the ring).
* **Hue filter 30 +- 30** keeps the tracks whose resolved colour falls in that
  band, independently of the range filter.

## What was verified

* `jusync-frame-index` indexes all 100 frames, 256 points each, 0 skipped -
  the launcher's own parser accepts the files.
* `particle_id` is identical in every frame (0..255), `attribute0` is constant
  per id, no duplicate or non-finite points.
* Largest per-frame displacement is 1.01 units, so the tracks are smooth at
  the default tube radius.
* `jusync-anim-dt` reports `dt = 0.05` over 100 steps, so ParaView's Time field
  and Unreal's sample times stay aligned (see the `libpvtime` shim in
  `docker/pv-anari-usd.Dockerfile`).
* With the dataset applied, the device publishes it as expected:
  `[JusyncAnim] sample+ 'vtk_actor_46' t=4.95 pts=256` (see
  `/tmp/paraview-session.log`).

## Notes

The shipped `large-cloud` and `fast-morph` presets are also point clouds with
`particle_id`, but at 1M / 30k points they exceed the default 1000-track cap -
UE samples a stable subset. This example stays under the cap so every particle
becomes a pathline, which makes dropped or duplicated tracks obvious.
