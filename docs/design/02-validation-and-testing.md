# duck_vtk — Validation & Testing Strategy

**Status:** normative for Phase 1.

The purpose of this document is to make "does it work?" a question with a mechanical answer. An implementation agent should be able to run one command and know.

---

## 1. The core validation principle: independent ground truth

The extension must never be validated against itself. Every numeric expectation in the test suite traces back to a **VTK-independent-of-our-code** source:

- **Primary oracle: Python.** `vtk`'s own Python bindings (or `meshio`) read the same file and report point counts, cell counts, cell-type histograms, array metadata, and sample values. Ground truth for three ASCII fixtures was derived by hand against the format spec and independently confirmed by VTK (see `docs/research/04-test-data-corpus.md` §4); everything else is checked elementwise by `scripts/validate_against_vtk.py`. There is no `ground_truth.json` — the harness computes expectations at run time, so they cannot go stale.
- **Why an oracle and not eyeballing:** a bug that reads a `float32` array through a `double` accessor produces plausible-looking numbers. A bug that reads a 64-bit integer array through `GetComponent` produces *correct* numbers for small values and wrong ones above 2^53. Only comparison against an independent reader catches these.
- **Self-consistency checks are a supplement, not a substitute.** They catch a different class of bug (internal incoherence) and are cheap, so both are used.

If the oracle is unavailable for a given file (e.g. no Python VTK for an exotic format), the test for that file is marked `mode skip` with a comment saying why, rather than asserting a value nobody verified. **An unverified expectation committed as if verified is the worst outcome in this project** — it makes the suite lie.

## 2. Test layers

| Layer | Location | Runner | What it proves |
|---|---|---|---|
| L0 — build & load | `scripts/smoke.sh` | shell | The extension compiles, links VTK, and `LOAD`s into the brew-installed DuckDB CLI |
| L1 — C++ units | `test/cpp/*.cpp` | Catch2 via `unittest` | *Not delivered as C++ — see §4. Covered via SQL.* |
| L2 — SQL behaviour | `test/sql/*.test` | sqllogictest via `build/release/test/unittest` | The user-visible contract: schemas, values, errors |
| L3 — oracle diff | `scripts/validate_against_vtk.py` | python + duckdb CLI | Every array in every corpus file, every tuple, compared elementwise against Python VTK |
| L4 — invariants | `scripts/run_invariants.sh` | shell + duckdb CLI | 15 self-consistency properties × every corpus file. A script, not a `.test` file, so the corpus can grow without editing tests |

L3 is the one that actually establishes correctness at scale; L2 is what runs fast in CI and pins the contract. Both are required. **Do not skip L3** — it is the only layer that checks more than a handful of sampled values.

## 3. L0 — build and load smoke test

`scripts/smoke.sh` must, from a clean checkout, do exactly this and exit non-zero on any failure:

```bash
#!/usr/bin/env bash
set -euo pipefail
make release
EXT=build/release/extension/vtk/vtk.duckdb_extension
test -f "$EXT"

# Load into the OWN build (must always work)
./build/release/duckdb -c "LOAD '$EXT'; SELECT vtk_version();"

# Load into the SYSTEM brew DuckDB v1.5.4 (proves ABI/platform compatibility)
duckdb -unsigned -c "LOAD '$EXT'; SELECT vtk_version();"

# Confirm the storage extension registered
duckdb -unsigned -c "LOAD '$EXT'; ATTACH 'test/data/legacy/uGridEx.vtk' AS m (TYPE vtk); SELECT count(*) FROM m.points;"
```

The second and third checks are the important ones: they are the difference between "it built" and "a user can use it". The system-CLI load is expected to work because the extension-template's pinned duckdb submodule (`08e34c447b`) is the exact commit the brew v1.5.4 binary was built from — if it *stops* working, that pin has drifted and the smoke test is the tripwire.

## 4. L1 — C++ unit tests

**Build-system gotcha, verified in the real toolchain:** `extension-ci-tools/makefiles/duckdb_extension.Makefile` hardcodes `-DENABLE_UNITTEST_CPP_TESTS=FALSE` into its `BUILD_FLAGS` (line 121 of the pinned `b777c70d` revision). Any `test/cpp/*.cpp` files are therefore **silently ignored** unless the flag is overridden. The project `Makefile` appends `-DENABLE_UNITTEST_CPP_TESTS=TRUE` via `EXT_FLAGS`, which is placed after `BUILD_FLAGS` on the cmake command line and so wins.

If the L1 tests appear to "pass" instantly with no output, they are not running — check that flag before believing them. A test suite that silently does not execute is indistinguishable from one that passes, which is precisely the failure this note exists to prevent.

> **DELIVERED DIFFERENTLY — read this before adding to it.**
>
> The L1 cases below were specified as Catch2 unit tests. In the delivered code they
> are covered through the SQL layer instead, because every L1/L2 function's
> observable behaviour is reachable from SQL and asserting it there also proves the
> wiring, which a unit test does not:
>
> | Specified L1 test | Where it actually lives |
> |---|---|
> | Type mapping table | `test/sql/precision.test` + `schema.test` (asserts `sql_type` and `information_schema` types per array) |
> | **int64 precision (2^53+1)** | `test/sql/precision.test` with the purpose-built `int64_precision.vtu` fixture — end-to-end, and it also covers INT64_MIN/MAX and UINT64_MAX |
> | Name mangling rules | `test/sql/quoting.test` — each design §6 rule, in order |
> | Cell-type table | `test/sql/values.test` (12 exact names) + the `legacy/cells/` sweep of ids 1–16 |
> | Reader dispatch | `test/sql/attach_errors.test` (content-based rejection) + the oracle's per-format coverage |
> | float32 exactness | The oracle harness compares float32 arrays at float32 precision across the whole corpus |
>
> `-DENABLE_UNITTEST_CPP_TESTS=TRUE` is still passed by the Makefile so that adding
> `test/cpp/*.cpp` later needs no build change. Nothing is currently there — so if
> you add a file, verify it actually runs rather than assuming, per the warning above.
>
> What a genuine Catch2 layer would still buy: testing `VtkResolveColumnNames` and
> `VtkArrayLogicalType` on inputs no real file produces (empty names, 36-component
> tensors, unmapped VTK type ids). Worth adding; not a gap in coverage of the
> shipped behaviour.

Pure, fast, no file I/O where avoidable. Originally specified cases:

**Type mapping** (`test/cpp/test_type_mapping.cpp`)
- Every `VTK_*` type constant maps to the documented DuckDB `LogicalType`. Table-driven; one assertion per row of the mapping table in `docs/research/03-*.md` §5.
- `VTK_ID_TYPE` maps correctly for both the 32- and 64-bit-ids case (test whichever the build has, and assert the *other* branch is at least reachable/compiles).
- A 1-component array yields a scalar type; a 3-component array yields `LIST(child)`; a 9-component array yields `LIST(child)`.

**Precision** (`test/cpp/test_precision.cpp`)
- The single most important unit test in the project: construct a `vtkTypeInt64Array` in memory containing `9007199254740993` (2^53 + 1) and `-9007199254740993`, read it through the production conversion path, and assert the values round-trip **exactly**. A `GetComponent`-based implementation fails this. Also test `uint64` values above `2^63`.
- `float32` values round-trip to `DOUBLE` exactly (they must, as widening is exact) — assert bit-exact for a few awkward values like `0.1f`.

**Name mangling** (`test/cpp/test_column_names.cpp`)
- Each rule in schema §6, in order: verbatim pass-through, empty→`unnamed_point_0`, collision with `x`→`x_1`, duplicate array names, case-insensitive ambiguity (`Pressure`/`pressure`).

**Cell type table** (`test/cpp/test_cell_types.cpp`)
- Spot-check ~15 ids including `VTK_VERTEX(1)`, `VTK_LINE(3)`, `VTK_TRIANGLE(5)`, `VTK_QUAD(9)`, `VTK_TETRA(10)`, `VTK_HEXAHEDRON(12)`, `VTK_WEDGE(13)`, `VTK_PYRAMID(14)`, `VTK_QUADRATIC_TETRA(24)`, `VTK_POLYHEDRON(42)`, `VTK_LAGRANGE_HEXAHEDRON(72)`.
- An unmapped id (e.g. 250) yields `VTK_UNKNOWN_250`, not a crash or NULL.

**Reader dispatch** (`test/cpp/test_reader_factory.cpp`)
- Correct reader class chosen for each corpus file, asserted by name — with `.vtk`, `.vtu`, `.vtp`, `.vti`, `.vtr`, `.vts`.
- A file whose extension lies about its content is dispatched on *content*, not extension.

## 5. L2 — sqllogictest suite

One file per concern. Every file starts with the standard header and `require vtk`.

| File | Asserts |
|---|---|
| `load.test` | Extension loads; `vtk_version()` non-empty |
| `attach_basic.test` | `ATTACH`/`DETACH`; `TYPE vtk` and `TYPE VTK`; `m.points` and `m.main.points` both resolve |
| `attach_errors.test` | Missing file, corrupt file, non-VTK content with `.vtk` extension, unknown attach option, `READ_WRITE` — each with the expected error substring |
| `schema_points.test` | Exact column names, order, and types of `points` for each Tier-1 file, via `DESCRIBE` |
| `schema_cells.test` | Same for `cells` |
| `introspection.test` | `vtk_info` single row with exact values; `vtk_arrays` full contents |
| `values_legacy_ascii.test` | Exact point coords and array values for the legacy ASCII fixture — full-precision expectations from the oracle |
| `values_xml_vtu.test` | Same for `.vtu` |
| `values_polydata.test` | Same for `.vtp`, including the polydata global-cell-id ordering question (schema §11.1) |
| `vectors.test` | `LIST` column round-trip: `velocity[1..3]`, `len()`, `unnest` |
| `cell_points.test` | Row count equals `sum(num_points)`; vertex order preserved; join to `points` yields no orphans |
| `implicit_geometry.test` | `.vti`/`.vtr`/`.vts` synthesised coordinates and point ordering |
| `degenerate.test` | Every row of schema §8: empty dataset, points-without-cells, cells-without-point-data, tuple-count mismatch, NaN/Inf pass-through |
| `quoting.test` | Array names with spaces, dots, unicode, mixed case — queried with double quotes |
| `read_only.test` | Every write operation from schema §7 raises the documented error |
| `projection.test` | `SELECT` of one column from a many-array file returns correct values (guards the `column_ids` code path, which is the easiest place to introduce an off-by-one that silently returns the *wrong column's* data) |
| `invariants.test` | See §7 |

### 5.1 Precision in expectations

Floating-point expectations are the classic source of flaky mesh tests. Rules:

- Never write a bare float expectation copied from a `printf`. Either compare with an explicit tolerance in SQL (`abs(x - 1.234) < 1e-12`), or cast to a fixed representation (`round(x, 9)`), or assert the exact `DOUBLE` if the value is exactly representable (integers, halves, and anything that came from a `float32` widened to `double`).
- Prefer aggregate assertions with exact-representable results (counts, sums of small integers) over per-row float dumps where possible.
- For a `float32` source array, the widened `DOUBLE` **is** exactly representable — so those can and should be asserted exactly, and the oracle must print them with `repr()`/17 significant digits, not `str()`.

### 5.2 Error assertions

Use `statement error` with the shortest substring that uniquely identifies the failure, e.g.:

```
statement error
ATTACH 'test/data/synthetic/truncated.vtu' AS bad (TYPE vtk);
----
duck_vtk:
```

Do not assert full VTK error text — it varies between VTK patch releases and would make the suite version-brittle. Assert our own prefix plus a distinguishing keyword.

## 6. L3 — the oracle diff harness

`scripts/validate_against_vtk.py`. This is the highest-value test artefact; specify it fully.

Behaviour:

1. Walk every file in `test/data/` covered by the corpus catalogue.
2. For each file, read it with Python VTK and build the expected relational form in memory: point coords, cell types, connectivity, and every point/cell array with all components.
3. Run the same queries through the DuckDB CLI with the extension loaded, emitting results as CSV or JSON.
4. Compare **elementwise**:
   - counts exactly
   - integer values exactly
   - float values with `math.isclose(rel_tol=0, abs_tol=0)` — i.e. **exactly** — for `float32`-sourced data widened to double, and `rel_tol=1e-15` for `float64`-sourced data where a text round-trip through CSV may cost a bit. Prefer JSON/Parquet output over CSV to avoid the text round-trip entirely, in which case exact comparison applies throughout.
   - `NaN` compares equal to `NaN` for this purpose; `+Inf`/`-Inf` must match sign.
5. Report a per-file pass/fail table and exit non-zero on any mismatch, printing the first 10 differing elements with indices.

Must be runnable as `python3 scripts/validate_against_vtk.py --data test/data --duckdb ./build/release/duckdb --ext build/release/extension/vtk/vtk.duckdb_extension`, and must work from a venv created by `scripts/setup_dev_env.sh`.

This harness is what justifies the claim "the extension reads VTK files correctly". L2 alone only proves it reads *these seventeen sampled values* correctly.

## 7. L4 — invariants

Properties that must hold for every non-empty dataset, expressed as SQL returning zero rows or a known constant. These are cheap, catch whole classes of bug, and keep working when the corpus grows.

```sql
-- Point ids are dense, 0-based, unique
SELECT count(*) FROM m.points;                                  -- = vtk_info.num_points
SELECT min(point_id), max(point_id) FROM m.points;               -- = 0, num_points-1
SELECT count(DISTINCT point_id) FROM m.points;                    -- = num_points

-- Cell ids likewise
SELECT min(cell_id), max(cell_id), count(DISTINCT cell_id) FROM m.cells;

-- No connectivity dangles outside the point set
SELECT count(*) FROM m.cell_points WHERE point_id < 0
   OR point_id >= (SELECT num_points FROM m.vtk_info);            -- = 0

-- cell_points is exactly the flattening of cells.point_ids
SELECT count(*) FROM m.cell_points;                               -- = (SELECT sum(num_points) FROM m.cells)
SELECT count(*) FROM (
  SELECT cell_id, vertex_index, point_id FROM m.cell_points
  EXCEPT
  SELECT cell_id, generate_subscripts(point_ids,1)-1, unnest(point_ids) FROM m.cells
);                                                                -- = 0

-- num_points agrees with the list it describes
SELECT count(*) FROM m.cells WHERE num_points <> len(point_ids);  -- = 0

-- SQL-computed bounds match VTK's reported bounds
SELECT count(*) FROM m.vtk_info i, (SELECT min(x) a, max(x) b, min(y) c, max(y) d, min(z) e, max(z) f FROM m.points) p
WHERE i.bounds_x_min <> p.a OR i.bounds_x_max <> p.b
   OR i.bounds_y_min <> p.c OR i.bounds_y_max <> p.d
   OR i.bounds_z_min <> p.e OR i.bounds_z_max <> p.f;             -- = 0

-- Every array in vtk_arrays actually appears as a column
-- (compare vtk_arrays.column_name against duckdb's information_schema)
SELECT count(*) FROM m.vtk_arrays a
WHERE a.association = 'POINT' AND a.column_name NOT IN (
  SELECT column_name FROM information_schema.columns
  WHERE table_catalog='m' AND table_name='points');               -- = 0

-- Every declared sql_type matches the actual column type
-- (same join, comparing a.sql_type to information_schema.columns.data_type)
```

The bounds invariant is worth highlighting: it independently cross-checks the coordinate-reading path against a completely separate VTK code path (`GetBounds`), and it catches x/y/z transposition — a bug that no per-value spot check on a symmetric mesh would find.

Run the invariants against **every** corpus file, driven by a loop in `scripts/run_invariants.sh` rather than duplicated per-file in sqllogictest.

## 8. What "Phase 1 is done" means

All of the following, verified by a single `make check` that runs L0–L4:

- [ ] `make release` succeeds from a clean clone with only `brew install cmake ninja ccache vtk` as prerequisites
- [ ] `scripts/smoke.sh` passes, including the load into the **system** brew DuckDB
- [ ] All L1 C++ tests pass, including the 2^53+1 precision test
- [ ] All L2 sqllogictest files pass with zero skips other than documented `mode skip` with a stated reason
- [ ] `scripts/validate_against_vtk.py` reports 0 mismatches across the whole Tier-1 and Tier-2 corpus
- [ ] `scripts/run_invariants.sh` passes for every corpus file
- [ ] The ten worked queries in schema §10 all execute and return sensible results on at least one real CAE file
- [ ] `README.md` documents install, the six tables, the type mapping, and the Phase-1 limitations from schema §9

## 9. Anti-goals for Phase 1 testing

Explicitly out of scope, so effort is not spent there:

- Performance benchmarks and regression thresholds (Phase 4, when there is something to optimise)
- Fuzzing the VTK readers (VTK's problem, not ours; we only must not crash on the truncated-file fixture)
- Windows/macOS CI (Linux x86_64 only for Phase 1; the CMake must not *preclude* others)
- Concurrency stress tests (single-threaded by design in Phase 1)
- Memory-usage assertions (documented as unbounded in Phase 1)

## 10. Debugging aids the implementation agent should build in from the start

Cheap now, invaluable when a value is wrong:

- `SELECT vtk_debug_dump('file.vtu')` — a table function returning a text dump of what VTK reports (arrays, types, counts) with no relational mapping applied. When a value disagrees with the oracle, this isolates *which side* is wrong in one query.
- Build with `-DCMAKE_BUILD_TYPE=Debug` enabling DuckDB's vector verification, and run the full L2 suite in debug at least once before declaring done. Malformed `LIST` vectors frequently pass in release and assert in debug.
- A `DUCK_VTK_TRACE=1` environment variable that logs reader selection and per-array conversion decisions to stderr.
