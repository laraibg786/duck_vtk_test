# Phase 0 ABI spike.
#
# Purpose: prove, before ANY extension code is written, that we can compile and
# link against the Homebrew VTK bottle with the compiler we intend to use for the
# DuckDB extension, and that the resulting binary actually runs and reads a file
# correctly.
#
# The risk being tested: Homebrew's Linux bottles may be built against a
# different libstdc++ than Debian's system GCC 14. A mismatch shows up either as
# a link error (best case, obvious) or as runtime corruption when std::string /
# std::vector cross the shared-library boundary (worst case, silent). VTK's API
# passes std::string by value in several places (e.g. vtkStdString returns), so
# this is a real exposure, not a theoretical one.
#
# Usage:
#   cmake -S . -B build -G Ninja
#   cmake --build build
#   ./build/vtk_spike <path-to-file.vtu>

cmake_minimum_required(VERSION 3.16)
project(vtk_spike CXX)

set(CMAKE_CXX_STANDARD 17)          # must match DuckDB v1.5.4's CMAKE_CXX_STANDARD
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)  # extension is a shared module; test PIC now

# Reuse the extension's own VTK discovery module rather than duplicating the
# search paths and component list. One source of truth means the spike cannot
# validate a different VTK than the extension will actually link — which would
# defeat the entire purpose of a toolchain spike.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/DuckVTKFindVTK.cmake)

message(STATUS "spike: VTK_VERSION      = ${VTK_VERSION}")
message(STATUS "spike: VTK_LIBRARIES    = ${VTK_LIBRARIES}")
message(STATUS "spike: VTK_PREFIX_PATH  = ${VTK_PREFIX_PATH}")

add_executable(vtk_spike main.cpp)
target_link_libraries(vtk_spike PRIVATE ${VTK_LIBRARIES})

# MANDATORY. Without this, VTK's object factories are never registered and the
# readers return null output at runtime while compiling and linking perfectly.
# Exercising it here means the extension build can copy a known-good invocation.
vtk_module_autoinit(TARGETS vtk_spike MODULES ${VTK_LIBRARIES})

# Bake the VTK lib dir into the RPATH so the binary runs without LD_LIBRARY_PATH.
# The extension will need the same treatment; proving it here de-risks that.
set_target_properties(vtk_spike PROPERTIES
  BUILD_WITH_INSTALL_RPATH FALSE
  BUILD_RPATH "${DUCK_VTK_LIBRARY_DIR}"
  INSTALL_RPATH "${DUCK_VTK_LIBRARY_DIR}"
)
