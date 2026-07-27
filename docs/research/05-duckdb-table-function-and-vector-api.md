# DuckDB v1.5.4 — Table Functions and the Vector/DataChunk API

> **VERSION SCOPE — read before trusting a line number.**
>
> This document was researched against DuckDB **v1.5.4** (`08e34c447b`), which was
> the pin at the time. The project now targets **v1.5.5** on the default line and
> **v1.4.5** on the LTS line; `git ls-tree HEAD duckdb` is the live pin.
>
> It is kept at v1.5.4 deliberately rather than rewritten. Every fact here was read
> from real headers at that tag, and the API facts have held: the `ExtensionLoader` /
> `DUCKDB_CPP_EXTENSION_ENTRY` entrypoint, the removal of `ExtensionUtil`, and the
> `LookupSchema`/`LookupEntry` pure virtuals are all unchanged in v1.5.5. Rewriting
> the citations to a new tag would cost the one thing that makes this useful — that
> every claim was verified against a specific, checkable revision.
>
> What that means in practice: trust the **API shapes**, re-check **`file:line`
> citations** against the tag you are actually building. The one API that genuinely
> differs between the 1.4 and 1.5 lines is storage-extension registration, and
> `CMakeLists.txt` handles it by probing the header rather than by version string —
> `make check-api-compat` proves both branches compile.


**Target version:** DuckDB `v1.5.4`, resolved tag commit **`08e34c447bae34eaee3723cac61f2878b6bdf787`**
(verified via `git ls-remote --tags https://github.com/duckdb/duckdb` and cross-checked against the
`duckdb --version` string `v1.5.4 (Variegata) 08e34c447b`).

**Method note:** A full `git clone` of duckdb was infeasible in this environment (bandwidth-constrained,
multiple concurrent clones). All citations below were obtained by fetching the *real* file content for
this exact tag over `https://raw.githubusercontent.com/duckdb/duckdb/v1.5.4/<path>` and reading it
directly — not from memory/training data. Context7 MCP was unavailable in this environment
("Invalid API key"), so it was not used; source code is authoritative for this doc regardless.
Every `file:line` citation refers to the path under the duckdb repo root, at tag `v1.5.4`.

Build config fact folded in from the coordinator (confirmed applicable): the extension entrypoint style
in v1.5.4 is the **new** `DUCKDB_CPP_EXTENSION_ENTRY(name, loader)` / `ExtensionLoader &` form —
the legacy `<name>_init(DatabaseInstance &)` entrypoint is gone. C++ standard is C++17.

## Table of contents

1. [TableFunction anatomy](#1-tablefunction-anatomy)
2. [The scan loop contract](#2-the-scan-loop-contract)
3. [Writing to Vectors — the practical core](#3-writing-to-vectors--the-practical-core)
4. [LogicalType → PhysicalType → C++ type reference](#4-logicaltype--physicaltype--c-type-reference)
5. [Errors, cancellation, progress](#5-errors-cancellation-progress)
6. [Replacement scans](#6-replacement-scans)
7. [Complete worked example: `demo_scan`](#7-complete-worked-example-demo_scan)
8. [Uncertainties / verify by compiling](#8-uncertainties--verify-by-compiling)

---

## 1. TableFunction anatomy

Source: `src/include/duckdb/function/table_function.hpp` (516 lines total).

### 1.1 Support structs

```cpp
struct GlobalTableFunctionState {
public:
	// value returned from MaxThreads when as many threads as possible should be used
	constexpr static const int64_t MAX_THREADS = 999999999;
public:
	DUCKDB_API virtual ~GlobalTableFunctionState();
	virtual idx_t MaxThreads() const {
		return 1;
	}
	template <class TARGET> TARGET &Cast();             // dynamic-cast-checked downcast
	template <class TARGET> const TARGET &Cast() const;
};
```
`src/include/duckdb/function/table_function.hpp:59-81`. Default `MaxThreads()` is `1` — a table
function is single-threaded unless it overrides this.

```cpp
struct LocalTableFunctionState {
	DUCKDB_API virtual ~LocalTableFunctionState();
	template <class TARGET> TARGET &Cast();
	template <class TARGET> const TARGET &Cast() const;
};
```
`table_function.hpp:83-96`. No `MaxThreads`; purely a thread-local scratchpad. Subclass both
`GlobalTableFunctionState` and `LocalTableFunctionState` — they have no other required overrides.

```cpp
struct TableFunctionBindInput {
	TableFunctionBindInput(vector<Value> &inputs, named_parameter_map_t &named_parameters,
	                       vector<LogicalType> &input_table_types, vector<string> &input_table_names,
	                       optional_ptr<TableFunctionInfo> info, optional_ptr<Binder> binder,
	                       TableFunction &table_function, const TableFunctionRef &ref)
	vector<Value> &inputs;
	named_parameter_map_t &named_parameters;
	vector<LogicalType> &input_table_types;
	vector<string> &input_table_names;
	optional_ptr<TableFunctionInfo> info;
	optional_ptr<Binder> binder;
	TableFunction &table_function;
	const TableFunctionRef &ref;
};
```
`table_function.hpp:98-115`. `input.inputs[i]` are the positional argument `Value`s;
`input.named_parameters` is a `named_parameter_map_t` (`unordered_map<string, Value>` — see §1.4).

```cpp
struct TableFunctionInitInput {
	TableFunctionInitInput(optional_ptr<const FunctionData> bind_data_p, vector<column_t> column_ids_p,
	                       const vector<idx_t> &projection_ids_p, optional_ptr<TableFilterSet> filters_p,
	                       optional_ptr<SampleOptions> sample_options_p = nullptr,
	                       optional_ptr<const PhysicalOperator> op_p = nullptr);
	// (+ a second overload taking vector<ColumnIndex> column_indexes_p instead of column_ids_p)

	optional_ptr<const FunctionData> bind_data;
	vector<column_t> column_ids;
	vector<ColumnIndex> column_indexes;
	const vector<idx_t> projection_ids;
	optional_ptr<TableFilterSet> filters;
	optional_ptr<SampleOptions> sample_options;
	optional_ptr<const PhysicalOperator> op;

	bool CanRemoveFilterColumns() const {
		if (projection_ids.empty()) return false;              // no filter columns to remove
		if (projection_ids.size() == column_ids.size()) return false; // used downstream, can't remove
		return true;                                             // fewer projected than scanned
	}
};
```
`table_function.hpp:117-160`. **Important, and corrects an assumption in the brief**: there is
**no `GetColumnIds()` method** in v1.5.4 — `column_ids` (`vector<column_t>`, `column_t = idx_t =
uint64_t`, `src/include/duckdb/common/typedefs.hpp:16,38`) is a **public data member**, read directly.
There is also a newer, richer `column_indexes` (`vector<ColumnIndex>`) member (each `ColumnIndex` can
represent a struct/variant sub-field extraction — `src/include/duckdb/common/column_index.hpp:16-42`);
both are kept in sync by whichever constructor overload was used (lines 122-127, 133-138). For a simple
scan, treat `column_ids[i]` as "the logical column index (into your bind-time `return_types`/`names`)
that must be written into `output.data[i]`" — see §2.3 for full semantics, including
`COLUMN_IDENTIFIER_ROW_ID`.

```cpp
struct TableFunctionInput {
	TableFunctionInput(optional_ptr<const FunctionData> bind_data_p,
	                   optional_ptr<LocalTableFunctionState> local_state_p,
	                   optional_ptr<GlobalTableFunctionState> global_state_p);
	optional_ptr<const FunctionData> bind_data;
	optional_ptr<LocalTableFunctionState> local_state;
	optional_ptr<GlobalTableFunctionState> global_state;
	AsyncResult async_result {};
	AsyncResultsExecutionMode results_execution_mode {AsyncResultsExecutionMode::SYNCHRONOUS};
};
```
`table_function.hpp:162-176`. `async_result`/`results_execution_mode` are new in the 1.5 line (async
task-executor support); a synchronous scan can ignore them entirely.

Also present (used for advanced hooks, not needed for a basic scan): `TableFunctionPartitionInput`
(178-185), `TableFunctionToStringInput` (187-193), `TableFunctionDynamicToStringInput` (195-207),
`TableFunctionGetPartitionInput` (209-224), `TableFunctionGetStatisticsInput` (226-235),
`GetPartitionStatsInput` (237-244), and `BindInfo` (248-286, used with `get_bind_info`).

### 1.2 Function-pointer typedefs

All from `table_function.hpp:288-357`:

```cpp
typedef unique_ptr<FunctionData> (*table_function_bind_t)(
    ClientContext &context, TableFunctionBindInput &input,
    vector<LogicalType> &return_types, vector<string> &names);                       // :288-289

typedef unique_ptr<TableRef> (*table_function_bind_replace_t)(
    ClientContext &context, TableFunctionBindInput &input);                          // :290

typedef unique_ptr<GlobalTableFunctionState> (*table_function_init_global_t)(
    ClientContext &context, TableFunctionInitInput &input);                          // :294-295

typedef unique_ptr<LocalTableFunctionState> (*table_function_init_local_t)(
    ExecutionContext &context, TableFunctionInitInput &input,
    GlobalTableFunctionState *global_state);                                         // :296-298

typedef void (*table_function_t)(
    ClientContext &context, TableFunctionInput &data, DataChunk &output);            // :303

typedef OperatorResultType (*table_in_out_function_t)(
    ExecutionContext &context, TableFunctionInput &data,
    DataChunk &input, DataChunk &output);                                            // :304-305

typedef unique_ptr<NodeStatistics> (*table_function_cardinality_t)(
    ClientContext &context, const FunctionData *bind_data);                          // :322-323

typedef double (*table_function_progress_t)(
    ClientContext &context, const FunctionData *bind_data,
    const GlobalTableFunctionState *global_state);                                   // :319-320

typedef BindInfo (*table_function_get_bind_info_t)(const optional_ptr<FunctionData> bind_data);   // :311
typedef void (*table_function_pushdown_complex_filter_t)(
    ClientContext &context, LogicalGet &get, FunctionData *bind_data,
    vector<unique_ptr<Expression>> &filters);                                        // :330-332
typedef InsertionOrderPreservingMap<string> (*table_function_to_string_t)(TableFunctionToStringInput &input); // :334
typedef void (*table_function_serialize_t)(Serializer &serializer,
    const optional_ptr<FunctionData> bind_data, const TableFunction &function);      // :338-339
typedef unique_ptr<FunctionData> (*table_function_deserialize_t)(
    Deserializer &deserializer, TableFunction &function);                            // :340
typedef virtual_column_map_t (*table_function_get_virtual_columns_t)(
    ClientContext &context, optional_ptr<FunctionData> bind_data);                   // :350-351
typedef vector<column_t> (*table_function_get_row_id_columns)(
    ClientContext &context, optional_ptr<FunctionData> bind_data);                   // :353-354
```

`table_in_out_function_t` is the "in-out"/table-in-table-out shape (used by `range`/`generate_series`,
§2.2) — **not** what you want for a plain file scan; use `table_function_t` (`function` member) for a
"produce rows from nothing/from bind-time args" scan, which is the shape needed for VTK file reading.

### 1.3 `TableFunction` class

`class TableFunction : public SimpleNamedParameterFunction` — `table_function.hpp:362`. It inherits
`named_parameters` (a `named_parameter_type_map_t`) from `SimpleNamedParameterFunction`
(`src/include/duckdb/function/function.hpp:155-167`) — **this is a member you get for free via
inheritance**, not declared again on `TableFunction` itself.

Constructors (`table_function.hpp:364-380`):

```cpp
DUCKDB_API TableFunction();
DUCKDB_API TableFunction(string name, const vector<LogicalType> &arguments, table_function_t function,
                         table_function_bind_t bind = nullptr, table_function_init_global_t init_global = nullptr,
                         table_function_init_local_t init_local = nullptr);
DUCKDB_API TableFunction(const vector<LogicalType> &arguments, table_function_t function,
                         table_function_bind_t bind = nullptr, table_function_init_global_t init_global = nullptr,
                         table_function_init_local_t init_local = nullptr);
// + two more overloads identical but taking std::nullptr_t for `function`
//   (used when only in_out_function is set, e.g. range.cpp)
```

Every settable member of `TableFunction` (`table_function.hpp:404-508`), verbatim field list:

```cpp
table_function_bind_t bind;
table_function_bind_replace_t bind_replace;
table_function_bind_operator_t bind_operator;
table_function_init_global_t init_global;
table_function_init_local_t init_local;
table_function_t function;
table_in_out_function_t in_out_function;
table_in_out_function_final_t in_out_function_final;
table_statistics_t statistics;
table_statistics_extended_t statistics_extended;
table_function_dependency_t dependency;
table_function_cardinality_t cardinality;
table_function_rows_scanned_t rows_scanned;              // deprecated compat, prefer get_metrics
table_function_get_metrics_t get_metrics;
table_function_pushdown_complex_filter_t pushdown_complex_filter;
table_function_pushdown_expression_t pushdown_expression;
table_function_to_string_t to_string;
table_function_dynamic_to_string_t dynamic_to_string;
table_function_progress_t table_scan_progress;
table_function_get_partition_data_t get_partition_data;
table_function_get_bind_info_t get_bind_info;
table_function_type_pushdown_t type_pushdown;
table_function_get_multi_file_reader_t get_multi_file_reader;
table_function_supports_pushdown_type_t supports_pushdown_type;
table_function_supports_pushdown_extract_t supports_pushdown_extract;
table_function_get_partition_info_t get_partition_info;
table_function_get_partition_stats_t get_partition_stats;
table_function_get_virtual_columns_t get_virtual_columns;
table_function_get_row_id_columns get_row_id_columns;
table_function_set_scan_order set_scan_order;

table_function_serialize_t serialize;
table_function_deserialize_t deserialize;
bool verify_serialization = true;

bool projection_pushdown;      // if false, DuckDB adds a Projection on top and filters columns for you
bool filter_pushdown;          // if false, DuckDB applies a Filter on top of your scan
bool filter_prune;             // can filter-only columns be pruned entirely from your output?
bool sampling_pushdown;
bool late_materialization;
shared_ptr<TableFunctionInfo> function_info;
OrderPreservationType order_preservation_type = OrderPreservationType::INSERTION_ORDER;
TableFunctionInitialization global_initialization = TableFunctionInitialization::INITIALIZE_ON_EXECUTE;

DUCKDB_API bool Equal(const TableFunction &rhs) const;
DUCKDB_API bool operator==(const TableFunction &rhs) const;
DUCKDB_API bool operator!=(const TableFunction &rhs) const;
```
(`table_function.hpp:407-513`; the `named_parameters` field named in the task brief is **not** here —
it is the inherited `named_parameter_type_map_t named_parameters` from
`SimpleNamedParameterFunction`, `function.hpp:162`.)

Also note the "when to init" enum:
```cpp
enum class TableFunctionInitialization { INITIALIZE_ON_EXECUTE, INITIALIZE_ON_SCHEDULE };
```
`table_function.hpp:360`.

### 1.4 `FunctionData` / `TableFunctionData`

`src/include/duckdb/function/function.hpp:58-91`:

```cpp
struct FunctionData {
	DUCKDB_API virtual ~FunctionData();
	DUCKDB_API virtual unique_ptr<FunctionData> Copy() const = 0;
	DUCKDB_API virtual bool Equals(const FunctionData &other) const = 0;
	DUCKDB_API static bool Equals(const FunctionData *left, const FunctionData *right);
	DUCKDB_API virtual bool SupportStatementCache() const;
	template <class TARGET> TARGET &Cast();
	template <class TARGET> const TARGET &Cast() const;
};

struct TableFunctionData : public FunctionData {
	// used to pass on projections to table functions that support them. NB, can contain COLUMN_IDENTIFIER_ROW_ID
	vector<idx_t> column_ids;
	DUCKDB_API ~TableFunctionData() override;
	DUCKDB_API unique_ptr<FunctionData> Copy() const override;
	DUCKDB_API bool Equals(const FunctionData &other) const override;
};
```

**Confirmed exact signature**: `Equals(const FunctionData &other) const` — takes a `const FunctionData &`,
not a pointer, and is `const`. `Copy()` returns `unique_ptr<FunctionData>` and is `const`.

Crucially, the *default* implementations of `TableFunctionData::Copy()`/`Equals()` in
`src/function/function.cpp:31-37` are **not safe generic copies** — they are stubs:

```cpp
unique_ptr<FunctionData> TableFunctionData::Copy() const {
	throw InternalException("Copy not supported for TableFunctionData");
}
bool TableFunctionData::Equals(const FunctionData &other) const {
	return false;
}
```
`src/function/function.cpp:31-37`. **Every custom bind-data subclass must override both `Copy()` and
`Equals()` itself**, or any code path that needs to copy the bound plan (prepared statements re-executed,
certain CTE/subquery duplication paths) will throw `InternalException`. `Equals()` returning `false`
is "safe" (just disables plan/statement-cache reuse) but should still normally be implemented properly.

### 1.5 Named parameters

Declare them by inserting into `TableFunction::named_parameters` (inherited
`named_parameter_type_map_t`, i.e. `unordered_map<string, LogicalType>`) after constructing the
`TableFunction`, e.g. `my_func.named_parameters["compression"] = LogicalType::VARCHAR;`. In `bind`,
read them from `TableFunctionBindInput::named_parameters` (`named_parameter_map_t`, i.e.
`unordered_map<string, Value>` — `table_function.hpp:108`), e.g.:
```cpp
auto it = input.named_parameters.find("compression");
if (it != input.named_parameters.end()) {
    auto compression = it->second.GetValue<string>();   // Value::GetValue<T>()
}
```
`Value` extraction helpers used throughout the codebase: `Value::GetValue<T>()` (generic template),
and type-specific static accessors such as `BooleanValue::Get(value)`, `StringValue::Get(value)`,
`IntegerValue::Get(value)` (declared in `src/include/duckdb/common/types/value.hpp`, not separately
fetched here — pattern confirmed via `BindInfo::GetOption<T>` at `table_function.hpp:263-269` which
itself calls `options[name].GetValue<T>()`).

### 1.6 v1.5.4-specific bind signature — confirmed

```cpp
typedef unique_ptr<FunctionData> (*table_function_bind_t)(
    ClientContext &context, TableFunctionBindInput &input,
    vector<LogicalType> &return_types, vector<string> &names);
```
`table_function.hpp:288-289`. Yes: `bind` receives `ClientContext &`, and you set the schema by
`push_back`/`emplace_back`-ing into the **by-reference** `return_types`/`names` vectors (see
`RangeFunctionBind`, `src/function/table/range.cpp:59-71`, and `DuckDBTablesBind`,
`src/function/table/system/duckdb_tables.cpp:24-75`, both of which do exactly this).

---

## 2. The scan loop contract

### 2.1 `SetCardinality` / end-of-stream — confirmed from `range.cpp`, `glob.cpp`, `duckdb_tables.cpp`

`table_function_t` returns `void`. End of stream is signaled by **not increasing `output`'s
cardinality above 0** on a call — concretely, either call `output.SetCardinality(0)` (explicit) or
simply `return;` without touching `output` at all, relying on the chunk having been reset to size 0
already (see §2.2 — DuckDB does this for you). Real example,
`src/function/table/system/duckdb_tables.cpp:99-104`:
```cpp
void DuckDBTablesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBTablesData>();
	if (data.offset >= data.entries.size()) {
		// finished returning values
		return;                                    // <-- no SetCardinality call; output stays at count 0
	}
	...
	output.SetCardinality(count);                    // count in [1, STANDARD_VECTOR_SIZE]
}
```
Same pattern in `src/function/table/glob.cpp:40-55` (`output.SetCardinality(count);` with `count` from
a `while (count < STANDARD_VECTOR_SIZE)` loop that `break`s early when the source is exhausted).

### 2.2 **Is `output` reset before each call? — Yes, definitively.**

`DataChunk::Reset()` is documented at `src/include/duckdb/common/types/data_chunk.hpp:141-144`:
```cpp
//! Resets the DataChunk to its state right after the DataChunk::Initialize
//! function was called. This sets the count to 0, the capacity to initial_capacity and resets each member
//! Vector to point back to the data owned by this DataChunk.
DUCKDB_API void Reset();
```
And the call site that proves it happens **before every invocation of your `function` callback**:
`src/parallel/pipeline_executor.cpp:229-230`, inside `PipelineExecutor::Execute`:
```cpp
// "Regular" path: fetch a chunk from the source and push it through the pipeline
source_chunk.Reset();
source_result = FetchFromSource(source_chunk);
```
`FetchFromSource` → `PipelineExecutor::GetData` (`pipeline_executor.cpp:492-509`) →
`pipeline.source->GetData(context, chunk, input)` → for a table scan,
`PhysicalTableScan::GetDataInternal`, which calls your function directly:
`src/execution/operator/scan/physical_table_scan.cpp:176`:
```cpp
function.function(context.client, data, chunk);
```
So **you never need to call `output.Reset()` yourself** inside `table_function_t` — by the time your
function runs, `output` already has count 0, all vectors are `FLAT_VECTOR` again, string/list heaps
are freed, and validity masks are back to "all valid" (`nullptr` validity_mask, see
`src/include/duckdb/common/types/validity_mask.hpp:82-84`, `AllValid()` returns true when
`validity_mask == nullptr`). You do **not** need `SetAllValid` between chunks unless you are reusing a
`Vector`/`DataChunk` object *outside* the normal per-call reset (e.g. hand-rolled state kept across
calls without going through `DataChunk::Reset()`).

### 2.3 Projection pushdown semantics — `column_ids`

**Key nuance not obvious from the header alone**: `column_ids` only matters if
`TableFunction::projection_pushdown = true`. If you leave it `false` (the default — no explicit
default shown, but it is a `bool` with no initializer so it must be explicitly set true for pushdown;
uninitialized `bool` members in `TableFunction()`'s default constructor are set false in practice,
confirmed by all example functions below never setting it), DuckDB **always** expects your `function`
to fill in *all* declared columns from `bind`'s `return_types`, in that fixed order, and it inserts a
`Projection` operator on top that discards the ones the query didn't ask for. This is what
`duckdb_tables.cpp`/`duckdb_columns.cpp`/`glob.cpp`/`range.cpp` all do — none of them read
`input.column_ids` in their `init`, and their `function` unconditionally writes every column.

If you *do* set `projection_pushdown = true`, DuckDB will pass you a possibly-reordered/possibly-
narrowed `column_ids` at `init_global`/`init_local` time, and **your `output` chunk must then contain
exactly `column_ids.size()` vectors, where `output.data[i]` holds the data for logical column
`column_ids[i]`** (i.e., the index into your original `return_types`/`names` list from `bind`). This
1:1, in-order mapping is proven by DuckDB's own internal table storage scan,
`src/function/table/table_scan.cpp`:
```cpp
scan_function.projection_pushdown = true;                                    // table_scan.cpp:925
...
for (const auto &col_idx : input.column_indexes) {
    l_state->column_ids.push_back(bind_data.table.GetStorageIndex(col_idx));  // table_scan.cpp:144-145
}
...
storage.Fetch(tx, output, column_ids, local_vector, scan_count, l_state.fetch_state); // table_scan.cpp:206
```
— `storage.Fetch`/`local_storage.Scan` write directly into `output`, one `Vector` per entry of
`column_ids`, in that order (`table_scan.cpp:203-232`). When filter columns must be scanned but not
returned to the caller (`CanRemoveFilterColumns()`, `table_function.hpp:148-159`), the pattern is to
scan into a wider `all_columns` chunk and then `output.ReferenceColumns(l_state.all_columns,
projection_ids)` (`table_scan.cpp:204,230`) to slice down to just the requested output columns — an
optimization you can skip for a phase-1 implementation.

**`COLUMN_IDENTIFIER_ROW_ID`** — a sentinel value that can appear inside `column_ids`/`column_indexes`
meaning "the query wants the virtual row-id column, not a real data column":
```cpp
//! Special value used to signify the ROW ID of a table
DUCKDB_API extern const column_t COLUMN_IDENTIFIER_ROW_ID;
//! Special value used to signify an empty column (used for e.g. COUNT(*))
DUCKDB_API extern const column_t COLUMN_IDENTIFIER_EMPTY;
DUCKDB_API extern const column_t VIRTUAL_COLUMN_START;
```
`src/include/duckdb/common/constants.hpp:41-45`. Concrete values, from
`src/common/constants.cpp:12-14`:
```cpp
const column_t COLUMN_IDENTIFIER_ROW_ID = UINT64_C(18446744073709551615);   // == UINT64_MAX
const column_t COLUMN_IDENTIFIER_EMPTY  = UINT64_C(18446744073709551614);   // UINT64_MAX - 1
const column_t VIRTUAL_COLUMN_START     = UINT64_C(9223372036854775808);    // 2^63
```
Virtual columns in general are declared via `get_virtual_columns` returning a
`virtual_column_map_t` (`= unordered_map<column_t, TableColumn>`,
`src/include/duckdb/common/table_column.hpp:16-28`, where `TableColumn{string name; LogicalType
type;}`). Real implementation, `table_scan.cpp:895-904`:
```cpp
virtual_column_map_t TableScanGetVirtualColumns(ClientContext &context, optional_ptr<FunctionData> bind_data_p) {
	auto &bind_data = bind_data_p->Cast<TableScanBindData>();
	return bind_data.table.GetVirtualColumns();
}
vector<column_t> TableScanGetRowIdColumns(ClientContext &context, optional_ptr<FunctionData> bind_data) {
	vector<column_t> result;
	result.emplace_back(COLUMN_IDENTIFIER_ROW_ID);
	return result;
}
```
For a VTK mesh scan you almost certainly don't need virtual columns / row IDs — skip
`get_virtual_columns`/`get_row_id_columns` entirely (leave them `nullptr`).

### 2.4 `STANDARD_VECTOR_SIZE`

```cpp
#define DEFAULT_STANDARD_VECTOR_SIZE 2048U
#ifndef STANDARD_VECTOR_SIZE
#define STANDARD_VECTOR_SIZE DEFAULT_STANDARD_VECTOR_SIZE
#endif
#if (STANDARD_VECTOR_SIZE & (STANDARD_VECTOR_SIZE - 1) != 0)
#error The vector size must be a power of two
#endif
```
`src/include/duckdb/common/vector_size.hpp:16-25`. Value is **2048** by default, must be a power of
two, and is a compile-time macro (overridable at CMake configure time, but 2048 is what you'll get
unless you deliberately override it).

### 2.5 Multi-threaded scans — the shape (kept short; you're single-threaded in phase 1)

The standard pattern used throughout duckdb (best real example: `table_scan.cpp`'s
`TableScanGlobalState`/`DuckIndexScanState`):

1. `init_global` builds a `GlobalTableFunctionState` subclass holding a **shared work queue** (e.g. a
   list of row-group/file/batch descriptors) plus a `mutex`, and overrides
   `idx_t MaxThreads() const override { return max_threads; }` (`table_scan.cpp:94-96`) — DuckDB uses
   this return value to decide how many pipeline threads to spin up for this scan.
2. `init_local` (signature `unique_ptr<LocalTableFunctionState> (*)(ExecutionContext&,
   TableFunctionInitInput&, GlobalTableFunctionState*)`) builds a small per-thread
   `LocalTableFunctionState` subclass (e.g. `IndexScanLocalState`, `table_scan.cpp:48-62`) that holds
   only thread-local scratch (a scan cursor, a reusable buffer) — no shared mutable state.
3. `function`/`table_scan_progress`/etc. take a `lock_guard<mutex>` (see `index_scan_lock` at
   `table_scan.cpp:124` and its use at `table_scan.cpp:169-189`) only around the tiny critical section
   that claims the next unit of work (e.g. `next_batch_index++`), then releases the lock before doing
   the actual (expensive) I/O/decoding work into `output`.

This is a drop-in-later shape: for phase 1, just implement `init_global` (return a state with
`MaxThreads() { return 1; }`, the default already), skip `init_local` entirely (it's optional — leave
`nullptr`), and keep all scan-position state in the global state. When you're ready to parallelize,
move the "current file offset" bookkeeping behind a mutex in the global state and add per-thread
buffers via `init_local`.

---

## 3. Writing to Vectors — the practical core

All from `src/include/duckdb/common/types/vector.hpp` unless noted; `Vector` layout fields (private/
protected) at `vector.hpp:298-312`: `VectorType vector_type; LogicalType type; data_ptr_t data;
ValidityMask validity; buffer_ptr<VectorBuffer> buffer; buffer_ptr<VectorBuffer> auxiliary;`.

### 3.1 Flat primitives

```cpp
struct FlatVector {
	static inline data_ptr_t GetData(Vector &vector);
	template <class T> static inline const T *GetData(const Vector &vector);
	template <class T> static inline T *GetData(Vector &vector);          // vector.hpp:462-465
	...
};
```
`vector.hpp:444-501`. Typical use:
```cpp
auto data = FlatVector::GetData<int64_t>(output.data[0]);
for (idx_t i = 0; i < count; i++) data[i] = some_value;
```
`FlatVector::GetData<T>` asserts (via `ConstantVector::VerifyVectorType<T>`,
`vector.hpp:334-344`) that the vector's `PhysicalType` is storage-compatible with `T`
(`StorageTypeCompatible<T>`, `src/include/duckdb/common/type_util.hpp:96-105`) — a type mismatch
throws `InternalException` (or asserts, if built with `-DDUCKDB_DEBUG_NO_SAFETY`, see §3.7).

**When must you call `vec.SetVectorType(VectorType::FLAT_VECTOR)`?** Every vector freshly obtained
from `DataChunk::Reset()`/`Initialize()` is already a `FLAT_VECTOR` (confirmed by `data_chunk.hpp:141-
144`'s doc comment: "resets each member Vector to point back to the data owned by this DataChunk" —
i.e., a flat, chunk-owned buffer). You only need `SetVectorType(VectorType::FLAT_VECTOR)` explicitly
if you had previously turned the vector into a `CONSTANT_VECTOR`/`DICTIONARY_VECTOR`/`SEQUENCE_VECTOR`
yourself (e.g. via `Vector::Sequence`, used by `range.cpp:171`,
`output.data[0].Sequence(current_value_i64, ..., remaining);`) and now want to write per-row data
again — not something you'll normally hit in a straightforward scan.

### 3.2 NULLs

```cpp
DUCKDB_API static void SetNull(Vector &vector, idx_t idx, bool is_null);   // FlatVector, vector.hpp:495
static inline ValidityMask &Validity(Vector &vector) { ... }               // FlatVector, vector.hpp:487-490
static inline bool IsNull(const Vector &vector, idx_t idx) { ... }         // FlatVector, vector.hpp:496-499
```
Simplest form:
```cpp
FlatVector::SetNull(output.data[col], row, true);
```
Bulk form via the mask directly (`src/include/duckdb/common/types/validity_mask.hpp`):
```cpp
FlatVector::Validity(vec).SetInvalid(row_idx);   // ValidityMask::SetInvalid, validity_mask.hpp:242-253
FlatVector::Validity(vec).SetValid(row_idx);     // validity_mask.hpp:213-226
FlatVector::Validity(vec).SetAllValid(count);    // validity_mask.hpp:296-309
FlatVector::Validity(vec).SetAllInvalid(count);  // validity_mask.hpp:291-293
```
`ValidityMask::AllValid()` (`validity_mask.hpp:82-84`) returns true whenever the internal
`validity_mask` pointer is `nullptr` — i.e. "no nulls" is represented by *absence* of a mask, not an
all-1s bitmap; the mask is lazily allocated on first `SetInvalid`/`Initialize` call
(`validity_mask.hpp:249-251`: `if (!validity_mask) { Initialize(capacity); }`).

**Do you need to reset validity between chunks?** No — per §2.2, `DataChunk::Reset()` re-points every
vector at a fresh chunk-owned buffer, which starts with `validity_mask == nullptr` (all valid). You
only call `SetAllValid` if you're manually reusing a `Vector`/mask object *outside* the normal
per-call chunk lifecycle.

### 3.3 Strings

```cpp
struct StringVector {
	DUCKDB_API static string_t AddString(Vector &vector, const char *data, idx_t len);
	DUCKDB_API static string_t AddStringOrBlob(Vector &vector, const char *data, idx_t len);
	DUCKDB_API static string_t AddString(Vector &vector, const char *data);
	DUCKDB_API static string_t AddString(Vector &vector, string_t data);
	DUCKDB_API static string_t AddString(Vector &vector, const string &data);
	DUCKDB_API static string_t AddStringOrBlob(Vector &vector, string_t data);
	DUCKDB_API static string_t EmptyString(Vector &vector, idx_t len);
	...
};
```
`vector.hpp:548-574`. **Critical usage detail confirmed from the real implementation**
(`src/common/types/vector.cpp:2235-2243`):
```cpp
string_t StringVector::AddString(Vector &vector, string_t data) {
	D_ASSERT(vector.GetType().id() == LogicalTypeId::VARCHAR || vector.GetType().id() == LogicalTypeId::BIT);
	if (data.IsInlined()) {
		// string will be inlined: no need to store in string heap
		return data;
	}
	auto &string_buffer = GetStringBuffer(vector);
	return string_buffer.AddString(data);
}
```
`AddString`/`AddStringOrBlob` **only copies the bytes into the vector's string heap and returns a
`string_t` handle** — it does **not** write that handle into the vector's data array for you. You must
still assign the returned value into the flat data slot yourself:
```cpp
string s = ComputeName(row);
FlatVector::GetData<string_t>(vec)[row] = StringVector::AddString(vec, s);
```
**Inline-vs-heap threshold**, `src/include/duckdb/common/types/string_type.hpp:28-38`:
```cpp
static constexpr idx_t PREFIX_BYTES = 4 * sizeof(char);
static constexpr idx_t INLINE_BYTES = 12 * sizeof(char);
...
static constexpr idx_t INLINE_LENGTH = INLINE_BYTES;   // 12, unless DUCKDB_DEBUG_NO_INLINE
```
Strings of length ≤ **12 bytes** are stored inline inside the 16-byte `string_t` struct itself (no heap
allocation, no ownership concerns — `string_t::IsInlined()`, `string_type.hpp:75-77`); longer strings
are copied into the vector's `VectorStringBuffer` heap (owned by the vector's `auxiliary` buffer) and
the `string_t` stores a `char *ptr` into that heap plus a 4-byte prefix cache for fast comparisons
(`string_type.hpp:229-240`). The heap-owned copy's lifetime is tied to the `Vector`/`DataChunk` it
belongs to — safe to reference for the lifetime of that chunk, invalid after the chunk is reset/
destroyed. You never need to manage this memory yourself; just don't hold onto a `string_t` past the
`DataChunk`'s lifetime.

### 3.4 LIST columns — the canonical pattern

This is the crux of the VTK use case (`point_ids BIGINT[]`, arbitrary-length cell connectivity).

**Type construction:**
```cpp
DUCKDB_API static LogicalType LIST(const LogicalType &child);   // src/include/duckdb/common/types.hpp:434
```
e.g. `LogicalType::LIST(LogicalType::BIGINT)`.

**API surface**, `vector.hpp:503-546`:
```cpp
struct ListVector {
	static inline const list_entry_t *GetData(const Vector &v);
	static inline list_entry_t *GetData(Vector &v);
	DUCKDB_API static const Vector &GetEntry(const Vector &vector);   // child vector, const
	DUCKDB_API static Vector &GetEntry(Vector &vector);               // child vector, mutable
	DUCKDB_API static idx_t GetListSize(const Vector &vector);        // current logical size of child
	DUCKDB_API static void SetListSize(Vector &vec, idx_t size);      // commit the size
	DUCKDB_API static idx_t GetListCapacity(const Vector &vector);    // allocated capacity of child
	DUCKDB_API static void Reserve(Vector &vec, idx_t required_capacity); // grow capacity if needed
	DUCKDB_API static void Append(Vector &target, const Vector &source, idx_t source_size, idx_t source_offset = 0);
	DUCKDB_API static void PushBack(Vector &target, const Value &insert);
	...
};
```
`list_entry_t` itself (`src/include/duckdb/common/types.hpp:41-54`):
```cpp
struct list_entry_t {
	list_entry_t() = default;
	list_entry_t(uint64_t offset, uint64_t length) : offset(offset), length(length) {}
	uint64_t offset;
	uint64_t length;
};
```
So the **parent** LIST vector is a normal flat vector of `list_entry_t {offset, length}` — one entry
per row, `offset`/`length` indexing into the **child** vector, which holds every element of every
row's list concatenated together.

**Why `Reserve` must happen before you take a pointer to the child data — proven from the real
implementation**, `src/common/types/vector_buffer.cpp:72-84`:
```cpp
void VectorListBuffer::Reserve(idx_t to_reserve) {
	if (to_reserve > capacity) {
		if (to_reserve > DConstants::MAX_VECTOR_SIZE) {
			throw OutOfRangeException(...);
		}
		idx_t new_capacity = NextPowerOfTwo(to_reserve);
		child->Resize(capacity, new_capacity);   // <-- REALLOCATES the child Vector's backing buffer
		capacity = new_capacity;
	}
}
```
`ListVector::Reserve` (`src/common/types/vector.cpp:2524-2532`) forwards directly into this
`VectorListBuffer::Reserve`, which calls `child->Resize(...)` — and `Vector::Resize` allocates a new
data buffer and copies/moves old contents into it (see `vector.hpp:256`, `DUCKDB_API void
Resize(idx_t cur_size, idx_t new_size);`). **Any raw `T*` you obtained from `FlatVector::GetData<T>`
on the child vector before calling `Reserve` is dangling after `Reserve` reallocates.** The correct,
safe order of operations is:

1. `ListVector::Reserve(list_vec, total_child_elements_this_chunk);`  — reserve **first**
2. `auto &child = ListVector::GetEntry(list_vec);`                    — get child **after** reserving
3. `auto *child_data = FlatVector::GetData<ChildT>(child);`           — take pointer **after** reserving
4. write `child_data[0..total_child_elements_this_chunk)`
5. write `list_entry_t{offset, length}` per row into `FlatVector::GetData<list_entry_t>(list_vec)`
6. `ListVector::SetListSize(list_vec, total_child_elements_this_chunk);` — commit the size **last**

Full copy-pasteable pattern for a `BIGINT[]` column (e.g. `point_ids`) written across a chunk of
`count` rows, where `row_lengths[i]` is the connectivity-list length for row `i`:
```cpp
idx_t total = 0;
for (idx_t i = 0; i < count; i++) total += row_lengths[i];

ListVector::Reserve(output.data[col], total);                  // 1. reserve first
auto &child = ListVector::GetEntry(output.data[col]);           // 2. child AFTER reserve
auto *child_data = FlatVector::GetData<int64_t>(child);          // 3. pointer AFTER reserve
auto *list_data = FlatVector::GetData<list_entry_t>(output.data[col]);

idx_t offset = 0;
for (idx_t i = 0; i < count; i++) {
    idx_t len = row_lengths[i];
    list_data[i] = list_entry_t(offset, len);
    for (idx_t j = 0; j < len; j++) {
        child_data[offset + j] = point_ids_for_row(i, j);
    }
    offset += len;
}
ListVector::SetListSize(output.data[col], offset);              // 4. commit size last
```
A NULL list is just `FlatVector::SetNull(output.data[col], i, true)` on the **parent** vector (leave
`list_entry_t{0,0}` or whatever — its content is ignored once the row is marked null) — the child
vector is completely unaffected by which rows are null.

**Real in-tree confirmation that this exact sequence is the established pattern**: grepping the
fetched `src/common/types/vector.cpp` for all four calls together shows the identical
Reserve→SetListSize ordering used internally, e.g. at `vector.cpp:1546-1547` (`ListVector::Reserve(*this,
list_size); ListVector::SetListSize(*this, list_size);`) and in `VectorListBuffer::Append`
(`vector_buffer.cpp:86-90`, which itself calls `Reserve(size + to_append_size - source_offset)` before
copying into the (now-valid) child). I was **not** able to find a table-function-specific (as opposed
to internal vector-operations/cast-code) call site of `ListVector::Reserve` in the small set of files
fetched for this task — the closest table-function-adjacent examples fetched
(`duckdb_tables.cpp`, `duckdb_columns.cpp`, `glob.cpp`, `range.cpp`) all only emit scalar columns, none
build a LIST. The Reserve/SetListSize contract above is derived directly from the real
`ListVector`/`VectorListBuffer` implementation (`vector.cpp:2524-2568`, `vector_buffer.cpp:72-84`),
not from an in-tree table-function example — flag this appropriately if a downstream reviewer wants an
extra example (`src/core_functions/scalar/list/` scalar functions such as `list_value`/`list_slice`
are the next place to look if one is needed; not fetched here due to time budget).

### 3.5 Fixed-size arrays (`LogicalType::ARRAY`)

```cpp
DUCKDB_API static LogicalType ARRAY(const LogicalType &child, optional_idx index);  // types.hpp:440
DUCKDB_API static constexpr idx_t MAX_ARRAY_SIZE = 100000; // 100k for now          // types.hpp:528 (ArrayType helper)
```
```cpp
struct ArrayVector {
	DUCKDB_API static const Vector &GetEntry(const Vector &vector);
	DUCKDB_API static Vector &GetEntry(Vector &vector);
	DUCKDB_API static idx_t GetTotalSize(const Vector &vector);
};
```
`vector.hpp:638-649`. Real implementation, `src/common/types/vector.cpp:2870-2903`:
```cpp
Vector &ArrayVector::GetEntry(Vector &vector) {
	return GetEntryInternal<Vector>(vector);   // asserts vector.auxiliary->GetBufferType() == ARRAY_BUFFER
}
idx_t ArrayVector::GetTotalSize(const Vector &vector) {
	return vector.auxiliary->Cast<VectorArrayBuffer>().GetChildSize();
}
```
**No `ArrayVector::Reserve`/`SetListSize` exist** — that's the key structural difference from LIST.
`VectorArrayBuffer` (`src/include/duckdb/common/types/vector_buffer.hpp:322-340`) is constructed with
a fixed `array_size` and a fixed row `capacity` up front (from the `LogicalType::ARRAY(child, n)` you
declared at bind time), so the child vector's size is always exactly `array_size * row_capacity` — you
write element `j` of row `i` at `child_data[i * array_size + j]` directly, with no reserve/commit
dance. Per-element NULLs inside an array row *are* supported (the child vector has a normal
`ValidityMask` just like any other vector), but you cannot vary the number of elements per row, and
the array size `n` must be fixed and known at `bind` time (it's baked into the `LogicalType`).

**Recommendation for `velocity DOUBLE[3]`**: prefer `LogicalType::ARRAY(LogicalType::DOUBLE, 3)` over
`LogicalType::LIST(LogicalType::DOUBLE)` when the length is always exactly 3. Trade-offs:
- **Ergonomics**: `ARRAY` gives you `arr[1]`-style fixed indexing and casts more naturally to/from
  `STRUCT(x DOUBLE, y DOUBLE, z DOUBLE)`; no `Reserve`/`SetListSize` bookkeeping — simpler, less
  error-prone code.
- **`UNNEST`/casting**: both `LIST` and `ARRAY` support `UNNEST`; `ARRAY` additionally supports casts
  to `LIST` implicitly in DuckDB's type system, but not vice versa without an explicit length check.
- **Performance**: `ARRAY`'s storage is denser (no per-row offset/length overhead, no possibility of
  overlapping/gapped child ranges) and avoids the reserve-and-copy growth pattern entirely.
- **Caveat**: `ARRAY` support in DuckDB is comparatively newer than `LIST`/`STRUCT` (`PhysicalType::ARRAY
  = 29` was added post-`LIST`/`STRUCT` in the physical-type enum ordering, `types.hpp:156`) — verify by
  compiling that any specific extension helper you need (e.g. Arrow/Parquet round-tripping, if that
  matters to you) has full `ARRAY` support in 1.5.4; for `point_ids` (variable-length connectivity),
  use `LIST` — `ARRAY` is only appropriate for genuinely fixed-width fields like a 3-component vector.

### 3.6 STRUCT columns (brief, as an alternative layout)

```cpp
DUCKDB_API static LogicalType STRUCT(child_list_t<LogicalType> children);  // types.hpp:435
```
e.g. `LogicalType::STRUCT({{"x", LogicalType::DOUBLE}, {"y", LogicalType::DOUBLE}, {"z", LogicalType::DOUBLE}})`.
```cpp
struct StructVector {
	DUCKDB_API static const vector<unique_ptr<Vector>> &GetEntries(const Vector &vector);
	DUCKDB_API static vector<unique_ptr<Vector>> &GetEntries(Vector &vector);
};
```
`vector.hpp:633-636`. Each entry is a normal, independently-flat `Vector` you write into with the
normal `FlatVector::GetData<T>` pattern — no reserve/offset bookkeeping (row count matches the parent
exactly, 1:1). Good alternative to `ARRAY` for a fixed 3-component field if you want named-field
(`.x`/`.y`/`.z`) access from SQL rather than positional (`arr[1]`) access.

### 3.7 Debug-build sanity checks

```cpp
DUCKDB_API void Verify(idx_t count);          // Vector::Verify, vector.hpp:230-232 ("DEBUG FUNCTION ONLY")
DUCKDB_API void Verify();                     // DataChunk::Verify, data_chunk.hpp:163-165 (same)
```
Both are **compiled to no-ops in release builds** — confirmed from the real implementation,
`src/common/types/vector.cpp:1667-1668` (`void Vector::Verify(Vector &vector_p, const SelectionVector
&sel_p, idx_t count) { #ifdef DEBUG ... `) and `src/common/types/data_chunk.cpp:360-362`
(`void DataChunk::Verify() { #ifdef DEBUG D_ASSERT(size() <= capacity); ... `). They only run real
checks (list offset/length bounds, string UTF-8 validity, struct/union invariants, etc.) in a `DEBUG`
CMake build. Relevant CMake options, all in the root `CMakeLists.txt`:
```
if(CRASH_ON_ASSERT)        set(... -DDUCKDB_CRASH_ON_ASSERT)      endif()   # CMakeLists.txt:537-539
if(DISABLE_STR_INLINE)     set(... -DDUCKDB_DEBUG_NO_INLINE)      endif()   # CMakeLists.txt:541-543
if(DISABLE_MEMORY_SAFETY)  set(... -DDUCKDB_DEBUG_NO_SAFETY)      endif()   # CMakeLists.txt:545-547
if(DISABLE_ASSERTIONS)     set(... -DDISABLE_ASSERTIONS)          endif()   # CMakeLists.txt:549-551
if(FORCE_ASSERT)           set(... -DDUCKDB_FORCE_ASSERT)         endif()   # CMakeLists.txt:597-599
```
Practical recommendation for developing the VTK extension: build with
`-DCMAKE_BUILD_TYPE=Debug` (enables `#ifdef DEBUG` blocks, i.e. `Vector::Verify`/`DataChunk::Verify`
run their real checks) and do **not** set `DISABLE_MEMORY_SAFETY` — leaving memory safety on means a
type mismatch (e.g. calling `FlatVector::GetData<int32_t>` on a `BIGINT` vector) throws a catchable
`InternalException` instead of hitting a raw `assert()`/UB (`vector.hpp:333-344` shows the
`#ifdef DUCKDB_DEBUG_NO_SAFETY ... D_ASSERT ... #else ... throw InternalException ... #endif` split
directly). `D_ASSERT` itself (`src/include/duckdb/common/assert.hpp:12-36`) is a plain `assert()` (a
no-op under `NDEBUG`/release) unless `DEBUG` or `DUCKDB_FORCE_ASSERT` is defined.

---

## 4. LogicalType → PhysicalType → C++ type reference

All type constants/factories from `src/include/duckdb/common/types.hpp`; the `PhysicalType` enum
(`types.hpp:63-180`) and `LogicalTypeId` enum (`types.hpp:185-248`); the physical→C++ mapping is
**directly** the `GetTypeId<T>()` template, `src/include/duckdb/common/type_util.hpp:23-83` (this is
literally the reverse-lookup table the DuckDB codebase itself uses — authoritative).

| `LogicalType` (static factory) | `LogicalTypeId` | `PhysicalType` | C++ type for `FlatVector::GetData<T>` |
|---|---|---|---|
| `LogicalType::BOOLEAN` | `BOOLEAN` (10) | `BOOL` (1) | `bool` |
| `LogicalType::TINYINT` | `TINYINT` (11) | `INT8` (3) | `int8_t` |
| `LogicalType::UTINYINT` | `UTINYINT` (28) | `UINT8` (2) | `uint8_t` |
| `LogicalType::SMALLINT` | `SMALLINT` (12) | `INT16` (5) | `int16_t` |
| `LogicalType::USMALLINT` | `USMALLINT` (29) | `UINT16` (4) | `uint16_t` |
| `LogicalType::INTEGER` | `INTEGER` (13) | `INT32` (7) | `int32_t` |
| `LogicalType::UINTEGER` | `UINTEGER` (30) | `UINT32` (6) | `uint32_t` |
| `LogicalType::BIGINT` | `BIGINT` (14) | `INT64` (9) | `int64_t` |
| `LogicalType::UBIGINT` | `UBIGINT` (31) | `UINT64` (8) | `uint64_t` |
| `LogicalType::HUGEINT` | `HUGEINT` (50) | `INT128` (204) | `hugeint_t` |
| `LogicalType::UHUGEINT` | `UHUGEINT` (49) | `UINT128` (203) | `uhugeint_t` |
| `LogicalType::FLOAT` | `FLOAT` (22) | `FLOAT` (11) | `float` |
| `LogicalType::DOUBLE` | `DOUBLE` (23) | `DOUBLE` (12) | `double` |
| `LogicalType::VARCHAR` | `VARCHAR` (25) | `VARCHAR` (200) | `string_t` (via `StringVector::AddString`) |
| `LogicalType::BLOB` | `BLOB` (26) | `VARCHAR` (200) | `string_t` (via `StringVector::AddStringOrBlob`) |
| `LogicalType::INTERVAL` | `INTERVAL` (27) | `INTERVAL` (21) | `interval_t` |
| `LogicalType::LIST(child)` | `LIST` (101) | `LIST` (23) | parent: `list_entry_t {uint64_t offset, length}`; child: per `ListVector::GetEntry` |
| `LogicalType::ARRAY(child, n)` | `ARRAY` (108) | `ARRAY` (29) | child vector only, size `n * row_capacity`, no parent entry type |
| `LogicalType::STRUCT({...})` | `STRUCT` (100) | `STRUCT` (24) | one independent `Vector` per field via `StructVector::GetEntries` |
| `LogicalType::MAP(k,v)` | `MAP` (102) | `LIST` (23) | stored as `LIST(STRUCT(key,value))` — see `MapVector` |
| `idx_t` (row ids, offsets) | — | `UINT64` (8) | `idx_t`/`uint64_t` (`GetTypeId<idx_t>()` maps to `UINT64`, `type_util.hpp:43-44`) |

Sources: `PhysicalType` enum values `types.hpp:63-180` (e.g. `BOOL = 1` at `:68`, `INT64 = 9` at `:92`,
`FLOAT = 11`/`DOUBLE = 12` at `:98/:101`, `INTERVAL = 21` at `:131`, `LIST = 23` at `:138`,
`STRUCT = 24` at `:141`, `ARRAY = 29` at `:156`, `VARCHAR = 200` at `:172`, `UINT128/INT128` at
`:173-174`); `LogicalTypeId` enum values `types.hpp:185-248`; static factory declarations
`types.hpp:432-454` (`DECIMAL`, `VARCHAR_COLLATION`, `LIST`, `STRUCT`, `AGGREGATE_STATE`, `MAP`
×2, `UNION`, `ARRAY`, `ENUM` ×2, `GEOMETRY` ×3, ...); simple scalar constants
`types.hpp:392-429` (`BOOLEAN`...`HUGEINT`...`ROW_TYPE = BIGINT`); C++-type mapping directly from
`GetTypeId<T>()`, `type_util.hpp:23-83`.

Not tabulated above but present if needed: `DATE`→`date_t` (physical `INT32`), `TIME`→`dtime_t`
(physical `INT64`), `TIMESTAMP`/`TIMESTAMP_TZ`/`TIMESTAMP_NS`/etc.→`timestamp_t`-family structs
(all physical `INT64`), `DECIMAL(w,s)`→physical type depends on width (`INT16`/`INT32`/`INT64`/
`INT128` chosen by DuckDB based on `w`), `UUID`→`hugeint_t`-shaped physical storage. These weren't
directly needed for the VTK column set in the brief (`BIGINT, DOUBLE, VARCHAR, LIST`) so were not
individually re-verified against `type_util.hpp` line-by-line beyond what's shown above (all present
in the fetched file, `type_util.hpp:47-63`).

---

## 5. Errors, cancellation, progress

### 5.1 Exceptions

Declared in `src/include/duckdb/common/exception.hpp` (base class hierarchy) and
`src/include/duckdb/common/exception/binder_exception.hpp` (split out separately):
```cpp
class Exception : public std::runtime_error { ... };                    // exception.hpp:94
class IOException : public Exception { ... };                           // exception.hpp:232
class NotImplementedException : public Exception { ... };                // exception.hpp:261
class InterruptException : public Exception { DUCKDB_API InterruptException(); }; // exception.hpp:302-306
class InternalException : public Exception { ... };                      // exception.hpp:325
class InvalidInputException : public Exception { ... };                  // exception.hpp:341
class BinderException : public Exception { ... };  // exception/binder_exception.hpp:17
```
Usage guidance (consistent with every example function fetched):
- **`bind`**: throw `BinderException` for "this SQL usage is invalid" (bad argument combination,
  unsupported named parameter, etc.) — e.g. `range.cpp:114`, `throw
  BinderException("interval cannot be 0!");`. Throw `IOException` if bind-time file access fails (file
  doesn't exist / can't be opened / malformed header) — appropriate for a VTK file whose header can't
  be parsed. `InvalidInputException` is a good fit for "the argument value itself is nonsensical"
  (e.g. a negative row count) as distinct from a purely syntactic binder error.
- **scan (`function`)**: `IOException` for I/O failures mid-scan (disk error, truncated file);
  `NotImplementedException` for VTK features you haven't implemented yet (e.g. an unsupported cell
  type or a VTK format variant); `InternalException` only for "this should be logically impossible"
  invariant violations (a bug in your own code, not user-facing).
All of these are ordinary C++ exceptions (`Exception : public std::runtime_error`) — DuckDB's query
executor catches them at the pipeline-execution boundary and surfaces `exception.what()`-style text as
the SQL client's error message/`duckdb::QueryResult` error state; you don't need to do anything special
to make a thrown exception "surface" — just `throw`, same as any other C++ exception.

### 5.2 Cancellation

Confirmed real check inside the pipeline execution loop, `src/parallel/pipeline_executor.cpp:193-194`:
```cpp
PipelineExecuteResult PipelineExecutor::Execute(idx_t max_chunks) {
	...
	do {
		if (context.client.interrupted) {
			throw InterruptException();
		}
		...
```
This check happens **between calls to your `function`**, at the top of each iteration of the
executor's main loop — i.e. DuckDB itself checks `ClientContext::interrupted` for you between chunks;
you do not need to poll anything inside your own scan loop *as long as you return control after each
`STANDARD_VECTOR_SIZE`-sized chunk* (which you should be doing anyway, per the scan-loop contract in
§2). If your `function` implementation has an inner loop that could run for a long time *within a
single call* (e.g. decoding one giant chunk in one call, or a synchronous "the whole file must be
parsed on first call" implementation), you should periodically check `context.interrupted` yourself
(a public member/accessor on `ClientContext`) and throw `InterruptException()` — but for a
straightforward "emit `STANDARD_VECTOR_SIZE` rows per call" scan this happens for free between calls.

### 5.3 `table_scan_progress`

```cpp
typedef double (*table_function_progress_t)(ClientContext &context, const FunctionData *bind_data,
                                            const GlobalTableFunctionState *global_state);
```
`table_function.hpp:319-320`. Return a percentage `0.0`–`100.0`. Trivial rows-done/rows-total
implementation, modeled directly on `table_scan.cpp:242-249` / `:784-788`:
```cpp
double DemoScanProgress(ClientContext &context, const FunctionData *bind_data_p,
                         const GlobalTableFunctionState *gstate_p) {
	auto &bind_data = bind_data_p->Cast<DemoScanBindData>();
	auto &gstate = gstate_p->Cast<DemoScanGlobalState>();
	if (bind_data.num_rows == 0) return 100.0;
	return 100.0 * static_cast<double>(gstate.row_idx) / static_cast<double>(bind_data.num_rows);
}
```
Assign it: `my_function.table_scan_progress = DemoScanProgress;`.

### 5.4 `cardinality`

```cpp
typedef unique_ptr<NodeStatistics> (*table_function_cardinality_t)(ClientContext &context,
                                                                    const FunctionData *bind_data);
```
`table_function.hpp:322-323`. `NodeStatistics` (`src/include/duckdb/storage/statistics/
node_statistics.hpp:14-32`):
```cpp
class NodeStatistics {
public:
	NodeStatistics() ...
	explicit NodeStatistics(idx_t estimated_cardinality) ...
	NodeStatistics(idx_t estimated_cardinality, idx_t max_cardinality) ...
	bool has_estimated_cardinality;
	idx_t estimated_cardinality;
	bool has_max_cardinality;
	idx_t max_cardinality;
};
```
Real usage, `range.cpp:185-191`:
```cpp
unique_ptr<NodeStatistics> RangeCardinality(ClientContext &context, const FunctionData *bind_data_p) {
	if (!bind_data_p) return nullptr;
	auto &bind_data = bind_data_p->Cast<RangeFunctionBindData>();
	return make_uniq<NodeStatistics>(bind_data.cardinality, bind_data.cardinality);
}
```
If you know the row count exactly at `bind` time (e.g. you already parsed the VTK header and know
`num_points`/`num_cells`), return `make_uniq<NodeStatistics>(count, count)` — both estimated and max
set to the same exact value, exactly as `range.cpp` does.

---

## 6. Replacement scans

Signature, `src/include/duckdb/function/replacement_scan.hpp:37-46`:
```cpp
struct ReplacementScanInput {
	explicit ReplacementScanInput(const string &catalog_name, const string &schema_name, const string &table_name);
	const string &catalog_name;
	const string &schema_name;
	const string &table_name;
};

typedef unique_ptr<TableRef> (*replacement_scan_t)(ClientContext &context, ReplacementScanInput &input,
                                                   optional_ptr<ReplacementScanData> data);

struct ReplacementScan {
	explicit ReplacementScan(replacement_scan_t function, unique_ptr<ReplacementScanData> data_p = nullptr);
	static bool CanReplace(const string &table_name, const vector<string> &extensions);
	static string GetFullPath(ReplacementScanInput &input);
	replacement_scan_t function;
	unique_ptr<ReplacementScanData> data;
};
```
Register via `DBConfig::config.replacement_scans` (`vector<ReplacementScan>`,
`src/include/duckdb/main/config.hpp:190`, `#include "duckdb/function/replacement_scan.hpp"` at
`config.hpp:31`).

**Real in-tree example**, `src/function/table/read_csv.cpp:163-199`:
```cpp
unique_ptr<TableRef> ReadCSVReplacement(ClientContext &context, ReplacementScanInput &input,
                                        optional_ptr<ReplacementScanData> data) {
	auto table_name = ReplacementScan::GetFullPath(input);
	auto lower_name = StringUtil::Lower(table_name);
	// ... strip compression suffixes ...
	if (!StringUtil::EndsWith(lower_name, ".csv") && !StringUtil::Contains(lower_name, ".csv?") &&
	    !StringUtil::EndsWith(lower_name, ".tsv") && !StringUtil::Contains(lower_name, ".tsv?")) {
		return nullptr;                                    // not our extension: decline, try the next scan
	}
	auto table_function = make_uniq<TableFunctionRef>();
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(make_uniq<ConstantExpression>(Value(table_name)));
	table_function->function = make_uniq<FunctionExpression>("read_csv_auto", std::move(children));
	if (!FileSystem::HasGlob(table_name)) {
		auto &fs = FileSystem::GetFileSystem(context);
		table_function->alias = fs.ExtractBaseName(table_name);
	}
	return std::move(table_function);
}
...
void BuiltinFunctions::RegisterReadFunctions() {
	...
	auto &config = DBConfig::GetConfig(*transaction.db);
	config.replacement_scans.emplace_back(ReadCSVReplacement);
}
```
For a VTK extension, register in your `DUCKDB_CPP_EXTENSION_ENTRY` body:
```cpp
unique_ptr<TableRef> VtkReplacementScan(ClientContext &context, ReplacementScanInput &input,
                                        optional_ptr<ReplacementScanData> data) {
	auto table_name = ReplacementScan::GetFullPath(input);
	auto lower_name = StringUtil::Lower(table_name);
	if (!StringUtil::EndsWith(lower_name, ".vtu") && !StringUtil::EndsWith(lower_name, ".vtp") &&
	    !StringUtil::EndsWith(lower_name, ".vtk")) {
		return nullptr;   // not ours; let DuckDB try other replacement scans / fail normally
	}
	auto table_function = make_uniq<TableFunctionRef>();
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(make_uniq<ConstantExpression>(Value(table_name)));
	table_function->function = make_uniq<FunctionExpression>("demo_scan", std::move(children));
	return std::move(table_function);
}

DUCKDB_CPP_EXTENSION_ENTRY(duck_vtk, loader) {
	RegisterDemoScan(loader);
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());  // extension_loader.hpp:37
	config.replacement_scans.emplace_back(VtkReplacementScan);
}
```
This makes `SELECT * FROM 'mesh.vtu'` automatically rewrite to `SELECT * FROM demo_scan('mesh.vtu')`.

---

## 7. Complete worked example: `demo_scan`

Self-contained, single-threaded, honors `column_ids` projection pushdown, sets NULLs on a `VARCHAR`
column, emits a `DOUBLE[]` LIST column, and chunks output at `STANDARD_VECTOR_SIZE`. Schema:
`demo_scan(path VARCHAR) -> TABLE(id BIGINT, name VARCHAR, coords DOUBLE[], flag BOOLEAN)`.

```cpp
// demo_scan.cpp — table function `demo_scan(path VARCHAR)`
// Columns: id BIGINT, name VARCHAR, coords DOUBLE[], flag BOOLEAN
//
// Verified against duckdb v1.5.4 (commit 08e34c447bae34eaee3723cac61f2878b6bdf787).
// Compile as part of a standard DuckDB C++ extension (extension-template layout, C++17,
// new ExtensionLoader entrypoint).

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Bind data — the FULL (unprojected) logical schema, in fixed column-index order:
//   0 = id, 1 = name, 2 = coords, 3 = flag
//===--------------------------------------------------------------------===//
struct DemoScanBindData : public TableFunctionData {
	string path;
	idx_t num_rows = 10000;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<DemoScanBindData>();
		result->path = path;
		result->num_rows = num_rows;
		result->column_ids = column_ids; // inherited from TableFunctionData
		return std::move(result);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<DemoScanBindData>();
		return path == other.path && num_rows == other.num_rows;
	}
};

static unique_ptr<FunctionData> DemoScanBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<DemoScanBindData>();
	result->path = input.inputs[0].GetValue<string>();

	return_types.push_back(LogicalType::BIGINT);
	names.push_back("id");
	return_types.push_back(LogicalType::VARCHAR);
	names.push_back("name");
	return_types.push_back(LogicalType::LIST(LogicalType::DOUBLE));
	names.push_back("coords");
	return_types.push_back(LogicalType::BOOLEAN);
	names.push_back("flag");

	return std::move(result);
}

//===--------------------------------------------------------------------===//
// Global state — single-threaded scan cursor + which logical columns are projected.
// (Drop-in-later for parallelism: add a mutex around row_idx, add init_local for
//  per-thread scratch, override MaxThreads() to return > 1.)
//===--------------------------------------------------------------------===//
struct DemoScanGlobalState : public GlobalTableFunctionState {
	idx_t row_idx = 0;
	vector<column_t> column_ids; // which of the 4 logical columns to emit, and in what order

	idx_t MaxThreads() const override {
		return 1; // single-threaded for phase 1
	}
};

static unique_ptr<GlobalTableFunctionState> DemoScanInitGlobal(ClientContext &context,
                                                               TableFunctionInitInput &input) {
	auto result = make_uniq<DemoScanGlobalState>();
	result->column_ids = input.column_ids; // honors projection pushdown (see §2.3)
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// Scan function
//===--------------------------------------------------------------------===//
static void DemoScanFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<DemoScanBindData>();
	auto &state = data_p.global_state->Cast<DemoScanGlobalState>();

	if (state.row_idx >= bind_data.num_rows) {
		return; // output stays at cardinality 0 (already Reset() for us) -> signals end-of-stream
	}

	idx_t count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, bind_data.num_rows - state.row_idx);

	for (idx_t out_col = 0; out_col < state.column_ids.size(); out_col++) {
		column_t logical_col = state.column_ids[out_col];
		auto &vec = output.data[out_col];

		switch (logical_col) {
		case 0: { // id BIGINT
			auto data = FlatVector::GetData<int64_t>(vec);
			for (idx_t i = 0; i < count; i++) {
				data[i] = static_cast<int64_t>(state.row_idx + i);
			}
			break;
		}
		case 1: { // name VARCHAR — NULL on every 5th row
			auto data = FlatVector::GetData<string_t>(vec);
			for (idx_t i = 0; i < count; i++) {
				idx_t row = state.row_idx + i;
				if (row % 5 == 0) {
					FlatVector::SetNull(vec, i, true);
				} else {
					string s = "row_" + std::to_string(row);
					data[i] = StringVector::AddString(vec, s);
				}
			}
			break;
		}
		case 2: { // coords DOUBLE[3] worth of data per row (LIST column)
			const idx_t per_row = 3;
			ListVector::Reserve(vec, count * per_row);              // 1. reserve BEFORE child pointer
			auto &child = ListVector::GetEntry(vec);                 // 2. child AFTER reserve
			auto child_data = FlatVector::GetData<double>(child);    // 3. pointer AFTER reserve
			auto list_data = FlatVector::GetData<list_entry_t>(vec);
			idx_t child_offset = 0;
			for (idx_t i = 0; i < count; i++) {
				idx_t row = state.row_idx + i;
				list_data[i] = list_entry_t(child_offset, per_row);
				for (idx_t j = 0; j < per_row; j++) {
					child_data[child_offset++] = static_cast<double>(row) + 0.1 * static_cast<double>(j);
				}
			}
			ListVector::SetListSize(vec, child_offset);              // 4. commit size AFTER writing
			break;
		}
		case 3: { // flag BOOLEAN
			auto data = FlatVector::GetData<bool>(vec);
			for (idx_t i = 0; i < count; i++) {
				data[i] = ((state.row_idx + i) % 2 == 0);
			}
			break;
		}
		default:
			throw InternalException("demo_scan: unexpected column id %d", logical_col);
		}
	}

	state.row_idx += count;
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// Cardinality + progress (optional but cheap to provide)
//===--------------------------------------------------------------------===//
static unique_ptr<NodeStatistics> DemoScanCardinality(ClientContext &context, const FunctionData *bind_data_p) {
	if (!bind_data_p) {
		return nullptr;
	}
	auto &bind_data = bind_data_p->Cast<DemoScanBindData>();
	return make_uniq<NodeStatistics>(bind_data.num_rows, bind_data.num_rows);
}

static double DemoScanProgress(ClientContext &context, const FunctionData *bind_data_p,
                               const GlobalTableFunctionState *gstate_p) {
	auto &bind_data = bind_data_p->Cast<DemoScanBindData>();
	auto &gstate = gstate_p->Cast<DemoScanGlobalState>();
	if (bind_data.num_rows == 0) {
		return 100.0;
	}
	return 100.0 * static_cast<double>(gstate.row_idx) / static_cast<double>(bind_data.num_rows);
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//
static void RegisterDemoScan(ExtensionLoader &loader) {
	TableFunction demo_scan("demo_scan", {LogicalType::VARCHAR}, DemoScanFunction, DemoScanBind,
	                        DemoScanInitGlobal);
	demo_scan.cardinality = DemoScanCardinality;
	demo_scan.table_scan_progress = DemoScanProgress;
	demo_scan.projection_pushdown = true; // REQUIRED for column_ids in init_global to reflect the
	                                       // query's actual projection; without it, DuckDB still adds
	                                       // its own Projection on top, but init_global.column_ids would
	                                       // simply enumerate every declared column in order (0,1,2,3)
	loader.RegisterFunction(demo_scan);
}

} // namespace duckdb

DUCKDB_CPP_EXTENSION_ENTRY(duck_vtk, loader) {
	duckdb::RegisterDemoScan(loader);
}
```

**What a downstream agent needs to change to compile this in-tree**: nothing structural — drop this
file into `src/` of an extension-template-based project (`extension-template@main` pins its `duckdb`
submodule to this exact commit `08e34c447b`, so headers match), add it to the extension's
`CMakeLists.txt` sources list, and call `RegisterDemoScan(loader)` (or inline its body) from your
extension's own `DUCKDB_CPP_EXTENSION_ENTRY` block if you already have one (only one entrypoint macro
per extension binary).

---

## 8. Uncertainties / verify by compiling

- **No in-tree table-function example of `ListVector::Reserve` was found** in the small set of files
  fetched for this task (`range.cpp`, `glob.cpp`, `duckdb_tables.cpp`, `duckdb_columns.cpp`,
  `table_scan.cpp`). The Reserve→GetEntry→write→SetListSize ordering in §3.4/§7 is derived directly
  from the real `ListVector`/`VectorListBuffer` implementations (`src/common/types/vector.cpp:2524-
  2568`, `src/common/types/vector_buffer.cpp:72-84`), which is authoritative for *why* the ordering
  matters, but a second, independent example from a real scan (e.g. `src/core_functions/scalar/list/`
  or an Arrow/Parquet-adjacent file, if vendored) was not fetched due to time budget. **Recommend
  compiling `demo_scan` against a real DuckDB build and running `SELECT coords FROM
  demo_scan('x') LIMIT 5;` plus a `PRAGMA enable_verification` / debug build run to catch any
  ordering mistake via `Vector::Verify`.**

- **`TableFunction`'s default value for `projection_pushdown`/`filter_pushdown`/etc.** — the header
  declares these as plain `bool` members with no in-class initializer visible in the fetched
  `table_function.hpp` (unlike, e.g., `order_preservation_type` which does have a default). The
  constructor implementations (`src/function/table_function.cpp`) were **not fetched**, so it is
  inferred (from every example function leaving them unset and behaving as "pushdown disabled") that
  the constructor zero-initializes them to `false`, but this was not directly confirmed by reading
  `table_function.cpp`. **Verify by compiling**: print/assert `TableFunction{}.projection_pushdown`
  is `false` before relying on it.

- **`Value::GetValue<T>()` / `BooleanValue::Get` / `StringValue::Get` exact signatures** — referenced
  in §1.5 by pattern (confirmed indirectly via `BindInfo::GetOption<T>`,
  `table_function.hpp:263-269`, which calls `.GetValue<T>()`) but `src/include/duckdb/common/types/
  value.hpp` itself was **not fetched** in this pass. The general shape (`template <class T> T
  Value::GetValue() const`, plus `struct BooleanValue { static bool Get(const Value &value); }`-style
  helpers) is extremely stable across DuckDB versions and used pervasively, but line-precise citation
  is missing here — fetch `src/include/duckdb/common/types/value.hpp` if exact signatures are needed
  for named-parameter extraction code.

- **`ExtensionLoader` constructors are not `DUCKDB_API`** (`explicit ExtensionLoader(ExtensionActiveLoad
  &load_info); ExtensionLoader(DatabaseInstance &db, const string &extension_name);` —
  `extension_loader.hpp:33-34`) — meaning extension authors don't construct one directly; it's handed
  to you as a parameter by the `DUCKDB_CPP_EXTENSION_ENTRY` macro. This is consistent with the worked
  example (§7) but wasn't independently tested by compiling a full extension binary in this pass.

- **ARRAY support maturity claims in §3.5** (casts to/from LIST, `unnest` behavior, arbitrary-`n`
  support) are based on the `ArrayVector`/`VectorArrayBuffer` API surface actually present in
  `vector.hpp`/`vector_buffer.hpp`/`vector.cpp`, plus `MAX_ARRAY_SIZE = 100000` at `types.hpp:528`, not
  on having compiled and run cast/unnest queries against a live `v1.5.4` binary. **Verify by
  compiling**: `SELECT [1.0,2.0,3.0]::DOUBLE[3];`, `SELECT unnest(col) FROM (VALUES ([1.0,2.0,3.0]::DOUBLE[3])) t(col);`,
  and a cast from `ARRAY` to `LIST` against an actual `v1.5.4` DuckDB CLI/build before depending on
  these behaviors in the extension.

- **Multi-threaded scan section (§2.5)** is a shape/pattern summary distilled from `table_scan.cpp`
  (DuckDB's own storage scan, not a "simple" third-party-style table function) — it is a correct and
  representative pattern, but not a minimal from-scratch multi-threaded example built for this task.
  When phase 2 (parallel VTK-file scanning) is implemented, re-derive the exact locking granularity
  needed for your specific work-splitting strategy (e.g. per-file vs per-row-group) rather than
  copying `table_scan.cpp`'s structure verbatim.

- **Context7 MCP was unavailable** (`resolve-library-id` returned "Invalid API key") in this
  environment, so no DuckDB documentation-site content was cross-checked against these source-derived
  claims. All facts above are sourced solely from real `v1.5.4`-tagged source files fetched via
  `raw.githubusercontent.com`, listed inline as `file:line` citations.
