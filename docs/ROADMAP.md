# duck_vtk — formats and features not yet supported

**Status:** planning document. Nothing here is committed to a release.

Every claim about what a format needs was checked against **VTK 9.6.2's own module
graph** (`IO/<module>/vtk.module` in the Kitware/VTK tree), not inferred. Where a
module's dependency makes something impossible, that is stated as a blocker rather
than an effort estimate. `docs/design/03-architecture-and-roadmap.md` §6 Phase 4
predates this check and is optimistic in two places; those are called out below.

Effort sizes are engineering days for someone already familiar with the codebase:

| | |
|---|---|
| **S** | 1–2 days |
| **M** | 3–8 days |
| **L** | 2–4 weeks |
| **XL** | more than a month, or gated on a design decision not yet made |

---

## 1. The one thing that unblocks most of the list

Six of the formats below produce a **composite** VTK dataset —
`vtkMultiBlockDataSet` or `vtkPartitionedDataSetCollection` — rather than a
`vtkDataSet`. `src/vtk/vtk_dataset.cpp:491` currently rejects all of them:

```cpp
throw NotImplementedException(
    "duck_vtk: '%s' contains a %s, which is not a simple dataset. …");
```

That rejection is correct behaviour today (far better than silently reading block 0),
but it means **the composite relational model is the single highest-leverage piece of
work in this document**. Until it exists, `.vtm`, `.vtpc`, ExodusII, CGNS and EnSight
are all blocked behind the same missing abstraction; after it exists, several of them
are a reader-factory entry plus tests.

The design decision it requires — and the reason it is not just "more code" — is how
a composite maps onto a relational schema. Three options, none obviously right:

| Option | `ATTACH 'x.vtm' AS m` yields | Cost |
|---|---|---|
| **Schema per block** | `m.block_0.points`, `m.block_1.points`, … | Natural for heterogeneous blocks; `VtkCatalog` currently assumes exactly one schema (`main`), so L4 changes |
| **Extra `block_id` column** | `m.points` with `block_id` prepended to the key | One schema, trivially unionable, filterable. Breaks down when blocks have *different array sets* — which is the normal case for ExodusII |
| **Flat union + `vtk_blocks` table** | `m.points` unioned, plus a catalogue table | Middle ground; column set becomes the union of all blocks with NULLs |

Recommendation: **schema per block**, with a `vtk_blocks` catalogue table listing
block index, name, class and counts. It is the only option that stays honest when
blocks disagree about their arrays, and it composes with the existing six-table
schema instead of changing it. `block_id` as a column is the wrong default precisely
because the heterogeneous case is the common one in CAE.

**Effort: L.** Mostly L4 (multi-schema catalog) and L1 (uniform composite traversal).
This is the prerequisite line item; everything marked *(composite)* below assumes it
is done.

---

## 2. Formats — multi-file and composite

None of these need a new VTK module or a new external dependency. The readers are
**already in `IOXML`, which is already enabled**. The blockers are entirely our own.

| Format | VTK reader | Module | Blocker | Effort |
|---|---|---|---|---|
| `.vtm` multiblock | `vtkXMLMultiBlockDataReader` | IOXML ✅ | §1 composite model | M *(composite)* |
| `.vtpc` partitioned collection | `vtkXMLPartitionedDataSetCollectionReader` | IOXML ✅ | §1 composite model | S *(composite)* |
| `.pvtu` / `.pvtp` parallel | `vtkXMLPUnstructuredGridReader`, `vtkXMLPPolyDataReader` | IOXML ✅ | See below | M |
| `.pvd` time series | **none — see below** | — | No VTK reader exists | L |

### `.pvtu` / `.pvtp` — closer than it looks

Currently rejected by the `was_parallel` flag at `src/vtk/vtk_dataset.cpp:472`. The
readers exist and are linked; the work is not parsing but **piece-path resolution**.
A `.pvtu` names its pieces by relative path, and VTK resolves them with its own file
I/O — which bypasses DuckDB's virtual filesystem entirely. So a `.pvtu` on `s3://`
would have its manifest read through the VFS and its pieces read through VTK,
i.e. not at all.

Two ways out:

1. **Resolve pieces ourselves**: read the manifest through `VtkFileSource`, fetch each
   piece through the VFS, and feed the serial readers from memory — the in-memory path
   `ReadDataObjectFromMemory` already exists and is exercised by
   `DUCK_VTK_FORCE_MEMORY_READ=1`. Then append into one dataset, or treat pieces as
   blocks. **This is the right answer** and it reuses machinery already tested.
2. Only support `.pvtu` for local paths. Cheaper, but introduces a
   "works locally, fails on S3" cliff, which is worse than the current honest refusal.

Note the pieces of a `.pvtu` are homogeneous by construction (same arrays, same cell
types), so this is the one composite-ish case where **`piece_id` as a column is the
right model** rather than schema-per-block. It therefore does *not* depend on §1.

**Effort: M.**

### `.pvd` — no VTK reader exists

> **CORRECTION to `docs/design/03` §6 Phase 4**, which lists `.pvd` alongside formats
> that are "mostly a `VtkReaderFactory` entry once L1 is uniform". That is wrong.

`.pvd` is a **ParaView** format. `vtkPVDReader` lives in ParaView, not VTK. Verified
directly: searching the entire VTK 9.6.2 source tree for paths matching `pvd`
(case-insensitive) returns **zero files**, and there is no `vtkXMLCollectionReader`
either — the only `*CollectionReader` in VTK is
`vtkXMLPartitionedDataSetCollectionReader`, which reads `.vtpc`, a different format.

So supporting `.pvd` means writing the collection handling ourselves: parse the
`<Collection>` manifest (`timestep`, `part`, `file` attributes), resolve each
referenced file through the VFS, and dispatch to the existing readers. The XML is
simple and `expat` is already a dependency, so the parsing is not the hard part —
the **time dimension** is:

- Does `ATTACH 'anim.pvd'` expose one table with a `time_value` column spanning every
  timestep (potentially loading the whole series into memory — for a transient CFD run
  that is routinely tens of GB), or
- one schema per timestep, or
- a `timestep` ATTACH option selecting one?

This is the same class of decision as §1 and should be made with it. Loading
everything eagerly is not viable given `ATTACH` currently reads the full dataset into
memory.

**Effort: L**, and it should not start before §1 and lazy loading (§5) are settled.

---

## 3. Formats — need a VTK module enabled

Each of these needs one line in `vcpkg_ports/vtk-minimal/portfile.cmake`, the same
component added to `DUCK_VTK_REQUIRED_COMPONENTS` in `cmake/DuckVTKFindVTK.cmake`
(`scripts/submit_check.sh` fails if those two disagree), and a reader-factory entry.

**The good news, verified:** `hdf5`, `exodusII`, `cgns` and `netcdf` are all
**vendored inside VTK** as `VTK::hdf5`, `VTK::exodusII`, `VTK::cgns`, `VTK::netcdf`
(`ThirdParty/<name>/vtk.module`). So none of these formats requires a new entry in
`vcpkg.json`. The cost is VTK build time and binary size, not dependency risk — a much
better position than it appears from the outside.

| Format | VTK reader | Module | New vcpkg deps | Notes | Effort |
|---|---|---|---|---|---|
| `.ply` | `vtkPLYReader` | `IOPLY` | **none** | Cleanest addition on the list. `IO/PLY/vtk.module` depends only on CommonCore, IOCore, CommonDataModel, CommonMisc, vtksys — no rendering, nothing external. Outputs plain `vtkPolyData`, so it drops into the existing model untouched | **S** |
| `.vtkhdf` | `vtkHDFReader` | `IOHDF` | none (vendored hdf5) | Deps are CommonCore/DataModel/ExecutionModel, FiltersCore (already required), IOCore, IOHDFTools, plus private hdf5, FiltersTemporal, ParallelCore. No rendering. VTK's own successor format, so worth doing early. Test fixture already committed: `test/data/hdf/warping_spheres.vtkhdf`. See the hdf5 caveat below | **M** |
| `.e` / `.ex2` ExodusII | `vtkExodusIIReader` | `IOExodus` | none (vendored exodusII) | Deps CommonCore/DataModel/ExecutionModel, IOCore, IOXMLParser, exodusII; private FiltersCore, vtksys. No rendering. **Outputs `vtkMultiBlockDataSet`** → needs §1. Fixtures committed: `mug.e`, `disk_out_ref.ex2` | **M** *(composite)* |
| `.cgns` | `vtkCGNSReader` | `IOCGNSReader` | none (vendored cgns + hdf5) | **Note the module is `IOCGNSReader`, not `IOCGNS`** — an earlier optional-component list in this repo used the wrong name and could never have resolved. Private deps cgns, hdf5, FiltersExtraction, ParallelCore. **Outputs `vtkPartitionedDataSetCollection`** → needs §1. Fixture committed: `sqnz_s.adf.cgns` | **M** *(composite)* |
| EnSight Gold | `vtkGenericEnSightReader` | `IOEnSight` | none | Deps CommonExecutionModel; private FiltersGeneral, CommonCore, CommonDataModel, FiltersTemporal, ParallelCore. No rendering. Outputs multiblock → needs §1. No fixture committed yet | **M** *(composite)* |

### The hdf5 caveat, worth knowing before enabling `IOHDF`

VTK's vendored hdf5 is mangled to `vtkhdf5`, so in principle it coexists with another
hdf5 in the same process. But DuckDB extensions are `dlopen`ed into one address space,
and the community `h5db` extension also links hdf5. Two hdf5 builds in one process is
a classic source of subtle breakage. The mangling should make it safe; **"should" is
not "verified"**, and the test for it is: load `vtk` and `h5db` together and read a
file with each. Do that before shipping `IOHDF`, not after.

The portfile currently prefers vcpkg's copies for `expat`, `lz4` and `zlib`
(`VTK_MODULE_USE_EXTERNAL_VTK_*=ON`) specifically to avoid shipping two copies of a
library in one process. Whether to do the same for hdf5 is a real decision: external
hdf5 avoids duplication but adds a vcpkg dependency and its own version-matching
problem with `h5db`. Vendored-and-mangled is probably better; measure it.

---

## 4. Formats that are BLOCKED, not merely unimplemented

| Format | VTK reader | Module | Blocker |
|---|---|---|---|
| **OpenFOAM** | `vtkOpenFOAMReader` | `IOGeometry` | Module `PRIVATE_DEPENDS` on `VTK::RenderingCore` |
| `.obj`, `.stl` | `vtkOBJReader`, `vtkSTLReader` | `IOGeometry` | Same |

`IO/Geometry/vtk.module` lists `VTK::RenderingCore` among its private dependencies
(also `FiltersHybrid`, `FiltersGeneral`, `FiltersVerdict`, `IOImage`,
`nlohmannjson`). With `VTK_GROUP_ENABLE_Rendering=NO`, **VTK's own configure step
fails** on `IOGeometry`. This is an upstream constraint, not a missing flag, and the
portfile already carries a warning not to "fix" a missing OBJ reader by re-enabling
rendering — doing so would pull OpenGL, X11, glew, freetype and gl2ps into a database
extension and break loading on headless machines.

This matters more than the table suggests: **OpenFOAM is arguably the single
most-requested open-source CFD format**, and it is the one thing on this list that
cannot be reached from where the build currently stands. Options, all unattractive:

1. **Enable rendering.** Rejected — see above. Non-negotiable for a loadable extension.
2. **Patch VTK** to sever `IOGeometry`'s RenderingCore dependency. Technically the
   cleanest outcome, but it breaks the port's "no source patches" rule, which is what
   currently makes a VTK bump a two-line change. A patch to re-base on every release
   is a permanent maintenance tax.
3. **Upstream the fix** — ask Kitware whether `IOGeometry`'s RenderingCore dependency
   can be made optional. Slowest, but the only route that does not leave us carrying
   something. Worth an issue on VTK's tracker regardless, since the cost to ask is low.
4. **Read OpenFOAM ourselves.** Rejected: it contradicts the project's founding
   decision not to hand-roll format parsing, and OpenFOAM's layout is genuinely
   complex.

Recommendation: **(3), then reassess.** Do not block other work on it. Until then
`.obj`/`.stl`/OpenFOAM should keep failing with a clear message — and the message
should say *why*, which it currently does not.

**Effort: XL / blocked.**

---

## 5. Features, not formats

| Feature | Current state | Notes | Effort |
|---|---|---|---|
| **Lazy schema discovery** | `ATTACH` reads the entire dataset into memory | The biggest usability problem in the extension. `UpdateInformation()` yields the array list and counts without reading arrays, so `ATTACH` on a 40 GB file could be near-instant. Interacts with §1 and `.pvd`: those are only viable *after* this | **M–L** |
| **Filter pushdown** | Not implemented (projection pushdown is) | `WHERE cell_type = 12` still materialises every cell. Highest query-performance win for CAE-sized data | **M** |
| **Parallel scans** | Single-threaded | Safe by design: `VtkDataset` is immutable after construction, so point/cell ranges can be split across threads with no locking. **Respect the `GetCell` landmine** documented in design §3 — it returns per-object scratch state and will race | **M** |
| **`ARRAY` instead of `LIST`** | All multi-component arrays are `LIST` | Component count is fixed per array, so `ARRAY(child, n)` is the truer type and cheaper. A **breaking change** to the user-visible schema — needs a `vector_layout` option or a major-version bump, not a silent switch | **M** |
| **Replacement scan** | Must write `vtk_points('f.vtu')` or `ATTACH` | `FROM 'mesh.vtu'` working directly is a large ergonomic win and cheap to implement | **S** |
| **`time_value` column / time steps** | Not supported | Only meaningful once `.pvd` or VTKHDF temporal data lands. Design it once, for both | **M** |
| **Writing** | Read-only, enforced throughout L4 | Deliberately out of scope. `COPY … TO 'x.vtu'` is a coherent future feature but a different project shape; the read-only guarantee is currently load-bearing in the catalog layer | **XL** |

---

## 5b. Remote access, including SFTP

Reading is already scheme-driven: no URL scheme → hand the path to VTK; any scheme →
fetch through DuckDB's VFS and parse from memory. So **every filesystem extension
works with no code here**, present or future. `test/sql/remote_schemes.test` pins it.

That was not true before 2026-07-27. The routing used `FileSystem::IsRemoteFile()`,
which matches `EXTENSION_FILE_PREFIXES` — a *hardcoded* table in DuckDB core listing
only the `httpfs` and `azure` schemes. Anything else (`sshfs://`, `sftp://`, `gdfs://`,
`file://`) was classified **local** and its URL handed to VTK's `ifstream`, which
cannot open a URL. Three defects came out of that one line:

| Was | Now |
|---|---|
| `sshfs://`, `sftp://`, `file://` treated as local paths → VTK fails on a file DuckDB could open | routed through the VFS |
| `vtk_info.source_kind` declared and surfaced, but **never assigned** — always `'local'`, including for `https://` | reports the scheme |
| `file_size_bytes` from `std::filesystem::file_size()`, which cannot stat a URL → silently **0** for every remote read | size of the bytes received |

### SFTP specifically

DuckDB core has no SFTP filesystem. Two community extensions provide one, and as of
DuckDB v1.5.5 on `linux_amd64` **neither works** — measured 2026-07-27:

- **`sshfs`** (`sshfs://`) installs, loads, and *is* dispatched by the VFS, but every
  handshake fails. Its bundled `libssh2_1.11.1_DEV` sends a `SSH2_MSG_GLOBAL_REQUEST`
  (packet type 80) before key exchange completes, and `sshd` answers
  `SSH2_MSG_DISCONNECT: protocol error: rcvd type 80`. Reproduced against OpenSSH
  **9.2** (Debian bookworm) and **10.3**, with legacy KEX/cipher/MAC/host-key
  algorithms explicitly re-enabled on the server, so it is not a configuration or
  algorithm-negotiation problem. A client-side defect, upstream of this project.
- **`cloudfs`** (`sftp://`) publishes no binary for `v1.5.5/linux_amd64` (HTTP 404).

Nothing here is blocked on us: both schemes already route correctly. The honest
statement for users is "SFTP works as soon as a working SFTP filesystem exists for
your DuckDB version", with HTTP/S3 (verified end to end) or `scp`-then-read as the
present-day answer.

**If SFTP becomes a requirement rather than a nice-to-have**, the options in order of
cost are: (1) wait for / report the `sshfs` libssh2 bug — cheapest, no code here;
(2) test `cloudfs` on a platform where it *is* published, to see whether it is a
viable recommendation at all; (3) implement an SFTP `FileSystem` in this extension —
**rejected**: it would make a mesh reader responsible for SSH transport and key
handling, it duplicates what a filesystem extension is *for*, and it would add
libssh2 to `vcpkg.json` for every platform. Effort **S** for (1)–(2), **L** and a
scope violation for (3).

Two properties of any remote read worth keeping in mind before building on them:

- **The whole object is buffered, then parsed** — roughly 2× file size in RAM during
  parse. Refused above 64 GiB (`MAX_IN_MEMORY_BYTES`).
- **No range requests.** VTK needs the whole file, so `LIMIT 1` still downloads it
  all. Lazy schema discovery (§5) would *not* fix this for remote sources, since
  `UpdateInformation()` also needs the bytes — worth remembering when scoping §5.

---

## 6. Suggested order

Sequenced so each step unblocks the next and each lands something usable:

1. **`.ply`** — S. Genuinely trivial, and it exercises the "add a module to the port +
   CMake + factory" path end to end while the stakes are low. Do it first as a rehearsal.
2. **Replacement scan** — S. Pure ergonomics, no dependencies, immediately visible.
3. **`.pvtu` / `.pvtp`** — M. Does not need §1, and forces the VFS piece-resolution
   question that `.pvd` will need later.
4. **Lazy schema discovery** — M–L. Do this *before* composite work, or every composite
   format inherits the eager-load problem.
5. **§1 composite model** — L. The gate. Ships `.vtm` and `.vtpc` with it.
6. **VTKHDF** — M. VTK's own future format; fixture already in the corpus.
7. **ExodusII, then CGNS** — M each, both now unblocked by (5).
8. **Filter pushdown, parallel scans** — M each. Performance, once coverage is broad.
9. **EnSight** — M. Lower demand than the rest; needs a test fixture first.
10. **OpenFOAM** — blocked. Open the VTK issue early (step 1, it costs nothing) so the
    answer is known by the time the rest is done.

Deliberately *not* in this list: `ARRAY` instead of `LIST` (breaking; bundle it with a
major version), and writing (different project).

---

## 7. Things to keep true while doing any of the above

- **Every new module goes in both the portfile and `DUCK_VTK_REQUIRED_COMPONENTS`.**
  `make submit-check` fails if they disagree. Never reintroduce an "optional component"
  probe — a build whose linked module set depends on which VTK is installed produces
  binaries that differ by machine. See the note in `cmake/DuckVTKFindVTK.cmake`.
- **Never trust `GetErrorCode()`.** It stays `Success` through truncated files and
  through VTK 9.1's appended-data failure. `VtkErrorScope` capturing the output window
  is the only reliable signal, and every new reader must go through it.
- **Add a fixture and an oracle entry, not just a `.test` file.**
  `scripts/validate_against_vtk.py` compares elementwise against an independent Python
  VTK; a format that is only covered by hand-written expectations is not really covered.
- **Refuse clearly rather than reading partially.** The current
  `NotImplementedException` messages name the class and list what *is* supported. That
  is why an unsupported file is a good error instead of a wrong answer — keep it.
