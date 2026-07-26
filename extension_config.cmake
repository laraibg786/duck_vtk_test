# Included by DuckDB's build system to discover which extensions to build.

duckdb_extension_load(vtk
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# No extra core extensions are loaded. The oracle harness uses the CLI's built-in
# `.mode json`, not the `json` extension, so building it would only add build time.
