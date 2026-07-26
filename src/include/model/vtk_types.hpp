#pragma once

#include "duckdb/common/types.hpp"

#include <cstdint>
#include <string>

namespace duckdb {

//! Human-readable VTK cell type name, e.g. 12 -> "VTK_HEXAHEDRON".
//!
//! Uses a static table generated from vtkCellType.h rather than
//! vtkCellTypes::GetClassNameFromTypeId, because:
//!   * the VTK_* macro spellings are what the file-format spec and every CAE
//!     tool's documentation use, so they are what a user recognises;
//!   * the runtime call returns class-style names and "Null"/nullptr for the
//!     several gaps in the enum, which would give NULL columns for valid ids;
//!   * GetClassNameFromTypeId is deprecated in newer VTK.
//! Unknown ids render as "VTK_UNKNOWN_<id>" so a newer VTK's new cell type
//! degrades gracefully rather than erroring or producing NULL.
std::string VtkCellTypeName(int32_t cell_type);

//! Maps a VTK array type + component count to the DuckDB type of its column.
//!
//! num_components == 1 yields a scalar type; > 1 yields LIST(element). A
//! 1-component array must never become a 1-element list (design §4.3).
LogicalType VtkArrayLogicalType(int32_t vtk_type, int32_t num_components, bool is_string_array);

//! The element type alone, ignoring component count.
LogicalType VtkElementLogicalType(int32_t vtk_type, bool is_string_array);

//! True when the VTK type carries integer values that cannot round-trip through
//! `double`. Reading these via vtkDataArray::GetComponent (which returns double)
//! silently corrupts values above 2^53, so the column writer must use a typed
//! accessor for them. This predicate exists so that requirement is testable.
bool VtkTypeNeedsExactIntegerPath(int32_t vtk_type);

} // namespace duckdb
