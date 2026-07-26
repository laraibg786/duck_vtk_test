# Test Data Corpus — `duck_vtk`

**Total:** 79 files, 17 MB, committed to the repository under `test/data/`.
**Integrity manifest:** `test/data/MANIFEST.sha256` (verify with `cd test/data && sha256sum -c MANIFEST.sha256`).

## 1. Provenance and licensing

Files were obtained from Kitware's publicly published VTK example/test data, primarily:

- `https://raw.githubusercontent.com/Kitware/vtk-examples/master/src/Testing/Data/...` — the data backing <https://examples.vtk.org/>. These are **real files**, not ExternalData stubs.
- `https://github.com/pyvista/vtk-data/raw/master/Data/...` — a mirror carrying real (non-stub) blobs.
- `https://www.vtk.org/files/ExternalData/SHA512/<hash>` — content-addressed blobs.

VTK and its data are distributed under the **BSD 3-Clause** licence, which permits redistribution. The `synthetic/` files were written by hand for this project and carry the project's own licence.

**A caveat worth stating plainly:** these files were collected by an automated agent across several sources, and per-file provenance was not recorded individually before that agent was terminated. The set has been verified to be *genuine VTK data* (see §2), but if this project is ever published, per-file attribution should be re-established before redistribution. That is a licensing-hygiene task, not a technical blocker.

### 1.1 The ExternalData stub trap

`gitlab.kitware.com/vtk/vtk-data` stores most files as **ExternalData content links**: a `.vtu` in a clone may be a 128-character SHA-512 hash in a text file, not mesh data. A test suite pointed at such a file sees a parse failure, or worse, an apparently empty dataset. This corpus deliberately avoided that repo in favour of sources carrying real blobs, and §2's verification exists to catch any that slipped through. If you add files later, check them the same way.

## 2. Integrity verification performed

Every one of the 79 files was checked for:

- **HTML error pages** masquerading as data (`<html`, `<!DOCTYPE`, `404`, `Not Found` in the first 100 bytes) — none found.
- **Git-LFS pointer stubs** (`version https://git-lfs...`) — none found.
- **Implausibly small size** (< 60 bytes) — none found.
- **Legacy VTK magic header**: all 23 `legacy/*.vtk` files begin `# vtk DataFile Version <n>`.

Result: **all 79 files are real data.**

**What was NOT done, and must be before test expectations are trusted:** no Python-VTK oracle run. `brew install vtk` had not completed and the `.venv` was never created, so numeric ground truth below was derived by **reading the ASCII files directly against the VTK file-format specification** — which is a legitimate independent oracle for ASCII files, but covers only those. Per `docs/design/02-validation-and-testing.md` §1, every binary/compressed file's ground truth is **UNVERIFIED** and must be established via `scripts/validate_against_vtk.py` before any expectation derived from it is committed. Do not write a sqllogictest expectation for a binary file until that harness runs.

## 3. Inventory

### 3.1 Tier 1 — the CAE validation core

| Path | Format | Dataset | Size | Why it matters |
|---|---|---|---|---|
| `legacy/uGridEx.vtk` | legacy ASCII | UnstructuredGrid | 1012 B | Canonical mixed-cell-type example; point data only (**no cell data**) |
| `legacy/VTKCellTypes.vtk` | legacy ASCII | UnstructuredGrid | 1127 B | 11 distinct cell types, **and both point *and* cell data** |
| `legacy/tensors.vtk` | legacy ASCII | UnstructuredGrid | 895 B | Multi-component tensor arrays |
| `legacy/office.binary.vtk` | legacy **binary** | StructuredGrid | 235 KB | Binary path + endianness |
| `legacy/blow.vtk` | legacy ASCII | UnstructuredGrid | 307 KB | Real FEA (plastic blow-moulding) with displacement vectors |
| `legacy/plate.vtk` | legacy ASCII | PolyData | 82 KB | Vibrational modes; point vectors |
| `legacy/fran_cut.vtk` | legacy ASCII | PolyData | 175 KB | Has `CELL_DATA` (4224) **and** `POINT_DATA` (2205) plus texture coords |
| `legacy/hello.vtk` | legacy ASCII | PolyData | 481 B | Tiny polydata (lines) |
| `xml/tetra.vtu` | XML ascii | UnstructuredGrid | 1471 B | Higher-order cells; **empty `<CellData>`** element |
| `xml/QuadraticTetra.vtu` | XML | UnstructuredGrid | 14 KB | Quadratic cells |
| `xml/QuadraticPyramid.vtu` | XML | UnstructuredGrid | 10 KB | Quadratic cells |
| `xml/QuadraticWedge.vtu` | XML | UnstructuredGrid | 5.7 KB | Quadratic cells |
| `xml/TriQuadraticHexahedron.vtu` | XML | UnstructuredGrid | 5.7 KB | 27-node hex |
| `xml/Disc_BiQuadraticQuads_0_0.vtu` | XML | UnstructuredGrid | 60 KB | Bi-quadratic quads |
| `xml/fire_ug.vtu` | XML | UnstructuredGrid | 632 KB | Larger realistic mesh |
| `xml/cow.vtp` | XML | PolyData | 60 KB | PolyData with normals; exercises the verts/lines/polys/strips cell-id question |
| `xml/Bunny.vtp`, `Ring.vtp`, `a.vtp`, `ObliqueCone.vtp`, `filledContours.vtp` | XML | PolyData | 3–33 KB | PolyData variants, all with both Point and Cell data sections |

**`legacy/cells/` — a complete linear cell-type sweep (16 files, 68 KB).** One tiny legacy file per cell type, giving full coverage of VTK cell types 1–16:

| File | Cell type id | File | Cell type id |
|---|---|---|---|
| `Vertex.vtk` | 1 | `Quad.vtk` | 9 |
| `PolyVertex.vtk` | 2 | `Tetra.vtk` | 10 |
| `Line.vtk` | 3 | `Voxel.vtk` | 11 |
| `PolyLine.vtk` | 4 | `Hexahedron.vtk` | 12 |
| `Triangle.vtk` | 5 | `Wedge.vtk` | 13 |
| `TriangleStrip.vtk` | 6 | `Pyramid.vtk` | 14 |
| `Polygon.vtk` | 7 | `PentagonalPrism.vtk` | 15 |
| `Pixel.vtk` | 8 | `HexagonalPrism.vtk` | 16 |

This set is the natural driver for `test/cpp/test_cell_types.cpp` and for an end-to-end loop asserting `cells.cell_type` and `cell_type_name` for every type.

### 3.2 Tier 2 — extensibility

| Path | Format | Size | Status |
|---|---|---|---|
| `xml/vase.vti` | ImageData | 146 KB | Implicit geometry — coordinate synthesis |
| `xml/RectilinearGrid.vtr` | RectilinearGrid | 1.0 MB | Per-axis coordinate arrays |
| `xml/StructuredGrid.vts` | StructuredGrid | 20 KB | Explicit points + extent |
| `legacy/StructuredPoints.vtk` | legacy STRUCTURED_POINTS | 315 KB | |
| `legacy/SampleStructGrid.vtk` | legacy STRUCTURED_GRID | 1.1 MB | |
| `legacy/ironProt.vtk` | legacy STRUCTURED_POINTS | 315 KB | Volume data |
| `legacy/hexa.vtk` | legacy | 931 KB | AMR-like mesh |
| `legacy/financial.vtk` | legacy | 140 KB | UNSTRUCTURED_GRID with many named field arrays |
| `parallel/blood_vessels/T0000000500.pvtu` + 4 pieces | parallel XML | 4.8 MB | **Complete** — all 4 `Source=` pieces present and usable |
| `exodus/disk_out_ref.ex2` | ExodusII | 704 KB | Real CAE/FEA format |
| `exodus/mug.e` | ExodusII | 1.9 MB | Real CAE/FEA format |
| `cgns/sqnz_s.adf.cgns` | CGNS | 1.0 MB | CFD standard |
| `hdf/warping_spheres.vtkhdf` | VTKHDF | 3.1 MB | Newest VTK format |
| `legacy/{DEC,GE,GM,IBM,k,t,v,teeth,texThres,polyline}.vtk` | legacy | 0.1–8 KB | Assorted small polydata |

**Incomplete composite fixtures — read this before writing tests against them:**

| Path | Problem |
|---|---|
| `multiblock/many_blocks.vtm` | References `many_blocks/many_blocks_*.vtp`; **sub-directory not downloaded** |
| `multiblock/ex-blow_5.vtm` | Sub-files missing |
| `multiblock/chombo3d.vtm` | Sub-files missing |
| `timeseries/singleSphereAnimation.pvd` | References `singleSphereAnimation/*.vtp`; **sub-files missing** |

These are **still useful in Phase 1**, but only as fixtures asserting that a multiblock/time-series file is rejected with a clear `NotImplementedException` (design §8) — not as data-reading tests. Before Phase 4 they must be re-fetched complete, or replaced by files generated locally with the Python VTK writer (which is the more robust option, since it removes the external dependency entirely).

### 3.3 Tier 3 — synthetic negative and edge-case fixtures

Written by hand for this project. Each targets a specific row of `docs/design/01-relational-schema.md` §8.

| Path | Construction | Failure mode tested |
|---|---|---|
| `synthetic/empty.vtk` | `UNSTRUCTURED_GRID` with `POINTS 0`, `CELLS 0 0`, `CELL_TYPES 0` | All six tables must exist and return 0 rows; `vtk_info` returns 1 row with zero counts and **NULL bounds** (not VTK's `±VTK_DOUBLE_MAX` sentinel). Must **not** be an error |
| `synthetic/points_only.vtk` | 4 points, 0 cells, one point scalar `temperature` = 10.5/20.5/30.5/40.5 | `cells` and `cell_points` return 0 rows while `points` returns 4 |
| `synthetic/awkward_names.vtk` | 3 points, 1 triangle; point arrays named `Pressure_Pa`, **`x`**, `Temperature`, `temperature` | Name-collision policy (design §6): `x` collides with the reserved coordinate column → becomes `x_1`; `Temperature`/`temperature` are ambiguous under DuckDB's case-insensitive unquoted resolution → the later is suffixed. `vtk_arrays.name` keeps originals, `column_name` reports the mangled result |
| `synthetic/nan_inf.vtk` | 4 points, point scalar `value` = `1.5 nan inf -inf` | NaN/±Inf must pass through as DOUBLE values, **not** be converted to NULL |
| `synthetic/truncated.vtk` | First 120 bytes of `legacy/uGridEx.vtk` | Must raise `IOException` at `ATTACH`, naming the file. Must **never** be reported as an empty mesh |
| `synthetic/not_really.vtk` | Plain prose with a `.vtk` extension | Reader dispatch must be **content**-based; must be rejected |
| `synthetic/bad_type.vtu` | Well-formed XML, `type="NotARealDataSetType"` | Valid XML but unknown dataset type → clear error, not a crash |

## 4. Ground truth (independently derived)

Derived by reading the ASCII files against the VTK legacy/XML format specification — **not** by running `duck_vtk`. Values here are exact and may be used directly as sqllogictest expectations.

### 4.1 `legacy/uGridEx.vtk`

```
dataset_class   vtkUnstructuredGrid
num_points      27
num_cells       12
bounds          x: 0.0 .. 2.0   y: 0.0 .. 1.0   z: 0.0 .. 6.0
```

Points are a 3×2 grid at z=0 and z=1 (ids 0–11), then a column of 3-point rows at y=1 for z=2..6 (ids 12–26). `point_id` 0 = `(0,0,0)`; `point_id` 26 = `(2,1,6)`.

Cell types, in `cell_id` order:

| cell_id | cell_type | cell_type_name | num_points | point_ids |
|---|---|---|---|---|
| 0 | 12 | `VTK_HEXAHEDRON` | 8 | [0,1,4,3,6,7,10,9] |
| 1 | 12 | `VTK_HEXAHEDRON` | 8 | [1,2,5,4,7,8,11,10] |
| 2 | 10 | `VTK_TETRA` | 4 | [7,10,9,12] |
| 3 | 10 | `VTK_TETRA` | 4 | [8,11,10,14] |
| 4 | 7 | `VTK_POLYGON` | 6 | [15,16,17,14,13,12] |
| 5 | 6 | `VTK_TRIANGLE_STRIP` | 6 | [18,15,19,16,20,17] |
| 6 | 9 | `VTK_QUAD` | 4 | [22,23,20,19] |
| 7 | 5 | `VTK_TRIANGLE` | 3 | [21,22,18] |
| 8 | 5 | `VTK_TRIANGLE` | 3 | [22,19,18] |
| 9 | 3 | `VTK_LINE` | 2 | [23,26] |
| 10 | 3 | `VTK_LINE` | 2 | [21,24] |
| 11 | 1 | `VTK_VERTEX` | 1 | [25] |

Derived invariants:
- `sum(num_points)` = 8+8+4+4+6+6+4+3+3+2+2+1 = **51** → `count(*) FROM cell_points` = 51
- The legacy `CELLS 12 63` size field = `sum(num_points + 1)` = 63 ✓ (a self-check on the parse)

Arrays:

| association | name | vtk type | ncomp | ntuples | values |
|---|---|---|---|---|---|
| POINT | `scalars` | float | 1 | 27 | `0.0, 1.0, … 26.0` (exactly `point_id`) |
| POINT | `vectors` | float | 3 | 27 | ids 0–11 cycle `[1,0,0], [1,1,0], [0,2,0]`; ids 12–26 all `[0,0,1]` |
| CELL | *(none)* | | | | **This file has no `CELL_DATA`** |

`vtk_arrays` must therefore return exactly 2 rows, both `association='POINT'`, and `cells` must have exactly its 5 reserved columns and no array columns. That makes this file the natural fixture for design §8's "cells but no cell data" row.

Active attributes: `scalars` is active `SCALARS`; `vectors` is active `VECTORS`.

### 4.2 `legacy/VTKCellTypes.vtk`

```
dataset_class   vtkUnstructuredGrid
num_points      27
num_cells       11
bounds          x: 0.0 .. 2.0   y: 0.0 .. 1.0   z: 0.0 .. 6.0
```

| cell_id | cell_type | cell_type_name | num_points | point_ids |
|---|---|---|---|---|
| 0 | 12 | `VTK_HEXAHEDRON` | 8 | [0,1,4,3,6,7,10,9] |
| 1 | 11 | `VTK_VOXEL` | 8 | [1,2,4,5,7,8,10,11] |
| 2 | 10 | `VTK_TETRA` | 4 | [6,10,9,12] |
| 3 | 8 | `VTK_PIXEL` | 4 | [11,14,10,13] |
| 4 | 7 | `VTK_POLYGON` | 6 | [15,16,17,14,13,12] |
| 5 | 6 | `VTK_TRIANGLE_STRIP` | 6 | [18,15,19,16,20,17] |
| 6 | 9 | `VTK_QUAD` | 4 | [22,23,20,19] |
| 7 | 5 | `VTK_TRIANGLE` | 3 | [21,22,18] |
| 8 | 4 | `VTK_POLY_LINE` | 3 | [22,19,18] |
| 9 | 3 | `VTK_LINE` | 2 | [26,25] |
| 10 | 1 | `VTK_VERTEX` | 1 | [24] |

- `sum(num_points)` = 8+8+4+4+6+6+4+3+3+2+1 = **49** → `count(*) FROM cell_points` = 49
- `CELLS 11 60` size field = 60 ✓

Arrays:

| association | name | vtk type | ncomp | ntuples | values |
|---|---|---|---|---|---|
| POINT | `scalars` | float | 1 | 27 | `0.0 … 26.0` |
| POINT | `vectors` | float | 3 | 27 | as in §4.1 |
| CELL | `scalars` | float | 1 | 11 | `0.0 … 10.0` (exactly `cell_id`) |

**This is the most valuable single fixture in the corpus**: 11 distinct cell types, and a point array and a cell array **with the same name `scalars`**. That name reuse is legal and must not trigger the collision policy, because the two arrays land in different tables — a good test that `association` is tracked rather than names being globally deduplicated.

Note the file also contains a trailing `LOOKUP_TABLE CellColors 11` block (11 RGBA rows). Lookup tables are colour-map metadata, **not** field data; VTK does not surface them via `GetCellData()`. `vtk_arrays` must therefore report exactly 3 rows, not 4. Verify this — if VTK does expose it, the expectation changes, and this is precisely the kind of thing the oracle run must settle.

### 4.3 `xml/tetra.vtu`

```
dataset_class   vtkUnstructuredGrid
num_points      22
num_cells       3
compressor      vtkZLibDataCompressor (declared, but arrays are format="ascii" so uncompressed)
```

| cell_id | cell_type | cell_type_name | num_points | point_ids |
|---|---|---|---|---|
| 0 | 24 | `VTK_QUADRATIC_TETRA` | 10 | [0,1,2,3,4,5,6,7,8,9] |
| 1 | 22 | `VTK_QUADRATIC_TRIANGLE` | 6 | [10,11,12,13,14,15] |
| 2 | 22 | `VTK_QUADRATIC_TRIANGLE` | 6 | [16,17,18,19,20,21] |

XML `offsets` are `10 16 22` (cumulative ends), so `num_points` = successive differences — a good check that the modern `vtkCellArray` offsets representation is handled, not the legacy inline-count format.

- `sum(num_points)` = 22 → `count(*) FROM cell_points` = 22
- `point_id` 0 = `(0,0,0)`; `point_id` 4 = `(0.5, 0, -0.2)`; `point_id` 21 = `(1, 0, -0.75)`
- bounds: x: 0.0 .. 3.0, y: -0.5 .. 0.8, z: -2.0 .. 1.0

Arrays:

| association | name | vtk type | ncomp | ntuples | values |
|---|---|---|---|---|---|
| POINT | `scalars` | Float32 | 1 | 22 | `1 1 1 1 0 0 0 0 0 0 1 1 1 0 0 0 1 1 1 0 0 0` |
| CELL | *(none)* | | | | `<CellData>` is present but **empty** |

The empty-but-present `<CellData>` element is deliberately covered: a naive implementation may create a spurious column or crash on a zero-array container.

### 4.4 Files whose ground truth is UNVERIFIED

Everything not in §4.1–4.3. Specifically all binary legacy files, all compressed/appended XML files, the `.pvtu`, `.ex2`, `.cgns`, `.vtkhdf`, `.vtm`, `.pvd`, and all XML files not read by hand. Establish these with `scripts/validate_against_vtk.py` before committing expectations. Mark any test you cannot back with an oracle as `mode skip` with a stated reason rather than guessing a value.

## 5. Re-fetching

The corpus is **committed to the repository** rather than fetched by a script. This was a deliberate change from the original plan: a fetch script depending on live third-party URLs is a test suite that breaks when someone else's server moves, and 17 MB is a reasonable price for reproducibility. `MANIFEST.sha256` detects corruption or accidental modification:

```bash
cd test/data && sha256sum -c MANIFEST.sha256
```

If files are ever regenerated or extended, prefer **generating** fixtures locally with the Python VTK writer over downloading them. That gives exact known-by-construction ground truth, needs no network, and is the right way to fill the Tier-2 gaps (complete multiblock and time-series sets).

## 6. Not obtained

| Wanted | Status |
|---|---|
| Complete multiblock `.vtm` with sub-files | Sub-directories not downloaded; generate locally instead |
| Complete `.pvd` time series with sub-files | Same |
| OpenFOAM case directory | Not attempted (directory-based format, awkward as a fixture) |
| EnSight Gold `.case` | Not obtained |
| PLOT3D `.bin` pair | Not obtained (`combxyz.bin`/`combq.bin` were candidates) |
| Files with `vtkStringArray` point/cell data | **Not obtained, and worth creating synthetically** — the `GetArray()`-returns-null-for-string-arrays trap (research doc 03 §4.2) is untested without one |
| Files with 64-bit integer arrays holding values above 2^53 | **Not obtained; must be created synthetically.** This is the highest-value missing fixture, since it is the one that catches the `GetComponent` precision bug end to end. The C++ unit test covers it in isolation, but an end-to-end fixture is also warranted |
