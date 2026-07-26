# duck_vtk — DuckDB extension exposing VTK mesh files as relational tables.
#
# Build via the Makefile (`make release` / `make debug`), which drives DuckDB's
# own CMake build with extension_config.cmake. Do not invoke cmake on this file
# directly; it expects DuckDB's build_static_extension/build_loadable_extension
# macros to already be defined.
#
# Structure of this file mirrors duckdb/extension-template, with the OpenSSL/vcpkg
# dependency replaced by VTK. Every non-obvious line is commented, because the
# VTK integration has two failure modes that are silent at build time
# (see the autoinit and RPATH blocks below).

cmake_minimum_required(VERSION 3.16)

set(TARGET_NAME vtk)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME} CXX)

# Must match DuckDB v1.5.4's own standard. Raising this independently of DuckDB
# risks ODR/ABI mismatches in headers shared with the loading process.
set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The loadable extension is a shared module, so everything linked into it must
# be position-independent — including the static library target.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include_directories(src/include)

# ---------------------------------------------------------------------------
# VTK discovery
# ---------------------------------------------------------------------------
# Kept in a separate module so the discovery/diagnostic logic stays readable and
# so alternate VTK sources (apt libvtk9-dev, a source build) can be selected by
# passing -DVTK_DIR=... without editing this file.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/DuckVTKOptimize.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/DuckVTKFindVTK.cmake)

# ---------------------------------------------------------------------------
# DuckDB API capability probe
# ---------------------------------------------------------------------------
# The one API that moved between DuckDB 1.4 (LTS) and 1.5 is storage-extension
# registration. Rather than branching on a version STRING — which breaks on
# nightlies, forks and unreleased revisions — read the actual header and look for
# the symbol. This makes a single source tree build against both LTS and current.
# Locate the header in the DuckDB source tree ACTUALLY being built, which is not
# necessarily the submodule: `make release DUCKDB_SRCDIR=/path/to/duckdb` is a
# supported way to build against a different version. DuckDB is the top-level
# CMake project here, so CMAKE_SOURCE_DIR points at its source root. Reading the
# submodule unconditionally would silently probe the wrong version and pick the
# wrong branch — which is exactly the bug this comment exists to prevent.
set(DUCK_VTK_STORAGE_EXTENSION_HEADER "")
foreach(_candidate
        "${CMAKE_SOURCE_DIR}/src/include/duckdb/storage/storage_extension.hpp"
        "${CMAKE_CURRENT_LIST_DIR}/duckdb/src/include/duckdb/storage/storage_extension.hpp")
  if(EXISTS "${_candidate}")
    set(DUCK_VTK_STORAGE_EXTENSION_HEADER "${_candidate}")
    break()
  endif()
endforeach()
set(DUCK_VTK_HAS_SE_REGISTER 0)
if(DUCK_VTK_STORAGE_EXTENSION_HEADER)
  file(READ "${DUCK_VTK_STORAGE_EXTENSION_HEADER}" _se_header)
  if(_se_header MATCHES "static[ \t\r\n]+void[ \t\r\n]+Register[ \t\r\n]*\\(")
    set(DUCK_VTK_HAS_SE_REGISTER 1)
  endif()
  message(STATUS "duck_vtk: probed ${DUCK_VTK_STORAGE_EXTENSION_HEADER}")
  message(STATUS "duck_vtk: StorageExtension::Register present = ${DUCK_VTK_HAS_SE_REGISTER} "
                 "(0 => DuckDB 1.4-style DBConfig::storage_extensions map)")
else()
  message(WARNING "duck_vtk: could not locate storage_extension.hpp in the DuckDB "
                  "source tree; assuming the 1.4-style registration API")
endif()

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
# Listed explicitly rather than globbed: a GLOB does not re-run when a file is
# added, which produces confusing "undefined symbol" link errors for a file the
# author believes is being compiled.
set(EXTENSION_SOURCES
    src/vtk_extension.cpp

    # L1 — VTK access
    src/vtk/vtk_error_scope.cpp
    src/vtk/vtk_file_source.cpp
    src/vtk/vtk_dataset.cpp

    # L2 — relational model
    src/model/vtk_types.cpp
    src/model/vtk_table_schema.cpp
    src/model/vtk_column_writer.cpp

    # L3 — table functions (all six tables plus vtk_debug_dump share one file,
    # because they share the scan loop, bind helper and emitters; splitting them
    # would duplicate that machinery six times)
    src/functions/vtk_table_functions.cpp

    # L4 — catalog / ATTACH
    src/catalog/vtk_catalog.cpp
)

# DuckDB builds TWO targets from the same sources:
#   ${EXTENSION_NAME}          — static lib, linked into a statically-built duckdb
#   ${LOADABLE_EXTENSION_NAME} — the .duckdb_extension shared module
# Third-party libraries and per-target CMake glue must be applied to BOTH, or the
# extension works in one build mode and fails to link in the other.
build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

foreach(_duck_vtk_target ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})
  target_link_libraries(${_duck_vtk_target} ${VTK_LIBRARIES})
  duck_vtk_apply_optimisations(${_duck_vtk_target})

  # MANDATORY, and the single most common VTK integration mistake.
  #
  # VTK 9 uses per-module object factories that are registered by generated
  # "autoinit" headers. vtk_module_autoinit() injects the required
  # VTK_MODULE_INIT defines into the target. Without it the build succeeds and
  # links cleanly, then at RUNTIME readers silently produce null/empty output —
  # which our design explicitly forbids confusing with an empty mesh.
  #
  # Called after target_link_libraries so the module list is settled.
  vtk_module_autoinit(TARGETS ${_duck_vtk_target} MODULES ${VTK_LIBRARIES})

  # Bake the VTK library directory into the RPATH. The loadable module is opened
  # by the duckdb process via dlopen(); without an RPATH the VTK .so files are
  # only found if the user happens to have LD_LIBRARY_PATH set, which is a
  # terrible user experience and an easy-to-miss CI-only failure.
  if(DUCK_VTK_LIBRARY_DIR)
    set_target_properties(${_duck_vtk_target} PROPERTIES
      BUILD_WITH_INSTALL_RPATH FALSE
      BUILD_RPATH   "${DUCK_VTK_LIBRARY_DIR}"
      INSTALL_RPATH "${DUCK_VTK_LIBRARY_DIR}"
    )
  endif()
endforeach()

# Surface the extension version to the C++ code (see Version() in the entrypoint).
if(NOT DEFINED DUCK_VTK_VERSION)
  set(DUCK_VTK_VERSION "0.1.0-dev")
endif()
foreach(_duck_vtk_target ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})
  target_compile_definitions(${_duck_vtk_target} PRIVATE
    DUCK_VTK_VERSION="${DUCK_VTK_VERSION}"
    DUCK_VTK_HAS_STORAGE_EXTENSION_REGISTER=${DUCK_VTK_HAS_SE_REGISTER})
endforeach()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
