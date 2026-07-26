#define DUCKDB_EXTENSION_MAIN

#include "vtk_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"

// VTK. Only what Phase 1 needs: enough to prove the library is linked,
// initialised, and callable from inside a dlopen'd DuckDB module.
#include "functions/vtk_table_functions.hpp"
#include "vtk/vtk_error_scope.hpp"

#include <vtkVersion.h>
#include <vtkType.h>

namespace duckdb {

//===--------------------------------------------------------------------===//
// vtk_version() -> VARCHAR
//===--------------------------------------------------------------------===//
// This is not a toy function. It is the Phase 1 exit criterion: a value
// returned from VTK, through the extension, to a SQL client, proves rather more
// than "it compiled" — it proves the VTK shared libraries were found at load
// time via RPATH and that calls across the boundary work.
static void VtkVersionFun(DataChunk &args, ExpressionState &state, Vector &result) {
	// A constant vector is correct here and avoids writing the same string N times.
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] =
	    StringVector::AddString(result, vtkVersion::GetVTKVersion());
}

//===--------------------------------------------------------------------===//
// vtk_build_info() -> VARCHAR
//===--------------------------------------------------------------------===//
// Build facts the relational layer depends on, surfaced for diagnostics. In
// particular vtkIdType's width decides whether point/cell ids are 32- or 64-bit
// in the VTK build we happen to be linked against; the type-mapping layer must
// not assume it, and a user filing a bug report should be able to tell us.
static void VtkBuildInfoFun(DataChunk &args, ExpressionState &state, Vector &result) {
	string info = StringUtil::Format("VTK %s; sizeof(vtkIdType)=%d; 64bit_ids=%s; duck_vtk %s",
	                                 vtkVersion::GetVTKVersion(), static_cast<int>(sizeof(vtkIdType)),
	                                 VTK_SIZEOF_ID_TYPE == 8 ? "yes" : "no",
#ifdef DUCK_VTK_VERSION
	                                 DUCK_VTK_VERSION
#else
	                                 "unknown"
#endif
	);
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddString(result, info);
}

void LoadInternal(ExtensionLoader &loader) {
	// Stop VTK writing to the host process's stderr. Must happen before any read.
	VtkSilenceVtkLogger();

	// v1.5.4 registration: loader.RegisterFunction(...), taking the function by
	// value. ExtensionUtil::RegisterFunction(db, fn) — used by most tutorials and
	// by every published extension tracking `main` — does not compile here.
	loader.RegisterFunction(
	    ScalarFunction("vtk_version", {}, LogicalType::VARCHAR, VtkVersionFun));
	loader.RegisterFunction(
	    ScalarFunction("vtk_build_info", {}, LogicalType::VARCHAR, VtkBuildInfoFun));

	for (auto &fn : VtkAllTableFunctions()) {
		loader.RegisterFunction(fn);
	}

	// Phase 3 registers the storage extension here, via
	//   auto &db = loader.GetDatabaseInstance();
	//   StorageExtension::Register(DBConfig::GetConfig(db), "vtk", ...);
	// Note StorageExtension::Register takes a shared_ptr, and
	// DBConfig::storage_extensions is no longer a public member in v1.5.4.
}

void VtkExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string VtkExtension::Name() {
	return "vtk";
}

std::string VtkExtension::Version() const {
#ifdef DUCK_VTK_VERSION
	return DUCK_VTK_VERSION;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(vtk, loader) {
	duckdb::LoadInternal(loader);
}
}
