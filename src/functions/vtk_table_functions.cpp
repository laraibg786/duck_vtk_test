#include "functions/vtk_table_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "model/vtk_column_writer.hpp"
#include "model/vtk_types.hpp"
#include "vtk/vtk_file_source.hpp"

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
	//! Per-cell connectivity lengths for the chunk currently being emitted into
	//! point_ids. Kept alongside cell_scratch so the chunk can be measured and
	//! reserved exactly before the child vector is touched — see EmitCellsFixed.
	std::vector<int32_t> cell_lengths;
	//! Resume position for the cell_points walk: the (cell, vertex) pair that row
	//! `cp_row` sits at. The scan is strictly sequential, so remembering where the
	//! last chunk stopped turns an O(n^2) rescan into a single pass. See
	//! EmitCellPoints. Zero-initialised, which is already the correct state for the
	//! first chunk (row 0 -> cell 0, vertex 0).
	int64_t cp_row = 0;
	int64_t cp_cell = 0;
	int32_t cp_vertex = 0;
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
		// Gather the chunk's connectivity FIRST, then reserve exactly what it needs.
		//
		// This used to reserve `count * MaxCellSize()`, where MaxCellSize() is the
		// maximum over the WHOLE dataset — so one large cell inflated every chunk,
		// not just the chunk containing it. Measured: an 873 KB PolyData holding a
		// single 50,000-point polyline plus 3,000 vertex cells (53,000 connectivity
		// ids in total, ~424 KB of real output) reserved 2048 * 50,000, which
		// VectorListBuffer rounds up to the next power of two — exactly 1 GiB — and
		// failed with "Out of Memory Error: Failed to allocate block of 1073741824
		// bytes". `SELECT count(*) FROM cells` on the same file was fine, so it
		// presented as a data problem rather than a sizing bug. A polyhedron or a
		// long polyline in a real CFD mesh triggers it just as easily.
		//
		// Measuring first also makes the reservation exact, which means the child
		// data pointer taken after it cannot be invalidated part-way through the
		// fill — the hazard that made the old single-pass shape tempting.
		state.cell_scratch.clear();
		state.cell_lengths.clear();
		state.cell_lengths.reserve(count);
		for (idx_t i = 0; i < count; i++) {
			// GetCellPoints appends and returns the number of ids it appended.
			state.cell_lengths.push_back(dataset.GetCellPoints(start + static_cast<int64_t>(i), state.cell_scratch));
		}

		ListVector::Reserve(out, state.cell_scratch.size());
		auto entries = FlatVector::GetData<list_entry_t>(out);
		auto &child = ListVector::GetEntry(out);
		auto child_data = FlatVector::GetData<int64_t>(child);

		idx_t offset = 0;
		for (idx_t i = 0; i < count; i++) {
			const auto length = static_cast<idx_t>(state.cell_lengths[i]);
			entries[i].offset = offset;
			entries[i].length = length;
			offset += length;
		}
		for (idx_t i = 0; i < state.cell_scratch.size(); i++) {
			child_data[i] = state.cell_scratch[i];
		}
		ListVector::SetListSize(out, offset);
		return;
	}
	default:
		throw InternalException("duck_vtk: unexpected fixed cells column %d", column);
	}
}

//! cell_points is the flattening of the connectivity: one row per (cell, vertex).
//!
//! Locating the first row of a chunk used to restart the cell walk from cell 0 every
//! time, making the whole scan O(n^2) in the cell count. Measured on single-vertex
//! PolyData: 400k cells took 0.50 s but 1.6M took 10.66 s — 4x the cells for 21x the
//! time, while `SELECT count(*) FROM cells` over the same files stayed linear.
//!
//! The scan is strictly sequential and single-threaded (VtkGlobalState::MaxThreads
//! returns 1), so the resume point is simply the previous chunk's end. Caching it
//! makes this linear. The walk-from-zero path is retained for the cold start and for
//! any future non-sequential entry, so correctness does not depend on the cache
//! being warm.
void EmitCellPoints(const VtkDataset &dataset, VtkGlobalState &state, const vector<column_t> &column_ids, int64_t start,
                    idx_t count, DataChunk &output) {
	int64_t cell = 0;
	int32_t vertex = 0;
	const int64_t num_cells = dataset.NumCells();

	if (start == state.cp_row) {
		cell = state.cp_cell;
		vertex = state.cp_vertex;
	} else {
		int64_t remaining = start;
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

	// Remember where this chunk stopped, so the next one does not rewalk.
	state.cp_row = start + static_cast<int64_t>(produced);
	state.cp_cell = cell;
	state.cp_vertex = vertex;

	// The caller sizes `count` from the rows remaining, so a short produce means the
	// connectivity disagrees with the row count this table was bound with — which
	// would silently drop rows rather than fail. Cheap to assert, impossible to
	// diagnose later.
	if (produced != count) {
		throw InternalException("duck_vtk: cell_points produced %llu rows for a chunk of %llu",
		                        (unsigned long long)produced, (unsigned long long)count);
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
				vtkVariant v =
				    array->GetVariantValue(static_cast<vtkIdType>(row.tuple * info.num_components + row.component));
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
					FlatVector::GetData<string_t>(child)[base + c] =
					    StringVector::AddString(child, info.component_names[c].data(), info.component_names[c].size());
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
		case 17:
			SetVarchar(vec, 0, dataset.SourceKind());
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
	// Route reads through DuckDB's virtual filesystem so https/s3/gcs/azure work
	// whenever the user has the corresponding extension loaded.
	auto source = VtkMakeDuckDBFileSource(context);
	auto dataset = VtkGetCachedDataset(path, source.get());
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
	auto source = VtkMakeDuckDBFileSource(context);
	auto dataset = VtkGetCachedDataset(input.inputs[0].GetValue<string>(), source.get());
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

//===--------------------------------------------------------------------===//
// Plan serialization
//===--------------------------------------------------------------------===//
//
// NOT optional, and not merely about prepared statements — omitting this caused a
// silent WRONG-RESULTS bug.
//
// DuckDB's common-subplan optimizer builds its signature by SERIALIZING each
// operator. With no serialize callback, LogicalGet::Serialize falls back to writing
// the function's input `parameters` (logical_get.cpp:248, "no serialize method:
// serialize input values and named_parameters for rebinding purposes").
//
// On the ATTACH path there ARE no input parameters: VtkTableEntry::GetScanFunction
// supplies bind data directly, so the path lives only in the bind data. Two scans of
// different attached VTK databases therefore serialized to identical bytes, the
// optimizer judged them the same subplan, materialised one as a CTE and pointed both
// at it. Observed symptom:
//
//   ATTACH 'a.vti' AS i; ATTACH 'b.vtp' AS pd;
//   SELECT (SELECT file_path FROM i.vtk_info), (SELECT file_path FROM pd.vtk_info);
//   -> 'a.vti' | 'a.vti'          -- both subqueries read the FIRST dataset
//
// Writing the path and table kind makes the signature distinguish them. The
// standalone vtk_*(path) functions were unaffected because their path IS an input
// parameter, which is why the bug only showed through ATTACH.
void VtkSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p, const TableFunction &) {
	auto &bind = bind_data_p->Cast<VtkBindData>();
	serializer.WriteProperty(100, "path", bind.dataset->Path());
	serializer.WriteProperty(101, "kind", static_cast<uint8_t>(bind.kind));
}

unique_ptr<FunctionData> VtkDeserialize(Deserializer &deserializer, TableFunction &) {
	auto &context = deserializer.Get<ClientContext &>();
	auto path = deserializer.ReadProperty<string>(100, "path");
	auto kind = static_cast<VtkTableKind>(deserializer.ReadProperty<uint8_t>(101, "kind"));
	// Re-reads through the cache, so a plan deserialised in the same session reuses
	// the dataset rather than fetching it again.
	auto source = VtkMakeDuckDBFileSource(context);
	auto dataset = VtkGetCachedDataset(path, source.get());
	auto schemas = std::make_shared<VtkSchemaSet>(VtkBuildSchemas(*dataset));
	return VtkMakeBindData(std::move(dataset), std::move(schemas), kind);
}

TableFunction MakeScan(const char *name, table_function_bind_t bind) {
	TableFunction fn(name, {LogicalType::VARCHAR}, VtkScan, bind, VtkInitGlobal);
	// Honoured from the start: with many array columns, converting unrequested
	// columns dominates the cost, and it is cheap to respect column_ids.
	fn.projection_pushdown = true;
	fn.filter_pushdown = false;
	fn.cardinality = VtkCardinality;
	fn.table_scan_progress = VtkProgress;
	// See the comment above VtkSerialize: without these, two scans of different
	// attached VTK databases are wrongly treated as the same subplan.
	fn.serialize = VtkSerialize;
	fn.deserialize = VtkDeserialize;
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
