#pragma once

#include "duckdb/function/table_function.hpp"
#include "model/vtk_table_schema.hpp"
#include "vtk/vtk_dataset.hpp"

#include <memory>
#include <vector>

namespace duckdb {

//! Bind data shared by the standalone table functions and the ATTACH catalog.
//!
//! Both entry points must produce EQUIVALENT bind data, or `SELECT * FROM
//! m.points` and `SELECT * FROM vtk_points('f.vtu')` could diverge. They are
//! constructed through the same helper (VtkMakeBindData) so they cannot.
struct VtkBindData : public TableFunctionData {
	std::shared_ptr<VtkDataset> dataset;
	std::shared_ptr<VtkSchemaSet> schemas;
	VtkTableKind kind = VtkTableKind::POINTS;

	//! TableFunctionData::Copy() throws InternalException by default, so any plan
	//! that copies bind data would crash without this override.
	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other) const override;

	const VtkTableSchema &Schema() const {
		return schemas->Get(kind);
	}
};

//! Constructs bind data for one table of an already-read dataset.
unique_ptr<FunctionData> VtkMakeBindData(std::shared_ptr<VtkDataset> dataset, std::shared_ptr<VtkSchemaSet> schemas,
                                         VtkTableKind kind);

//! The scan function for one table kind. Identical object for both entry points.
TableFunction VtkGetScanFunction(VtkTableKind kind);

//! Every function this extension registers: the six tables plus vtk_debug_dump.
std::vector<TableFunction> VtkAllTableFunctions();

} // namespace duckdb
