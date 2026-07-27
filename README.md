# duck_vtk

Query VTK mesh files as relational tables in DuckDB.

```sql
LOAD vtk;
ATTACH 'wing.vtu' AS mesh (TYPE vtk);

-- What is in this file?
SELECT association, name, sql_type, num_components, num_tuples FROM mesh.vtk_arrays;

-- Cell-type histogram with a cell-data field
SELECT cell_type_name, count(*) AS cells, avg(pressure) FROM mesh.cells GROUP BY 1 ORDER BY 2 DESC;

-- Cell centroids, joining connectivity to coordinates
SELECT cp.cell_id, avg(p.x) cx, avg(p.y) cy, avg(p.z) cz
FROM mesh.cell_points cp JOIN mesh.points p USING (point_id)
GROUP BY cp.cell_id;
```

File parsing is done by the **official VTK C++ library** (Kitware), not a hand-rolled parser — so legacy/XML, ascii/binary/appended/compressed, endianness and the long tail of real-world CAE files are Kitware's problem, not ours.

---

## Quick start

```bash
git clone <this repo> && cd duck_vtk    # plain clone is fine; --recursive not needed
make release                            # fetches what it needs, then builds
make install                            # so `LOAD vtk;` works with no path
duckdb -unsigned                        # locally built extensions are unsigned
```

`make release` bootstraps its own dependencies, so a **non-recursive clone builds
with one command**. `make configure` does the same setup explicitly (plus the test
oracle) and is idempotent, but is not required.

A plain clone is 26 MB and leaves `duckdb/` and `extension-ci-tools/` empty — the
submodules cost 218 bytes of metadata and are only materialised when the build
needs them. They exist because the DuckDB community-extensions CI requires a
`duckdb` submodule (it runs `cd duckdb && git checkout <version>`).

## Requirements

| | |
|---|---|
| OS | Linux x86_64 (macOS should work; not yet tested) |
| Compiler | GCC ≥ 11 or Clang ≥ 14, C++17. Verified with GCC 14.2 |
| Build tools | CMake ≥ 3.16, Ninja (optional but much faster), ccache (optional, big win) |
| VTK | ≥ **9.6**, and the build enforces it. `make configure` builds a minimal 9.6.2 if none is found. **Ubuntu 24.04's `libvtk9-dev` (9.1) does not work** — it fails to parse any XML file with an `<AppendedData>` section, leaves `GetErrorCode()` at Success, and hands back an empty mesh. See `cmake/DuckVTKFindVTK.cmake` |
| DuckDB | Built from the pinned submodule. **Verified against 1.5.5 and 1.4.5 (LTS)** — full suite green on both |

Override the compiler the normal CMake way:

```bash
make release CC=gcc-13 CXX=g++-13
make release CC=clang CXX=clang++
```

### DuckDB version

The extension's metadata footer records the DuckDB version it was built for, and `LOAD` refuses a mismatch. Keep these three in step:

```bash
make check-pin     # verifies submodule SHA, DUCKDB_VERSION_TAG, and the installed CLI agree
```

To build against a different DuckDB, point `DUCKDB_SRCDIR` at its source tree —
the Makefile notices the change and re-configures, so no manual `make clean`:

```bash
curl -fSL -o duckdb-1.4.5.tar.gz \
  https://codeload.github.com/duckdb/duckdb/tar.gz/refs/tags/v1.4.5
mkdir -p /tmp/duckdb-1.4.5 && tar xzf duckdb-1.4.5.tar.gz -C /tmp/duckdb-1.4.5 --strip-components=1
make release DUCKDB_SRCDIR=/tmp/duckdb-1.4.5/ DUCKDB_VERSION_TAG=v1.4.5
```

A source tarball is far quicker than `git fetch` here (~50 MB vs ~425 MB of git
objects). To check the version shim without a full build:

```bash
make check-api-compat EXTRA_DUCKDB_SRC=/tmp/duckdb-1.4.5
```

The one API that differs between 1.4 and 1.5 is storage-extension registration. `CMakeLists.txt` detects it by **reading the actual DuckDB header** for the `StorageExtension::Register` symbol rather than parsing a version string, so nightlies and forks work too.

### VTK

Two supported ways, in order of preference:

```bash
./scripts/build_minimal_vtk.sh          # recommended: ABI-matched to your compiler
brew install vtk                        # large: pulls Qt/mesa/llvm (~2.8 GB)
```

`apt install libvtk9-dev` is **not** a supported path: Debian/Ubuntu ship 9.1-9.3,
below the enforced 9.6 floor, and `find_package` will reject them.

Point at a specific one with `make release VTK_DIR=/path/to/lib/cmake/vtk-9.6`.

The source build is recommended for a real reason: compiling VTK with the *same* compiler that builds the extension eliminates the `libstdc++` ABI mismatch risk that a prebuilt binary carries, rather than merely testing for it.

## The six tables

`ATTACH` exposes exactly six tables in schema `main`. The attached database is **always read-only**.

### `points` — one row per point

| Column | Type | Notes |
|---|---|---|
| `point_id` | `BIGINT` | 0-based, dense |
| `x`, `y`, `z` | `DOUBLE` | Always `DOUBLE`, even when the file stores `float`. Synthesised for implicit-geometry datasets |
| *one per point array* | see below | Named exactly as in the file |

### `cells` — one row per cell

| Column | Type | Notes |
|---|---|---|
| `cell_id` | `BIGINT` | 0-based |
| `cell_type` | `INTEGER` | Numeric `VTK_*` id |
| `cell_type_name` | `VARCHAR` | e.g. `VTK_HEXAHEDRON` |
| `num_points` | `INTEGER` | `len(point_ids)`, denormalised so `WHERE num_points = 8` avoids materialising the list |
| `point_ids` | `BIGINT[]` | Connectivity in VTK's canonical vertex order — **never sorted**, because that order defines face/normal orientation |
| *one per cell array* | see below | |

### `cell_points` — one row per (cell, vertex slot)

`cell_id`, `vertex_index`, `point_id`. The normalised connectivity, so cell↔point joins do not need `unnest` boilerplate. Row count is `sum(num_points)` — 8× the cell count for a hex mesh.

### `field_data` — dataset-level metadata, long form

`array_name`, `tuple_index`, `component_index`, `component_name`, `value_double`, `value_varchar`. Long-form because dataset field data has no per-point or per-cell grain. `value_varchar` is always populated; `value_double` is NULL for string arrays.

### `vtk_arrays` — the schema catalogue

One row per array: `association` (`POINT`/`CELL`/`FIELD`), `array_index`, `name`, `column_name`, `vtk_type`, `vtk_type_name`, `sql_type`, `num_components`, `num_tuples`, `component_names`, `active_as`. **Start here** on an unfamiliar file.

### `vtk_info` — exactly one row

`file_path`, `file_size_bytes`, `reader_class`, `dataset_class`, `num_points`, `num_cells`, `num_point_arrays`, `num_cell_arrays`, `num_field_arrays`, `bounds_{x,y,z}_{min,max}`, `vtk_version`, `extension_version`. Bounds are NULL for an empty dataset.

## Table functions

The same engine is reachable without `ATTACH`:

```sql
SELECT * FROM vtk_points('mesh.vtu');
SELECT * FROM vtk_cells('mesh.vtu');
SELECT * FROM vtk_cell_points('mesh.vtu');
SELECT * FROM vtk_field_data('mesh.vtu');
SELECT * FROM vtk_arrays('mesh.vtu');
SELECT * FROM vtk_info('mesh.vtu');
SELECT * FROM vtk_debug_dump('mesh.vtu');   -- raw VTK view, for diagnosing a wrong value
```

`ATTACH` and these share one scan implementation, so they cannot disagree. Scalar functions: `vtk_version()`, `vtk_build_info()`.

## Type mapping

| VTK | DuckDB |
|---|---|
| `VTK_BIT` | `BOOLEAN` |
| `VTK_CHAR` / `VTK_SIGNED_CHAR` | `TINYINT` |
| `VTK_UNSIGNED_CHAR` | `UTINYINT` |
| `VTK_SHORT` / `VTK_UNSIGNED_SHORT` | `SMALLINT` / `USMALLINT` |
| `VTK_INT` / `VTK_UNSIGNED_INT` | `INTEGER` / `UINTEGER` |
| `VTK_LONG`, `VTK_LONG_LONG`, `VTK_ID_TYPE` | `BIGINT` |
| `VTK_UNSIGNED_LONG`, `VTK_UNSIGNED_LONG_LONG` | `UBIGINT` |
| `VTK_FLOAT` / `VTK_DOUBLE` | `FLOAT` / `DOUBLE` |
| `VTK_STRING` (`vtkStringArray`) | `VARCHAR` |

**Multi-component arrays become `LIST` columns** — `velocity DOUBLE[]`, not three columns. One code path covers 3-vectors, 9-component tensors and arbitrary N. Use `velocity[1]` (1-based), `unnest`, or `list_*`. A 1-component array is always a scalar column, never a 1-element list.

64-bit integer arrays are read through typed accessors, never through `vtkDataArray::GetComponent` (which returns `double` and silently corrupts values above 2^53).

### Column names

Array names are used **verbatim**, including spaces, dots, unicode and mixed case — quote them: `SELECT "Pressure [Pa]" FROM m.points`. Collisions are resolved by suffixing `_1`, `_2`:

- clash with a reserved column (`x` → `x_1`)
- duplicate names
- case-insensitive ambiguity (`Temperature` / `temperature` → the latter becomes `temperature_1`), because DuckDB resolves unquoted identifiers case-insensitively

`vtk_arrays.name` keeps the original; `vtk_arrays.column_name` gives the usable one.

## Supported formats

**Working:** legacy `.vtk` (ascii + binary; UnstructuredGrid, PolyData, StructuredGrid, StructuredPoints, RectilinearGrid, and field-only `DATASET FIELD`), XML `.vtu` `.vtp` `.vts` `.vtr` `.vti` (ascii, binary, appended, compressed). Higher-order/quadratic/Lagrange/Bézier cells all read.

**Rejected with a clear error, not yet supported:** multiblock `.vtm`, time series `.pvd`, parallel `.pvtu`, and the CAE importers (ExodusII, CGNS, VTKHDF, EnSight, OpenFOAM). The last group needs VTK built with those optional modules; `cmake/DuckVTKFindVTK.cmake` probes for them and degrades gracefully.

## Limitations (deliberate, documented)

- **Whole file in RAM.** `ATTACH` reads the entire dataset. Not suitable for out-of-core work yet.
- **Single-threaded scans.** `MaxThreads()` returns 1. State is kept so a parallel version needs only a work-range split.
- **Data converted per scan**, not cached.
- **No filter pushdown.** Projection pushdown *is* implemented.
- **`cell_points` positions rows by walking cells** from the start of each chunk.
- **Read-only.** Writing VTK files is a separate problem.
- **Not a self-contained binary**: it links VTK's shared libraries via RPATH.

## Testing

```bash
make test            # sqllogictest — 13 files, 328 assertions
make invariants      # 15 properties × every corpus file
make oracle          # elementwise diff against an independent Python VTK
make smoke           # build + load, including into the SYSTEM duckdb
make python-smoke    # via the DuckDB Python client
make check           # everything
make check-api-compat EXTRA_DUCKDB_SRC=/tmp/duckdb-1.4.5   # version shim, in seconds
```

Verified on **DuckDB 1.5.5 and 1.4.5 (LTS)**: 328 sqllogictest assertions across 13
files, and 15 invariants over 66 corpus files (2 skipped), pass on both. Re-running
the SQL suite through the in-memory parse path used for remote reads
(`make test-memory-reads`) yields 784 assertions and must agree elementwise. Building against LTS is what caught a
`CREATE INDEX` crash that 1.5.x masks — see the `BindCreateIndex` override.

Correctness is established against **independent ground truth**, never against the extension itself: a Python VTK build that shares no code with the C++ one, plus hand-derived values for three ASCII fixtures (see `docs/research/04-test-data-corpus.md` §4). The 82-file corpus is committed with a checksum manifest (`test/data/MANIFEST.sha256`, verified by `make data`).

The standout invariant cross-checks SQL-computed bounds against VTK's independent `GetBounds`, which catches x/y/z transposition that no spot check on a symmetric mesh would find.

## Submitting to DuckDB community extensions

The repo is structured for submission. `community-extension/description.yml` is
the descriptor to copy into a `duckdb/community-extensions` PR; it has two TODOs
(the GitHub repo path and maintainer handle).

```bash
make submit-check     # everything that must hold before submitting
make ci-verify        # cold-boot build in Docker, no local state involved
```

What compliance required, and how it is handled:

| Their requirement | Here |
|---|---|
| `description.yml` in their repo under `extensions/vtk/` | `community-extension/description.yml` |
| `duckdb` git submodule (their CI runs `cd duckdb && git checkout <ver>`) | present; empty on a plain clone |
| Dependencies via **vcpkg**, nothing assumed on the runner | `vcpkg.json` + `vcpkg_ports/vtk-minimal` |
| Build via `make <build_type>` at repo root | our `Makefile` |
| DuckDB v1.5.5 (default) and v1.4.5 (Andium/LTS) | both compile; `make check-api-compat` proves the version shim |

**VTK comes from our own vcpkg overlay port**, `vcpkg_ports/vtk-minimal` (VTK
9.6.2, the current stable release). vcpkg's official `vtk` port is unusable for a
database extension on two counts: its portfile **hardcodes**
`VTK_GROUP_ENABLE_Rendering=YES` with no feature to turn it off, pulling 26 base
manifest entries including glew, freetype and gl2ps — hours of CI per platform for
an OpenGL stack a SQL engine never calls — and it is pinned at
**`9.3.0-pv5.12.1`**, a ParaView fork below our 9.6 floor. (Qt is an opt-in
*feature* there rather than a default dependency; an earlier version of this note
said otherwise.)

The overlay disables rendering/Views/Web/imaging/MPI/Python/testing using upstream
VTK's own CMake options, with **no source patches**, so upgrading VTK is a version
and hash change rather than a patch to re-base. The dependency closure drops from
26 to 3 (`expat`, `lz4`, `zlib`).

### Platform scope

**Claimed:** `linux_amd64`, `linux_arm64`, `osx_amd64`, `osx_arm64`,
`windows_amd64`. All five build green through the same reusable workflow
community-extensions uses.

**Excluded** (6 of 11), each for a stated reason:

| Excluded | Why |
|---|---|
| `wasm_mvp`, `wasm_eh`, `wasm_threads` | VTK does not build under Emscripten. Permanent |
| `linux_amd64_musl`, `linux_arm64_musl` | No static-musl VTK validated. A no-op on the v1.5 matrix (opt-in there) but load-bearing on v1.4, where `linux_amd64_musl` builds unless named |
| `windows_amd64_mingw` | **DuckDB's own code**, not ours: `tools/sqlite3_api_wrapper` fails under rtools42 mingw with an ambiguous `byte` from `objidl.h`. `h5db` and the core `iceberg` extension exclude it for the same reason |

Note `linux_arm64` is **built but not tested** upstream — both test steps carry
`if: matrix.duckdb_arch != 'linux_arm64'` because arm64 runs under emulation. A
green arm64 means it compiled and linked, not that the suite passed.

`excluded_platforms` in `description.yml` and `exclude_archs` in
`.github/workflows/MainDistributionPipeline.yml` must stay in step;
`make submit-check` enforces it.

## Documentation

| Document | Contents |
|---|---|
| `docs/ROADMAP.md` | **What is missing and what it costs** — unsupported formats with blockers and effort sizes, verified against VTK's module graph |
| `docs/IMPLEMENTATION_PLAN.md` | Phase plan, verified environment facts, v1.5.x API gotchas |
| `docs/PHASE0_RESULTS.md` | Toolchain gate results and three corrected assumptions |
| `docs/design/01-relational-schema.md` | Normative table/column/type contract |
| `docs/design/02-validation-and-testing.md` | Test layers and definition of done |
| `docs/design/03-architecture-and-roadmap.md` | Layering, ownership rules, risks |
| `docs/research/01`–`05` | Source-backed DuckDB and VTK API references |

## Architecture

Four layers, strictly downward-depending:

```
L4  catalog/    VtkCatalog, VtkSchemaEntry, VtkTableEntry, VtkTransactionManager
L3  functions/  table functions — converts L2's model into DataChunks
L2  model/      type mapping, cell types, schema builder, column writer
L1  vtk/        VtkDataset (uniform accessors), VtkErrorScope
```

No VTK type reaches L4; no DuckDB type reaches L1. That is what makes the type-mapping and precision logic testable without a database.

Two rules the code upholds:

- **`vtkDataSet::GetCell()` is never called** — it returns a shared per-object scratch cell, a data race under concurrent scans. Connectivity comes from `GetCellPoints` with a thread-local `vtkIdList`.
- **`VtkDataset` is immutable after construction**, so all scans share it without locking.

### One thing worth knowing if you extend this

A **truncated VTK file is silently accepted by VTK**: `GetErrorCode()` returns `Success`, the *declared* point count is reported, and coordinates are uninitialised memory. The only signal is a `WARN` on VTK's output window. `VtkErrorScope` therefore is not a tidiness measure — it is the only mechanism that can tell a corrupt file from a valid one. See `docs/PHASE0_RESULTS.md` §4.

## Licence

MIT (this extension). VTK is BSD-3-Clause; DuckDB is MIT. The test corpus is Kitware's VTK example data (BSD-3-Clause) plus hand-written synthetic fixtures.
