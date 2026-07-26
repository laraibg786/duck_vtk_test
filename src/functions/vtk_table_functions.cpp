#include "functions/vtk_table_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "model/vtk_column_writer.hpp"
#include "model/vtk_types.hpp"

#include <vtkAbstractArray.h>
#include <vtkDataArray.h>
#include <vtkStringArray.h>
#include <vtkVersion.h>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Bind data
//===--------------------------------------------------------------------===//

unique_ptr<FunctionData> VtkBindData::Copy() const {
	auto result = make_uniq<VtkBindData>();
	result->dataset = dataset;
	result->schemas = schemas;
	result->kind = kind;
	return std::move(result);
}

bool VtkBindData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<VtkBindData>();
	return dataset == other.dataset && schemas == other.schemas && kind == other.kind;
}

unique_ptr<FunctionData> VtkMakeBindData(std::shared_ptr<VtkDataset> dataset, std::shared_ptr<VtkSchemaSet> schemas,
                                         VtkTableKind kind) {
	auto result = make_uniq<VtkBindData>();
	result->dataset = std::move(dataset);
	result->schemas = std::move(schemas);
	result->kind = kind;
	return std::move(result);
}

namespace {

//===--------------------------------------------------------------------===//
// Global state
//===--------------------------------------------------------------------===//

struct FieldDataRow {
	int32_t array_slot;
	int64_t tuple;
	int32_t component;
};

struct VtkGlobalState : public GlobalTableFunctionState {
	int64_t cursor = 0;
	int64_t row_count = 0;
	//! Flattened (array, tuple, component) triples for the long-form field_data
	//! table. Precomputed because field data is tiny and this keeps the scan loop
	//! uniform with every other table.
	std::vector<FieldDataRow> field_rows;
	//! Reused across rows so connectivity reads do not allocate per row.
	std::vector<int64_t> cell_scratch;
	//! Captured at init: column_ids lives on TableFunctionInitInput, NOT on
	//! TableFunctionInput, so the scan cannot read it directly.
	vector<column_t> column_ids;

	//! Single-threaded in Phase 1. State is deliberately kept here rather than in
	//! local state so a later parallel version only needs a work-range split.
	idx_t MaxThreads() const override {
		return 1;
	}
};

unique_ptr<GlobalTableFunctionState> VtkInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<VtkBindData>();
	auto state = make_uniq<VtkGlobalState>();
	state->column_ids = input.column_ids;
	state->row_count = VtkTableRowCount(*bind.dataset, *bind.schemas, bind.kind);

	if (bind.kind == VtkTableKind::FIELD_DATA) {
		auto &arrays = bind.schemas->arrays;
		for (size_t slot = 0; slot < arrays.size(); slot++) {
			auto &info = arrays[slot];
			if (info.association != VtkAssociation::FIELD) {
				continue;
			}
			for (int64_t t = 0; t < info.num_tuples; t++) {
				for (int32_t c = 0; c < info.num_components; c++) {
					state->field_rows.push_back({static_cast<int32_t>(slot), t, c});
				}
			}
		}
		state->row_count = static_cast<int64_t>(state->field_rows.size());
	}
	return std::move(state);
}

unique_ptr<NodeStatistics> VtkCardinality(ClientContext &context, const FunctionData *bind_data) {
	auto &bind = bind_data->Cast<VtkBindData>();
	const auto rows = VtkTableRowCount(*bind.dataset, *bind.schemas, bind.kind);
	return make_uniq<NodeStatistics>(static_cast<idx_t>(rows), static_cast<idx_t>(rows));
}

double VtkProgress(ClientContext &context, const FunctionData *bind_data, const GlobalTableFunctionState *gstate) {
	auto &state = gstate->Cast<VtkGlobalState>();
	if (state.row_count <= 0) {
		return 100.0;
	}
	return 100.0 * static_cast<double>(state.cursor) / static_cast<double>(state.row_count);
}

//===--------------------------------------------------------------------===//
// Column emitters
//===--------------------------------------------------------------------===//

void SetVarchar(Vector &vec, idx_t row, const std::string &value) {
	FlatVector::GetData<string_t>(vec)[row] = StringVector::AddString(vec, value.data(), value.size());
}

//! Writes one fixed (non-array) column of `points`.
void EmitPointsFixed(const VtkDataset &dataset, int32_t column, int64_t start, idx_t count, Vector &out) {
	switch (column) {
	case 0: { // point_id
		auto data = FlatVector::GetData<int64_t>(out);
		for (idx_t i = 0; i < count; i++) {
			data[i] = start + static_cast<int64_t>(i);
		}
		return;
	}
	case 1:
	case 2:
	case 3: {
		const int axis = column - 1;
		auto data = FlatVector::GetData<double>(out);
		double p[3];
		for (idx_t i = 0; i < count; i++) {
			dataset.GetPoint(start + static_cast<int64_t>(i), p);
			data[i] = p[axis];
		}
		return;
	}
	default:
		throw InternalException("duck_vtk: unexpected fixed points column %d", column);
	}
}

void EmitCellsFixed(const VtkDataset &dataset, VtkGlobalState &state, int32_t column, int64_t start, idx_t count,
                    Vector &out) {
	switch (column) {
	case 0: { // cell_id
		auto data = FlatVector::GetData<int64_t>(out);
		for (idx_t i = 0; i < count; i++) {
			data[i] = start + static_cast<int64_t>(i);
		}
		return;
	}
	case 1: { // cell_type
		auto data = FlatVector::GetData<int32_t>(out);
		for (idx_t i = 0; i < count; i++) {
			data[i] = dataset.GetCellType(start + static_cast<int64_t>(i));
		}
		return;
	}
	case 2: { // cell_type_name
		for (idx_t i = 0; i < count; i++) {
			SetVarchar(out, i, VtkCellTypeName(dataset.GetCellType(start + static_cast<int64_t>(i))));
		}
		return;
	}
	case 3: { // num_points
		auto data = FlatVector::GetData<int32_t>(out);
		for (idx_t i = 0; i < count; i++) {
			state.cell_scratch.clear();
			data[i] = dataset.GetCellPoints(start + static_cast<int64_t>(i), state.cell_scratch);
		}
		return;
	}
	case 4: { // point_ids — LIST(BIGINT)
		// Reserve before touching the child, per the LIST contract.
		const idx_t capacity = count * static_cast<idx_t>(dataset.MaxCellSize() > 0 ? dataset.MaxCellSize() : 1);
		ListVector::Reserve(out, capacity);
		auto entries = FlatVector::GetData<list_entry_t>(out);
		auto &child = ListVector::GetEntry(out);
		auto child_data = FlatVector::GetData<int64_t>(child);
		idx_t offset = 0;
		for (idx_t i = 0; i < count; i++) {
			state.cell_scratch.clear();
			dataset.GetCellPoints(start + static_cast<int64_t>(i), state.cell_scratch);
			entries[i].offset = offset;
			entries[i].length = state.cell_scratch.size();
			for (auto id : state.cell_scratch) {
				child_data[offset++] = id;
			}
		}
		ListVector::SetListSize(out, offset);
		return;
	}
	default:
		throw InternalException("duck_vtk: unexpected fixed cells column %d", column);
	}
}

//! cell_points is the flattening of the connectivity. Rows are located by walking
//! cells from the start; for Phase 1 that is acceptable, and the cost is
//! documented. A later phase can precompute a cell->offset index.
void EmitCellPoints(const VtkDataset &dataset, VtkGlobalState &state, const vector<column_t> &column_ids,
                    int64_t start, idx_t count, DataChunk &output) {
	// Resolve the starting (cell, vertex) position for `start`.
	int64_t remaining = start;
	int64_t cell = 0;
	int32_t vertex = 0;
	const int64_t num_cells = dataset.NumCells();
	while (cell < num_cells) {
		state.cell_scratch.clear();
		const int32_t n = dataset.GetCellPoints(cell, state.cell_scratch);
		if (remaining < n) {
			vertex = static_cast<int32_t>(remaining);
			break;
		}
		remaining -= n;
		cell++;
	}

	std::vector<int64_t> cell_ids(count);
	std::vector<int32_t> vertex_indexes(count);
	std::vector<int64_t> point_ids(count);

	idx_t produced = 0;
	while (produced < count && cell < num_cells) {
		state.cell_scratch.clear();
		const int32_t n = dataset.GetCellPoints(cell, state.cell_scratch);
		while (vertex < n && produced < count) {
			cell_ids[produced] = cell;
			vertex_indexes[produced] = vertex;
			point_ids[produced] = state.cell_scratch[static_cast<size_t>(vertex)];
			produced++;
			vertex++;
		}
		if (vertex >= n) {
			cell++;
			vertex = 0;
		}
	}

	for (idx_t col = 0; col < column_ids.size(); col++) {
		auto &vec = output.data[col];
		vec.SetVectorType(VectorType::FLAT_VECTOR);
		const auto id = column_ids[col];
		if (id == COLUMN_IDENTIFIER_ROW_ID) {
			auto data = FlatVector::GetData<int64_t>(vec);
			for (idx_t i = 0; i < produced; i++) {
				data[i] = start + static_cast<int64_t>(i);
			}
			continue;
		}
		switch (id) {
		case 0: {
			auto data = FlatVector::GetData<int64_t>(vec);
			for (idx_t i = 0; i < produced; i++) {
				data[i] = cell_ids[i];
			}
			break;
		}
		case 1: {
			auto data = FlatVector::GetData<int32_t>(vec);
			for (idx_t i = 0; i < produced; i++) {
				data[i] = vertex_indexes[i];
			}
			break;
		}
		default: {
			auto data = FlatVector::GetData<int64_t>(vec);
			for (idx_t i = 0; i < produced; i++) {
				data[i] = point_ids[i];
			}
			break;
		}
		}
	}
	output.SetCardinality(produced);
}

void EmitFieldData(const VtkBindData &bind, VtkGlobalState &state, const vector<column_t> &column_ids, int64_t start,
                   idx_t count, DataChunk &output) {
	auto &arrays = bind.schemas->arrays;
	for (idx_t col = 0; col < column_ids.size(); col++) {
		auto &vec = output.data[col];
		vec.SetVectorType(VectorType::FLAT_VECTOR);
		const auto id = column_ids[col];
		for (idx_t i = 0; i < count; i++) {
			const auto &row = state.field_rows[static_cast<size_t>(start) + i];
			const auto &info = arrays[static_cast<size_t>(row.array_slot)];
			auto *array = bind.dataset->Array(info);
			switch (id == COLUMN_IDENTIFIER_ROW_ID ? 99 : id) {
			case 0:
				SetVarchar(vec, i, info.name);
				break;
			case 1:
				FlatVector::GetData<int64_t>(vec)[i] = row.tuple;
				break;
			case 2:
				FlatVector::GetData<int32_t>(vec)[i] = row.component;
				break;
			case 3: {
				if (row.component < static_cast<int32_t>(info.component_names.size()) &&
				    !info.component_names[static_cast<size_t>(row.component)].empty()) {
					SetVarchar(vec, i, info.component_names[static_cast<size_t>(row.component)]);
				} else {
					FlatVector::SetNull(vec, i, true);
				}
				break;
			}
			case 4: { // value_double — NULL for string arrays
				auto *numeric = array ? vtkDataArray::SafeDownCast(array) : nullptr;
				if (!numeric) {
					FlatVector::SetNull(vec, i, true);
				} else {
					FlatVector::GetData<double>(vec)[i] =
					    numeric->GetComponent(static_cast<vtkIdType>(row.tuple), row.component);
				}
				break;
			}
			case 5: { // value_varchar — always populated
				if (!array) {
					FlatVector::SetNull(vec, i, true);
					break;
				}
				vtkVariant v = array->GetVariantValue(static_cast<vtkIdType>(row.tuple * info.num_components +
				                                                            row.component));
				auto text = v.ToString();
				SetVarchar(vec, i, text);
				break;
			}
			default:
				FlatVector::GetData<int64_t>(vec)[i] = start + static_cast<int64_t>(i);
				break;
			}
		}
	}
	output.SetCardinality(count);
}

void EmitArraysTable(const VtkBindData &bind, const vector<column_t> &column_ids, int64_t start, idx_t count,
                     DataChunk &output) {
	auto &arrays = bind.schemas->arrays;
	for (idx_t col = 0; col < column_ids.size(); col++) {
		auto &vec = output.data[col];
		vec.SetVectorType(VectorType::FLAT_VECTOR);
		const auto id = column_ids[col];
		if (id == COLUMN_IDENTIFIER_ROW_ID) {
			auto data = FlatVector::GetData<int64_t>(vec);
			for (idx_t i = 0; i < count; i++) {
				data[i] = start + static_cast<int64_t>(i);
			}
			continue;
		}
		for (idx_t i = 0; i < count; i++) {
			const auto &info = arrays[static_cast<size_t>(start) + i];
			switch (id) {
			case 0:
				SetVarchar(vec, i, VtkAssociationName(info.association));
				break;
			case 1:
				FlatVector::GetData<int32_t>(vec)[i] = info.array_index;
				break;
			case 2:
				SetVarchar(vec, i, info.name);
				break;
			case 3:
				SetVarchar(vec, i, info.column_name);
				break;
			case 4:
				FlatVector::GetData<int32_t>(vec)[i] = info.vtk_type;
				break;
			case 5:
				SetVarchar(vec, i, info.vtk_type_name);
				break;
			case 6:
				SetVarchar(vec, i,
				           VtkArrayLogicalType(info.vtk_type, info.num_components, info.is_string_array).ToString());
				break;
			case 7:
				FlatVector::GetData<int32_t>(vec)[i] = info.num_components;
				break;
			case 8:
				FlatVector::GetData<int64_t>(vec)[i] = info.num_tuples;
				break;
			case 9: { // component_names LIST(VARCHAR)
				if (info.component_names.empty()) {
					FlatVector::SetNull(vec, i, true);
					auto entries = FlatVector::GetData<list_entry_t>(vec);
					entries[i] = {ListVector::GetListSize(vec), 0};
					break;
				}
				const idx_t base = ListVector::GetListSize(vec);
				ListVector::Reserve(vec, base + info.component_names.size());
				auto &child = ListVector::GetEntry(vec);
				for (size_t c = 0; c < info.component_names.size(); c++) {
					FlatVector::GetData<string_t>(child)[base + c] = StringVector::AddString(
					    child, info.component_names[c].data(), info.component_names[c].size());
				}
				FlatVector::GetData<list_entry_t>(vec)[i] = {base, info.component_names.size()};
				ListVector::SetListSize(vec, base + info.component_names.size());
				break;
			}
			default:
				if (info.active_as.empty()) {
					FlatVector::SetNull(vec, i, true);
				} else {
					SetVarchar(vec, i, info.active_as);
				}
				break;
			}
		}
	}
	output.SetCardinality(count);
}

void EmitInfo(const VtkBindData &bind, const vector<column_t> &column_ids, DataChunk &output) {
	auto &dataset = *bind.dataset;
	int32_t n_point = 0, n_cell = 0, n_field = 0;
	for (auto &a : bind.schemas->arrays) {
		switch (a.association) {
		case VtkAssociation::POINT:
			n_point++;
			break;
		case VtkAssociation::CELL:
			n_cell++;
			break;
		default:
			n_field++;
			break;
		}
	}
	const bool has_bounds = dataset.HasBounds();
	for (idx_t col = 0; col < column_ids.size(); col++) {
		auto &vec = output.data[col];
		vec.SetVectorType(VectorType::FLAT_VECTOR);
		const auto id = column_ids[col];
		switch (id == COLUMN_IDENTIFIER_ROW_ID ? 99 : id) {
		case 0:
			SetVarchar(vec, 0, dataset.Path());
			break;
		case 1:
			FlatVector::GetData<int64_t>(vec)[0] = dataset.FileSizeBytes();
			break;
		case 2:
			SetVarchar(vec, 0, dataset.ReaderClass());
			break;
		case 3:
			SetVarchar(vec, 0, dataset.DatasetClass());
			break;
		case 4:
			FlatVector::GetData<int64_t>(vec)[0] = dataset.NumPoints();
			break;
		case 5:
			FlatVector::GetData<int64_t>(vec)[0] = dataset.NumCells();
			break;
		case 6:
			FlatVector::GetData<int32_t>(vec)[0] = n_point;
			break;
		case 7:
			FlatVector::GetData<int32_t>(vec)[0] = n_cell;
			break;
		case 8:
			FlatVector::GetData<int32_t>(vec)[0] = n_field;
			break;
		case 9:
		case 10:
		case 11:
		case 12:
		case 13:
		case 14: {
			// NULL rather than VTK's inverted sentinel box for an empty dataset.
			if (!has_bounds) {
				FlatVector::SetNull(vec, 0, true);
			} else {
				FlatVector::GetData<double>(vec)[0] = dataset.Bounds()[id - 9];
			}
			break;
		}
		case 15:
			SetVarchar(vec, 0, vtkVersion::GetVTKVersion());
			break;
		case 16:
#ifdef DUCK_VTK_VERSION
			SetVarchar(vec, 0, DUCK_VTK_VERSION);
#else
			SetVarchar(vec, 0, "unknown");
#endif
			break;
		default:
			FlatVector::GetData<int64_t>(vec)[0] = 0;
			break;
		}
	}
	output.SetCardinality(1);
}

//! points / cells share a shape: fixed columns then array columns.
void EmitRowTable(const VtkBindData &bind, VtkGlobalState &state, const vector<column_t> &column_ids, int64_t start,
                  idx_t count, DataChunk &output) {
	auto &schema = bind.Schema();
	const bool is_points = bind.kind == VtkTableKind::POINTS;

	for (idx_t col = 0; col < column_ids.size(); col++) {
		auto &vec = output.data[col];
		vec.SetVectorType(VectorType::FLAT_VECTOR);
		const auto id = column_ids[col];
		if (id == COLUMN_IDENTIFIER_ROW_ID) {
			auto data = FlatVector::GetData<int64_t>(vec);
			for (idx_t i = 0; i < count; i++) {
				data[i] = start + static_cast<int64_t>(i);
			}
			continue;
		}
		// column_ids maps positionally onto the schema's columns. An off-by-one
		// here would silently return a DIFFERENT column's data, so it is asserted
		// by test/sql/projection.test.
		if (id >= schema.columns.size()) {
			throw InternalException("duck_vtk: projection referenced column %llu of %llu", (uint64_t)id,
			                        (uint64_t)schema.columns.size());
		}
		auto &column = schema.columns[id];
		if (column.array_slot < 0) {
			if (is_points) {
				EmitPointsFixed(*bind.dataset, static_cast<int32_t>(id), start, count, vec);
			} else {
				EmitCellsFixed(*bind.dataset, state, static_cast<int32_t>(id), start, count, vec);
			}
			continue;
		}
		auto &info = bind.schemas->arrays[static_cast<size_t>(column.array_slot)];
		VtkWriteArrayColumn(info, bind.dataset->Array(info), start, count, vec);
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// Scan entry point
//===--------------------------------------------------------------------===//

void VtkScan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &bind = input.bind_data->Cast<VtkBindData>();
	auto &state = input.global_state->Cast<VtkGlobalState>();

	if (state.cursor >= state.row_count) {
		output.SetCardinality(0); // end of stream
		return;
	}
	const idx_t remaining = static_cast<idx_t>(state.row_count - state.cursor);
	const idx_t count = MinValue<idx_t>(remaining, STANDARD_VECTOR_SIZE);
	const int64_t start = state.cursor;

	// `output` is reset by the executor before every call (verified:
	// src/parallel/pipeline_executor.cpp:229), so no output.Reset() here.
	auto &column_ids = state.column_ids;

	switch (bind.kind) {
	case VtkTableKind::POINTS:
	case VtkTableKind::CELLS:
		EmitRowTable(bind, state, column_ids, start, count, output);
		break;
	case VtkTableKind::CELL_POINTS:
		EmitCellPoints(*bind.dataset, state, column_ids, start, count, output);
		break;
	case VtkTableKind::FIELD_DATA:
		EmitFieldData(bind, state, column_ids, start, count, output);
		break;
	case VtkTableKind::ARRAYS:
		EmitArraysTable(bind, column_ids, start, count, output);
		break;
	case VtkTableKind::INFO:
		EmitInfo(bind, column_ids, output);
		break;
	}
	state.cursor += static_cast<int64_t>(count);
}

//===--------------------------------------------------------------------===//
// Bind for the standalone path-taking functions
//===--------------------------------------------------------------------===//

template <VtkTableKind KIND>
unique_ptr<FunctionData> VtkBind(ClientContext &context, TableFunctionBindInput &input,
                                 vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("duck_vtk: %s requires a file path", VtkTableName(KIND));
	}
	const auto path = input.inputs[0].GetValue<string>();
	auto dataset = VtkGetCachedDataset(path);
	auto schemas = std::make_shared<VtkSchemaSet>(VtkBuildSchemas(*dataset));

	auto &schema = schemas->Get(KIND);
	names = schema.Names();
	return_types = schema.Types();
	return VtkMakeBindData(std::move(dataset), std::move(schemas), KIND);
}

//===--------------------------------------------------------------------===//
// vtk_debug_dump
//===--------------------------------------------------------------------===//

struct VtkDebugBindData : public TableFunctionData {
	std::string dump;
	unique_ptr<FunctionData> Copy() const override {
		auto r = make_uniq<VtkDebugBindData>();
		r->dump = dump;
		return std::move(r);
	}
	bool Equals(const FunctionData &other) const override {
		return dump == other.Cast<VtkDebugBindData>().dump;
	}
};

struct VtkDebugState : public GlobalTableFunctionState {
	bool emitted = false;
};

unique_ptr<FunctionData> VtkDebugBind(ClientContext &context, TableFunctionBindInput &input,
                                      vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("duck_vtk: vtk_debug_dump requires a file path");
	}
	auto result = make_uniq<VtkDebugBindData>();
	auto dataset = VtkGetCachedDataset(input.inputs[0].GetValue<string>());
	auto schemas = VtkBuildSchemas(*dataset);
	result->dump = dataset->DebugDump();
	// Append the resolved column names, which is where a mapping bug would show.
	for (auto kind : {VtkTableKind::POINTS, VtkTableKind::CELLS}) {
		result->dump += StringUtil::Format("table %s columns:\n", VtkTableName(kind));
		for (auto &c : schemas.Get(kind).columns) {
			result->dump += StringUtil::Format("  %-32s %s\n", c.name, c.type.ToString());
		}
	}
	names = {"info"};
	return_types = {LogicalType::VARCHAR};
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> VtkDebugInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<VtkDebugState>();
}

void VtkDebugScan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<VtkDebugState>();
	if (state.emitted) {
		output.SetCardinality(0);
		return;
	}
	auto &bind = input.bind_data->Cast<VtkDebugBindData>();
	output.data[0].SetVectorType(VectorType::FLAT_VECTOR);
	SetVarchar(output.data[0], 0, bind.dump);
	output.SetCardinality(1);
	state.emitted = true;
}

TableFunction MakeScan(const char *name, table_function_bind_t bind) {
	TableFunction fn(name, {LogicalType::VARCHAR}, VtkScan, bind, VtkInitGlobal);
	// Honoured from the start: with many array columns, converting unrequested
	// columns dominates the cost, and it is cheap to respect column_ids.
	fn.projection_pushdown = true;
	fn.filter_pushdown = false;
	fn.cardinality = VtkCardinality;
	fn.table_scan_progress = VtkProgress;
	return fn;
}

} // namespace

TableFunction VtkGetScanFunction(VtkTableKind kind) {
	switch (kind) {
	case VtkTableKind::POINTS:
		return MakeScan("vtk_points", VtkBind<VtkTableKind::POINTS>);
	case VtkTableKind::CELLS:
		return MakeScan("vtk_cells", VtkBind<VtkTableKind::CELLS>);
	case VtkTableKind::CELL_POINTS:
		return MakeScan("vtk_cell_points", VtkBind<VtkTableKind::CELL_POINTS>);
	case VtkTableKind::FIELD_DATA:
		return MakeScan("vtk_field_data", VtkBind<VtkTableKind::FIELD_DATA>);
	case VtkTableKind::ARRAYS:
		return MakeScan("vtk_arrays", VtkBind<VtkTableKind::ARRAYS>);
	default:
		return MakeScan("vtk_info", VtkBind<VtkTableKind::INFO>);
	}
}

std::vector<TableFunction> VtkAllTableFunctions() {
	std::vector<TableFunction> result;
	for (auto kind : VtkAllTableKinds()) {
		result.push_back(VtkGetScanFunction(kind));
	}

	TableFunction debug("vtk_debug_dump", {LogicalType::VARCHAR}, VtkDebugScan, VtkDebugBind, VtkDebugInit);
	result.push_back(std::move(debug));
	return result;
}

} // namespace duckdb
