# Phase 0 Results — toolchain gate **PASSED**, with three corrections

**Date:** 2026-07-26
**Verdict:** the approach is sound. Proceed to Phase 1.

Phase 0's job was to prove we can link and run against VTK before writing 2000 lines that depend on it. It did that — and it also found three things that invalidate assumptions in the research and design docs. Finding them here, in a 200-line spike, rather than in Phase 2 while simultaneously debugging DuckDB vector code, is precisely why this gate exists.

---

## 1. Environment as built

| Fact | Value |
|---|---|
| VTK | 9.6.2, built from source by `scripts/build_minimal_vtk.sh` |
| Install prefix | `$HOME/.local/vtk-9.6.2` (142 MB, 29 shared libs) |
| `VTK_DIR` | `$HOME/.local/vtk-9.6.2/lib/cmake/vtk-9.6` |
| Compiler | `/usr/bin/c++` — GNU 14.2.0, the same compiler that will build the extension |
| Build size | 1275 ninja targets (a full VTK is ~5000) |
| `sizeof(vtkIdType)` | **8** — `VTK_USE_64BIT_IDS=ON` |
| Modules linked | `CommonCore`, `CommonDataModel`, `CommonExecutionModel`, `CommonMisc`, `IOLegacy`, `IOXML` |

## 2. What passed

- **Links and runs.** No ABI problems. `std::string` crosses the VTK shared-library boundary in both directions (filename in, array names out) without corruption. Building VTK with the extension's own compiler makes this a non-issue by construction, as intended.
- **RPATH works.** All runs were done with `LD_LIBRARY_PATH=""` explicitly. The baked-in RPATH resolves every VTK library, so the same approach will work for a dlopen'd DuckDB module.
- **The minimal module set is sufficient** for every Phase-1 dataset type. Verified reads:

  | File | Class | Points | Cells |
  |---|---|---|---|
  | `legacy/uGridEx.vtk` | vtkUnstructuredGrid | 27 | 12 |
  | `legacy/VTKCellTypes.vtk` | vtkUnstructuredGrid | 27 | 11 |
  | `legacy/office.binary.vtk` | vtkStructuredGrid | 8400 | 7220 |
  | `legacy/tensors.vtk` | vtkUnstructuredGrid | 8 | 1 |
  | `xml/tetra.vtu` | vtkUnstructuredGrid | 22 | 3 |
  | `xml/QuadraticTetra.vtu` | vtkUnstructuredGrid | 189 | 96 |
  | `xml/cow.vtp` | vtkPolyData | 2903 | 3263 |
  | `xml/vase.vti` | vtkImageData | 871125 | 843600 |
  | `xml/RectilinearGrid.vtr` | vtkRectilinearGrid | 545025 | 524288 |
  | `xml/StructuredGrid.vts` | vtkStructuredGrid | 648 | 440 |
  | `synthetic/empty.vtk` | vtkUnstructuredGrid | 0 | 0 |
  | `synthetic/points_only.vtk` | vtkUnstructuredGrid | 4 | 0 |

- **Ground truth confirmed.** All three hand-derived entries in `docs/research/04-test-data-corpus.md` §4 match the spike exactly:
  - `uGridEx.vtk`: 27 points, 12 cells, bounds `0 2 0 1 0 6`, `point[0]=(0,0,0)`, distinct cell types `{1,3,5,6,7,9,10,12}`, point arrays `scalars`(float,1c,27t) + `vectors`(float,3c,27t), **0 cell arrays**.
  - `VTKCellTypes.vtk`: 27 points, 11 cells, distinct types `{1,3,4,5,6,7,8,9,10,11,12}` (all 11), plus `scalars`(float,1c,11t) on **cell** data. Notably the trailing `LOOKUP_TABLE CellColors` block is **not** surfaced as a 4th array, confirming the prediction in the corpus doc that `vtk_arrays` must report exactly 3 rows.
  - `tetra.vtu`: 22 points, 3 cells.

  This mutual confirmation matters: the hand-derived values and VTK agree, so both the corpus catalogue and the spike can be trusted as oracles.

## 3. Correction 1 — `vtkXMLGenericDataObjectReader::CanReadFile()` is unusable

**Research doc 03 §2.3's reader factory is wrong and must not be used as written.**

Measured on VTK 9.6.2 with a valid `test/data/xml/tetra.vtu`:

| Call | Result |
|---|---|
| `vtkXMLGenericDataObjectReader::CanReadFile(f)` (no SetFileName) | **0** |
| `vtkXMLGenericDataObjectReader::CanReadFile(f)` (after SetFileName) | **0** |
| `vtkXMLGenericDataObjectReader::ReadOutputType(f, parallel)` | **4** (`VTK_UNSTRUCTURED_GRID`) |
| `vtkXMLGenericDataObjectReader::Update()` then `GetOutput()` | `vtkUnstructuredGrid`, 22 pts, 3 cells |
| `vtkXMLUnstructuredGridReader::CanReadFile(f)` | **1** |

`CanReadFile` is simply not overridden on the *generic* reader, so it returns 0 for everything. It is reliable on the **concrete** readers.

**Required fix:** dispatch on `ReadOutputType(path, bool &parallel)`, which returns a `VTK_*` data-object type id or a negative value if the file is not readable XML. The `parallel` out-parameter also tells you whether it is a `.pvtu`-style file, which Phase 1 must reject — a useful bonus.

A factory gated on `CanReadFile` would have sent **every** XML file down the legacy path and reported all of them unreadable. This one finding alone justifies Phase 0.

## 4. Correction 2 — a truncated file is silently accepted, and `GetErrorCode()` does not catch it

This is the most serious finding.

`test/data/synthetic/truncated.vtk` is the first 120 bytes of `uGridEx.vtk` — it declares `POINTS 27 float` and then hits EOF after 1.5 points. VTK's behaviour:

```
legacy_error_code=0  legacy_error_string=Success     <-- NO error signal
num_points=27                                        <-- the DECLARED count
bounds=0 30761.765625 0 30761.765625 0 30761.75      <-- uninitialised garbage
point[0]=0 0 0
status=ok
```

So the read "succeeds", reports the declared point count, and hands back **uninitialised memory as coordinates**. `vtkErrorCode::GetStringFromErrorCode(GetErrorCode())` says `Success`.

The **only** signal is a message on VTK's output window, and it is a `WARN`, not an `ERR`:

```
vtkDataReader.cxx:1527  WARN| Error reading ascii data. Possible mismatch of datasize with declaration.
```

### Consequences

1. **`VtkErrorScope` is correctness-critical, not cosmetic.** It was specified in the architecture doc for tidiness — to stop VTK chatter polluting a DuckDB session. It is in fact **the only mechanism capable of satisfying design §8's requirement** that a truncated file raise `IOException` rather than being mistaken for valid data. Build it first, and treat it as load-bearing.
2. **Specific warnings must be escalated to hard errors.** Blanket "any warning is fatal" is too aggressive — VTK warns about benign things. The implementation needs a curated escalation list, starting with:
   - `"Possible mismatch of datasize with declaration"`
   - `"Error reading ascii data"`
   - `"Error reading binary data"`
   - `"Unrecognized file type"`
   Anything matching escalates to `IOException` carrying the captured text. Anything else is captured and discarded (or logged under `DUCK_VTK_TRACE=1`).
3. **Design §8's truncated-file row was wrong** in saying the message includes "the VTK error string" — there is no error string. We synthesise the exception from captured output-window text.
4. **A defence-in-depth check is warranted:** after reading, verify that the coordinate array actually has the declared tuple count and that bounds are finite. The garbage bounds above (`30761.765625` for a mesh whose real extent is 0–2) would be caught by a finiteness/sanity check even if the warning text changes in a future VTK.

## 5. Correction 3 — probing pollutes the output window on every legacy file

Every **valid** legacy `.vtk` read emits a red error line, because the XML probe runs first and fails:

```
$ ./build/phase0/vtk_spike test/data/legacy/uGridEx.vtk   # succeeds, 27 points
vtkXMLGenericDataObject:147  ERR| vtkXMLGenericDataObjectReader: could not load test/data/legacy/uGridEx.vtk
```

`ReadOutputType` itself writes this before returning its negative result. Unmitigated, a user running `ATTACH 'mesh.vtk'` would see an alarming `ERR|` about a reader we didn't ultimately use, on a read that worked perfectly.

**Required fix:** `VtkErrorScope` must be active during **probing** as well as reading, and must *discard* probe-phase messages entirely. This reinforces §4: the scope has to be in place before the reader factory does anything, so it is the first thing to implement in L1.

## 6. Also resolved

- `vtkDataSet::GetCellTypes(vtkCellTypes*)` is **deprecated** in 9.6 — `vtkDataSet.h:183` says "Use `GetDistinctCellTypes(vtkCellTypes* types)` instead". Using it emits `-Wdeprecated-declarations`, which matters because DuckDB extension builds can enable warnings-as-errors. Closes research doc 03 §9 items 1–2.
- `vtkErrorCode` lives in the **`CommonMisc`** module, not `CommonCore`. Omitting it gives a link error (`DSO missing from command line`). `CommonMisc` has been added to both the spike's and the extension's required component list.
- `ReadOutputType`'s exact signature is `int ReadOutputType(const char *name, bool &parallel)` — an instance method, not static. Closes research doc 03 §9 item 3.

## 7. Net effect on the plan

No phase reordering. The changes are:

| Change | Where |
|---|---|
| Reader dispatch uses `ReadOutputType`, not `CanReadFile` | research doc 03 §2.3, and `VtkReaderFactory` |
| `VtkErrorScope` promoted to correctness-critical; implement first in L1 | architecture doc §2, plan Phase 2 |
| Warning-escalation list required | new requirement, design §8 |
| Post-read sanity check (tuple counts, finite bounds) | new requirement, design §8 |
| `CommonMisc` added to required VTK components | `cmake/DuckVTKFindVTK.cmake` |
| Use `GetDistinctCellTypes` | `VtkDataset` |

## 8. Reproducing

```bash
./scripts/build_minimal_vtk.sh                 # ~142 MB installed, 1275 targets
export VTK_DIR=$HOME/.local/vtk-9.6.2/lib/cmake/vtk-9.6
make phase0
./build/phase0/vtk_spike test/data/legacy/uGridEx.vtk
```
