# VTK C++ API and File Formats — Reference for `duck_vtk`

**Scope:** VTK **9.6.2** as packaged by Homebrew (`brew install vtk`, prefix
`/home/linuxbrew/.linuxbrew/opt/vtk`). Focused on *reading* mesh files and
extracting point/cell geometry, point-data and cell-data field arrays, for a
DuckDB extension that exposes VTK files as relational tables.

> **Research method note:** At the time this document was written,
> `brew install vtk` had **not yet completed** in the target environment
> (`ls /home/linuxbrew/.linuxbrew/opt/vtk` → not found; `brew info vtk` shows
> it as bottled but "Not installed"). All signatures below come from the VTK
> nightly Doxygen (`https://vtk.org/doc/nightly/html/`), the VTK file-format
> design docs (`https://docs.vtk.org/en/latest/...` and the pinned 9.5.0
> copy), and the Kitware/VTK GitHub source. Context7 MCP was attempted first
> per instructions but returned `Invalid API key` (its API key is
> misconfigured in this environment — not something fixable from here), so
> Doxygen/WebFetch/WebSearch were used instead. **Every signature should be
> re-verified against the real headers under
> `/home/linuxbrew/.linuxbrew/opt/vtk/include/vtk-9.6/` as soon as the brew
> install finishes** — see the "Uncertainties" section at the end for the
> specific items most likely to need correction (a few are already flagged
> inline where two sources disagreed).

## Table of Contents

1. [File format landscape](#1-file-format-landscape)
2. [Auto-detecting a reader from an arbitrary path](#2-auto-detecting-a-reader-from-an-arbitrary-path)
3. [The data model](#3-the-data-model)
4. [Field data / attributes](#4-field-data--attributes)
5. [Type mapping table (VTK → C++ → DuckDB)](#5-type-mapping-table-vtk--c--duckdb)
6. [Reading efficiently / streaming / schema discovery](#6-reading-efficiently--streaming--schema-discovery)
7. [CMake integration](#7-cmake-integration)
8. [Error handling and output-window suppression](#8-error-handling-and-output-window-suppression)
9. [Uncertainties / verify against installed VTK](#9-uncertainties--verify-against-installed-vtk)

---

## 1. File format landscape

### 1.1 Legacy `.vtk`

One file, one dataset, ASCII or binary. Format: `vtk DataFile Version 3.0`
header line, a title line (≤256 chars), `ASCII`/`BINARY` line, then
`DATASET <TYPE>` and geometry/topology, then optional `POINT_DATA`/`CELL_DATA`
attribute blocks. Reference:
[VTK File Formats (v9.5.0 pinned doc)](https://docs.vtk.org/en/v9.5.0/design_documents/VTKFileFormats.html).

| Dataset keyword | Reader class | Produces |
|---|---|---|
| `STRUCTURED_POINTS` | `vtkStructuredPointsReader` | `vtkStructuredPoints` (≈ `vtkImageData`) |
| `STRUCTURED_GRID` | `vtkStructuredGridReader` | `vtkStructuredGrid` |
| `RECTILINEAR_GRID` | `vtkRectilinearGridReader` | `vtkRectilinearGrid` |
| `POLYDATA` | `vtkPolyDataReader` | `vtkPolyData` |
| `UNSTRUCTURED_GRID` | `vtkUnstructuredGridReader` | `vtkUnstructuredGrid` |
| `FIELD` | `vtkDataReader`-derived generic field readers | `vtkFieldData` (rarely used standalone) |
| *(any of the above, auto-detected)* | `vtkDataSetReader` | `vtkDataSet` subclass (auto-picks the concrete reader above and forwards) |
| *(any of the above or field/graph/table, auto-detected)* | `vtkGenericDataObjectReader` | `vtkDataObject` subclass (superset of `vtkDataSetReader`, also handles `vtkGraph`/`vtkTable`) |

All of these live in module **`VTK::IOLegacy`** and are declared in
`IO/Legacy/vtk*Reader.h`. All inherit from **`vtkDataReader`**
(`IO/Legacy/vtkDataReader.h`), which does the actual header parsing.

Legacy cell connectivity in the file is the **old flat format**:
`CELLS n size` followed by, per cell, `numPoints, id0, id1, ..., id(numPoints-1)`.
When VTK 9.x reads this into memory it is transparently converted into the
**new offsets+connectivity `vtkCellArray`** representation described in §3 —
you never see the old flat array once the object is in memory in VTK ≥ 8.2.

Binary/ASCII: both supported for the whole file (single `ASCII`/`BINARY`
keyword applies to all data sections). No compression, no "appended" concept
— that's an XML-only feature. No parallel/multi-piece variant of legacy
format exists; splitting across files is only done by hand or via EnSight/XML.

### 1.2 XML serial formats

Module **`VTK::IOXML`** (parsing support in **`VTK::IOXMLParser`**). Root
element `<VTKFile type="..." version="1.0" byte_order="LittleEndian"
header_type="UInt32" compressor="vtkZLibDataCompressor">`. Reference:
[XML File Formats](https://docs.vtk.org/en/latest/vtk_file_formats/vtkxml_file_format.html).

| Extension | `type=` attribute | Reader class | Dataset |
|---|---|---|---|
| `.vti` | `ImageData` | `vtkXMLImageDataReader` | `vtkImageData` |
| `.vtp` | `PolyData` | `vtkXMLPolyDataReader` | `vtkPolyData` |
| `.vtr` | `RectilinearGrid` | `vtkXMLRectilinearGridReader` | `vtkRectilinearGrid` |
| `.vts` | `StructuredGrid` | `vtkXMLStructuredGridReader` | `vtkStructuredGrid` |
| `.vtu` | `UnstructuredGrid` | `vtkXMLUnstructuredGridReader` | `vtkUnstructuredGrid` |

(Note: a WebFetch summary of the XML doc page mis-rendered these as
`vtkImageDataReader`/`vtkPolyDataReader`/etc. — those names belong to the
**legacy** readers. The correct XML-serial class names all have the
`vtkXML...Reader` prefix as shown above; this is standard, well-known VTK
naming and is corrected here.)

Each dataset is divided into `<Piece>` elements (serial files usually have
exactly one piece, but can have more). `DataArray` elements carry
`type` (`Int8`/`UInt8`/`Int16`/`UInt16`/`Int32`/`UInt32`/`Int64`/`UInt64`/
`Float32`/`Float64`), `Name`, `NumberOfComponents`, and `format`:

- `format="ascii"` — whitespace-separated numbers inline.
- `format="binary"` — base64-encoded, optionally zlib/lz4/lzma-compressed
  (compressor named on `<VTKFile compressor="...">`), each `DataArray`
  self-contained.
- `format="appended"` — raw bytes (optionally compressed) concatenated in a
  single `<AppendedData encoding="raw"|"base64">` blob at the end of the
  file; each array stores a byte `offset=` into that blob. This is the
  fastest to read (no per-array base64 decode of a giant block) and the most
  common format written by ParaView/VTK writers.

### 1.3 XML parallel formats

Same module, "meta" files that describe *structure* + reference per-piece
serial files; no bulk data of their own.

| Extension | Reader class | Assembles |
|---|---|---|
| `.pvti` | `vtkXMLPImageDataReader` | `vtkImageData` (via `vtkPImageDataReader`... no — see below) |
| `.pvtp` | `vtkXMLPPolyDataReader` | `vtkPolyData` |
| `.pvtr` | `vtkXMLPRectilinearGridReader` | `vtkRectilinearGrid` |
| `.pvts` | `vtkXMLPStructuredGridReader` | `vtkStructuredGrid` |
| `.pvtu` | `vtkXMLPUnstructuredGridReader` | `vtkUnstructuredGrid` |

(Same correction as above applies: the actual class names are
`vtkXMLP...Reader`, e.g. `vtkXMLPUnstructuredGridReader`, not
`vtkPUnstructuredGridReader` — the latter prefix belongs to VTK's older,
separate legacy-parallel `IO/Parallel` classes for `.pvtk`-family formats,
which still exist but are legacy, MPI-oriented, and not XML.) All
`vtkXMLP*Reader` classes derive from `vtkXMLPDataReader` → `vtkXMLReader`, and
when run single-process they simply read every referenced piece file and
append into one in-memory dataset — useful for "read a `.pvtu` on one thread"
without needing MPI.

### 1.4 Multi-block / composite

- `.vtm` / `.vtmb` — `vtkXMLMultiBlockDataReader` (module `VTK::IOXML`) →
  `vtkMultiBlockDataSet` (a tree of named blocks, each itself any dataset
  type, including nested multi-block). This is VTK's own composite XML
  format (root element `<VTKFile type="vtkMultiBlockDataSet">`), not
  ParaView-specific.
- `.pvd` — **this is a ParaView "Collection" file**, not a core-VTK
  multi-block. It is a thin XML listing `<DataSet timestep="..." group="..."
  part="..." file="relative/path.vtu"/>` entries, primarily used to
  represent **time series** (and/or multi-part datasets) as a directory of
  per-timestep `.vtu`/`.vtp`/etc. files. The reader is
  **`vtkPVDReader`**, which *is* shipped in core VTK (module `VTK::IOXML`,
  header `vtkPVDReader.h`) despite the "PV" prefix suggesting ParaView —
  it's a small convenience reader in VTK itself, not a ParaView-only class.
  It produces a `vtkMultiBlockDataSet` per timestep and exposes time-step
  metadata like `vtkXMLReader`. (Older VTK/ParaView docs also reference
  `vtkXMLCollectionReader` — treat that name as likely superseded/legacy; the
  currently-documented class is `vtkPVDReader`. **Verify which one exists in
  9.6 headers.**)

### 1.5 VTKHDF (`.hdf`, `.vtkhdf`)

Reference: [VTKHDF File Format](https://docs.vtk.org/en/latest/vtk_file_formats/vtkhdf_file_format/index.html).

- Preferred extension is **`.vtkhdf`**; **`.hdf`** is also accepted but not
  preferred.
- Current on-disk format version documented is **2.5**; the spec is
  explicitly marked "not considered complete, and lacks support for some VTK
  data types" — i.e. still evolving as of the 9.6 timeframe.
- Reader: **`vtkHDFReader`** (writer: `vtkHDFWriter`), module is
  **`VTK::IOHDF`** (depends on HDF5, which is already a brew-vtk dependency).
- Supports as of the latest doc: `PolyData`, `UnstructuredGrid`, `ImageData`,
  `HyperTreeGrid` as "basic" types, plus composite `OverlappingAMR`,
  `MultiBlockDataSet`, `PartitionedDataSetCollection`.
- Built on HDF5 groups/datasets directly (no XML wrapper); designed for
  parallel I/O performance and supports static and time-dependent data in
  one file. **Status in 9.6.2 should be verified** — VTKHDF has been
  actively gaining dataset-type support release over release (this is a
  moving target); treat `vtkHDFReader`'s exact capability list for 9.6.2 as
  needing confirmation from the installed headers/changelog once available.

### 1.6 CAE-specific readers shipped with VTK

These are all real VTK-core reader classes (not ParaView plugins), each
gated behind its own CMake module so they may or may not be enabled in a
given VTK build. Homebrew's `vtk` formula (per `brew info vtk`) depends on
`cgns`, `hdf5`, `netcdf`, `boost`, `eigen`, `proj`, etc., which is strong
circumstantial evidence these CAE readers are compiled into the bottle,
**but this must be confirmed by grepping the installed headers**
(`find /home/linuxbrew/.linuxbrew/opt/vtk/include -iname 'vtk*Reader.h'`)
once available:

| Reader class | Module | Produces | Notes |
|---|---|---|---|
| `vtkOpenFOAMReader` | `VTK::IOGeometry` | `vtkMultiBlockDataSet` | Reads OpenFOAM case directories (`.foam` stub file convention or a directory). Pure VTK parsing, no external lib dependency — near-certain to be present in any VTK build. |
| `vtkGenericEnSightReader` / `vtkEnSightGoldReader` (+ `vtkEnSightGoldBinaryReader`, etc.) | `VTK::IOEnSight` | `vtkMultiBlockDataSet` (via `vtkEnSightReader` base) | `vtkGenericEnSightReader` auto-detects the EnSight case-file variant (Gold ASCII/binary, 6, etc.) and delegates. |
| `vtkCGNSReader` | `VTK::IOCGNSReader` (NOT `IOCGNS` — verified in `IO/CGNS/vtk.module`) | `vtkMultiBlockDataSet` | Reads binary CGNS files, low-level dependency on the CGNS library (present as a brew dependency: `cgns`). Supports structured and unstructured CGNS meshes, node/cell/face-centered data. Has `CanReadFile(const char*)`. |
| `vtkExodusIIReader` | `VTK::IOExodus` | `vtkMultiBlockDataSet` | Reads Exodus II (NetCDF-based) files; needs `netcdf`/`hdf5` (both present as brew deps). |
| `vtkPLOT3DReader` | `VTK::IOGeometry` | `vtkStructuredGrid` (or multi-block of them for multi-grid files) | Classic CFD structured-grid format; needs separate `.xyz`/`.q` (grid/solution) files typically. |
| `vtkAbaqusReader` | *(does not exist in core VTK)* | — | There is **no `vtkAbaqusReader` in core VTK** as of current Doxygen/GitHub search — Abaqus `.odb`/`.inp` reading in the Kitware ecosystem is a ParaView plugin (`ioss`/`Exodus`-adjacent) or lives in third-party projects, not `VTK::IO*`. Do not plan on this class existing; confirm absence via header search. |

Given brew's dependency list includes `cgns`, `hdf5`, `netcdf` explicitly,
CGNS and Exodus support are very likely compiled in. OpenFOAM and EnSight
readers have no external library dependency (pure parsers) so they are
almost always enabled by default VTK module logic unless explicitly
`-DVTK_MODULE_ENABLE_VTK_IOOpenFOAM=NO`'d, which brew's formula is unlikely
to do. **Still verify** with, e.g.:
```
find /home/linuxbrew/.linuxbrew/opt/vtk/include -name 'vtkOpenFOAMReader.h' \
                                                  -o -name 'vtkCGNSReader.h' \
                                                  -o -name 'vtkExodusIIReader.h' \
                                                  -o -name 'vtkGenericEnSightReader.h' \
                                                  -o -name 'vtkPLOT3DReader.h'
find /home/linuxbrew/.linuxbrew/opt/vtk/lib -name 'libvtkIOCGNS*' \
                                             -o -name 'libvtkIOExodus*' \
                                             -o -name 'libvtkIOEnSight*'
```

---

## 2. Auto-detecting a reader from an arbitrary path

> ### ⚠ CORRECTION — measured on VTK 9.6.2 in Phase 0, supersedes §2.2 and §2.3 below
>
> **`vtkXMLGenericDataObjectReader::CanReadFile()` returns 0 for every file, including a
> perfectly valid `.vtu`.** It is not overridden on the *generic* reader. The
> `ReaderFactory` in §2.3 gates on it and therefore sends every XML file down the
> legacy path and reports it unreadable. Do not use it.
>
> Measured against `test/data/xml/tetra.vtu` (a valid 22-point, 3-cell UnstructuredGrid):
>
> | Call | Result |
> |---|---|
> | `vtkXMLGenericDataObjectReader::CanReadFile(f)` | **0** (broken) |
> | `vtkXMLGenericDataObjectReader::CanReadFile(f)` after `SetFileName` | **0** (broken) |
> | `vtkXMLGenericDataObjectReader::ReadOutputType(f, parallel)` | **4** = `VTK_UNSTRUCTURED_GRID` ✅ |
> | `vtkXMLUnstructuredGridReader::CanReadFile(f)` (concrete reader) | **1** ✅ |
>
> **Use `int ReadOutputType(const char *name, bool &parallel)`** — an instance method,
> returning a `VTK_*` data-object type id or a negative value for non-XML. Its
> `parallel` out-param also flags `.pvtu`-style files, which Phase 1 must reject.
> `CanReadFile` *is* reliable on the concrete readers, just not the generic one.
>
> Two further Phase 0 findings that affect this section:
> - `ReadOutputType` **writes a red `ERR| ... could not load <file>` line to VTK's output
>   window for every legacy `.vtk` file**, even ones that then read perfectly. The error
>   scope of §8.2 must be active during probing and must discard probe-phase messages.
> - A **truncated** legacy file is silently accepted: `GetErrorCode()` returns `Success`,
>   the declared point count is reported, and coordinates are uninitialised memory. The
>   only signal is a `WARN` on the output window. See `docs/PHASE0_RESULTS.md` §4 — this
>   makes §8.2's error capture a correctness requirement, not a cosmetic one.


VTK gives you two orthogonal "any file of this family" readers, plus a
manual sniff you can do yourself for the ultimate top-level dispatch (legacy
vs. XML vs. HDF5 vs. CAE-specific).

### 2.1 `vtkGenericDataObjectReader` (legacy family)

Declared in `IO/Legacy/vtkGenericDataObjectReader.h`, subclass of
`vtkDataReader`. It **reads the legacy ASCII header itself** and picks the
right concrete output type:

```cpp
vtkDataObject* GetOutput();
vtkDataObject* GetOutput(int idx);
vtkPolyData*          GetPolyDataOutput();
vtkStructuredGrid*    GetStructuredGridOutput();
vtkStructuredPoints*  GetStructuredPointsOutput();
vtkUnstructuredGrid*  GetUnstructuredGridOutput();
vtkRectilinearGrid*   GetRectilinearGridOutput();
vtkGraph*             GetGraphOutput();
vtkTable*             GetTableOutput();
```
Inherited from `vtkDataReader` (`IO/Legacy/vtkDataReader.h`) — the actual
sniffing primitives:
```cpp
int  OpenVTKFile(const char* fname = nullptr);
int  ReadHeader(const char* fname = nullptr, bool quiet = false); // parses "vtk DataFile Version x.x" + title + ASCII/BINARY + DATASET line
void CloseVTKFile();
int  GetFileType();                 // VTK_ASCII or VTK_BINARY
int  IsFileStructuredPoints();
int  IsFilePolyData();
int  IsFileStructuredGrid();
int  IsFileUnstructuredGrid();
int  IsFileRectilinearGrid();
```
`vtkDataSetReader` is the same idea but restricted to `vtkDataSet` subtypes
(no graph/table); prefer `vtkGenericDataObjectReader` unless you specifically
want to reject graphs/tables.

### 2.2 `vtkXMLGenericDataObjectReader` (XML family)

`IO/XML/vtkXMLGenericDataObjectReader.h`, subclass of `vtkXMLDataReader` →
`vtkXMLReader`. Peeks at the root `<VTKFile type="...">` attribute (via
`vtkXMLDataParser`, not a full parse) to decide which concrete `vtkXML*Reader`
to instantiate internally, then forwards all pipeline calls to it.
```cpp
static vtkXMLGenericDataObjectReader* New();
vtkDataObject* GetOutput();
vtkDataObject* GetOutput(int idx);
vtkImageDataOutput / GetPolyDataOutput() / GetUnstructuredGridOutput() /
GetStructuredGridOutput() / GetRectilinearGridOutput() /
GetMultiBlockDataSetOutput() / GetOverlappingAMROutput();  // typed convenience getters, nullptr if type mismatch

// Type detection without a full read:
virtual int ReadOutputType(const char* name, bool& parallel);
```
`ReadOutputType` is the key "cheap sniff" call — it opens just enough of the
file to read the root element's `type=` attribute and returns one of the
`VTK_*_DATA` constants from `vtkType.h` (e.g. `VTK_UNSTRUCTURED_GRID`), and
sets `parallel` true for `.pvt*` files. **Verify exact `static`-ness of this
method against the header** — two doc fetches disagreed on whether it's
`static` or a plain virtual instance method; both call patterns
(`vtkXMLGenericDataObjectReader::ReadOutputType(path, parallel)` vs.
`reader->ReadOutputType(...)`) appear in different VTK doc snapshots.

All XML reader classes (and only XML readers — this is not on the legacy
`vtkDataReader` hierarchy) implement the static-style sniff used by
`vtkXMLReader`'s own `CanReadFile`:
```cpp
virtual int CanReadFile(const char* name);       // vtkXMLReader — "quick check of file header"
bool        CanReadFile(vtkResourceStream* stream);
```
so an alternative dispatch strategy is "try each concrete `vtkXML*Reader`'s
`CanReadFile()` until one returns 1" — more expensive than
`ReadOutputType`/extension dispatch but robust to misleading extensions.

### 2.3 Recommended `ReaderFactory`

In practice, for a DuckDB extension you want a fast, extension-first,
content-sniff-as-fallback dispatcher, because:
- extension dispatch is O(1) and correct 99% of the time for `.vtu/.vtp/...`
  and CAE formats (OpenFOAM/CGNS/Exodus/EnSight all have unambiguous
  extensions or directory conventions);
- legacy `.vtk` needs `vtkGenericDataObjectReader`/its header sniff because
  a `.vtk` file's *actual* dataset type is only knowable from its content;
- XML files are self-describing via `<VTKFile type=...>` — `CanReadFile` or
  `ReadOutputType` are the correct ways to confirm/dispatch, but a `.vtu`
  extension is normally trustworthy enough to skip the sniff for the common
  case and only fall back to sniffing on extension-less/ambiguous paths.

```cpp
#include <vtkSmartPointer.h>
#include <vtkDataObject.h>
#include <vtkGenericDataObjectReader.h>
#include <vtkXMLGenericDataObjectReader.h>
#include <vtkXMLUnstructuredGridReader.h>
#include <vtkXMLPolyDataReader.h>
#include <vtkXMLStructuredGridReader.h>
#include <vtkXMLRectilinearGridReader.h>
#include <vtkXMLImageDataReader.h>
#include <vtkXMLMultiBlockDataReader.h>
#include <vtkHDFReader.h>
#include <vtksys/SystemTools.hxx>   // for lower-casing / extension helpers, or use std::filesystem
#include <filesystem>
#include <algorithm>
#include <cctype>

vtkSmartPointer<vtkDataObject> ReaderFactory(const std::string& path)
{
  namespace fs = std::filesystem;
  std::string ext = fs::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  auto readAndReturn = [](auto reader, const std::string& p) -> vtkSmartPointer<vtkDataObject> {
    reader->SetFileName(p.c_str());
    reader->Update();
    if (reader->GetErrorCode() != vtkErrorCode::NoError)
      return nullptr;
    return vtkSmartPointer<vtkDataObject>(reader->GetOutput());
  };

  if (ext == ".vtu") return readAndReturn(vtkSmartPointer<vtkXMLUnstructuredGridReader>::New(), path);
  if (ext == ".vtp") return readAndReturn(vtkSmartPointer<vtkXMLPolyDataReader>::New(), path);
  if (ext == ".vts") return readAndReturn(vtkSmartPointer<vtkXMLStructuredGridReader>::New(), path);
  if (ext == ".vtr") return readAndReturn(vtkSmartPointer<vtkXMLRectilinearGridReader>::New(), path);
  if (ext == ".vti") return readAndReturn(vtkSmartPointer<vtkXMLImageDataReader>::New(), path);
  if (ext == ".vtm" || ext == ".vtmb")
    return readAndReturn(vtkSmartPointer<vtkXMLMultiBlockDataReader>::New(), path);
  if (ext == ".vtkhdf" || ext == ".hdf")
    return readAndReturn(vtkSmartPointer<vtkHDFReader>::New(), path);

  if (ext == ".vtk") {
    // Legacy: content-sniff is mandatory, the extension alone is ambiguous.
    auto reader = vtkSmartPointer<vtkGenericDataObjectReader>::New();
    reader->SetFileName(path.c_str());
    reader->Update();
    if (reader->GetErrorCode() != vtkErrorCode::NoError) return nullptr;
    return vtkSmartPointer<vtkDataObject>(reader->GetOutput());
  }

  // Unknown/ambiguous extension: try the generic XML sniff, then legacy.
  {
    auto xmlReader = vtkSmartPointer<vtkXMLGenericDataObjectReader>::New();
    if (xmlReader->CanReadFile(path.c_str())) {
      xmlReader->SetFileName(path.c_str());
      xmlReader->Update();
      if (xmlReader->GetErrorCode() == vtkErrorCode::NoError)
        return vtkSmartPointer<vtkDataObject>(xmlReader->GetOutput());
    }
  }
  {
    auto legacy = vtkSmartPointer<vtkGenericDataObjectReader>::New();
    legacy->SetFileName(path.c_str());
    if (legacy->OpenVTKFile() && legacy->ReadHeader()) {
      legacy->Update();
      if (legacy->GetErrorCode() == vtkErrorCode::NoError)
        return vtkSmartPointer<vtkDataObject>(legacy->GetOutput());
    }
  }
  return nullptr; // caller should then try CAE-specific readers keyed off
                   // directory conventions (OpenFOAM "case/constant+system"
                   // layout, CGNS/Exodus magic-byte sniff, etc.)
}
```

CAE formats need their own dispatch branch above generic XML/legacy: an
OpenFOAM "reader" is pointed at a stub file (commonly named `case.foam`) or
a directory, not sniffable via `vtkXMLGenericDataObjectReader`/
`vtkGenericDataObjectReader`; CGNS/Exodus/EnSight all have their own
`CanReadFile`/case-file conventions and should be tried explicitly based on
extension (`.cgns`, `.e`/`.exo`, `.case`) rather than folded into the generic
dispatch above.

---

## 3. The data model

### 3.1 `vtkDataSet` (abstract base of all "simple", non-composite datasets)

```cpp
virtual vtkIdType GetNumberOfPoints() = 0;
virtual vtkIdType GetNumberOfCells()  = 0;
virtual double*   GetPoint(vtkIdType ptId) = 0;              // internal pointer, valid until next call
virtual void      GetPoint(vtkIdType id, double x[3]);        // safe, copies into caller buffer — prefer this one
virtual vtkCell*  GetCell(vtkIdType cellId) = 0;
virtual void      GetCell(vtkIdType cellId, vtkGenericCell* cell) = 0; // thread-safe variant (reentrant, no shared scratch cell)
virtual int       GetCellType(vtkIdType cellId) = 0;           // one of the VTK_* enum values, §3.6
virtual void      GetCellPoints(vtkIdType cellId, vtkIdList* ptIds) = 0;
vtkPointData*     GetPointData();
vtkCellData*      GetCellData();
double*           GetBounds();
void              GetBounds(double bounds[6]);
```
`GetPoint(vtkIdType, double[3])`/`GetCellPoints` etc. are documented as
"thread safe if first called from a single thread and the dataset is not
modified" — i.e. safe for read-only concurrent access after a warm-up call,
which matters if you parallelize row generation with `vtkSMPTools` or your
own thread pool.
Source: [`vtkDataSet`](https://vtk.org/doc/nightly/html/classvtkDataSet.html).

`GetFieldData()` (dataset-level, not point/cell) is inherited from
`vtkDataObject`, see §4.6.

### 3.2 `vtkPointSet` and `vtkPoints` — the coordinate array

`vtkPointSet` (base of `vtkPolyData`/`vtkUnstructuredGrid`/`vtkStructuredGrid`
— i.e. every dataset type with *explicit* point coordinates, as opposed to
`vtkImageData`/`vtkRectilinearGrid` which compute them implicitly):
```cpp
vtkPoints* GetPoints() override;   // vtkPointSet.h — nullptr only if the dataset truly has no points
```
`vtkPoints` wraps a single `vtkDataArray` of 3-component tuples:
```cpp
vtkDataArray* GetData();                 // the raw contiguous coordinate array — this IS the storage, not a copy
virtual int   GetDataType() const;       // VTK_FLOAT or VTK_DOUBLE (occasionally others if user-constructed)
void          GetPoint(vtkIdType id, double x[3]);
double*       GetPoint(vtkIdType ptId);  // internal pointer
vtkIdType     GetNumberOfPoints() const;
void          SetDataTypeToFloat();      // convenience setters (writer-side, rarely needed for reading)
void          SetDataTypeToDouble();
```
Practical implication: `GetPoints()->GetData()` gives you the coordinate
array directly. Its `GetDataType()` is very commonly `VTK_FLOAT` (writers
often use float for size), so if you want doubles for SQL you must convert,
not just reinterpret-cast — check `GetDataType()` before assuming `double*`.
Source: [`vtkPoints`](https://vtk.org/doc/nightly/html/classvtkPoints.html).

### 3.3 `vtkUnstructuredGrid` — the modern `vtkCellArray`

`vtkUnstructuredGrid` (`Common/DataModel/vtkUnstructuredGrid.h`) stores
connectivity as a single `vtkCellArray` (all cells, any mix of types) plus a
parallel per-cell type-id array:
```cpp
vtkCellArray*        GetCells();                 // the connectivity container (offsets + connectivity ids)
vtkUnsignedCharArray* GetCellTypesArray();        // per-cell VTK_* type id, length == GetNumberOfCells()
                                                   // (NOTE: some doc snapshots show this as GetCellTypes() returning
                                                   //  a plain vtkDataArray* — verify exact name/return type in the
                                                   //  9.6 header; "GetCellTypesArray()/vtkUnsignedCharArray*" is the
                                                   //  name used in this task's own spec and matches modern VTK source)
vtkUnsignedCharArray* GetDistinctCellTypesArray(); // the SET of cell types present, deduplicated
void GetCellPoints(vtkIdType cellId, vtkIdType& npts, vtkIdType const*& pts);              // fast, no copy
void GetCellPoints(vtkIdType cellId, vtkIdType& npts, vtkIdType const*& pts, vtkIdList* ptIds); // fast + also fills ptIds
void GetCellPoints(vtkIdType cellId, vtkIdList* ptIds);                                    // classic, always copies
```
**`vtkCellArray` itself (VTK ≥ 8.2, the "new" cell array — this is what's in
9.6)** stores connectivity as two flat arrays instead of the old
"[n, id0..idn-1, n, id0..idn-1, ...]" packed format:
```cpp
vtkDataArray* GetOffsetsArray() const;      // length == GetNumberOfCells() + 1; offsets[i+1]-offsets[i] == cell i's point count
vtkDataArray* GetConnectivityArray() const; // length == total point-id references; flat concatenation of every cell's point ids
bool IsStorage64Bit() const;                // true if offsets/connectivity are stored as vtkTypeInt64Array, false => vtkTypeInt32Array
bool IsStorage32Bit() const;
void Use32BitStorage(); void Use64BitStorage(); void UseDefaultStorage(); // writer-side; readers choose based on file/VTK_USE_64BIT_IDS
vtkIdType GetNumberOfCells() const;
void GetCellAtId(vtkIdType cellId, vtkIdType& cellSize, vtkIdType const*& cellPoints); // zero-copy, fastest
void GetCellAtId(vtkIdType cellId, vtkIdType& cellSize, vtkIdType const*& cellPoints, vtkIdList* ptIds); // + also copies to ptIds
void GetCellAtId(vtkIdType cellId, vtkIdList* pts);   // classic, always copies through vtkIdList
vtkSmartPointer<vtkCellArrayIterator> NewIterator();  // thread-safe scoped iterator; preferred over legacy InitTraversal()/GetNextCell()
```
This is the single most important API surface for a fast bulk-export path:
`GetOffsetsArray()`/`GetConnectivityArray()` let you read the **entire**
connectivity of an unstructured grid as two typed numeric arrays (via the
type-dispatch approach in §4.4) with **zero per-cell virtual calls** — vastly
faster than looping `GetCellPoints()` cell-by-cell for meshes with millions
of cells. `IsStorage64Bit()` tells you whether to read those two arrays as
`vtkTypeInt32Array` or `vtkTypeInt64Array` (independent of `vtkIdType`'s own
32/64-bitness — see §5).
Source: [`vtkCellArray`](https://vtk.org/doc/nightly/html/classvtkCellArray.html).

### 3.4 `vtkPolyData` — four parallel cell arrays

```cpp
vtkCellArray* GetVerts();    // vtkVertex / vtkPolyVertex
vtkCellArray* GetLines();    // vtkLine / vtkPolyLine
vtkCellArray* GetPolys();    // vtkTriangle / vtkQuad / vtkPolygon
vtkCellArray* GetStrips();   // vtkTriangleStrip
vtkIdType GetNumberOfVerts(); vtkIdType GetNumberOfLines();
vtkIdType GetNumberOfPolys(); vtkIdType GetNumberOfStrips();
int GetCellType(vtkIdType cellId) override; // dispatches into whichever of the 4 arrays that global cellId falls into
```
Global cell ids in a `vtkPolyData` are assigned by concatenating the four
arrays **in the fixed order verts → lines → polys → strips**: cell `0..
GetNumberOfVerts()-1` are verts, the next `GetNumberOfLines()` are lines, then
polys, then strips. There's a helper,
`GetCellIdRelativeToCellArray(vtkIdType cellId)`, that maps a global id back
to its position inside whichever of the four `vtkCellArray`s actually holds
it — useful if you need to correlate a global cell-data row with the correct
per-array connectivity entry. For bulk export it's usually simpler to walk
the four arrays separately (each is a `vtkCellArray` with the same
offsets/connectivity API as §3.3) and assign your own sequential row id in
that same verts/lines/polys/strips order to stay consistent with
`GetCellData()`, which is indexed by this same global cell id.
Source: [`vtkPolyData`](https://vtk.org/doc/nightly/html/classvtkPolyData.html).

### 3.5 Implicit-geometry datasets

**`vtkImageData`** (`Common/DataModel/vtkImageData.h`) — regular grid, no
stored point coordinates at all:
```cpp
virtual int* GetExtent();  void GetExtent(int[6]); void GetExtent(int&,int&,int&,int&,int&,int&); // [xmin,xmax,ymin,ymax,zmin,zmax]
int* GetDimensions();      void GetDimensions(int dims[3]);       // number of points along each axis
virtual double* GetSpacing(); void GetSpacing(double[3]);
virtual double* GetOrigin();  void GetOrigin(double[3]);
vtkIdType ComputePointId(int ijk[3]);
vtkIdType ComputeCellId(int ijk[3]);
int ComputeStructuredCoordinates(const double x[3], int ijk[3], double pcoords[3]);
vtkIdType GetNumberOfPoints(); vtkIdType GetNumberOfCells();
```
Point `(i,j,k)` (each relative to `extent[0]`, `extent[2]`, `extent[4]`) has
physical coordinate `origin + (i,j,k) * spacing` (plus a direction-matrix
rotation if `SetDirectionMatrix` was used — rare for CFD output but present
in the API since VTK 8). To synthesize the full point table, iterate `k`
outer, `j` middle, `i` inner (see ordering note below) and compute directly
— no need to call `GetPoint()` in a hot loop.

**`vtkRectilinearGrid`** — regular topology, but per-axis *irregular*
spacing, stored as three 1-D coordinate arrays:
```cpp
virtual vtkDataArray* GetXCoordinates();
virtual vtkDataArray* GetYCoordinates();
virtual vtkDataArray* GetZCoordinates();
int* GetDimensions(); void GetDimensions(int dims[3]);
int* GetExtent();     // as above
void GetPoint(vtkIdType id, double x[3]);   // computed on the fly from the 3 coordinate arrays + flat-to-ijk math
double* GetPoint(vtkIdType ptId);
void GetPoint(int i, int j, int k, double p[3]); // rectilinear-grid-specific direct ijk accessor
```
Point `(i,j,k)` coordinate is `(X[i], Y[j], Z[k])` — to bulk-export, read the
three coordinate `vtkDataArray`s once (small: `nx+ny+nz` values) and do the
cross-product yourself rather than calling `GetPoint` per point.

**`vtkStructuredGrid`** — like `vtkImageData`/`vtkRectilinearGrid`
topologically (an `extent`/`dimensions`-defined structured i/j/k mesh) but
with **fully explicit** point coordinates (it's a `vtkPointSet`, so
`GetPoints()` works exactly as in §3.2) — used for curvilinear/body-fitted
structured CFD meshes (e.g. PLOT3D output). Cells are implicit (hexahedra/
quads formed from adjacent grid points) — there is no `vtkCellArray` to
read; `GetCellType(cellId)` returns `VTK_HEXAHEDRON`/`VTK_QUAD`/`VTK_VOXEL`-
family types based on grid dimensionality (3D/2D/1D) and `GetCellPoints`
computes point ids from `(i,j,k)` arithmetic internally.

**Structured point/cell ordering** (`vtkImageData`, `vtkRectilinearGrid`,
`vtkStructuredGrid` alike): flat index `id = i + j*dimX + k*dimX*dimY` — **i
varies fastest**, then j, then k (row-major / "X fastest", sometimes called
Fortran-like ordering in VTK docs because early VTK examples used
column-major-looking loops, but the actual flattening formula is as just
given). The same ordering applies to cells using the cell-dimension
`(dimX-1, dimY-1, dimZ-1)` (for 3D; a dimension of 1 drops out and lowers
cell dimensionality, e.g. `dimZ==1` → 2D quad cells, `dimY==dimZ==1` → 1D
line cells).

### 3.6 Cell type enum and name lookup

Numeric cell-type ids are defined in `Common/DataModel/vtkCellType.h` as a
plain C enum (`VTKCellType`). Full list (confirmed via the doxygen page for
`vtkCellType.h`):

```
VTK_EMPTY_CELL = 0
VTK_VERTEX = 1
VTK_POLY_VERTEX = 2
VTK_LINE = 3
VTK_POLY_LINE = 4
VTK_TRIANGLE = 5
VTK_TRIANGLE_STRIP = 6
VTK_POLYGON = 7
VTK_PIXEL = 8
VTK_QUAD = 9
VTK_TETRA = 10
VTK_VOXEL = 11
VTK_HEXAHEDRON = 12
VTK_WEDGE = 13
VTK_PYRAMID = 14
VTK_PENTAGONAL_PRISM = 15
VTK_HEXAGONAL_PRISM = 16
# (17-20 reserved/unused in the enum)
VTK_QUADRATIC_EDGE = 21
VTK_QUADRATIC_TRIANGLE = 22
VTK_QUADRATIC_QUAD = 23
VTK_QUADRATIC_TETRA = 24
VTK_QUADRATIC_HEXAHEDRON = 25
VTK_QUADRATIC_WEDGE = 26
VTK_QUADRATIC_PYRAMID = 27
VTK_BIQUADRATIC_QUAD = 28
VTK_TRIQUADRATIC_HEXAHEDRON = 29
VTK_QUADRATIC_LINEAR_QUAD = 30
VTK_QUADRATIC_LINEAR_WEDGE = 31
VTK_BIQUADRATIC_QUADRATIC_WEDGE = 32
VTK_BIQUADRATIC_QUADRATIC_HEXAHEDRON = 33
VTK_BIQUADRATIC_TRIANGLE = 34
VTK_CUBIC_LINE = 35
VTK_QUADRATIC_POLYGON = 36
VTK_TRIQUADRATIC_PYRAMID = 37
# (38-40 reserved/unused)
VTK_CONVEX_POINT_SET = 41
VTK_POLYHEDRON = 42
# (43-59 reserved/unused)
VTK_HIGHER_ORDER_CURVE = 60
VTK_HIGHER_ORDER_TRIANGLE = 61
VTK_HIGHER_ORDER_QUADRILATERAL = 62
# (63 reserved/unused — VTK_HIGHER_ORDER_POLYGON)
VTK_HIGHER_ORDER_TETRAHEDRON = 64
VTK_HIGHER_ORDER_WEDGE = 65
VTK_HIGHER_ORDER_PYRAMID = 66
VTK_HIGHER_ORDER_HEXAHEDRON = 67
VTK_LAGRANGE_CURVE = 68
VTK_LAGRANGE_TRIANGLE = 69
VTK_LAGRANGE_QUADRILATERAL = 70
VTK_LAGRANGE_TETRAHEDRON = 71
VTK_LAGRANGE_HEXAHEDRON = 72
VTK_LAGRANGE_WEDGE = 73
VTK_LAGRANGE_PYRAMID = 74
VTK_BEZIER_CURVE = 75
VTK_BEZIER_TRIANGLE = 76
VTK_BEZIER_QUADRILATERAL = 77
VTK_BEZIER_TETRAHEDRON = 78
VTK_BEZIER_HEXAHEDRON = 79
VTK_BEZIER_WEDGE = 80
VTK_BEZIER_PYRAMID = 81
VTK_NUMBER_OF_CELL_TYPES = 82
```
(Gaps like 17-20, 38-40, 43-59, 63 are intentionally-unused reserved values
in the real header — treat any id not in this list defensively, i.e. fall
back to `"UNKNOWN_CELL_TYPE_<n>"` rather than assuming contiguity.)

Human-readable class name from a numeric type:
```cpp
static const char* vtkCellTypes::GetClassNameFromTypeId(int typeId);   // e.g. 12 -> "vtkHexahedron"
static int         vtkCellTypes::GetTypeIdFromClassName(const char* classname);
static int         vtkCellTypes::IsLinear(int type);
```
**Deprecation note (verify against 9.6 headers):** web search results
indicate `vtkCellTypes::GetClassNameFromTypeId` was marked **deprecated as
of VTK 9.6.0** in favor of a `vtkCellTypeUtilities` equivalent (presumably
`vtkCellTypeUtilities::GetClassNameFromTypeId` or similar). Since this
project targets exactly 9.6.2, **check whether `vtkCellTypeUtilities.h`
exists in the installed headers** and prefer it if so, falling back to the
deprecated `vtkCellTypes` static only if `vtkCellTypeUtilities` isn't
present (it may still work, just emit a deprecation warning through
`vtkOutputWindow` — see §8 for suppressing that noise).

---

## 4. Field data / attributes

### 4.1 Getting to the attribute containers

```cpp
vtkPointData* vtkDataSet::GetPointData();   // per-point arrays; vtkPointData : public vtkDataSetAttributes
vtkCellData*  vtkDataSet::GetCellData();    // per-cell arrays;  vtkCellData  : public vtkDataSetAttributes
```
`vtkDataSetAttributes : public vtkFieldData` adds the notion of "active"
attribute roles (§4.5) on top of plain array storage; `vtkPointData` and
`vtkCellData` add no further API of their own beyond a few convenience
overrides — nearly everything you need is declared on `vtkFieldData`/
`vtkDataSetAttributes`.

### 4.2 Enumerating arrays — `vtkFieldData`

```cpp
int GetNumberOfArrays();                                  // vtkFieldData.h
const char* GetArrayName(int i);
vtkDataArray*      GetArray(int i);                        // nullptr if array i is NOT a vtkDataArray (e.g. it's a vtkStringArray!)
vtkDataArray*      GetArray(const char* name);
vtkAbstractArray*  GetAbstractArray(int i);                // always non-null for a valid index, regardless of concrete subtype
vtkAbstractArray*  GetAbstractArray(const char* name, int& index);  // also returns the index found, -1 if not found
vtkTypeBool        HasArray(const char* name);
```

**The string-array trap:** `GetArray(int)`/`GetArray(const char*)` are typed
to return `vtkDataArray*` — the base class of purely *numeric* arrays
(`vtkFloatArray`, `vtkDoubleArray`, `vtkIntArray`, `vtkIdTypeArray`, …).
`vtkStringArray` (and `vtkVariantArray`, `vtkUnicodeStringArray`) do **not**
derive from `vtkDataArray` — they derive from `vtkAbstractArray` directly.
So `GetArray(i)` **silently returns `nullptr`** for a string-typed field —
it is not an error, and if you only ever call `GetArray()` in a loop you
will silently skip every string column. **Always enumerate with
`GetAbstractArray(i)` and check `GetArrayType()`/`GetDataType()`** (see
§4.3), and only narrow to `vtkDataArray` (via `vtkArrayDownCast<vtkDataArray>`
or a `dynamic_cast`) once you know the array is numeric; use
`vtkStringArray::SafeDownCast(field->GetAbstractArray(i))` for `VTK_STRING`
columns.

### 4.3 Per-array metadata — `vtkAbstractArray`

```cpp
int       GetNumberOfComponents() const;
vtkIdType GetNumberOfTuples() const;
virtual int GetDataType() const;              // a VTK_* constant, see §5 — works for BOTH vtkDataArray and vtkStringArray etc.
virtual int GetDataTypeSize() const = 0;      // sizeof one component's underlying storage element, in bytes (0 for variable-size types like VTK_STRING)
virtual const char* GetComponentName(vtkIdType component) const;  // e.g. "X"/"Y"/"Z" if the writer set them; often nullptr
void SetComponentName(vtkIdType component, const char* name);
virtual char* GetName();
virtual int GetArrayType() const;             // discriminates concrete C++ family: AbstractArray, DataArray,
                                               // AoSDataArrayTemplate, SoADataArrayTemplate, TypedDataArray,
                                               // StringArray, VariantArray, ... (exact enumerator names/values:
                                               // verify against vtkAbstractArray.h; used to pick a fast-path cast)
```
Source: [`vtkAbstractArray`](https://vtk.org/doc/nightly/html/classvtkAbstractArray.html).

### 4.4 Typed value access — pick the right strategy

Four ways to pull numeric values out, from slowest/safest to fastest/most
error-prone:

1. **`vtkDataArray::GetComponent(vtkIdType tupleIdx, int compIdx) -> double`**
   — universally available on every numeric array regardless of concrete
   type, but **lossy**: it always goes through a `double`, so a
   `VTK_ID_TYPE`/`VTK_LONG_LONG`/`VTK_UNSIGNED_LONG_LONG` array with values
   beyond ±2^53 (double's exact-integer range) **silently loses precision**.
   Fine for float/double/short-integer scalar fields; **not acceptable**
   for large 64-bit global/cell ids or similarly wide integer data — which
   is exactly the kind of column a CAE mesh reasonably has (e.g. large
   global point/cell ids from a partitioned solver run).

2. **`GetVoidPointer(vtkIdType valueIdx)` + `switch (array->GetDataType())`**
   — reinterpret the void* according to the runtime type constant (`VTK_INT`
   → `int*`, `VTK_ID_TYPE` → `vtkIdType*`, etc.) and index directly. Fast
   and precision-correct, **but only valid if the array is contiguous
   array-of-structs (AOS) storage** — i.e. `vtkAOSDataArrayTemplate<T>`. It
   is **not valid** for `vtkSOADataArrayTemplate<T>` (structure-of-arrays:
   each component stored in its own separate buffer) because there the
   "tuple stride" assumption behind flat void-pointer indexing breaks for
   multi-component arrays. Guard with
   `array->GetArrayType() == vtkAbstractArray::AoSDataArrayTemplate` (verify
   exact enumerator name) before using this path, or restrict it to
   single-component arrays where AOS vs SOA is moot.

3. **`vtkGenericDataArray<DerivedT,ValueType>::GetTypedComponent(tuple, comp) -> ValueType`**
   (the CRTP base every concrete `vtkAOSDataArrayTemplate<T>` /
   `vtkSOADataArrayTemplate<T>` inherits) — type-correct **and** SOA-safe,
   but you must already know the concrete `T` at the call site, i.e. you
   need the array downcast to its concrete template instantiation first
   (`vtkAOSDataArrayTemplate<vtkIdType>::SafeDownCast(...)`, etc.) — which is
   exactly what `vtkArrayDispatch` automates for you (#4 below).

4. **`vtkArrayDispatch::Dispatch::Execute(array, functor)`** — **the
   recommended strategy for this project.** You write one templated functor
   whose `operator()` is instantiated by the dispatcher for every concrete
   array type VTK considers "common" (all the `vtkAOSDataArrayTemplate<T>`
   and `vtkSOADataArrayTemplate<T>` instantiations for the built-in numeric
   `T`s), so inside the functor you have a real `ArrayType*` and can safely
   call `GetTypedComponent`/`GetValue` at full precision and near-native
   speed (the compiler generates a dedicated code path per type — no
   virtual-call-per-value overhead), while `Dispatch::Execute` handles the
   AOS/SOA distinction transparently. This gets you both correctness
   (64-bit ints exact) and speed (no `GetComponent` double round-trip, no
   per-element virtual dispatch). Use `vtkArrayDispatch::AllTypes` (or the
   narrower `vtkArrayDispatch::Reals`/`Integrals` lists) as the dispatcher's
   type-list parameter if you want to keep compile times/binary size down.
   `vtkTemplateMacro` (a preprocessor switch over `GetDataType()`) is the
   older, manual equivalent of what `vtkArrayDispatch` generates — reach for
   it only if you need a fallback path for exotic array subtypes the
   dispatcher's default type-list doesn't cover.

**Recommended concrete pattern** — enumerate every array as an
`vtkAbstractArray*`, branch once on `GetDataType()`/dynamic-cast for
string vs numeric, then for numeric arrays run one dispatch per array with a
functor that writes into your DuckDB vector using the correct native width
(e.g. `int64_t` for `VTK_ID_TYPE`/`VTK_LONG_LONG`, `uint64_t` for the
unsigned 64-bit types, `double`/`float` for reals):

```cpp
#include <vtkArrayDispatch.h>
#include <vtkDataArray.h>
#include <vtkStringArray.h>
#include <vtkAbstractArray.h>
#include <vtkFieldData.h>
#include <vtkAssume.h>

// A generic "append every tuple of this array as a row batch" functor.
// Sink is whatever your DuckDB column-append abstraction is.
struct AppendArrayWorker
{
  template <typename ArrayType>
  void operator()(ArrayType* array, ColumnSink& sink) const
  {
    // ArrayType::ValueType is the exact C++ type (int64_t, float, ...)
    // GetTypedComponent is correct for BOTH AOS and SOA storage.
    const vtkIdType nTuples = array->GetNumberOfTuples();
    const int nComps = array->GetNumberOfComponents();
    for (vtkIdType t = 0; t < nTuples; ++t)
      for (int c = 0; c < nComps; ++c)
        sink.Append(t, c, static_cast<typename ArrayType::ValueType>(
                              array->GetTypedComponent(t, c)));
  }
};

void ReadOneArray(vtkFieldData* fd, int arrayIndex, ColumnSink& sink)
{
  vtkAbstractArray* abs = fd->GetAbstractArray(arrayIndex);
  if (auto* str = vtkStringArray::SafeDownCast(abs)) {
    for (vtkIdType t = 0; t < str->GetNumberOfTuples(); ++t)
      sink.AppendString(t, str->GetValue(t));
    return;
  }
  if (auto* numeric = vtkDataArray::SafeDownCast(abs)) {
    AppendArrayWorker worker;
    using Dispatcher = vtkArrayDispatch::Dispatch;
    if (!Dispatcher::Execute(numeric, worker, sink)) {
      // Fell through the compiled type list (rare/exotic array subtype):
      // fall back to a precision-safe generic path, e.g. vtkTemplateMacro
      // switching on numeric->GetDataType(), or GetVoidPointer()+switch
      // guarded by GetArrayType()==AoSDataArrayTemplate for single-component.
      worker(numeric, sink);  // vtkDataArray itself also has GetTypedComponent-like
                               // access via GetComponent (lossy) as last resort
    }
  }
}
```

**AOS vs SOA in practice:** VTK readers (legacy and XML alike) almost always
produce **AOS** arrays (`vtkAOSDataArrayTemplate<T>`, i.e. `vtkFloatArray`,
`vtkDoubleArray`, `vtkIntArray`, `vtkIdTypeArray`, ... are all typedefs for
AOS instantiations) because that's what the file formats store — SOA arrays
are mainly produced by in-memory filters/zero-copy interop (e.g. numpy/VTK
bridges) rather than by file readers. Still, always go through
`vtkArrayDispatch`/`GetTypedComponent` rather than assuming AOS, since a
`vtkHDFReader` or a future format could plausibly hand back SOA-backed
arrays, and the dispatch-based code is correct either way at negligible
extra cost.

### 4.5 "Active" attribute roles

`vtkDataSetAttributes` (base of `vtkPointData`/`vtkCellData`) tracks which
array (if any) currently plays each of several special semantic roles:
```cpp
vtkDataArray*     GetScalars();      // active SCALARS array (or nullptr)
vtkDataArray*     GetVectors();      // active VECTORS (3-component)
vtkDataArray*     GetNormals();      // active NORMALS (3-component, unit length by convention)
vtkDataArray*     GetTCoords();      // active texture coordinates
vtkDataArray*     GetTensors();      // active TENSORS (9-component)
vtkDataArray*     GetGlobalIds();    // active GLOBALIDS
vtkAbstractArray* GetPedigreeIds();  // active PEDIGREEIDS (may be a vtkVariantArray/vtkStringArray, hence AbstractArray return)
int SetActiveScalars(const char* name);   // (and SetActiveVectors/Normals/... siblings)
```
`AttributeTypes` enum (`vtkDataSetAttributes.h`): `SCALARS=0, VECTORS=1,
NORMALS=2, TCOORDS=3, TENSORS=4, GLOBALIDS=5, PEDIGREEIDS=6, EDGEFLAG=7,
TANGENTS=8, RATIONALWEIGHTS=9, HIGHERORDERDEGREES=10, PROCESSIDS=11`.
To report "which array is active" for a given role in your schema/metadata
output, compare each `GetXxx()` pointer's identity (or its `GetName()`)
against every array from `GetAbstractArray(i)` while enumerating — there is
no reverse "what role is array i" query, you must build that mapping
yourself by checking pointer equality against each `GetScalars()`/
`GetVectors()`/etc. call once per attributes object.

### 4.6 Dataset-level field data

`vtkDataObject::GetFieldData() -> vtkFieldData*` (declared on
`vtkDataObject`, the base of `vtkDataSet` itself) holds arrays that describe
the *whole* dataset rather than any individual point or cell — e.g. a
solver's global metadata (time value, iteration count, case name) written
as a `FIELD` block in legacy files or a `<FieldData>` element in XML files.
Same `GetNumberOfArrays()`/`GetAbstractArray(i)` API as §4.2 applies; expose
these as a separate small "dataset metadata" table/row rather than trying to
fold them into the point/cell tables.

---

## 5. Type mapping table (VTK → C++ → DuckDB)

All `VTK_*` constants below are from `Common/Core/vtkType.h`.

| VTK constant | C++ type | Size | Recommended DuckDB `LogicalType` |
|---|---|---|---|
| `VTK_VOID` | `void` | — | n/a |
| `VTK_BIT` | packed bits (`vtkBitArray`, 1 bit/value) | 1 bit | `BOOLEAN` |
| `VTK_CHAR` | `char` (signedness is platform-defined!) | 1 B | `TINYINT` if `char` is signed on the build platform (Linux/x86-64/ARM with GCC/Clang: yes), else treat as `UTINYINT` — check `std::is_signed<char>` at compile time rather than assuming |
| `VTK_SIGNED_CHAR` | `signed char` | 1 B | `TINYINT` |
| `VTK_UNSIGNED_CHAR` | `unsigned char` | 1 B | `UTINYINT` |
| `VTK_SHORT` | `short` | 2 B | `SMALLINT` |
| `VTK_UNSIGNED_SHORT` | `unsigned short` | 2 B | `USMALLINT` |
| `VTK_INT` | `int` | 4 B | `INTEGER` |
| `VTK_UNSIGNED_INT` | `unsigned int` | 4 B | `UINTEGER` |
| `VTK_LONG` | `long` (4 B on Windows LLP64, 8 B on Linux/macOS LP64!) | 4 or 8 B | `BIGINT` (safe superset; or `INTEGER` only if you've confirmed 4-byte `long` on the target platform — don't do this, just always use `BIGINT`) |
| `VTK_UNSIGNED_LONG` | `unsigned long` | 4 or 8 B | `UBIGINT` |
| `VTK_LONG_LONG` | `long long` | 8 B | `BIGINT` |
| `VTK_UNSIGNED_LONG_LONG` | `unsigned long long` | 8 B | `UBIGINT` |
| `VTK_FLOAT` | `float` | 4 B | `FLOAT` |
| `VTK_DOUBLE` | `double` | 8 B | `DOUBLE` |
| `VTK_ID_TYPE` | `vtkIdType` — **`int` (32-bit) or `long long` (64-bit) depending on the VTK build**, see below | 4 or 8 B | `INTEGER` if 32-bit build, `BIGINT` if 64-bit build — detect at compile time, don't hardcode |
| `VTK_STRING` | `std::string`/`vtkStdString` (via `vtkStringArray`, **not** `vtkDataArray`!) | variable | `VARCHAR` |
| `VTK_UNICODE_STRING` *(legacy/deprecated in modern VTK)* | — | variable | `VARCHAR` |
| `VTK_VARIANT` | `vtkVariant` (via `vtkVariantArray`) | variable | `VARCHAR` (stringify) or a tagged union if you need full fidelity — recommend stringify for simplicity |
| `VTK_OBJECT` | `vtkObjectBase*` | — | not representable as a scalar column; skip/ignore |
| `VTK_TYPE_INT8` | `int8_t` (alias family alongside the "classic" names above, same as `VTK_SIGNED_CHAR` numerically) | 1 B | `TINYINT` |
| `VTK_TYPE_UINT8` | `uint8_t` | 1 B | `UTINYINT` |
| `VTK_TYPE_INT16` | `int16_t` | 2 B | `SMALLINT` |
| `VTK_TYPE_UINT16` | `uint16_t` | 2 B | `USMALLINT` |
| `VTK_TYPE_INT32` | `int32_t` | 4 B | `INTEGER` |
| `VTK_TYPE_UINT32` | `uint32_t` | 4 B | `UINTEGER` |
| `VTK_TYPE_INT64` | `int64_t` | 8 B | `BIGINT` |
| `VTK_TYPE_UINT64` | `uint64_t` | 8 B | `UBIGINT` (careful: values above `INT64_MAX` need `UBIGINT`, not `BIGINT`, or you'll silently wrap/misread — DuckDB's `UBIGINT` covers the full unsigned range; only escalate to `HUGEINT` if you must round-trip a `VTK_TYPE_UINT64` value that legitimately exceeds `UINT64_MAX`, which cannot happen, so `UBIGINT` is always sufficient here) |
| `VTK_TYPE_FLOAT32` | `float` | 4 B | `FLOAT` |
| `VTK_TYPE_FLOAT64` | `double` | 8 B | `DOUBLE` |

`HUGEINT`/`UHUGEINT` (128-bit) are **not needed** for any native VTK numeric
type — VTK has nothing wider than 64-bit integers, so reserve `HUGEINT` only
if you decide to losslessly represent `VTK_ID_TYPE`-based *derived*
computations (e.g. a hash) that you generate yourself, not for raw field
data.

### `VTK_ID_TYPE` 32- vs 64-bit detection

`vtkIdType` (used for all point/cell ids, `vtkIdList`, `GetNumberOfPoints()`
return, etc.) is a build-time typedef:
```cpp
// Common/Core/vtkType.h (paraphrased from long-standing VTK convention)
#if VTK_SIZEOF_ID_TYPE == 4
typedef int vtkIdType;
#elif VTK_SIZEOF_ID_TYPE == 8
typedef long long vtkIdType;
#endif
```
Detect this at your extension's compile time (not runtime) via the
preprocessor macros VTK's own headers define:
```cpp
#include <vtkType.h>
#if VTK_SIZEOF_ID_TYPE == 8
  // 64-bit vtkIdType build — map to DuckDB BIGINT
#else
  // 32-bit vtkIdType build — still safe to widen to BIGINT, just don't assume 8 bytes when sizing buffers
#endif
// VTK_USE_64BIT_IDS is the CMake option name that controls this (ON by default
// in modern VTK/Homebrew builds targeting large meshes); VTK_SIZEOF_ID_TYPE
// is the resulting compile-time constant to branch on in code.
```
Homebrew's VTK 9.6.2 (like essentially every current distro/vcpkg/conda
build) is virtually certain to have `VTK_USE_64BIT_IDS=ON` (i.e. 8-byte
`vtkIdType`) since 32-bit ids can't address meshes beyond ~2 billion
points/cells and this has been the default for large-mesh-capable builds
for many VTK releases — **but confirm by grepping
`vtkType.h`/`vtkBuildInformation.h` in the installed headers** rather than
assuming. Regardless of `vtkIdType`'s width, always widen point/cell-id
output columns to DuckDB `BIGINT`, so your extension behaves identically
against a hypothetical 32-bit-id VTK build too.

### Representing multi-component arrays (vectors, tensors, etc.)

Three options, with trade-offs:

1. **Separate scalar columns** (`v_x DOUBLE, v_y DOUBLE, v_z DOUBLE` for a
   3-vector; `t_00..t_22` for a 9-component tensor). *Pros:* simplest SQL
   ergonomics (`SELECT v_x FROM ...`, works with every DuckDB feature,
   trivially indexable/filterable); no nesting to unpack. *Cons:* awkward
   for arbitrary/variable component counts (a 36-component higher-order
   tensor field needs 36 columns), naming scheme is arbitrary beyond 3-4
   components, schema differs per array (no uniform "value" column type
   across differently-shaped fields).
2. **Fixed-size list column** (`DOUBLE[3]` for vectors, or `DOUBLE[]` if you
   don't want to fix the size in the schema). *Pros:* one column per VTK
   array regardless of component count, matches VTK's own tuple model
   exactly, trivial to implement generically (append `GetNumberOfComponents()`
   values per row into a `duckdb::list_vector`). *Cons:* consumers must use
   list-indexing/`list_extract`/`UNNEST` syntax; less immediately
   spreadsheet-friendly; loses per-component names (`GetComponentName`) —
   unless you expose those separately as array-level metadata.
3. **STRUCT column** (`STRUCT(x DOUBLE, y DOUBLE, z DOUBLE)`), populated
   from `GetComponentName(i)` when present, else generic `component_0..n-1`.
   *Pros:* keeps named-field ergonomics (`.x`) while still being a single
   column per array; natural fit when `GetComponentName` metadata exists
   (common for XML-writer-tagged vector fields) or you know the
   scientific convention (velocity xyz, stress tensor). *Cons:* variable
   component-name availability makes it inconsistent across files/arrays
   (falls back to synthetic names often); still awkward for very
   high-component-count fields (tensors, higher-order Lagrange
   coefficients).

**Recommendation:** default to a **fixed-size `LIST`/array column**
(`col_type[ncomp]`) for every field-data array uniformly — this is the only
option that requires zero special-casing across scalar (`ncomp==1`, which
you can just emit as a plain scalar column, not a 1-element list),
vector, tensor, and arbitrary-N-component arrays, and it round-trips
`GetNumberOfComponents()` exactly. Layer a **STRUCT** rendering on top only
for the well-known small cases where `GetVectors()`/`GetNormals()` are the
*active* array and have exactly 3 components with recognizable names (or
none) — i.e. offer both, but make the generic list form the default/always-
available path and the STRUCT/x-y-z-columns form an optional convenience
view for the common 3-vector case. Do not attempt separate named columns as
the general mechanism — it doesn't generalize past 3-4 components and forces
per-array schema special-casing that conflicts with a single uniform "one
row per point/cell, N field-data columns" table model.

---

## 6. Reading efficiently / streaming / schema discovery

**Short answer: `reader->Update()` always materializes the entire
requested piece/extent into RAM as concrete VTK arrays — there is no
"streaming row cursor" API comparable to a database cursor.** VTK's
pipeline model supports requesting a *subset* (a piece, a spatial extent, a
single timestep) so you don't have to load an entire multi-GB parallel
dataset in one call, but whatever subset you do request is read completely
before `Update()` returns; there's no partial/incremental "read the next
1000 rows" primitive within a single piece. For `duck_vtk`'s purposes this
means: use piece/extent/timestep requests to avoid reading *more* than one
logical chunk at a time (e.g. one `.vtu` piece file referenced by a
`.pvtu`, or one timestep of a `.pvd`/time-aware XML file), but expect each
such chunk, once requested, to be fully RAM-resident before you can iterate
its arrays.

### 6.1 Schema discovery *without* reading data

This is the critical capability for a DuckDB `ATTACH`/schema-introspection
step. Call **`UpdateInformation()`** (declared on `vtkAlgorithm`, inherited
by every reader) instead of `Update()`:
```cpp
virtual bool UpdateInformation(); // vtkAlgorithm.h — runs only the REQUEST_INFORMATION pipeline pass
```
This runs each reader's `RequestInformation()` override, which for XML
readers means parsing just the `<VTKFile>`/`<Piece>` structural metadata
(via `vtkXMLDataElement`, see `vtkXMLReader::ReadXMLInformation()`) —
**not** decoding any `DataArray` payload bytes — and for legacy readers
means reading the header + a lightweight pre-scan (legacy format doesn't
XML-tag array metadata the same way, so the legacy readers' "cheap
metadata" pass is less complete than XML's — expect to need a fuller parse
for legacy files to get all array names/types, though point/cell *counts*
are available cheaply since they're stated early in the file).

After `UpdateInformation()`:
- **Array names, types, and component counts**: available via
  `reader->GetPointDataArraySelection()` /
  `reader->GetCellDataArraySelection()` (`vtkXMLReader` methods) which
  expose `GetNumberOfArrays()`/`GetArrayName(i)` for the *point* and *cell*
  attribute sets **without requiring `Update()`** — this is populated
  during `RequestInformation()` for XML readers by parsing the `Name`/
  `type`/`NumberOfComponents` attributes straight out of the `<DataArray>`
  XML tags (no payload decode needed since those are all element
  *attributes*, not element *content*). This is exactly the cheap
  "what columns exist and what are their types" query your `ATTACH`/schema
  step needs.
- **Point/cell counts**: for XML structured types (`.vti/.vtr/.vts`), the
  whole-extent is in `reader->GetOutputInformation(0)` as the standard
  `vtkDataObject::DATA_EXTENT()` key populated during
  `RequestInformation()` — no data read needed, extent arithmetic gives you
  exact point/cell counts. For unstructured types (`.vtu/.vtp`), exact
  global point/cell counts require summing each `<Piece
  NumberOfPoints="..." NumberOfCells="...">` attribute — which **is**
  present as plain XML attributes on the `<Piece>` element and thus **is**
  obtainable from `RequestInformation()`/`ReadXMLInformation()` parsing
  alone, without decoding any `DataArray` payload. So: **yes, you can get
  point/cell counts and full array name/type/component-count schema without
  reading bulk data**, for both structured and unstructured XML formats.
  Legacy `.vtk` files are read progressively top-to-bottom with no equivalent
  upfront "totals" header for point/cell counts beyond what's stated at the
  `POINTS n dataType`/`CELLS n size` line itself — those lines occur before
  any bulk numeric payload, so a lightweight partial-parse (stop right after
  reading the `n` on that line) gives you counts cheaply too, but VTK's own
  `vtkDataReader` does not expose a public "just tell me the counts" call
  distinct from a full read — you'd need to do that partial-parse yourself
  or accept a full `Update()` for legacy files if such a helper doesn't
  exist in the class (**verify**: check for any
  `vtkDataReader::Read*Header`-style helpers in the installed header before
  concluding you must hand-roll this).
- **`GetOutputInformation(0)`** — general entry point: `vtkInformation*
  vtkAlgorithm::GetOutputInformation(int port)`, holds pipeline metadata keys
  (`vtkDataObject::DATA_EXTENT()`, `vtkDataObject::DATA_TIME_STEP()`,
  `vtkStreamingDemandDrivenPipeline::TIME_STEPS()`, etc.) populated by
  `UpdateInformation()` alone.

### 6.2 Piece / extent / timestep partial reads

```cpp
// vtkAlgorithm.h
virtual bool Update();                       // whole default request
virtual bool Update(int port);
virtual int  UpdatePiece(int piece, int numPieces, int ghostLevels, const int extents[6] = nullptr);
virtual int  UpdateExtent(const int extents[6]);
```
(Note: some current Doxygen snapshots show `Update()` returning `bool`;
older VTK/most public tutorials show `void Update()`. **This return-type
detail changed in a relatively recent VTK release and must be re-verified
against the 9.6 header** — don't rely on the return value's truthiness
without confirming.)

`UpdatePiece(piece, numPieces, ghostLevels)` is the standard way to read
just one logical piece of a parallel-capable dataset (works for both a
`.pvtu`'s referenced pieces and — more generally — any reader that supports
piece-based `RequestUpdateExtent`). `reader->GetNumberOfPieces()` (or the
equivalent `vtkStreamingDemandDrivenPipeline::GetPieceCount` at pipeline
level) tells you how many logical pieces exist so you can iterate
`UpdatePiece(i, N, 0)` for `i in [0,N)` and process one piece's worth of
points/cells/arrays at a time — this is the closest thing VTK offers to
"streaming" a large parallel dataset without materializing the whole thing
across all pieces simultaneously.

### 6.3 Time series

```cpp
// vtkXMLReader.h (and mirrored on vtkAlgorithm's information-key mechanism)
virtual int  GetNumberOfTimeSteps();
virtual int  GetTimeStep();
virtual int* GetTimeStepRange();
virtual void SetTimeStep(int);
virtual void SetTimeStepRange(int, int);
```
Time-aware sources (a `.pvd` via `vtkPVDReader`, or a single XML file that
embeds a `TimeValue`/multiple timesteps) populate
`vtkStreamingDemandDrivenPipeline::TIME_STEPS()` /
`TIME_RANGE()` info keys during `UpdateInformation()`; to fetch one
timestep's data you set the requested time on the output information
(`UPDATE_TIME_STEP()` key) or call the reader's `SetTimeStep`/pipeline
`SetUpdateTimeStep` equivalent, then `Update()` — again materializing just
that one timestep's arrays, not the whole series.

---

## 7. CMake integration

### 7.1 Modern VTK 9 `find_package` usage

```cmake
find_package(VTK 9.6 REQUIRED COMPONENTS
  CommonCore
  CommonDataModel
  CommonExecutionModel   # vtkAlgorithm / pipeline base — usually pulled in transitively but harmless to be explicit
  IOCore                 # vtkErrorCode, base IO utilities
  IOLegacy                # vtk*Reader (legacy .vtk) + vtkGenericDataObjectReader / vtkDataSetReader
  IOXML                    # vtkXML*Reader (.vtu/.vtp/.vts/.vtr/.vti/.vtm) + vtkXMLGenericDataObjectReader + vtkPVDReader
  IOXMLParser              # underlying XML parsing support IOXML depends on
  IOHDF                    # vtkHDFReader (.vtkhdf/.hdf) — only if VTKHDF support is wanted
  # Optional CAE-specific components, add only what you need (each pulls its own dependency chain):
  # IOGeometry             # vtkOpenFOAMReader, vtkPLOT3DReader
  # IOEnSight              # vtkGenericEnSightReader family
  # IOCGNS                 # vtkCGNSReader (needs system/brew cgns lib)
  # IOExodus               # vtkExodusIIReader (needs netcdf/hdf5)
)
```
Minimal component set for the "read + introspect points/cells/field-data"
core of `duck_vtk` (legacy + XML serial + XML parallel, no CAE-specific
readers yet): `CommonCore`, `CommonDataModel`, `IOLegacy`, `IOXML`,
`IOXMLParser`. Add `IOHDF` when VTKHDF support lands, and the CAE-specific
components (`IOGeometry`/`IOEnSight`/`IOCGNS`/`IOExodus`) incrementally per
format you choose to support.

### 7.2 `vtk_module_autoinit` — what it does and why it's mandatory

VTK 9's module system uses **runtime object-factory registration** for a lot
of its polymorphism: many "generic" classes (readers that need to hand back
a concrete `vtkDataArray` subtype for a given `VTK_*` type, rendering
backends, parallel/MPI controller selection, etc.) are resolved at *run
time* by asking a factory "give me the best available implementation of X",
and the concrete implementations register themselves with that factory via
static-initializer code that runs when the implementing module's shared
library is loaded/linked in. Because C++ has no portable guarantee that a
transitively-linked shared library's static initializers actually run
before your `main()` executes registration-dependent code (and because
which modules are even linked together is only known at the *consuming*
target's `target_link_libraries()` call, not at VTK-build time), VTK cannot
bake this registration into the libraries themselves — it needs
target-specific glue code generated for **your** binary that explicitly
forces the right set of module-init functions to run.

`vtk_module_autoinit(TARGETS <tgt>... MODULES <module>...)` is exactly that
glue-code generator: given the *actual* set of linked modules for target
`<tgt>`, it walks each module's declared factory/implements relationships
and generates (via `target_sources`/generated headers under the hood) the
autoinit translation unit that calls each needed
`vtkFooModule_AutoInit`/factory-registration entry point before your `main`
executes, ensuring every module you link actually gets its factory
overrides installed.

**What breaks without it:** your code compiles and links fine, but at run
time, factory-mediated lookups **silently fail to find the implementation
you linked in** — the most common symptom for I/O code is a reader/writer
object that constructs successfully but produces empty/wrong output, or (for
rendering-touching code, less relevant here but the classic complaint on
the VTK forums) errors like "vtkOpenGLRenderWindow ... no override found"
/ blank render windows — because the concrete class never got registered
with `vtkObjectFactory`, so `vtkXxx::New()` falls back to whatever
default/no-op is compiled into the abstract base, if any, or fails
outright. For `duck_vtk` specifically, skipping `vtk_module_autoinit` is a
classic way to get readers whose `New()` succeeds but whose actual concrete
behavior is missing/wrong in ways that are easy to misdiagnose as an
application bug rather than a missing linker-glue step.

```cmake
find_package(VTK 9.6 REQUIRED COMPONENTS CommonCore CommonDataModel IOLegacy IOXML IOXMLParser)

add_library(duck_vtk MODULE duck_vtk.cpp ...)   # a DuckDB loadable extension is typically a shared/module library
target_link_libraries(duck_vtk PRIVATE ${VTK_LIBRARIES})

# Must be called AFTER the target exists and link libraries are set, and
# MODULES must list the exact same VTK libraries you linked:
vtk_module_autoinit(
  TARGETS duck_vtk
  MODULES ${VTK_LIBRARIES}
)
```
(VTK's own CMake docs/examples put `vtk_module_autoinit` *after*
`target_link_libraries` in most current examples, though the exact ordering
requirement is "must know the target and its VTK module list", which
`target_link_libraries` establishes — **double check the canonical ordering
against `VTKConfig.cmake`'s own comments once installed**, since one of the
two web sources fetched during this research showed autoinit called before
`target_link_libraries` and current upstream examples typically show it
after; either may be technically fine since `vtk_module_autoinit` just
generates sources/compile definitions rather than depending on already-
resolved link information, but match upstream's own documented order to be
safe.)

### 7.3 Where brew puts the CMake config, and shared-lib/RPATH implications

Homebrew installs the VTK CMake package files under
`/home/linuxbrew/.linuxbrew/opt/vtk/lib/cmake/vtk-9.6/` (the standard
`<prefix>/lib/cmake/vtk-<major>.<minor>/VTKConfig.cmake` +
`vtk-config.cmake` + per-module `*-targets.cmake`/`*-targets-release.cmake`
layout every recent VTK release uses) — **confirm exact directory name
once installed**:
```
ls /home/linuxbrew/.linuxbrew/opt/vtk/lib/cmake/
```
Point `VTK_DIR` (or `CMAKE_PREFIX_PATH`) at that directory (or its parent
`opt/vtk`) if `find_package(VTK ...)` doesn't discover it automatically:
```
cmake -DVTK_DIR=/home/linuxbrew/.linuxbrew/opt/vtk/lib/cmake/vtk-9.6 ...
# or simply:
cmake -DCMAKE_PREFIX_PATH=/home/linuxbrew/.linuxbrew/opt/vtk ...
```
Homebrew's bottled VTK is **built shared** (per the formula: "Bottle Size:
72.7MB / Installed Size: 350.1MB" for a from-source-capable multi-module
build strongly implies shared `.so` libraries rather than one giant static
archive, consistent with every other current Homebrew C++ library
convention) — expect `libvtkCommonCore-9.6.so`, `libvtkIOXML-9.6.so`, etc.
under `/home/linuxbrew/.linuxbrew/opt/vtk/lib/`. **Confirm with**
`ls /home/linuxbrew/.linuxbrew/opt/vtk/lib/*.so* | head` once installed.

**RPATH implications for a DuckDB loadable extension:** a DuckDB extension
is itself a dynamically-loaded module (`.duckdb_extension`, ultimately a
shared object) loaded into the running `duckdb` process at query time. If
your extension links against ~20+ separate `libvtkFoo-9.6.so` libraries
under a non-standard prefix (`/home/linuxbrew/.linuxbrew/opt/vtk/lib`) that
isn't on the default dynamic linker search path, the loader **must** be able
to find them at `dlopen()` time or the extension load fails with an
"undefined symbol"/"cannot open shared object file" error — this is not
optional given how many separate VTK module `.so`s a single reader chain
pulls in transitively. Two standard fixes: (1) bake an `RPATH`/`RUNPATH`
into your built extension pointing at the brew VTK lib dir (CMake does this
automatically for build-tree targets via `CMAKE_INSTALL_RPATH`/
`BUILD_RPATH` if you don't explicitly disable it — for a portable installed
extension you likely want `set(CMAKE_INSTALL_RPATH
"/home/linuxbrew/.linuxbrew/opt/vtk/lib")` or a `$ORIGIN`-relative path if
you vendor the `.so`s alongside the extension), or (2) rely on the end
user's environment already having brew's `lib` dir on `LD_LIBRARY_PATH`
(fragile — don't rely on this for a distributable extension). Given brew's
non-FHS install prefix, **explicit RPATH embedding is the safer default**
for anyone loading this extension outside a shell that's already sourced
brew's environment.

---

## 8. Error handling and output-window suppression

### 8.1 Detecting read failure

```cpp
// vtkAlgorithm.h (inherited by every reader)
unsigned long vtkAlgorithm::GetErrorCode();   // returns a vtkErrorCode::ErrorIds value, 0 (vtkErrorCode::NoError) on success

// vtkErrorCode.h
static const char* vtkErrorCode::GetStringFromErrorCode(unsigned long error);
```
`ErrorIds` enum values of interest (`vtkErrorCode.h`): `NoError = 0`, then a
block starting at `FirstVTKErrorCode = 20000` covering
`FileNotFoundError`, `CannotOpenFileError`, `UnrecognizedFileTypeError`,
`PrematureEndOfFileError`, `FileFormatError`, `NoFileNameError`,
`OutOfDiskSpaceError`, `UnknownError`, up to `UserError = 40000` reserved
for application-defined codes above that. Practical pattern:
```cpp
reader->SetFileName(path.c_str());
reader->Update();
if (reader->GetErrorCode() != vtkErrorCode::NoError) {
  throw std::runtime_error(std::string("VTK read failed: ") +
    vtkErrorCode::GetStringFromErrorCode(reader->GetErrorCode()));
}
```
Note `GetErrorCode()` reflects the *reader's own* detected errors (bad
header, missing file, malformed XML, etc.) — it does not necessarily catch
every possible malformed-but-parseable-enough case (e.g. some readers warn
via the output window in §8.2 rather than setting an error code for
recoverable oddities), so treat both signals as complementary.

### 8.2 Suppressing/redirecting VTK's output-window chatter

VTK routes *all* of its warnings/errors/debug text through a global
singleton `vtkOutputWindow` (`Common/Core/vtkOutputWindow.h`) rather than
directly to stderr — by default this prints to stderr (on POSIX) with a
"press Enter to continue" prompt-suppression toggle. For an embedded
DuckDB session you want neither the prompt behavior nor arbitrary stderr
spam from routine oddities (deprecated-API warnings — recall the
`vtkCellTypes` deprecation noted in §3.6 — or minor format quirks) leaking
into the user's terminal/log unexpectedly.

**Fastest blunt-force option** — turn off all VTK warning/error display
globally:
```cpp
#include <vtkObject.h>
vtkObject::GlobalWarningDisplayOff();   // suppresses ALL vtkErrorMacro/vtkWarningMacro output process-wide
// vtkObject::GlobalWarningDisplayOn(); to re-enable
```
This is coarse (it silences genuine errors too, not just noise), so prefer
it only if you're separately checking `GetErrorCode()`/return values for
correctness and just don't want VTK's own text hitting the terminal.

**Precise option** — install a custom `vtkOutputWindow` subclass that
routes messages into your extension's own logging (e.g. DuckDB's warning
mechanism, or drop entirely) instead of stderr, keeping the ability to
still see/programmatically inspect messages if desired:
```cpp
#include <vtkOutputWindow.h>
#include <vtkObjectFactory.h>
#include <vtkSmartPointer.h>
#include <string>

class DuckVtkOutputWindow : public vtkOutputWindow
{
public:
  static DuckVtkOutputWindow* New();
  vtkTypeMacro(DuckVtkOutputWindow, vtkOutputWindow);

  void DisplayText(const char* text) override {
    // route to your own logger instead of stderr; or simply drop it.
    // e.g. duckdb::Logger::Info(std::string("[vtk] ") + text);
  }
  void DisplayErrorText(const char* text) override {
    // still worth surfacing errors distinctly, e.g. as a DuckDB WARNING
  }
  void DisplayWarningText(const char* text) override { /* drop or downgrade */ }
  void DisplayGenericWarningText(const char* text) override { /* drop or downgrade */ }
  void DisplayDebugText(const char* text) override { /* drop */ }
};
vtkStandardNewMacro(DuckVtkOutputWindow);

// Install once, e.g. in your extension's Load() entry point:
void InstallVtkOutputWindow()
{
  vtkSmartPointer<DuckVtkOutputWindow> win = vtkSmartPointer<DuckVtkOutputWindow>::New();
  vtkOutputWindow::SetInstance(win);   // per vtkOutputWindow docs: caller retains ownership semantics via smart pointer;
                                        // do not call ->Delete() explicitly if held in a vtkSmartPointer that outlives the call
}
```
`vtkFileOutputWindow` (also shipped in `Common/Core`) is a ready-made
alternative if you'd rather redirect everything to a log file on disk
instead of writing your own subclass — set it as the instance the same way
via `vtkOutputWindow::SetInstance(...)` and call
`SetFileName(...)`/`AppendOn()` per its own API rather than writing a custom
subclass, if a plain file sink is all you need. Install whichever approach
you choose **once**, early (module load / first reader construction), since
`vtkOutputWindow::SetInstance` affects the process-wide singleton used by
every subsequent VTK call in that process, including from other threads.

---

## 9. Uncertainties / verify against installed VTK

`brew install vtk` had not completed by the time this research concluded
(polled `ls /home/linuxbrew/.linuxbrew/opt/vtk` several times; `brew info
vtk` shows 9.6.2 as bottled but not-yet-installed). Every item below should
be re-checked directly against
`/home/linuxbrew/.linuxbrew/opt/vtk/include/vtk-9.6/` headers as soon as
installation finishes, before relying on this document for implementation:

1. ~~`vtkUnstructuredGrid::GetCellTypesArray()`~~ — **partially RESOLVED in Phase 0.**
   `vtkDataSet::GetCellTypes(vtkCellTypes*)` is **deprecated** in 9.6:
   `vtkDataSet.h:183` says "Use `GetDistinctCellTypes(vtkCellTypes* types)` instead".
   Use `GetDistinctCellTypes`. Building against the old name emits
   `-Wdeprecated-declarations`, which matters if warnings-as-errors is on.
2. **`vtkCellTypes::GetClassNameFromTypeId`** — still unverified, but *moot*:
   design §5 mandates a static `VTK_*` name table rather than a runtime VTK call.
3. ~~`ReadOutputType` signature~~ — **RESOLVED in Phase 0.** It is an **instance**
   method: `int ReadOutputType(const char *name, bool &parallel)`. Verified working;
   see the correction box at the top of §2.
4. **`vtkAlgorithm::Update()` return type** — `bool`/`vtkTypeBool` vs. the
   long-standing `void` — this changed at some point in the VTK 9 series and
   the exact 9.6.2 signature needs confirming (affects whether you can use
   the return value as a success signal at all).
5. **`.pvd` reader class name** — `vtkPVDReader` (found via search/current
   docs) vs. the older/commonly-referenced `vtkXMLCollectionReader` name
   seen in older VTK/ParaView material. Confirm which header actually ships
   in 9.6.2's `IOXML` module, and whether both exist (one may be a
   deprecated alias).
6. **CAE reader presence in the brew bottle** — OpenFOAM/EnSight/CGNS/
   Exodus/PLOT3D reader availability was inferred from the brew formula's
   dependency list (`cgns`, `hdf5`, `netcdf`) plus general knowledge that
   OpenFOAM/EnSight readers have no external dependency; **not yet
   confirmed** by actually finding the headers/shared libraries. Run the
   `find`/`ls` commands given at the end of §1.6 once installed.
7. **VTKHDF (`vtkHDFReader`) exact capability set in 9.6.2** — the format
   itself is explicitly documented upstream as still evolving
   ("not considered complete"); the precise set of dataset types/features
   `vtkHDFReader` supports *in 9.6.2 specifically* (vs. a newer nightly)
   needs confirming against that version's release notes/header comments.
8. **`vtkAbstractArray::GetArrayType()` enumerator names** (`AoSDataArrayTemplate`
   / `SoADataArrayTemplate` / etc.) — used in §4.4's guard logic; exact
   enumerator spelling/values should be confirmed against
   `vtkAbstractArray.h` directly rather than the paraphrased doc-fetch
   summary used here.
9. **`vtk_module_autoinit` call ordering relative to
   `target_link_libraries`** — web sources disagreed on before-vs-after;
   confirm against the comments in the installed `VTKConfig.cmake`/
   `vtkModuleAPI.cmake`.
10. **Homebrew VTK bottle specifics** — shared-vs-static linkage, exact
    `lib/cmake/vtk-9.6/` directory name, and the precise list of enabled
    optional modules were all inferred from `brew info vtk` metadata (no
    install completed); confirm directly with `ls`/`otool`/`ldd`-equivalent
    once the package is present:
    ```
    ls /home/linuxbrew/.linuxbrew/opt/vtk/lib/cmake/
    ls /home/linuxbrew/.linuxbrew/opt/vtk/lib/*.so* | head -30
    find /home/linuxbrew/.linuxbrew/opt/vtk/include -maxdepth 2 -type d
    ```
11. **Legacy `vtkDataReader` "cheap counts without full read" capability**
    (§6.1) — no public VTK API was confirmed for extracting just
    point/cell counts from a legacy `.vtk` file without a full `Update()`;
    this may require a hand-rolled partial parse in `duck_vtk` itself, or
    there may be an existing helper not surfaced by the doc fetches used
    here — worth a header grep for anything on `vtkDataReader`/
    `vtkDataSetReader` before assuming you must write one.
