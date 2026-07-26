#pragma once

#include "duckdb.hpp"

namespace duckdb {

class VtkExtension : public Extension {
public:
	//! NOTE the signature: DuckDB v1.5.4 passes an ExtensionLoader, not a
	//! DatabaseInstance. The pre-1.5 `void Load(DuckDB &db)` form and the
	//! `ExtensionUtil` helpers no longer exist — extension_util.hpp is a
	//! static_assert(false) stub in this version.
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

//! Registers everything the extension provides. Called from both
//! VtkExtension::Load and the C entrypoint, so there is exactly one
//! registration path.
void LoadInternal(ExtensionLoader &loader);

} // namespace duckdb
