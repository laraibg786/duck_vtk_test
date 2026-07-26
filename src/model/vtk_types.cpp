#include "model/vtk_types.hpp"

#include "duckdb/common/string_util.hpp"

#include <vtkType.h>

namespace duckdb {

namespace {

struct CellTypeEntry {
	int32_t id;
	const char *name;
};

//! Generated from vtkCellType.h (VTK 9.6.2). The enum has gaps (17-20, 38-40,
//! 43-50, 57-59, ...) which is exactly why a table with an explicit fallback
//! beats a runtime lookup.
const CellTypeEntry CELL_TYPES[] = {
    {0, "VTK_EMPTY_CELL"},
    {1, "VTK_VERTEX"},
    {2, "VTK_POLY_VERTEX"},
    {3, "VTK_LINE"},
    {4, "VTK_POLY_LINE"},
    {5, "VTK_TRIANGLE"},
    {6, "VTK_TRIANGLE_STRIP"},
    {7, "VTK_POLYGON"},
    {8, "VTK_PIXEL"},
    {9, "VTK_QUAD"},
    {10, "VTK_TETRA"},
    {11, "VTK_VOXEL"},
    {12, "VTK_HEXAHEDRON"},
    {13, "VTK_WEDGE"},
    {14, "VTK_PYRAMID"},
    {15, "VTK_PENTAGONAL_PRISM"},
    {16, "VTK_HEXAGONAL_PRISM"},
    {21, "VTK_QUADRATIC_EDGE"},
    {22, "VTK_QUADRATIC_TRIANGLE"},
    {23, "VTK_QUADRATIC_QUAD"},
    {24, "VTK_QUADRATIC_TETRA"},
    {25, "VTK_QUADRATIC_HEXAHEDRON"},
    {26, "VTK_QUADRATIC_WEDGE"},
    {27, "VTK_QUADRATIC_PYRAMID"},
    {28, "VTK_BIQUADRATIC_QUAD"},
    {29, "VTK_TRIQUADRATIC_HEXAHEDRON"},
    {30, "VTK_QUADRATIC_LINEAR_QUAD"},
    {31, "VTK_QUADRATIC_LINEAR_WEDGE"},
    {32, "VTK_BIQUADRATIC_QUADRATIC_WEDGE"},
    {33, "VTK_BIQUADRATIC_QUADRATIC_HEXAHEDRON"},
    {34, "VTK_BIQUADRATIC_TRIANGLE"},
    {35, "VTK_CUBIC_LINE"},
    {36, "VTK_QUADRATIC_POLYGON"},
    {37, "VTK_TRIQUADRATIC_PYRAMID"},
    {41, "VTK_CONVEX_POINT_SET"},
    {42, "VTK_POLYHEDRON"},
    {51, "VTK_PARAMETRIC_CURVE"},
    {52, "VTK_PARAMETRIC_SURFACE"},
    {53, "VTK_PARAMETRIC_TRI_SURFACE"},
    {54, "VTK_PARAMETRIC_QUAD_SURFACE"},
    {55, "VTK_PARAMETRIC_TETRA_REGION"},
    {56, "VTK_PARAMETRIC_HEX_REGION"},
    {60, "VTK_HIGHER_ORDER_EDGE"},
    {61, "VTK_HIGHER_ORDER_TRIANGLE"},
    {62, "VTK_HIGHER_ORDER_QUAD"},
    {63, "VTK_HIGHER_ORDER_POLYGON"},
    {64, "VTK_HIGHER_ORDER_TETRAHEDRON"},
    {65, "VTK_HIGHER_ORDER_WEDGE"},
    {66, "VTK_HIGHER_ORDER_PYRAMID"},
    {67, "VTK_HIGHER_ORDER_HEXAHEDRON"},
    {68, "VTK_LAGRANGE_CURVE"},
    {69, "VTK_LAGRANGE_TRIANGLE"},
    {70, "VTK_LAGRANGE_QUADRILATERAL"},
    {71, "VTK_LAGRANGE_TETRAHEDRON"},
    {72, "VTK_LAGRANGE_HEXAHEDRON"},
    {73, "VTK_LAGRANGE_WEDGE"},
    {74, "VTK_LAGRANGE_PYRAMID"},
    {75, "VTK_BEZIER_CURVE"},
    {76, "VTK_BEZIER_TRIANGLE"},
    {77, "VTK_BEZIER_QUADRILATERAL"},
    {78, "VTK_BEZIER_TETRAHEDRON"},
    {79, "VTK_BEZIER_HEXAHEDRON"},
    {80, "VTK_BEZIER_WEDGE"},
    {81, "VTK_BEZIER_PYRAMID"},
};

} // namespace

std::string VtkCellTypeName(int32_t cell_type) {
	for (auto &entry : CELL_TYPES) {
		if (entry.id == cell_type) {
			return entry.name;
		}
	}
	return StringUtil::Format("VTK_UNKNOWN_%d", cell_type);
}

LogicalType VtkElementLogicalType(int32_t vtk_type, bool is_string_array) {
	if (is_string_array) {
		return LogicalType::VARCHAR;
	}
	switch (vtk_type) {
	case VTK_BIT:
		return LogicalType::BOOLEAN;
	// VTK_CHAR is a *number* in VTK's model, not a one-character string. Its
	// signedness is platform-defined, so branch on the actual compiler.
	case VTK_CHAR:
		return std::is_signed<char>::value ? LogicalType::TINYINT : LogicalType::UTINYINT;
	case VTK_SIGNED_CHAR:
		return LogicalType::TINYINT;
	case VTK_UNSIGNED_CHAR:
		return LogicalType::UTINYINT;
	case VTK_SHORT:
		return LogicalType::SMALLINT;
	case VTK_UNSIGNED_SHORT:
		return LogicalType::USMALLINT;
	case VTK_INT:
		return LogicalType::INTEGER;
	case VTK_UNSIGNED_INT:
		return LogicalType::UINTEGER;
	// long is 4 bytes on LLP64 and 8 on LP64. BIGINT is a safe superset in both
	// cases and keeps the SQL schema stable across platforms.
	case VTK_LONG:
	case VTK_LONG_LONG:
		return LogicalType::BIGINT;
	case VTK_UNSIGNED_LONG:
	case VTK_UNSIGNED_LONG_LONG:
		return LogicalType::UBIGINT;
	case VTK_FLOAT:
		return LogicalType::FLOAT;
	case VTK_DOUBLE:
		return LogicalType::DOUBLE;
	case VTK_ID_TYPE:
		// Widen to BIGINT regardless of the build's vtkIdType width, so the SQL
		// schema does not change between a 32- and 64-bit-ids VTK.
		return LogicalType::BIGINT;
	case VTK_STRING:
		return LogicalType::VARCHAR;
	default:
		// vtkVariantArray and anything unforeseen: render as text rather than
		// dropping the column, so the data is at least visible.
		return LogicalType::VARCHAR;
	}
}

LogicalType VtkArrayLogicalType(int32_t vtk_type, int32_t num_components, bool is_string_array) {
	auto element = VtkElementLogicalType(vtk_type, is_string_array);
	if (num_components <= 1) {
		return element;
	}
	// LIST rather than a fixed-size ARRAY for Phase 1: one code path covers
	// 3-vectors, 9-component tensors and arbitrary N. ARRAY is the documented
	// upgrade once its NULL/large-N behaviour is verified. See design §4.3.
	return LogicalType::LIST(element);
}

bool VtkTypeNeedsExactIntegerPath(int32_t vtk_type) {
	switch (vtk_type) {
	case VTK_LONG:
	case VTK_UNSIGNED_LONG:
	case VTK_LONG_LONG:
	case VTK_UNSIGNED_LONG_LONG:
	case VTK_ID_TYPE:
		return true;
	default:
		return false;
	}
}

} // namespace duckdb
