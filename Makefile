PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Extension identity
EXT_NAME=vtk
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# We link the system/Homebrew VTK via find_package rather than vcpkg, so there is
# no vcpkg.json in this repo and VCPKG_TOOLCHAIN_PATH is intentionally unset.
# See docs/design/03-architecture-and-roadmap.md §5 for why.

# Use Ninja when available: DuckDB's build is large and Ninja's dependency
# handling makes incremental extension rebuilds substantially faster.
# extension-ci-tools turns this into `-G "Ninja"` when GEN is exactly "ninja".
ifneq ($(shell command -v ninja 2>/dev/null),)
	GEN ?= ninja
endif

# Enable the C++ (Catch2) unit tests.
#
# extension-ci-tools hardcodes -DENABLE_UNITTEST_CPP_TESTS=FALSE into its
# BUILD_FLAGS, so test/cpp/*.cpp would be silently ignored. EXT_FLAGS is appended
# AFTER that in the cmake command line, so repeating the option here wins.
# Without this, the L1 unit tests (including the int64 precision test, which is
# the most important single test in the project) never run.
EXT_FLAGS += -DENABLE_UNITTEST_CPP_TESTS=TRUE

# Pin the C++ standard explicitly.
#
# DuckDB core's CMakeLists defaults CMAKE_CXX_STANDARD to 11, while our own
# CMakeLists sets it to 17 as a CACHE variable. Since DuckDB is the top-level
# project here (cmake -S ./duckdb), whichever writes the cache first wins, and a
# CACHE set() will not overwrite an existing entry. Forcing it on the command
# line removes the ambiguity entirely rather than relying on ordering.
# Verify with: grep CMAKE_CXX_STANDARD build/release/CMakeCache.txt
EXT_FLAGS += -DCMAKE_CXX_STANDARD=17

# Allow pointing at a non-default VTK without editing CMake:
#   make release VTK_DIR=/usr/lib/x86_64-linux-gnu/cmake/vtk-9.3
ifneq ($(VTK_DIR),)
	EXT_FLAGS += -DVTK_DIR=$(VTK_DIR)
endif

# All DuckDB extension targets (release, debug, relassert, reldebug, test,
# clean, format, update, ...) come from extension-ci-tools.
# Note: that makefile skips tests when LINUX_CI_IN_DOCKER is explicitly 0, so
# leave it unset locally.
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# ---------------------------------------------------------------------------
# duck_vtk-specific convenience targets
# ---------------------------------------------------------------------------

EXT_RELEASE_PATH := build/release/extension/$(EXT_NAME)/$(EXT_NAME).duckdb_extension
EXT_DEBUG_PATH   := build/debug/extension/$(EXT_NAME)/$(EXT_NAME).duckdb_extension

.PHONY: setup data smoke oracle invariants check phase0 print-vtk

## Install host prerequisites (brew toolchain + python venv for the test oracle)
setup:
	./scripts/setup_dev_env.sh

## Fetch and verify the VTK test-data corpus
data:
	./test/data/fetch_data.sh

## Phase 0 ABI spike: prove we can link and run against the installed VTK
## BEFORE relying on it from inside DuckDB. See scripts/phase0_spike/.
phase0:
	cmake -S scripts/phase0_spike -B build/phase0 $(if $(GEN),-G Ninja,)
	cmake --build build/phase0
	./build/phase0/vtk_spike

## Build + load smoke test, including a load into the SYSTEM duckdb CLI
smoke: release
	./scripts/smoke.sh

## Elementwise diff of every corpus file against Python VTK (the real oracle)
oracle: release
	.venv/bin/python scripts/validate_against_vtk.py \
		--data test/data \
		--duckdb ./build/release/duckdb \
		--ext $(EXT_RELEASE_PATH)

## Run the SQL invariant suite across every corpus file
invariants: release
	./scripts/run_invariants.sh

## Everything that must pass before Phase 1 is considered done.
## Ordered cheapest-first so a failure surfaces as early as possible.
check: phase0 release smoke test oracle invariants
	@echo "=== duck_vtk: all Phase 1 checks passed ==="

## Print the resolved VTK configuration (useful when diagnosing load failures)
print-vtk:
	@grep -E "duck_vtk: (VTK_|VTK library|required|optional)" build/release/CMakeCache.txt 2>/dev/null \
		|| echo "No configured build yet; run 'make release' first."
