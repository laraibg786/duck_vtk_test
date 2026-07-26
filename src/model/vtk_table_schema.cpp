#include "model/vtk_table_schema.hpp"

#include "duckdb/common/string_util.hpp"
#include "model/vtk_types.hpp"

#include <set>

namespace duckdb {

const std::vector<VtkTableKind> &VtkAllTableKinds() {
	static const std::vector<VtkTableKind> kinds = {
	    VtkTableKind::POINTS,     VtkTableKind::CELLS,  VtkTableKind::CELL_POINTS,
	    VtkTableKind::FIELD_DATA, VtkTableKind::ARRAYS, VtkTableKind::INFO,
	};
	return kinds;
}

const char *VtkTableName(VtkTableKind kind) {
	switch (kind) {
	case VtkTableKind::POINTS:
		return "points";
	case VtkTableKind::CELLS:
		return "cells";
	case VtkTableKind::CELL_POINTS:
		return "cell_points";
	case VtkTableKind::FIELD_DATA:
		return "field_data";
	case VtkTableKind::ARRAYS:
		return "vtk_arrays";
	default:
		return "vtk_info";
	}
}

bool VtkTableKindFromName(const std::string &name, VtkTableKind &out) {
	for (auto kind : VtkAllTableKinds()) {
		if (StringUtil::CIEquals(name, VtkTableName(kind))) {
			out = kind;
			return true;
		}
	}
	return false;
}

std::vector<std::string> VtkTableSchema::Names() const {
	std::vector<std::string> result;
	result.reserve(columns.size());
	for (auto &c : columns) {
		result.push_back(c.name);
	}
	return result;
}

std::vector<LogicalType> VtkTableSchema::Types() const {
	std::vector<LogicalType> result;
	result.reserve(columns.size());
	for (auto &c : columns) {
		result.push_back(c.type);
	}
	return result;
}

const VtkTableSchema &VtkSchemaSet::Get(VtkTableKind kind) const {
	return tables[static_cast<size_t>(kind)];
}

std::vector<std::string> VtkResolveColumnNames(const std::vector<std::string> &desired,
                                               const std::vector<std::string> &reserved) {
	std::set<std::string> used_exact;
	std::set<std::string> used_lower;
	for (auto &r : reserved) {
		used_exact.insert(r);
		used_lower.insert(StringUtil::Lower(r));
	}

	std::vector<std::string> result;
	result.reserve(desired.size());
	for (auto &want : desired) {
		// Names are used VERBATIM — including spaces, dots, unicode and mixed
		// case. DuckDB handles those with double quoting, and mangling them would
		// break the user's ability to correlate columns with their source file.
		std::string base = want;
		std::string candidate = base;
		int suffix = 1;
		while (used_exact.count(candidate) > 0 || used_lower.count(StringUtil::Lower(candidate)) > 0) {
			candidate = base + "_" + std::to_string(suffix++);
		}
		used_exact.insert(candidate);
		used_lower.insert(StringUtil::Lower(candidate));
		result.push_back(candidate);
	}
	return result;
}

namespace {

const char *const POINTS_RESERVED[] = {"point_id", "x", "y", "z"};
const char *const CELLS_RESERVED[] = {"cell_id", "cell_type", "cell_type_name", "num_points", "point_ids"};

//! Builds the array-backed portion of `points` or `cells`.
void AppendArrayColumns(VtkTableSchema &schema, std::vector<VtkArrayInfo> &arrays, VtkAssociation association,
                        const std::vector<std::string> &reserved) {
	std::vector<int32_t> slots;
	std::vector<std::string> desired;
	for (size_t i = 0; i < arrays.size(); i++) {
		if (arrays[i].association != association) {
			continue;
		}
		slots.push_back(static_cast<int32_t>(i));
		// Rule 2: an empty or whitespace-only name gets a deterministic
		// placeholder, so the column is still addressable.
		std::string name = arrays[i].name;
		bool blank = name.empty();
		if (!blank) {
			blank = name.find_first_not_of(" \t\r\n") == std::string::npos;
		}
		if (blank) {
			name = StringUtil::Format("unnamed_%s_%d", StringUtil::Lower(VtkAssociationName(association)),
			                          arrays[i].array_index);
		}
		desired.push_back(name);
	}

	auto resolved = VtkResolveColumnNames(desired, reserved);
	for (size_t i = 0; i < slots.size(); i++) {
		auto &info = arrays[static_cast<size_t>(slots[i])];
		info.column_name = resolved[i];
		VtkColumn column;
		column.name = resolved[i];
		column.type = VtkArrayLogicalType(info.vtk_type, info.num_components, info.is_string_array);
		column.array_slot = slots[i];
		schema.columns.push_back(std::move(column));
	}
}

void AddFixed(VtkTableSchema &schema, const char *name, LogicalType type) {
	VtkColumn c;
	c.name = name;
	c.type = std::move(type);
	schema.columns.push_back(std::move(c));
}

} // namespace

VtkSchemaSet VtkBuildSchemas(const VtkDataset &dataset) {
	VtkSchemaSet result;
	result.arrays = dataset.Arrays();
	result.tables.resize(VtkAllTableKinds().size());

	// ---- points -----------------------------------------------------------
	{
		auto &t = result.tables[static_cast<size_t>(VtkTableKind::POINTS)];
		t.kind = VtkTableKind::POINTS;
		AddFixed(t, "point_id", LogicalType::BIGINT);
		// Always DOUBLE, even when the file stores float: GetPoint returns
		// double[3] for every dataset type including the implicit ones, and a
		// schema whose types depend on internal storage precision makes portable
		// SQL impossible. float -> double widening is exact.
		AddFixed(t, "x", LogicalType::DOUBLE);
		AddFixed(t, "y", LogicalType::DOUBLE);
		AddFixed(t, "z", LogicalType::DOUBLE);
		AppendArrayColumns(t, result.arrays, VtkAssociation::POINT,
		                   {POINTS_RESERVED[0], POINTS_RESERVED[1], POINTS_RESERVED[2], POINTS_RESERVED[3]});
	}

	// ---- cells ------------------------------------------------------------
	{
		auto &t = result.tables[static_cast<size_t>(VtkTableKind::CELLS)];
		t.kind = VtkTableKind::CELLS;
		AddFixed(t, "cell_id", LogicalType::BIGINT);
		// INTEGER, not UTINYINT: VTK's ids fit a byte today but the API type is
		// int, and using INTEGER avoids a future widening break.
		AddFixed(t, "cell_type", LogicalType::INTEGER);
		AddFixed(t, "cell_type_name", LogicalType::VARCHAR);
		AddFixed(t, "num_points", LogicalType::INTEGER);
		AddFixed(t, "point_ids", LogicalType::LIST(LogicalType::BIGINT));
		AppendArrayColumns(t, result.arrays, VtkAssociation::CELL,
		                   {CELLS_RESERVED[0], CELLS_RESERVED[1], CELLS_RESERVED[2], CELLS_RESERVED[3],
		                    CELLS_RESERVED[4]});
	}

	// ---- cell_points ------------------------------------------------------
	{
		auto &t = result.tables[static_cast<size_t>(VtkTableKind::CELL_POINTS)];
		t.kind = VtkTableKind::CELL_POINTS;
		AddFixed(t, "cell_id", LogicalType::BIGINT);
		AddFixed(t, "vertex_index", LogicalType::INTEGER);
		AddFixed(t, "point_id", LogicalType::BIGINT);
	}

	// ---- field_data (long form; its grain is not per-point or per-cell) ---
	{
		auto &t = result.tables[static_cast<size_t>(VtkTableKind::FIELD_DATA)];
		t.kind = VtkTableKind::FIELD_DATA;
		AddFixed(t, "array_name", LogicalType::VARCHAR);
		AddFixed(t, "tuple_index", LogicalType::BIGINT);
		AddFixed(t, "component_index", LogicalType::INTEGER);
		AddFixed(t, "component_name", LogicalType::VARCHAR);
		AddFixed(t, "value_double", LogicalType::DOUBLE);
		AddFixed(t, "value_varchar", LogicalType::VARCHAR);
	}

	// ---- vtk_arrays -------------------------------------------------------
	{
		auto &t = result.tables[static_cast<size_t>(VtkTableKind::ARRAYS)];
		t.kind = VtkTableKind::ARRAYS;
		AddFixed(t, "association", LogicalType::VARCHAR);
		AddFixed(t, "array_index", LogicalType::INTEGER);
		AddFixed(t, "name", LogicalType::VARCHAR);
		AddFixed(t, "column_name", LogicalType::VARCHAR);
		AddFixed(t, "vtk_type", LogicalType::INTEGER);
		AddFixed(t, "vtk_type_name", LogicalType::VARCHAR);
		AddFixed(t, "sql_type", LogicalType::VARCHAR);
		AddFixed(t, "num_components", LogicalType::INTEGER);
		AddFixed(t, "num_tuples", LogicalType::BIGINT);
		AddFixed(t, "component_names", LogicalType::LIST(LogicalType::VARCHAR));
		AddFixed(t, "active_as", LogicalType::VARCHAR);
	}

	// ---- vtk_info ---------------------------------------------------------
	{
		auto &t = result.tables[static_cast<size_t>(VtkTableKind::INFO)];
		t.kind = VtkTableKind::INFO;
		AddFixed(t, "file_path", LogicalType::VARCHAR);
		AddFixed(t, "file_size_bytes", LogicalType::BIGINT);
		AddFixed(t, "reader_class", LogicalType::VARCHAR);
		AddFixed(t, "dataset_class", LogicalType::VARCHAR);
		AddFixed(t, "num_points", LogicalType::BIGINT);
		AddFixed(t, "num_cells", LogicalType::BIGINT);
		AddFixed(t, "num_point_arrays", LogicalType::INTEGER);
		AddFixed(t, "num_cell_arrays", LogicalType::INTEGER);
		AddFixed(t, "num_field_arrays", LogicalType::INTEGER);
		AddFixed(t, "bounds_x_min", LogicalType::DOUBLE);
		AddFixed(t, "bounds_x_max", LogicalType::DOUBLE);
		AddFixed(t, "bounds_y_min", LogicalType::DOUBLE);
		AddFixed(t, "bounds_y_max", LogicalType::DOUBLE);
		AddFixed(t, "bounds_z_min", LogicalType::DOUBLE);
		AddFixed(t, "bounds_z_max", LogicalType::DOUBLE);
		AddFixed(t, "vtk_version", LogicalType::VARCHAR);
		AddFixed(t, "extension_version", LogicalType::VARCHAR);
	}

	return result;
}

int64_t VtkTableRowCount(const VtkDataset &dataset, const VtkSchemaSet &schemas, VtkTableKind kind) {
	switch (kind) {
	case VtkTableKind::POINTS:
		return dataset.NumPoints();
	case VtkTableKind::CELLS:
		return dataset.NumCells();
	case VtkTableKind::CELL_POINTS:
		// Reported honestly: for a hex mesh this is 8x the cell count, and a
		// dishonest estimate here makes the planner choose bad join orders.
		return dataset.NumCellPoints();
	case VtkTableKind::FIELD_DATA: {
		int64_t rows = 0;
		for (auto &a : schemas.arrays) {
			if (a.association == VtkAssociation::FIELD) {
				rows += a.num_tuples * a.num_components;
			}
		}
		return rows;
	}
	case VtkTableKind::ARRAYS:
		return static_cast<int64_t>(schemas.arrays.size());
	default:
		return 1;
	}
}

} // namespace duckdb
