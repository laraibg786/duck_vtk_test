#pragma once

#include "duckdb/common/types.hpp"
#include "vtk/vtk_dataset.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {

enum class VtkTableKind : uint8_t {
	POINTS = 0,
	CELLS = 1,
	CELL_POINTS = 2,
	FIELD_DATA = 3,
	ARRAYS = 4,
	INFO = 5,
};

//! All six table kinds, in a stable order.
const std::vector<VtkTableKind> &VtkAllTableKinds();
const char *VtkTableName(VtkTableKind kind);
bool VtkTableKindFromName(const std::string &name, VtkTableKind &out);

struct VtkColumn {
	std::string name;
	LogicalType type;
	//! >= 0 for columns backed by a VTK array; indexes into VtkSchemaSet::arrays.
	int32_t array_slot = -1;
};

struct VtkTableSchema {
	VtkTableKind kind = VtkTableKind::POINTS;
	std::vector<VtkColumn> columns;

	std::vector<std::string> Names() const;
	std::vector<LogicalType> Types() const;
};

//! The resolved relational shape of one dataset: every table's columns, plus the
//! array metadata with final `column_name` values filled in.
//!
//! Column-name assignment (design §6) lives here rather than in VtkDataset so
//! that the policy is a pure function of the array list and therefore unit
//! testable without touching a file.
struct VtkSchemaSet {
	std::vector<VtkArrayInfo> arrays;
	std::vector<VtkTableSchema> tables; //! indexed by (int)VtkTableKind

	const VtkTableSchema &Get(VtkTableKind kind) const;
};

VtkSchemaSet VtkBuildSchemas(const VtkDataset &dataset);

//! Exposed for unit testing: applies design §6's collision rules in order to a
//! list of desired names, given a set of already-reserved names. Returns the
//! final, unique, addressable names.
//!
//! `reserved` are the fixed geometry columns of the target table. Matching is
//! done both exactly and case-insensitively, because DuckDB resolves unquoted
//! identifiers case-insensitively — so `Pressure` and `pressure` in the same
//! table would be ambiguous to an unquoted query even though they differ.
std::vector<std::string> VtkResolveColumnNames(const std::vector<std::string> &desired,
                                               const std::vector<std::string> &reserved);

//! Number of rows a given table has for a dataset.
int64_t VtkTableRowCount(const VtkDataset &dataset, const VtkSchemaSet &schemas, VtkTableKind kind);

} // namespace duckdb
