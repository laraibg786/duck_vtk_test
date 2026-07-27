# duck_vtk — Architecture & Roadmap

**Status:** normative for Phase 1; indicative for later phases.

---

## 1. What this extension is

A DuckDB extension that makes VTK mesh files queryable as relational tables via `ATTACH`:

```sql
LOAD vtk;
ATTACH 'wing.vtu' AS mesh (TYPE vtk);
SELECT cell_type_name, count(*), avg(pressure) FROM mesh.cells GROUP BY 1;
```

It uses the **official VTK C++ library** (Kitware; pinned at 9.6.2, floor 9.6) for all file parsing. We do not write a VTK parser. This is a deliberate, load-bearing decision: VTK's readers handle ~20 formats, legacy and XML, ascii/binary/appended/compressed, endianness, and the long tail of real-world CAE files that hand-rolled parsers get wrong. The cost is a heavyweight dependency; the benefit is that format correctness is Kitware's problem, not ours.

## 2. Layering

Four layers, each independently testable. The dependency arrows point strictly downward — in particular **no VTK type appears in the DuckDB catalog layer, and no DuckDB type appears in the VTK reader layer**. This is what makes the type-mapping and precision logic unit-testable without a database, and it is the main structural rule of the codebase.

```
┌──────────────────────────────────────────────────────────────┐
│ L4  Catalog / ATTACH                                         │
│     VtkStorageExtension, VtkCatalog, VtkSchemaEntry,          │
│     VtkTableEntry, VtkTransactionManager                      │
│     → knows: duckdb catalog API. knows nothing about VTK.     │
├──────────────────────────────────────────────────────────────┤
│ L3  Table functions / scan                                    │
│     VtkPointsFunction, VtkCellsFunction, VtkCellPointsFunction,│
│     VtkFieldDataFunction, VtkArraysFunction, VtkInfoFunction   │
│     → converts L2's row model into DataChunks                  │
├──────────────────────────────────────────────────────────────┤
│ L2  Relational model (the bridge — the only bilingual layer)  │
│     VtkTableSchema  : column names + LogicalTypes             │
│     VtkTypeMapping  : VTK_* → LogicalType                     │
│     VtkColumnWriter : typed array → Vector, precision-correct  │
├──────────────────────────────────────────────────────────────┤
│ L1  VTK access                                                │
│     VtkReaderFactory : path → vtkSmartPointer<vtkDataObject>  │
│     VtkDataset       : owns the dataset, uniform accessors for │
│                       points/cells/arrays across dataset types │
│     VtkErrorScope    : captures vtkOutputWindow messages       │
│     → knows: VTK. knows nothing about duckdb.                  │
└──────────────────────────────────────────────────────────────┘
```

### Why this layering, concretely

- **L1 hides dataset-type variation.** `vtkUnstructuredGrid`, `vtkPolyData`, `vtkImageData`, `vtkRectilinearGrid`, and `vtkStructuredGrid` expose geometry and connectivity through genuinely different APIs (explicit points vs. origin+spacing; one `vtkCellArray` vs. four; implicit vs. explicit cells). If that variation leaks upward, every table function grows a five-way switch. `VtkDataset` normalises it once.
- **L2 is where correctness lives.** Type mapping and the int64 precision problem are pure functions of (VTK type, component count) → (LogicalType, writer). Isolating them means the highest-risk logic is covered by fast unit tests instead of end-to-end SQL.
- **L3 reuses L2 for both entry points.** The table functions are the *only* scan implementation. L4's `VtkTableEntry::GetScanFunction` returns these same functions with pre-bound data — so `SELECT * FROM mesh.points` and `SELECT * FROM vtk_points('f.vtu')` execute identical code. This is why Phase 1 builds table functions first: it is not scaffolding to be thrown away, it is the engine.
- **L4 is thin and mostly boilerplate.** Roughly 700 lines of pure-virtual overrides, most of them throwing "read-only".

## 3. Object lifetime and ownership

The one thing to get right, because it determines thread-safety:

- `ATTACH` constructs a `VtkCatalog`, which owns exactly one `shared_ptr<VtkDataset>`.
- `VtkDataset` owns the `vtkSmartPointer<vtkDataObject>` and is **immutable after construction**. It is built once, on the attaching thread, and never mutated again.
- Because it is immutable, all scans share it without locking. This is the reason the design will parallelise cleanly later: read-only shared state needs no synchronisation.
- **Caveat the implementer must respect:** VTK objects are not guaranteed thread-safe even for reads, because `vtkDataSet::GetCell` returns a *cached, per-object scratch* `vtkCell*`. Never call `GetCell` from a scan. Use the raw connectivity arrays (`vtkCellArray::GetOffsetsArray`/`GetConnectivityArray`) and `GetCellPoints(id, vtkIdList*)` with a caller-owned `vtkIdList`. This is a genuine landmine — `GetCell` looks like the obvious API and will produce data races the moment scans go parallel, and possibly wrong results even single-threaded when interleaved.
- `DETACH` drops the catalog; the `shared_ptr` releases the dataset once in-flight scans finish.

## 4. Repository layout

```
duck_vtk/
├── CMakeLists.txt              # extension build; VTK, optimisation, API probe
├── Makefile                    # configure / release / test / install / check
├── extension_config.cmake
├── cmake/
│   ├── DuckVTKFindVTK.cmake    # VTK discovery, component list, RPATH dir
│   └── DuckVTKOptimize.cmake   # compiler flags; opt-in LTO/native/asan/werror
├── duckdb/                     # submodule, pinned (see `git ls-tree HEAD duckdb`)
├── extension-ci-tools/         # submodule, pinned
├── src/
│   ├── vtk_extension.cpp                 # entrypoint + registration
│   ├── vtk/vtk_error_scope.cpp           # L1 — output-window capture (load-bearing)
│   ├── vtk/vtk_dataset.cpp               # L1 — reader factory + uniform accessors
│   ├── model/vtk_types.cpp               # L2 — type mapping + cell-type table
│   ├── model/vtk_table_schema.cpp        # L2 — six schemas + collision policy
│   ├── model/vtk_column_writer.cpp       # L2 — typed array -> Vector
│   ├── functions/vtk_table_functions.cpp # L3 — all six tables + debug dump
│   ├── catalog/vtk_catalog.cpp           # L4 — catalog/schema/table/txn manager
│   └── include/                          # mirrors the above
├── test/
│   ├── data/                   # 82-file corpus, COMMITTED, + MANIFEST.sha256
│   └── sql/*.test              # sqllogictest
├── scripts/
│   ├── configure.sh            # `make configure` — one-command setup
│   ├── install_extension.sh    # `make install`
│   ├── build_minimal_vtk.sh    # the default VTK path
│   ├── smoke.sh, run_invariants.sh, validate_against_vtk.py, python_smoke.py
│   └── phase0_spike/           # toolchain go/no-go spike
├── vcpkg.json                  # declares vtk-minimal + the overlay dirs
├── vcpkg_ports/vtk-minimal/    # our own VTK port; see §5.1
├── .github/workflows/
│   ├── MainDistributionPipeline.yml  # mirrors extension-template; calls extension-ci-tools
│   └── ci.yml                       # project-owned checks only; calls nothing upstream
└── docs/{design,research}/
```

Note the file count is lower than originally planned: the six table functions share
one translation unit because they share the scan loop, bind helper and emitters,
and the reader factory lives with `VtkDataset` because the two are tightly coupled.
Splitting either would duplicate machinery rather than separate concerns.

Header layout follows duckdb's own convention: `src/include/<subdir>/<name>.hpp`, included as `"vtk/vtk_dataset.hpp"`, with `include_directories(src/include)`.

## 5. Dependency strategy: a vcpkg overlay port, plus a minimal source build for local work

> **SUPERSEDED IN PART — read this box before believing §5.1.**
>
> This section was written before the community-extensions work and said "we use
> neither vcpkg nor Homebrew for VTK", with no `vcpkg.json` in the repo. That is no
> longer true, and it cannot be: their CI sets `VCPKG_TOOLCHAIN_PATH` and assumes
> nothing on the runner, so **every** third-party dependency must be declared in a
> manifest. The repo now has:
>
> * `vcpkg.json` — depends on `vtk-minimal`, with `./vcpkg_ports` as an overlay.
> * `vcpkg_ports/vtk-minimal/` — our own port. See §5.1 for why not the official one.
>
> What did NOT change is the *reasoning* in §5.2–§5.3, and it is worth being clear
> that the two paths coexist rather than one replacing the other:
>
> | | Used by | VTK from |
> |---|---|---|
> | Local development | `make release`, `make check` | `scripts/build_minimal_vtk.sh`, or any `-DVTK_DIR=` |
> | community-extensions CI | `MainDistributionPipeline.yml` | the `vtk-minimal` vcpkg port |
>
> Both build the **same module set from the same VTK version**, and
> `scripts/submit_check.sh` fails if the port and the script drift apart. That
> equivalence is the whole point: the fast local path and the shipping path are not
> allowed to disagree about what VTK is.

### 5.1 Why not vcpkg's official `vtk` port

Not "why not vcpkg" — that question is settled above. The official port specifically is unusable here:

- It is pinned at **`9.3.0-pv5.12.1`**, a ParaView fork three minor lines behind current VTK, and below our 9.6 floor.
- Its portfile **hardcodes** `VTK_GROUP_ENABLE_Rendering=YES` with no feature to turn it off, and carries 26 base manifest entries including `glew`, `gl2ps` and `freetype`, plus a second VTK-derived host build (`vtk-compile-tools`).
- A full-feature source build costs hours and gigabytes of build tree, with a long tail of feature-flag breakage.

`vtk-minimal` reduces the dependency closure from 26 to 3 (`expat`, `lz4`, `zlib`) using only documented upstream CMake options — **no source patches**, so a VTK bump is a version-and-hash change with no patch to re-base.

### 5.2 Why not the Homebrew bottle (revised after measurement)

Homebrew *was* the original plan, on the reasoning that a prebuilt bottle beats any source build. Measurement disproved it:

- Homebrew's VTK is built **with** rendering support, so the bottle's dependency closure includes `qtbase`, `qtdeclarative`, `gtk+3`, `mesa`, `gcc` and `llvm`. Observed cost: **~1.4 GB downloaded with a further ~1.35 GB outstanding** (`llvm` alone is 550 MB) — to link five small IO/data-model modules that need none of it.
- `ghcr.io` repeatedly failed on the large blobs with `curl (92) HTTP/2 stream 1 was not closed cleanly: PROTOCOL_ERROR`, and Homebrew's resumed downloads corrupted its own cache (a ~10 MB `proj` bottle grew to 475 MB of garbage across retries).
- Homebrew's Linux bottles are built with Homebrew's own toolchain, so linking them into an extension compiled by Debian's GCC 14 carries a **`libstdc++` ABI mismatch risk** — link errors at best, silent corruption of `std::string`/`std::vector` across the boundary at worst.

### 5.3 Why the minimal source build wins

- **54 MB of source**, versus 2.8 GB of bottles.
- **Compiled by the same compiler as the extension**, which *eliminates* the ABI risk rather than merely testing for it. This is the decisive argument: it removes the project's highest-severity risk from the register.
- No rendering, Qt, Python, MPI, or testing — the smallest module closure that still reads every serial VTK mesh format.
- Sets up Phase 5's distribution story: the same script with `-DBUILD_SHARED_LIBS=OFF` yields a statically linkable VTK, which is what a portable community extension needs.

### 5.4 Alternatives, retained as supported paths

`cmake/DuckVTKFindVTK.cmake` auto-detects several prefixes and honours `-DVTK_DIR=...`, so all of these work without editing CMake:

| Path | Command | Notes |
|---|---|---|
| Minimal source build (**default**) | `./scripts/build_minimal_vtk.sh` | No root. ABI-matched |
| vcpkg overlay port | automatic under `VCPKG_TOOLCHAIN_PATH` | What community-extensions CI uses. Static, release-only |
| Homebrew bottle | `brew install vtk` | Works if the download completes; carries the ABI risk in §5.2 |

> **CORRECTION — the Debian package row was removed.** It read "VTK 9.3, also
> ABI-matched, ~50 MB. The fastest option if you have sudo", and recommending it was
> wrong. Ubuntu 24.04's `libvtk9-dev` is **9.1**, and it cannot parse any XML file
> containing an `<AppendedData>` section: it reports the parse error to the output
> window, leaves `GetErrorCode()` at `Success`, and returns a valid but **empty**
> dataset. `cow.vtp` read as 0 points instead of 2903. Appended data is what most
> real VTK writers emit, so this is not an edge case.
>
> A CI job built against that package and passed for months while exercising a
> broken VTK — the failure is silent in exactly the way that defeats a test suite.
> The floor is now enforced at **9.6** in `cmake/DuckVTKFindVTK.cmake`, so
> `find_package` rejects 9.1/9.3 outright rather than letting them through.
>
> 9.2–9.5 are untested, not known-bad. Validating one and lowering the floor is a
> legitimate change; assuming it works is not.

### 5.5 Consequences to handle explicitly

- **A local build links VTK's shared libraries**, so a locally built `.duckdb_extension` is *not* self-contained and only loads where a compatible VTK is resolvable. **This is no longer true of the shipped artifact:** the `vtk-minimal` port builds a static, release-only VTK (`VCPKG_BUILD_TYPE release`, `BUILD_SHARED_LIBS` off for static triplets), which is what makes the community-extensions binaries portable. The distinction matters when reading the RPATH note below — that exists for the local path.
- **RPATH must be set** so the dlopen'd module finds `libvtkCommonCore-9.6.so` without the user exporting `LD_LIBRARY_PATH`. `CMakeLists.txt` sets `BUILD_RPATH`/`INSTALL_RPATH` from `DUCK_VTK_LIBRARY_DIR`; `scripts/smoke.sh` step 2 verifies it with `ldd`.
- **Phase 0 still exists and is still a gate**, even though the source build removes the ABI risk. Its job is now to confirm the module set is sufficient, that `vtk_module_autoinit` is wired correctly, and that reads produce correct numbers — cheap insurance before 2000 lines depend on it.
- `vtk_module_autoinit(TARGETS <both targets> MODULES ${VTK_LIBRARIES})` is **mandatory**. Without it, VTK's object factories are not registered, and readers fail at runtime with confusing "no reader found" or null-output errors while compiling and linking perfectly. This is the most common VTK-integration mistake and it must be applied to *both* the static and the loadable target.

## 6. Phases

Each phase has a hard exit criterion. Do not start a phase before the previous one's criterion is met — the phase ordering is chosen so that each phase de-risks the next.

### Phase 0 — Prove the toolchain (exit: a linked binary that runs)

Not scaffolding; a spike whose only job is to kill the ABI risk in §5.

1. Install `cmake ninja ccache vtk` via brew. Confirm `VTKConfig.cmake`/`vtk-config.cmake` location.
2. Write a ~30-line standalone CMake project that `find_package(VTK)`s, reads one `.vtu` from the corpus with `vtkXMLUnstructuredGridReader`, and prints point/cell counts. Build it with the compiler the extension will use.
3. Run it. Confirm correct counts against the oracle.

**Exit criterion:** the spike binary prints the right numbers. If it does not link or crashes, resolve via a §5 fallback before continuing. This is the go/no-go gate for the whole approach.

### Phase 1 — Extension skeleton that loads (exit: `LOAD` works in the system CLI)

1. Copy the extension-template layout; rename `waddle` → `vtk`. Replace the template's OpenSSL vcpkg dependency with our own `vtk-minimal` overlay port (the template's *use* of vcpkg is kept — see the correction box in §5).
2. Pin submodules. The pins have since moved — do **not** treat the values that were written here (`08e34c447b` / `b777c70d`) as current. The gitlinks recorded by `git ls-tree HEAD duckdb extension-ci-tools` are the single source of truth; `scripts/configure.sh` and `scripts/bootstrap_deps.sh` both derive from them rather than hardcoding, and `make check-pin` verifies the derived DuckDB version tag is recorded in `DUCKDB_KNOWN_VERSIONS`.
3. Register one scalar function `vtk_version()` returning the linked VTK version — this proves VTK is *actually linked and callable from inside DuckDB*, which is strictly more than "it compiled".
4. Wire `find_package(VTK)` + `vtk_module_autoinit` into both targets.

**Exit criterion:** `scripts/smoke.sh` steps 1–3 pass, i.e. `duckdb -unsigned -c "LOAD ...; SELECT vtk_version();"` prints `9.6.2`.

### Phase 2 — Table functions over real data (exit: correct values, oracle-verified)

Build L1, L2, L3. Six table functions taking a path:
`vtk_points(path)`, `vtk_cells(path)`, `vtk_cell_points(path)`, `vtk_field_data(path)`, `vtk_arrays(path)`, `vtk_info(path)`.

Cover `vtkUnstructuredGrid` and `vtkPolyData` first (the CAE core), then the implicit-geometry types. Implement projection pushdown from the start. Include `vtk_debug_dump(path)`.

**Exit criterion:** `scripts/validate_against_vtk.py` reports 0 mismatches over the Tier-1 corpus, and the L1 precision unit test passes. This is the phase where "does it read CAE data correctly" is actually answered — and deliberately, it is answered *before* any catalog complexity is introduced.

### Phase 3 — ATTACH (exit: the schema §10 queries run)

Build L4 on top of Phase 2's functions. `VtkTableEntry::GetScanFunction` returns the Phase-2 functions with pre-bound `FunctionData`. Implement every pure virtual; throw the documented read-only errors.

**Exit criterion:** all ten worked queries in schema §10 run; `read_only.test` passes with no crashes; `DESCRIBE`, `SHOW TABLES`, and `duckdb_tables()` work against the attached database.

### Phase 4 — Coverage and ergonomics

> **SUPERSEDED BY `docs/ROADMAP.md`.** The sketch below was written before anyone
> checked VTK's module graph, and it is wrong in two ways that matter:
>
> * "**each is mostly a `VtkReaderFactory` entry once L1 is uniform**" does not hold.
>   ExodusII, CGNS and EnSight all emit *composite* datasets, so they are gated on a
>   relational model for multiblock data that does not exist yet — the single largest
>   item on the roadmap.
> * **`.pvd` has no reader in VTK at all.** It is a ParaView format; searching VTK
>   9.6.2 for any path matching `pvd` returns zero files. Supporting it means parsing
>   the collection manifest ourselves.
> * **`vtkOpenFOAMReader` is unreachable**, not merely unimplemented: it lives in
>   `IOGeometry`, which privately depends on `VTK::RenderingCore` and therefore cannot
>   be built in a rendering-free VTK.
>
> `docs/ROADMAP.md` has the verified version, with blockers and effort sizes.

Multiblock (`.vtm`) as one schema per block; time series (`.pvd`, numbered sequences) with a `time_step` option or a `time_value` column; `.pvtu` parallel pieces; VTKHDF; the CAE importers VTK already ships (`vtkOpenFOAMReader`, `vtkEnSightGoldReader`, `vtkCGNSReader`, `vtkExodusIIReader`). `vector_layout` option. Replacement scan so `FROM 'mesh.vtu'` works.

### Phase 5 — Performance and portability

Only now: cache converted column chunks; parallel scans (split point/cell ranges across threads — safe because `VtkDataset` is immutable); `ARRAY` instead of `LIST` for fixed-component fields; zero-copy where VTK's AOS buffer layout permits; filter pushdown; lazy schema discovery via `UpdateInformation()` so `ATTACH` on a huge file is cheap. Separately: static linking / self-contained binaries and multi-platform CI, which is what community-extension distribution requires.

## 7. Decisions already made, with rationale (do not re-litigate)

| Decision | Rationale |
|---|---|
| Official VTK C++ library, not a custom parser | Format correctness across ~20 formats and their binary/compressed variants is a multi-year problem VTK has already solved |
| Minimal VTK build (source locally, `vtk-minimal` overlay port in CI) rather than the official vcpkg port or the Homebrew bottle | 54 MB vs 2.8 GB of bottle dependencies; compiling VTK with the extension's own compiler eliminates the libstdc++ ABI risk outright (§5.2-5.3); the official vcpkg port is a ParaView fork at 9.3 with rendering forced on (§5.1) |
| `ATTACH` via `StorageExtension`, with table functions underneath | Matches the user-facing requirement, but the table functions come first so file-reading correctness is proven before catalog boilerplate |
| Read-only | Writing VTK files is a separate problem with its own design questions; nothing about this design precludes it later |
| Eager full read at `ATTACH` | Correct schema discovery for legacy formats requires it, and errors surface where the user can see them. Lazy discovery is a Phase-5 optimisation |
| Whole dataset in RAM | "Make it work before making it fast." Out-of-core is a Phase-5 concern |
| `LIST` for multi-component arrays | Generalises to any component count with one code path; `ARRAY` is the Phase-5 upgrade once verified |
| Six tables including three introspection tables | Discoverability is a first-class requirement for a format users cannot read by eye |
| `cell_points` normalised join table | Cell↔point joins are the central CAE query pattern; without it every user writes the same `unnest` boilerplate |
| Single schema `main` | Multiblock will use additional schemas; keeping serial files in `main` means that extension is additive |

## 8. Principal risks

| Risk | Severity | Mitigation |
|---|---|---|
| VTK/compiler `libstdc++` ABI mismatch | ~~Blocks everything~~ **Eliminated** | Resolved by building VTK from source with the same compiler as the extension (§5.3). Was the top risk while the Homebrew bottle was the plan; Phase 0 still verifies empirically |
| Missing `vtk_module_autoinit` | High — compiles fine, fails at runtime confusingly | Called out in §5; Phase 1 exit criterion exercises a real VTK call from inside DuckDB |
| `-Wl,--gc-sections` stripping VTK's autoinit static initializers | Medium — same silent symptom as missing autoinit | DuckDB's `build_loadable_extension_directory` links the module with `-fvisibility=hidden -Wl,--gc-sections -Wl,--exclude-libs,ALL`. `--exclude-libs,ALL` only affects **static** archives, so brew's shared VTK is unaffected. `--gc-sections` should retain `.init_array` entries, but if the ATTACH canary in `scripts/smoke.sh` step 6 returns 0 rows on a non-empty file, test `-Wl,--no-gc-sections` before suspecting the reader code |
| Catalog pure-virtual drift between v1.5.4 and the example extensions (duckdb-sqlite tracks main) | High | v1.5.4 headers are authoritative; research doc 02 must flag every discrepancy; compile early |
| int64 precision loss via `GetComponent` | High — silent wrong data | Dedicated unit test with 2^53+1; type mapping isolated in L2 |
| `vtkDataSet::GetCell` scratch-object reuse | Medium now, high when parallel | §3 forbids it; use raw connectivity arrays |
| PolyData global cell-id ordering vs. cell-data tuple order | Medium — silently misattributes cell data | Explicit open question, empirically resolved in Phase 2, asserted in `values_polydata.test` |
| Non-portable extension binary (shared VTK) | Low for Phase 1, blocks distribution | Accepted and documented; Phase 5 |
| Corpus files being ExternalData stubs rather than real data | Medium — tests would silently pass on empty files | Corpus catalogue requires content verification and a sha256 manifest |
