# duck_vtk — Relational Schema Design

**Status:** normative for Phase 1. An implementation agent MUST follow this exactly; deviations require an explicit note in the PR description.

**Audience:** the implementation agent. Every table, column name, column order, and type below is a contract that the sqllogictest suite will assert against.

---

## 1. Design goals, in priority order

1. **Correctness over speed.** Phase 1 loads the whole VTK dataset into memory and converts it to DuckDB vectors. No streaming, no zero-copy, no parallelism. These are Phase 4+ concerns and the design must not preclude them.
2. **CAE-usable on day one.** The engineer's core workflow is: read a mesh, look at point-data and cell-data fields, filter/aggregate them, and join cells to point coordinates. That workflow must work end to end in Phase 1.
3. **Extensible without breaking changes.** Adding multiblock, time series, new formats, and new column layouts must be additive. Column *ordering* rules below exist so new features append rather than insert.
4. **Discoverable.** A user who has never seen the file must be able to learn its structure using only SQL.

## 2. Attach surface

```sql
ATTACH 'path/to/mesh.vtu' AS m (TYPE vtk);
SELECT * FROM m.points LIMIT 5;
```

Rules:

- `TYPE vtk` is matched case-insensitively by DuckDB (`TYPE VTK` and `TYPE vtk` both work). Register the storage extension under the lowercase key `vtk`.
- The attached database is **read-only, always**. Any DDL/DML against it raises an error (see §7). `ATTACH ... (TYPE vtk, READ_ONLY)` is accepted and is a no-op; if a user passes `READ_WRITE` explicitly, raise a `NotImplementedException` rather than silently ignoring it — silently accepting a write-mode request that cannot be honoured is worse than failing.
- All tables live in a single schema named `main`. `m.points` and `m.main.points` both resolve.
- The file is opened and fully read **at attach time**, not lazily. Rationale: schema discovery needs array names, component counts, and element types, and for legacy `.vtk` files those are only reliably known after a full read. Eager reading also means a bad path fails at `ATTACH` — where the user can see it — rather than at first `SELECT`. The cost is documented in §9.

### 2.1 Attach options (Phase 1)

| Option | Type | Default | Meaning |
|---|---|---|---|
| `TYPE` | identifier | — | Must be `vtk`. |
| `READ_ONLY` | flag | implied | Accepted, no-op. |

Options reserved for later phases — an implementation agent MUST reject unknown options with a clear error listing the accepted ones, so that adding these later is not a silent behaviour change:

| Option | Phase | Meaning |
|---|---|---|
| `vector_layout` | 3 | `'list'` (default) \| `'columns'` \| `'struct'` — see §4.3 |
| `block` | 3 | Select one block of a multiblock dataset |
| `time_step` | 3 | Select a time step index for time-series files |
| `null_value` | 3 | Numeric sentinel in the file to map to SQL NULL |

## 3. Tables

Exactly six tables in Phase 1. Names are lowercase.

| Table | Grain | Purpose |
|---|---|---|
| `points` | one row per point | Point coordinates + all point-data arrays |
| `cells` | one row per cell | Cell type + connectivity + all cell-data arrays |
| `cell_points` | one row per (cell, vertex slot) | Normalised connectivity, for joining cells to point coordinates |
| `field_data` | one row per dataset-level field-data array | Dataset metadata arrays |
| `vtk_arrays` | one row per array in the file | Schema/attribute catalogue |
| `vtk_info` | exactly one row | File- and dataset-level summary |

`points`, `cells`, and `cell_points` are the data tables. `field_data`, `vtk_arrays`, and `vtk_info` are the introspection tables and always exist, even for an empty dataset.

### 3.1 `points`

Column order is **fixed**: the three geometry columns first, then one column per point-data array **in VTK's array index order** (`vtkPointData::GetArrayName(0..n-1)`). New geometry columns are never inserted before existing ones.

| # | Column | Type | Notes |
|---|---|---|---|
| 0 | `point_id` | `BIGINT` | 0-based `vtkIdType`. Matches `cells.point_ids` values and `cell_points.point_id`. |
| 1 | `x` | `DOUBLE` | Always `DOUBLE`, even if the file stores `float`. See §4.1. |
| 2 | `y` | `DOUBLE` | |
| 3 | `z` | `DOUBLE` | Always present; 2D datasets report `0.0`. |
| 4..n | *one per point-data array* | per §4 | Named exactly as in the file. |

For datasets with implicit geometry (`vtkImageData`, `vtkRectilinearGrid`), coordinates are **synthesised** from origin/spacing/extent or the per-axis coordinate arrays. `point_id` follows VTK's own point ordering (x fastest, then y, then z) so that `point_id` agrees with `vtkDataSet::GetPoint(point_id, ...)`.

### 3.2 `cells`

| # | Column | Type | Notes |
|---|---|---|---|
| 0 | `cell_id` | `BIGINT` | 0-based. For `vtkPolyData`, this is the **global** cell id across the verts/lines/polys/strips arrays, matching `vtkDataSet::GetCell(cell_id)`. |
| 1 | `cell_type` | `INTEGER` | Numeric `VTK_*` cell type id. `INTEGER`, not `UTINYINT`: VTK's ids fit in a byte today but the type is `int` in the API and higher-order cell ids already exceed 40; using `INTEGER` avoids a future widening break. |
| 2 | `cell_type_name` | `VARCHAR` | e.g. `VTK_HEXAHEDRON`. From `vtkCellTypes::GetClassNameFromTypeId`, normalised — see §5. |
| 3 | `num_points` | `INTEGER` | `len(point_ids)`. Denormalised deliberately: it lets `WHERE num_points = 8` avoid materialising the list. |
| 4 | `point_ids` | `BIGINT[]` | Connectivity, in VTK's canonical vertex order for that cell type. **Vertex order is semantically load-bearing** for CAE (it defines face/normal orientation) and MUST be preserved exactly as VTK reports it — never sorted. |
| 5..n | *one per cell-data array* | per §4 | Named exactly as in the file. |

### 3.3 `cell_points`

The long-form join table. This is what makes the schema usable for real work:

```sql
-- cell centroids
SELECT cp.cell_id, avg(p.x), avg(p.y), avg(p.z)
FROM m.cell_points cp JOIN m.points p USING (point_id)
GROUP BY cp.cell_id;
```

| # | Column | Type | Notes |
|---|---|---|---|
| 0 | `cell_id` | `BIGINT` | |
| 1 | `vertex_index` | `INTEGER` | 0-based position within the cell's connectivity. Preserves vertex order. |
| 2 | `point_id` | `BIGINT` | |

No data columns. Users join to `points`/`cells` for those. Cardinality is `sum(num_points)`, which for a hex mesh is 8× the cell count — the `cardinality` callback must report this honestly so the planner does not pick a bad join order.

### 3.4 `field_data`

Dataset-level field data (`vtkDataObject::GetFieldData()`) is *not* per-point or per-cell — it is arbitrary metadata, often a single tuple, sometimes a short table. Exposing it as one column per array would produce a table whose grain is undefined. Instead it is exposed **long-form**:

| # | Column | Type | Notes |
|---|---|---|---|
| 0 | `array_name` | `VARCHAR` | |
| 1 | `tuple_index` | `BIGINT` | 0-based |
| 2 | `component_index` | `INTEGER` | 0-based |
| 3 | `component_name` | `VARCHAR` | NULL if the file names no components |
| 4 | `value_double` | `DOUBLE` | NULL for string arrays |
| 5 | `value_varchar` | `VARCHAR` | Always populated: the value rendered as text. For string arrays this is the only populated value column. |

Two value columns rather than a `UNION` type: keeps Phase 1 simple, and `value_double` covers the overwhelmingly common numeric case with a directly aggregatable type. Documented as a candidate for `UNION`/`ANY` in a later phase.

### 3.5 `vtk_arrays`

The schema catalogue. This is the table a user hits first to find out what is in a file.

| # | Column | Type | Notes |
|---|---|---|---|
| 0 | `association` | `VARCHAR` | `'POINT'`, `'CELL'`, or `'FIELD'` |
| 1 | `array_index` | `INTEGER` | Index within its association, matching VTK's order |
| 2 | `name` | `VARCHAR` | As stored in the file |
| 3 | `column_name` | `VARCHAR` | The column name actually used in `points`/`cells` after de-duplication (§6) — usually identical to `name` |
| 4 | `vtk_type` | `INTEGER` | `VTK_FLOAT` etc. numeric id |
| 5 | `vtk_type_name` | `VARCHAR` | e.g. `'float'`, `'vtkIdType'` |
| 6 | `sql_type` | `VARCHAR` | The DuckDB type used, e.g. `'DOUBLE'`, `'BIGINT[]'` |
| 7 | `num_components` | `INTEGER` | |
| 8 | `num_tuples` | `BIGINT` | Should equal the point/cell count; if it does not, see §8 |
| 9 | `component_names` | `VARCHAR[]` | NULL if unnamed |
| 10 | `active_as` | `VARCHAR` | `'SCALARS'`, `'VECTORS'`, `'NORMALS'`, `'TCOORDS'`, `'TENSORS'`, `'GLOBALIDS'`, `'PEDIGREEIDS'`, or NULL. If an array is active for several roles, a comma-joined list in that order. |

### 3.6 `vtk_info`

Exactly one row. Wide and flat, so `SELECT * FROM m.vtk_info` is a readable summary.

| # | Column | Type | Notes |
|---|---|---|---|
| 0 | `file_path` | `VARCHAR` | The path as given to `ATTACH` |
| 1 | `file_size_bytes` | `BIGINT` | |
| 2 | `reader_class` | `VARCHAR` | The VTK reader actually used, e.g. `vtkXMLUnstructuredGridReader` |
| 3 | `dataset_class` | `VARCHAR` | e.g. `vtkUnstructuredGrid` |
| 4 | `num_points` | `BIGINT` | |
| 5 | `num_cells` | `BIGINT` | |
| 6 | `num_point_arrays` | `INTEGER` | |
| 7 | `num_cell_arrays` | `INTEGER` | |
| 8 | `num_field_arrays` | `INTEGER` | |
| 9 | `bounds_x_min` .. | `DOUBLE` | Six columns: `bounds_x_min`, `bounds_x_max`, `bounds_y_min`, `bounds_y_max`, `bounds_z_min`, `bounds_z_max`. From `vtkDataSet::GetBounds`. NULL for an empty dataset (VTK returns an inverted/uninitialised bounds box for 0 points — detect `num_points == 0` and emit NULL rather than propagating VTK's `±VTK_DOUBLE_MAX` sentinel). |
| 15 | `vtk_version` | `VARCHAR` | Runtime VTK version, e.g. `'9.6.2'` |
| 16 | `extension_version` | `VARCHAR` | duck_vtk version |

## 4. Type mapping

### 4.1 Coordinates

`x`, `y`, `z` are **always `DOUBLE`**, regardless of whether the file stores `float` or `double`. Rationale: `vtkDataSet::GetPoint` returns `double[3]` for every dataset type including the implicit-geometry ones, so `DOUBLE` is the only type that is uniformly correct; and a schema that changes type based on a file's internal storage precision makes portable SQL impossible. The float→double widening is exact.

### 4.2 Scalar (single-component) arrays

Single-component arrays become a single column whose type is the **narrowest DuckDB type that holds the VTK type without loss**. The authoritative mapping table lives in `docs/research/03-vtk-cpp-api-and-formats.md` §5; the implementation agent MUST use that table and MUST NOT collapse everything to `DOUBLE`. In particular:

- 64-bit integer arrays (`VTK_LONG_LONG`, `VTK_ID_TYPE` on a 64-bit-ids build) map to `BIGINT`/`UBIGINT` and MUST be read through a typed accessor, **not** `vtkDataArray::GetComponent`, which returns `double` and silently loses precision above 2^53. This is a correctness requirement, not an optimisation.
- `VTK_BIT` maps to `BOOLEAN`.
- `VTK_CHAR` maps to `TINYINT` (a numeric, not a one-character string) because VTK treats it as a number; `vtkStringArray` (`VTK_STRING`) maps to `VARCHAR`.

### 4.3 Multi-component arrays

Phase 1: a multi-component array becomes **one `LIST` column** of the mapped element type — `velocity DOUBLE[]`, not three columns.

Reasoning:

- It generalises to any component count, so 3-vectors, 9-component tensors, and a 15-component custom array all work with one code path.
- `SELECT velocity[1]` (1-based in DuckDB), `unnest(velocity)`, and `list_*` functions cover the ergonomic cases.
- It keeps the column count equal to the array count, so `vtk_arrays.array_index` maps 1:1 onto columns and column-order rules stay simple.

The rejected alternatives, recorded so this is not re-litigated:

- **Separate columns** (`velocity_x/_y/_z`): nicest for the common 3-vector case, but needs a naming policy for unnamed 9-component tensors, collides with existing array names, and makes column count depend on component counts. Offered later via `vector_layout='columns'`.
- **`ARRAY(child, n)`** (fixed-size): a better semantic fit than `LIST` since component count *is* fixed per array, and it is cheaper. It is a strong candidate for Phase 4 — but only after `docs/research/05-duckdb-table-function-and-vector-api.md` §3 confirms `ARRAY` construction and `NULL` handling are solid in v1.5.4. Phase 1 uses `LIST` because it is unambiguously well supported.
- **`STRUCT`**: best when `GetComponentName` is populated, meaningless when it is not. Offered later via `vector_layout='struct'`.

`num_components == 1` MUST produce a scalar column, never a 1-element list.

## 5. Cell type names

`cell_type_name` uses the `VTK_*` macro spelling (`VTK_TRIANGLE`, `VTK_HEXAHEDRON`, `VTK_QUADRATIC_TETRA`), not VTK's class name (`vtkTriangle`). Reasoning: the macro names are what the file-format spec and every CAE tool's documentation use, they are stable, and they are unambiguous — whereas `vtkCellTypes::GetClassNameFromTypeId` returns `"vtkEmptyCell"`-style strings and `NULL`/`"Null"` for gaps in the enum.

The implementation MUST use a static, exhaustive lookup table generated from `vtkCellType.h`, not a runtime VTK call. Unknown ids render as `'VTK_UNKNOWN_<id>'` rather than NULL or an error, so a newer VTK's new cell type degrades gracefully.

## 6. Column name collisions

VTK array names are arbitrary strings. They may collide with the reserved geometry columns, with each other, be empty, or contain characters needing SQL quoting.

Policy, applied in this order:

1. Array names are used **verbatim** — including spaces, dots, unicode, and mixed case. DuckDB handles these with double-quoting; mangling them would break the user's ability to correlate with their source file. `SELECT "Pressure [Pa]" FROM m.points` is expected to work.
2. An empty or whitespace-only array name becomes `unnamed_<association>_<array_index>`, e.g. `unnamed_point_2`.
3. If an array name collides (case-**sensitively**) with a reserved column of its table (`point_id`, `x`, `y`, `z` for `points`; `cell_id`, `cell_type`, `cell_type_name`, `num_points`, `point_ids` for `cells`), the array column is suffixed `_1`, then `_2`, until unique.
4. If two arrays in the same association have the same name, the later one is suffixed the same way. (VTK itself usually prevents this, but legacy readers can produce it.)
5. DuckDB resolves unquoted identifiers case-insensitively. So arrays named `Pressure` and `pressure` in the same association do **not** collide by rule 4 but *are* ambiguous to an unquoted query. Detect this case and suffix the later one, so every column is addressable.

`vtk_arrays.column_name` always reports the final, post-mangling name, and `vtk_arrays.name` the original. That pair is the user's escape hatch.

## 7. Read-only enforcement

Every write path must fail with a clear, actionable message rather than a crash or a confusing internal error. The catalog/schema-entry overrides for `CreateTable`, `CreateView`, `CreateIndex`, `CreateFunction`, `CreateSequence`, `CreateType`, `CreateCollation`, `CreateTableFunction`, `CreateCopyFunction`, `CreatePragmaFunction`, `DropEntry`, `Alter`, `PlanInsert`, `PlanDelete`, `PlanUpdate`, and `PlanCreateTableAs` MUST throw with a message of the form:

```
Not implemented Error: duck_vtk: attached VTK databases are read-only (attempted: CREATE TABLE)
```

Use `NotImplementedException` for "this operation could exist someday" and `BinderException` for "this can never work here". Every one of these is asserted in `test/sql/read_only.test` — an unimplemented override that segfaults instead of throwing is the most likely serious bug in this project, so the tests exist to force each override to be written.

## 8. Degenerate and malformed inputs

These are not edge cases to handle later; they are the cases a real CAE file hits first. Required Phase 1 behaviour:

| Situation | Required behaviour |
|---|---|
| 0 points, 0 cells | All six tables exist. `points`/`cells`/`cell_points`/`field_data` return 0 rows. `vtk_info` returns 1 row with zero counts and NULL bounds. **Not** an error. |
| Points but no cells (point cloud) | `cells` and `cell_points` return 0 rows. |
| Cells but no point data | `points` has only `point_id,x,y,z`. |
| Array `num_tuples` < point/cell count | Emit the available tuples and **NULL** for the remainder. Do not error, do not truncate the table. Record the mismatch in `vtk_arrays.num_tuples` so it is visible. |
| Array `num_tuples` > point/cell count | Emit the first `n` tuples, ignore the rest. Visible via `vtk_arrays.num_tuples`. |
| File does not exist | `IOException` at `ATTACH`, message including the path. |
| File exists but is not VTK / is truncated | `IOException` at `ATTACH` naming the file and the VTK error string. **Never** a crash, and never a silent empty dataset — a reader that fails must not be mistaken for an empty mesh. |
| Non-VTK content with a `.vtk` extension | Same as above. Extension is a hint, never the sole basis for dispatch. |
| `NaN` / `Inf` in a float array | Pass through as-is. DuckDB `DOUBLE` represents both. Do **not** convert to NULL. |
| Multiblock file in Phase 1 | Clear `NotImplementedException` at `ATTACH` naming the format and pointing at the roadmap — not a partial read of block 0. |

VTK writes warnings to its own output window on stderr, which would pollute a DuckDB session. The implementation MUST install a custom `vtkOutputWindow` that captures messages so they can be attached to the thrown exception, instead of leaking to the terminal.

## 9. Known Phase-1 costs (accepted, documented, not bugs)

- **Whole file in RAM.** `ATTACH` reads the entire dataset. A 4 GB `.vtu` needs ~4 GB plus conversion buffers. Not suitable for out-of-core work yet.
- **Data converted per scan, not cached.** Each `SELECT` re-walks the VTK arrays into DuckDB vectors. Fine at Phase-1 scale; the natural fix is a cached column-chunk store.
- **Single-threaded scans.** `MaxThreads()` returns 1. The design keeps state in a `GlobalTableFunctionState` so a later parallel version only needs a work-range split.
- **No filter pushdown.** Projection pushdown *is* implemented from the start, because with hundreds of array columns the conversion cost of unrequested columns dominates and it is cheap to honour `column_ids`.
- **`cell_points` is materialised per scan** from the connectivity arrays. No index.

## 10. Worked validation queries

These are the acceptance queries for Phase 1 — a CAE engineer's actual first session. Each must work on the Tier-1 corpus files.

```sql
-- 1. What is in this file?
ATTACH 'test/data/xml/quadraticTetra01.vtu' AS m (TYPE vtk);
SELECT * FROM m.vtk_info;
SELECT association, name, sql_type, num_components, num_tuples FROM m.vtk_arrays ORDER BY association, array_index;

-- 2. Geometry sanity: bounds computed by SQL must match VTK's own bounds
SELECT min(x), max(x), min(y), max(y), min(z), max(z) FROM m.points;
SELECT bounds_x_min, bounds_x_max, bounds_y_min, bounds_y_max, bounds_z_min, bounds_z_max FROM m.vtk_info;

-- 3. Cell type histogram
SELECT cell_type_name, count(*) FROM m.cells GROUP BY 1 ORDER BY 2 DESC;

-- 4. Connectivity integrity: every referenced point id must exist, exactly once per slot
SELECT count(*) FROM m.cell_points cp ANTI JOIN m.points p USING (point_id);   -- must be 0
SELECT sum(num_points) FROM m.cells;                                          -- must equal count(*) from cell_points

-- 5. Point-data field statistics (the everyday CAE query)
SELECT count(*), min(scalars), max(scalars), avg(scalars) FROM m.points;

-- 6. Cell centroids via the join table
SELECT cp.cell_id, avg(p.x) AS cx, avg(p.y) AS cy, avg(p.z) AS cz
FROM m.cell_points cp JOIN m.points p USING (point_id)
GROUP BY cp.cell_id ORDER BY cp.cell_id LIMIT 5;

-- 7. Vector field component access
SELECT point_id, velocity[1], velocity[2], velocity[3] FROM m.points LIMIT 5;

-- 8. Read-only enforcement
CREATE TABLE m.foo(i INT);  -- must raise, not crash
```

## 11. Open questions for the implementer to resolve and record

1. Does `vtkPolyData`'s global `cell_id` ordering (verts → lines → polys → strips) match `vtkCellData` array tuple ordering? Verify empirically against a `.vtp` with cell data and record the answer in a code comment; the whole `cells` table's correctness for polydata depends on it.
2. For `vtkImageData`, is synthesising coordinates from origin/spacing cheaper and exactly equal to calling `GetPoint` per point? Prefer whichever is exactly correct; note the measured difference.
3. Confirm whether `vtk_arrays` and `vtk_info` should be excluded from `SHOW TABLES` (they are introspection, not data). Phase 1: include them — a user should be able to see everything. Revisit if it proves noisy.
