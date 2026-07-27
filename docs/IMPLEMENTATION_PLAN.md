# duck_vtk — Implementation Plan

**Read this document first, in full, before writing any code.**

This is the execution plan for building `duck_vtk`, a DuckDB extension that exposes VTK mesh files as relational tables via `ATTACH`. It is written so that an implementation agent can work from start to finish without guessing: every API is either cited in a research document or stated here as a verified fact.

**The cardinal rule:** if this plan or a research document tells you an API signature, use it. If you find that reality disagrees with the document, **the compiler wins** — fix the code, then add a correction note to the research doc's "Uncertainties" section. Do not silently substitute an API you remember from a different DuckDB or VTK version; every published DuckDB extension example currently tracks `main`, not v1.5.4, and several of their patterns **do not compile here** (see §3).

---

## 1. Documents and reading order

| Order | Document | What it gives you |
|---|---|---|
| 1 | This file | Task sequence, acceptance criteria, file-by-file responsibilities |
| 2 | `docs/design/03-architecture-and-roadmap.md` | Layering, ownership/lifetime rules, phase gates, risk register |
| 3 | `docs/design/01-relational-schema.md` | **Normative** table/column/type contract. Column names and order are testable requirements |
| 4 | `docs/design/02-validation-and-testing.md` | Test layers, the oracle, invariants, definition of done |
| 5 | `docs/research/01-duckdb-extension-basics.md` | Build system, entrypoint, loading, sqllogictest, linking |
| 6 | `docs/research/05-duckdb-table-function-and-vector-api.md` | TableFunction anatomy, Vector/DataChunk writing, LIST columns, projection pushdown |
| 7 | `docs/research/02-duckdb-attach-storage-extension.md` | Catalog/StorageExtension/TransactionManager API and read-only skeleton |
| 8 | `docs/research/03-vtk-cpp-api-and-formats.md` | VTK readers, data model, array access, type mapping, CMake |
| 9 | `docs/research/04-test-data-corpus.md` | Test files with independently derived ground truth |

Documents 2–4 are **normative design decisions** — follow them. Documents 5–9 are **reference material** — consult them.

## 2. Environment: already-established facts

These were verified directly on this machine. Do not re-derive them.

| Fact | Value |
|---|---|
| Platform | Linux x86_64, Debian 13 (trixie), kernel 6.12 |
| System compiler | Debian GCC 14.2.0 (`/usr/bin/g++`) |
| System DuckDB CLI | `v1.5.4 (Variegata) 08e34c447b`, at `/home/linuxbrew/.linuxbrew/bin/duckdb` |
| **community-extensions targets** | `v1.5.5` (default, ci_tools `v1.5-variegata`) and `v1.4.5` (Andium/LTS, `v1.4-andium`). Their CI checks DuckDB out itself, so these are what must COMPILE; the submodule pin only decides what a local build produces |
| DuckDB platform string | `linux_amd64` (from `PRAGMA platform`) |
| Toolchain | cmake 4.4.0, ninja 1.13.2, ccache 4.13.6 (all via brew) |
| **duckdb submodule pin** | `08e34c447bae34eaee3723cac61f2878b6bdf787` — tag `v1.5.4`, **identical to the installed CLI's build**, and identical to what `extension-template@main` pins |
| **extension-ci-tools pin** | `b777c70d30942cca5bef62d6d4fa23a13362f398` |
| C++ standard | 17 |
| VTK | **Minimal source build** via `scripts/build_minimal_vtk.sh` → `$HOME/.local/vtk-9.6.2`. The Homebrew bottle was abandoned: ~2.8 GB of Qt/mesa/llvm dependencies, repeated ghcr.io HTTP/2 failures, and a libstdc++ ABI risk. See architecture doc §5 |
| vcpkg | **Not used.** No `vcpkg.json` in this repo. VTK comes from `find_package` |
| sudo | **Not available** without a password. Anything needing root must be handed to the user |

The pin equality is load-bearing: it is why a locally built extension loads into the user's already-installed `duckdb` CLI. `scripts/smoke.sh` step 4 is the tripwire for that assumption.

## 3. v1.5.4 API facts that contradict published examples

Every one of these was read from the v1.5.4 headers. Getting these wrong costs hours, because the published extensions (duckdb-sqlite, duckdb-postgres, most blog posts and tutorials) track `main` and use the older forms.

### 3.1 The entrypoint is the new `ExtensionLoader` style

`ExtensionUtil` **no longer exists** — `extension_util.hpp` in v1.5.4 is a `static_assert(false, ...)` pointing at PR #17772. Any code calling `ExtensionUtil::RegisterFunction(db, fn)` will not compile.

The correct form, from `src/include/duckdb/main/extension/extension_loader.hpp:122-123`:

```cpp
#define DUCKDB_CPP_EXTENSION_ENTRY(EXTENSION_NAME, LOADER_NAME) \
    DUCKDB_EXTENSION_API void EXTENSION_NAME##_duckdb_cpp_init(duckdb::ExtensionLoader &LOADER_NAME)
```

Registration is via loader methods (`extension_loader.hpp:45-71`), all taking by value:

```cpp
void RegisterFunction(ScalarFunction function);      // :45
void RegisterFunction(ScalarFunctionSet function);   // :46
void RegisterFunction(TableFunction function);       // :55
void RegisterFunction(TableFunctionSet function);    // :56
void RegisterFunction(CreateTableFunctionInfo info); // :57
DatabaseInstance &GetDatabaseInstance();             // :37
```

### 3.2 Storage extensions register through a static helper, not a map

`DBConfig::storage_extensions` is **not** a member of `DBConfig` in v1.5.4 — `grep storage_extension src/include/duckdb/main/config.hpp` returns nothing. The published `config.storage_extensions["sqlite"] = make_uniq<...>()` pattern will not compile.

From `src/include/duckdb/storage/storage_extension.hpp:48-49`:

```cpp
static optional_ptr<StorageExtension> Find(const DBConfig &config, const string &extension_name);
static void Register(DBConfig &config, const string &extension_name, shared_ptr<StorageExtension> extension);
```

Note **`shared_ptr`**, not `unique_ptr`. So `Load()` looks like:

```cpp
auto &db = loader.GetDatabaseInstance();
auto &config = DBConfig::GetConfig(db);
StorageExtension::Register(config, "vtk", make_shared_ptr<VtkStorageExtension>());
```

### 3.3 The attach callback signature

From `storage_extension.hpp:25-29`:

```cpp
typedef unique_ptr<Catalog> (*attach_function_t)(optional_ptr<StorageExtensionInfo> storage_info,
                                                ClientContext &context, AttachedDatabase &db, const string &name,
                                                AttachInfo &info, AttachOptions &options);
typedef unique_ptr<TransactionManager> (*create_transaction_manager_t)(optional_ptr<StorageExtensionInfo> storage_info,
                                                                      AttachedDatabase &db, Catalog &catalog);
```

ATTACH parameters arrive via `AttachOptions` (`src/include/duckdb/main/attached_database.hpp:58-82`), whose relevant members are:

```cpp
AccessMode access_mode;                      // :65   honour READ_ONLY / reject READ_WRITE
string db_type;                              // :69   the TYPE value, e.g. "vtk"
unordered_map<string, Value> options;        // :71   remaining custom (key, value) options
```

Reject unknown keys in `options` with a message listing the accepted ones (design §2.1).

`StorageExtension` also has optional virtuals `OnCheckpointStart` / `OnCheckpointEnd` (`:42-46`) — no-op for a read-only source.

### 3.4 Pure virtuals you must implement

Counted directly from the v1.5.4 headers. Research doc 02 has verbatim signatures; this is the checklist.

- **`Catalog`** (`catalog.hpp`) — 13 pure virtuals: `Initialize` (:116), `GetCatalogType` (:134), `CreateSchema` (:140), a schema lookup taking `OnEntryNotFound` (:223), `ScanSchemas` (:253), `PlanCreateTableAs` (:309), `PlanInsert` (:311), `PlanDelete` (:313), `PlanUpdate` (:316), `GetDatabaseSize` (:327), `InMemory` (:330), `GetDBPath` (:331), `DropSchema` (:452).
- **`SchemaCatalogEntry`** (`schema_catalog_entry.hpp`) — 15 pure virtuals: two `Scan` overloads (:55, :57), `CreateIndex` (:63), `CreateFunction` (:66), `CreateTable` (:68), `CreateView` (:70), `CreateSequence` (:72), `CreateTableFunction` (:75), `CreateCopyFunction` (:78), `CreatePragmaFunction` (:81), `CreateCollation` (:83), `CreateType` (:91), `LookupEntry` (:95), `DropEntry` (:105), `Alter` (:108).
- **`TableCatalogEntry`** (`table_catalog_entry.hpp`) — `GetStatistics` (:89), `GetScanFunction(ClientContext &, unique_ptr<FunctionData> &)` (:101), `GetStorageInfo` (:118).
- **`TransactionManager`** (`transaction_manager.hpp`) — `StartTransaction` (:35), `CommitTransaction` (:37), `RollbackTransaction` (:39), `Checkpoint` (:41).

Note v1.5.4 uses `LookupEntry(CatalogTransaction, const EntryLookupInfo &)` — **not** the older `GetEntry(CatalogTransaction, CatalogType, const string &)`. `EntryLookupInfo` is at `src/include/duckdb/catalog/entry_lookup_info.hpp`.

### 3.5 Scan-loop facts

- The `output` `DataChunk` **is reset for you** before every `function()` call (`src/parallel/pipeline_executor.cpp:229`). Do not call `output.Reset()`.
- `column_ids` is a **public member** of `TableFunctionInitInput`, not a `GetColumnIds()` method.
- `column_ids` only takes effect when you set `projection_pushdown = true`; otherwise DuckDB expects all columns and projects afterwards. When enabled, `output.data[i]` corresponds to `column_ids[i]` — a 1:1 positional mapping.
- `TableFunctionData::Copy()` throws `InternalException` by default. **You must override it**, or any plan that copies bind data crashes.
- `STANDARD_VECTOR_SIZE` is in `src/include/duckdb/common/vector_size.hpp`.

### 3.6 Build-system facts

- `extension-ci-tools`' `BUILD_FLAGS` hardcodes `-DENABLE_UNITTEST_CPP_TESTS=FALSE`, so `test/cpp/*.cpp` is silently ignored. The project `Makefile` overrides it via `EXT_FLAGS`. **If the C++ tests appear to pass instantly, they did not run.**
- **Confirmed:** `duckdb/CMakeLists.txt:54` reads `set(CMAKE_CXX_STANDARD "11" CACHE STRING "C++ standard to enforce")`, while our `CMakeLists.txt` sets 17 the same way. DuckDB is the top-level project (`cmake -S ./duckdb`), so its `set(... CACHE ...)` runs first and a later CACHE set does **not** overwrite an existing entry — meaning the extension would silently compile as C++11 and fail on `std::optional`/structured bindings/etc. The project `Makefile` therefore passes `-DCMAKE_CXX_STANDARD=17` on the cmake command line, which pre-seeds the cache entry so *both* `set(... CACHE ...)` calls become no-ops. This was listed as an open uncertainty in research doc 01; it is now verified. Sanity-check after configuring: `grep CMAKE_CXX_STANDARD build/release/CMakeCache.txt` must show `17`.
- `build_loadable_extension_directory` links the module with `-fvisibility=hidden -Wl,--gc-sections -Wl,--exclude-libs,ALL`. `--exclude-libs,ALL` affects only **static** archives, so brew's shared VTK is fine.
- Output paths: `build/release/extension/vtk/vtk.duckdb_extension` and `build/release/duckdb`.
- Loading a locally built extension needs `duckdb -unsigned` (or `SET allow_unsigned_extensions=true` before `LOAD`).

## 4. Task sequence

Work the phases in order. Each has a **gate** — do not start the next phase until the gate passes. The ordering exists so that each phase de-risks the next; skipping ahead means debugging VTK correctness and catalog boilerplate simultaneously, which is the main way this project could go badly.

---

### Phase 0 — Prove the VTK toolchain works (gate: spike prints correct numbers)

**Why first:** Homebrew's Linux bottles may be built against a different `libstdc++` than Debian's GCC 14. If `std::string` cannot safely cross the VTK shared-library boundary, everything built on top is worthless. Find out in 10 minutes, not after 2000 lines.

Already written for you: `scripts/phase0_spike/{CMakeLists.txt,main.cpp}`.

1. `make configure` — installs toolchain, VTK, submodules, and the Python oracle venv.
2. `make phase0`
3. Compare the printed `num_points` / `num_cells` / array metadata against `docs/research/04-test-data-corpus.md` ground truth.

**Gate:** the spike builds, runs, and prints numbers matching the oracle. Record `vtk_sizeof_id_type` and `vtk_use_64bit_ids` from its output — the type-mapping layer depends on them.

**If it fails**, in priority order:
1. Build everything with Homebrew's compiler (`CC=/home/linuxbrew/.linuxbrew/bin/gcc CXX=.../g++`).
2. Use Debian's VTK 9.3, which is ABI-matched to the system GCC: **ask the user to run `sudo apt install libvtk9-dev`** (no passwordless sudo here), then `make phase0 VTK_DIR=/usr/lib/x86_64-linux-gnu/cmake/vtk-9.3`.
3. Minimal VTK source build with rendering/Qt disabled.

Do not proceed past this gate with a workaround you have not verified.

---

### Phase 0b — Resolve VTK doc uncertainties against real headers (gate: doc 03 §9 cleared)

Research doc 03 was written before VTK finished installing, so it cites docs rather than headers. Its §9 lists 11 items to confirm. Resolve them now, against `/home/linuxbrew/.linuxbrew/opt/vtk/include/vtk-9.6/`, and update the doc in place. The ones that affect code you are about to write:

- `vtkUnstructuredGrid::GetCellTypesArray()` exact name and return type
- Whether `vtkCellTypes::GetClassNameFromTypeId` is deprecated in 9.6 in favour of `vtkCellTypeUtilities` (we use a static table anyway — design §5 — so this only affects fallbacks)
- `vtkAlgorithm::Update()` return type (whether it can be used as a success signal)
- `vtkAbstractArray::GetArrayType()` enumerator spellings, used by the AOS/SOA guard
- `vtk_module_autoinit` ordering relative to `target_link_libraries`
- Which optional IO modules the bottle actually ships (`IOEnSight`, `IOExodus`, `IOCGNSReader`, `IOHDF`). Moot now: the build links a fixed module set and no longer probes for optional ones — see the note in `cmake/DuckVTKFindVTK.cmake` and `docs/ROADMAP.md` §3

---

### Phase 1 — Extension skeleton that loads — ✅ **DONE**

> `make release` produces an extension that loads into the system `duckdb v1.5.4` and
> reports `VTK 9.6.2; sizeof(vtkIdType)=8; 64bit_ids=yes`. Confirmed:
> `CMAKE_CXX_STANDARD=17` in the cache, RUNPATH resolving with `LD_LIBRARY_PATH=""`.
> This phase also surfaced the mandatory `OVERRIDE_GIT_DESCRIBE` (see §3.6) —
> without it the extension links perfectly and fails only at `LOAD`.

1. Create the submodules at the pinned commits (`configure.sh` does this; verify the pin).
2. Copy `.clang-format` and `.clang-tidy` from `extension-template` so formatting matches DuckDB house style.
3. Write `src/vtk_extension.cpp` + `src/include/vtk_extension.hpp` using the §3.1 entrypoint. Register **one** scalar function:
   - `vtk_version() -> VARCHAR` returning `vtkVersion::GetVTKVersion()`.
     This is not a toy: it proves VTK is linked, initialised, and callable from inside a dlopen'd DuckDB module — strictly more than "it compiled".
4. Build files are already written: `CMakeLists.txt`, `Makefile`, `extension_config.cmake`, `cmake/DuckVTKFindVTK.cmake`. `CMakeLists.txt` lists all Phase 2–3 source files, so **create empty stub `.cpp` files** for them (or trim the list and restore entries as you go — but keep the list explicit, never glob).
5. `make release && ./scripts/smoke.sh`

**Gate:** smoke steps 1–5 pass, including the load into the system `duckdb -unsigned`. Step 6 will skip until Phase 2.

---

### Phase 2 — Table functions over real data — ✅ **DONE**

> All 67 readable corpus files pass; the 4 negative fixtures are rejected. Values match
> the hand-derived ground truth in research doc 04 §4 exactly. See the Phase-2 commit
> message for the four silent-data-loss bugs this phase surfaced (vtkDataReader's
> ReadAll* flags defaulting to off being the worst).

This is where correctness is actually established, deliberately before any catalog work. Build L1 → L2 → L3 (architecture doc §2).

**L1 — `src/vtk/`**

- `vtk_error_scope.{hpp,cpp}` — RAII installer of a custom `vtkOutputWindow` that **captures** VTK's messages into a buffer instead of letting them reach the terminal. Every reader call happens inside one of these, and on failure the captured text goes into the thrown exception. Do this first: without it, debugging is much harder and a DuckDB session gets polluted with VTK chatter. Code sketch in research doc 03 §8.2.
- `vtk_reader_factory.{hpp,cpp}` — `path → vtkSmartPointer<vtkDataObject>`, dispatching on **content**, not extension (design §8 requires a `.vtk`-named non-VTK file to be rejected). Try `vtkXMLGenericDataObjectReader::CanReadFile`, then the legacy `vtkGenericDataObjectReader` header sniff. Research doc 03 §2.3 has a full implementation. Must distinguish "unreadable" from "empty" and throw `IOException` for the former.
- `vtk_dataset.{hpp,cpp}` — the uniform accessor that hides dataset-type variation. **This is the most important design work in Phase 2.** It must present one interface over `vtkUnstructuredGrid`, `vtkPolyData`, `vtkImageData`, `vtkRectilinearGrid`, and `vtkStructuredGrid`, covering: point count, cell count, `GetPoint(i) → double[3]` (synthesising coordinates for implicit-geometry types), cell type by id, connectivity by id, and array enumeration for point/cell/field data.

  **Two hard rules:**
  - **Never call `vtkDataSet::GetCell(i)`.** It returns a shared per-object scratch object — a data race once scans go parallel and a correctness hazard even single-threaded if interleaved. Use `GetCellPoints(id, vtkIdList*)` with a caller-owned `vtkIdList`, or the raw `vtkCellArray` offsets/connectivity arrays.
  - The object is **immutable after construction**. This is what lets all scans share it lock-free and is the basis of the Phase-5 parallelisation.

  Resolve the polydata ordering question (design §11.1) here: verify empirically that the global `cell_id` ordering (verts → lines → polys → strips) matches `vtkCellData` tuple order, and record the answer in a code comment plus a test.

**L2 — `src/model/`**

- `vtk_type_mapping.{hpp,cpp}` — `(VTK type, num_components) → LogicalType`, per research doc 03 §5 and design §4. Pure function, no VTK objects in the signature if avoidable, so it is trivially unit-testable.
- `vtk_cell_types.{hpp,cpp}` — the static, exhaustive `VTK_*` id → name table (design §5). Unknown ids render `VTK_UNKNOWN_<id>`.
- `vtk_table_schema.{hpp,cpp}` — builds the column name/type lists for all six tables, applying the name-collision policy (design §6) in the specified order. Returns the `column_name` values that `vtk_arrays` reports.
- `vtk_column_writer.{hpp,cpp}` — the typed array → `Vector` conversion. **The highest-risk code in the project.**

  **Non-negotiable:** do not read integer arrays via `vtkDataArray::GetComponent`, which returns `double` and silently loses precision above 2^53. Use `vtkArrayDispatch` with a functor, or `switch(GetDataType())` on a typed downcast. Research doc 03 §4.4 has a working dispatch implementation; it must handle both AOS and SOA arrays. The dedicated unit test (2^53+1) exists to catch exactly this.

  For LIST columns follow research doc 05 §3 precisely: `ListVector::Reserve` **before** taking a pointer to the child data, because `Reserve` calls `child->Resize()` and invalidates any pointer taken earlier. Then write `list_entry_t{offset, length}` into the parent and `ListVector::SetListSize(vec, total)` at the end.

**L3 — `src/functions/`**

Six table functions plus a debug one, each `f(path VARCHAR)`:
`vtk_points`, `vtk_cells`, `vtk_cell_points`, `vtk_field_data`, `vtk_arrays`, `vtk_info`, `vtk_debug_dump`.

- Bind opens the file (via a small cache keyed by path, so `vtk_points('f') JOIN vtk_cells('f')` reads once) and sets `return_types`/`names` from `VtkTableSchema`.
- Override `FunctionData::Copy()` and `Equals()` — the default `Copy()` throws.
- Set `projection_pushdown = true` and honour `column_ids` positionally. `test/sql/projection.test` exists because an off-by-one here silently returns *a different column's data*, which no aggregate check would notice.
- Emit `STANDARD_VECTOR_SIZE` chunks; `output.SetCardinality(0)` ends the scan.
- `MaxThreads()` returns 1 in Phase 1.
- Implement `cardinality` (`make_uniq<NodeStatistics>(n, n)`). For `cell_points`, report `sum(num_points)` honestly, or the planner will choose bad join orders.

**Gate:** `make oracle` reports 0 mismatches across the Tier-1 corpus, and the L1 precision test passes. Run the suite once in a **debug** build too — malformed LIST vectors often pass in release and assert in debug.

---

### Phase 3 — ATTACH — ✅ **DONE**

> `ATTACH ... (TYPE vtk)` works; all six tables appear in `duckdb_tables()`,
> `SHOW ALL TABLES`, `DESCRIBE` and `information_schema`; all write paths refuse
> without crashing; 65 files × 15 SQL invariants pass. Builds against both DuckDB
> 1.5.x and 1.4.x LTS via a configure-time header probe.

Build L4 on Phase 2's functions. Follow research doc 02's skeleton, checked against the §3.4 pure-virtual checklist.

- `vtk_storage_extension.cpp` — sets `attach` and `create_transaction_manager`; registered per §3.2.
- `vtk_catalog.cpp` — owns one `shared_ptr<VtkDataset>`, built eagerly at attach (design §2). Holds a single `VtkSchemaEntry` named `main`.
- `vtk_schema_entry.cpp` — `Scan`/`LookupEntry` over the six fixed tables. All `Create*`/`Drop*`/`Alter` throw the design §7 message.
- `vtk_table_entry.cpp` — `GetScanFunction` returns the **Phase-2 table function** with pre-bound `FunctionData`. This is why Phase 2 came first: there is exactly one scan implementation, so `SELECT * FROM m.points` and `SELECT * FROM vtk_points('f.vtu')` cannot diverge.
- `vtk_transaction_manager.cpp` — minimal read-only: hand out a trivial transaction, commit as no-op, `Checkpoint` no-op.

**Gate:** all 10 queries in design §10 run; `test/sql/read_only.test` passes with **no crashes** (an unimplemented pure virtual that segfaults instead of throwing is the most likely serious bug in this project); `DESCRIBE`, `SHOW TABLES`, and `duckdb_tables()` work against the attached database.

---

### Phase 4 — Tests, docs, CI — ✅ **DONE**

> 14 sqllogictest files / 349 assertions (measured), invariant runner, oracle harness, Python
> client smoke test, README, LICENSE, and a CI matrix over DuckDB 1.5.4 + 1.4.5 LTS
> + clang.

### Phase 4 (original text) — Tests, docs, CI

Write the full L2 suite and the L3/L4 harnesses per design doc 02 §5–7. `scripts/validate_against_vtk.py` and `scripts/run_invariants.sh` need writing. Then `README.md`: install, the six tables, the type mapping, and the Phase-1 limitations from design §9 stated plainly.

**Gate:** `make check` green end to end.

## 5. Pitfalls, ranked by cost

1. **Reading int64 arrays through `GetComponent`** — silent wrong data above 2^53. Dedicated unit test.
2. **Missing `vtk_module_autoinit`** — compiles and links, then readers return empty output at runtime, which design §8 forbids confusing with an empty mesh. The smoke test's ATTACH step is the canary.
3. **Copying catalog code from duckdb-sqlite/postgres** — they track `main`; §3.2 and §3.4 differ. Compile early and often.
4. **Taking a LIST child-vector pointer before `ListVector::Reserve`** — use-after-free, often passing in release and asserting only in debug.
5. **`vtkDataSet::GetCell` in a scan** — shared scratch object; a latent data race.
6. **Assuming C++ tests ran** — they are disabled unless `-DENABLE_UNITTEST_CPP_TESTS=TRUE` reaches cmake.
6b. **Forgetting `OVERRIDE_GIT_DESCRIBE`** — builds and links fine, then `LOAD` reports the extension was built for DuckDB `v0.0.1`. Already handled in the Makefile; `make check-pin` guards it.
7. **Not overriding `TableFunctionData::Copy()`** — default throws `InternalException`.
8. **Off-by-one in `column_ids`** — returns the wrong column's data with no error.
9. **Committing test expectations not derived from the oracle** — makes the suite lie. Mark `mode skip` with a reason instead.
10. **Mangling array names** — design §6 requires verbatim names; `SELECT "Pressure [Pa]"` must work.

## 6. Definition of done for Phase 1

The checklist in `docs/design/02-validation-and-testing.md` §8, verified by a single `make check`. Report honestly: if a layer is skipped or a corpus file unverified, say so explicitly rather than reporting green.
