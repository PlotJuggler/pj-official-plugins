# 3D Loader (PLY / PCD)

Loads a **static** `.ply` or `.pcd` 3D file as a single object at timestamp 0.
Unlike the streaming/log loaders, there is no time axis — the file is one
snapshot.

## What loads as what

| Input | Becomes |
|-------|---------|
| `.pcd` (any encoding) | `sdk::PointCloud` |
| `.ply` with only vertices | `sdk::PointCloud` |
| `.ply` with faces | `sdk::Mesh3D` (raw asset bytes; the renderer/Assimp parses the geometry) |

The plugin hand-rolls the PLY/PCD **header** parsing and packs points itself; it
never links a heavy point-cloud library. A file with faces is handed to the
renderer verbatim as a `Mesh3D` — the plugin does not parse mesh geometry.

## Supported encodings

- **PCD** `DATA`: `ascii`, `binary`, and `binary_compressed` (LZF, via vendored
  [liblzf](contrib/liblzf/) 3.6, BSD-2-Clause). `TYPE`/`SIZE` map to the canonical
  `PointField` datatypes; `COUNT > 1` fields are preserved.
- **PLY** `format`: `ascii`, `binary_little_endian`, `binary_big_endian`
  (big-endian vertex data is byte-swapped to the little-endian packed layout).

All readers are **total functions over untrusted bytes**: malformed input returns
an error, it never throws on a parse failure, and `binary_compressed` validates
the declared uncompressed size against `width*height*point_step` **before**
allocating (decompression-bomb guard).

## What the host receives

For a file `foo.pcd` / `foo.ply` the loader emits:

- **`foo`** — an ObjectStore topic carrying the serialized `PointCloud` / `Mesh3D`
  at t=0, tagged with `{"builtin_object_type":"kPointCloud"}` (or `kMesh3D`) so
  the 3D scene classifies and renders it. Falls back to summary-only (with a
  warning) if the host binds no object-write service.
- **`foo/summary`** — scalar fields at t=0: `num_points` (clouds), `num_faces`
  (meshes), and `centroid/x,y,z` (clouds with x/y/z fields).

## Limitations

- A PCD `VIEWPOINT` (sensor pose) is **ignored** — points are delivered in their
  stored frame.
- One object per file; no folder/sequence loading.

## Build & test

```bash
./build.sh data_load_3d
ctest --test-dir build/data_load_3d/Release
```

The reader core (`cloud_common`, `pcd_reader`, `ply_reader`) is pure and unit
tested via serialize/deserialize round-trips; `loader_3d.cpp` is the thin
`FileSourceBase` glue that does file I/O and the host calls.
