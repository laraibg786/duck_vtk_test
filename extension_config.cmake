# Included by DuckDB's build system to discover which extensions to build.

duckdb_extension_load(vtk
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# The JSON extension is loaded in tests so that the oracle harness can compare
# nested LIST columns via json serialisation rather than a lossy CSV round-trip.
# Comment out if it slows the build unacceptably.
duckdb_extension_load(json)
