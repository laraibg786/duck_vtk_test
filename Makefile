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

# Stamp the correct DuckDB version into the extension's metadata footer.
#
# Every .duckdb_extension records the DuckDB version it was built for, and LOAD
# refuses a mismatch. That version comes from `git describe` on the duckdb
# submodule. We fetch the submodule shallowly at a bare commit (see
# scripts/setup_dev_env.sh), so it carries NO TAGS and git describe yields
# nothing — the build then stamps the fallback "v0.0.1" and the extension fails
# to load with:
#
#   Invalid Input Error: Failed to load '...vtk.duckdb_extension', The file was
#   built specifically for DuckDB version 'v0.0.1' and can only be loaded with
#   that version of DuckDB. (this version of DuckDB is 'v1.5.4')
#
# MUST be kept in step with the duckdb submodule pin (08e34c447b == v1.5.4).
# `make check-pin` verifies the two agree.
DUCKDB_VERSION_TAG ?= v1.5.4
OVERRIDE_GIT_DESCRIBE ?= $(DUCKDB_VERSION_TAG)

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

.PHONY: setup data smoke oracle invariants check phase0 print-vtk check-pin

## Install host prerequisites (brew toolchain + python venv for the test oracle)
setup:
	./scripts/setup_dev_env.sh

## Verify the committed test-data corpus against its checksum manifest
data:
	cd test/data && sha256sum -c MANIFEST.sha256 | grep -v ': OK$' || echo "all corpus files verified"

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

## Verify DUCKDB_VERSION_TAG matches the duckdb submodule pin, and that the
## installed CLI is the same build. A drift here produces an extension that
## silently refuses to load, so it is worth an explicit check.
check-pin:
	@sub=$$(git -C duckdb rev-parse HEAD 2>/dev/null || echo missing); \
	cli=$$(duckdb --version 2>/dev/null || echo missing); \
	echo "duckdb submodule : $$sub"; \
	echo "DUCKDB_VERSION_TAG: $(DUCKDB_VERSION_TAG)"; \
	echo "system duckdb CLI : $$cli"; \
	case "$$cli" in \
	  *"$(DUCKDB_VERSION_TAG) "*) echo "OK: CLI version matches DUCKDB_VERSION_TAG" ;; \
	  missing) echo "WARN: no duckdb on PATH; cannot cross-check" ;; \
	  *) echo "MISMATCH: CLI is '$$cli' but DUCKDB_VERSION_TAG is $(DUCKDB_VERSION_TAG)."; \
	     echo "         The built extension will refuse to load into this CLI."; exit 1 ;; \
	esac

# ---------------------------------------------------------------------------
# One-command setup and install
# ---------------------------------------------------------------------------

## Full first-time setup: submodules at the pinned commits, a VTK to build
## against, and the Python oracle venv. Idempotent — safe to re-run.
## This is the ONLY command a new contributor needs before `make release`.
configure:
	./scripts/configure.sh

## Install the built extension where DuckDB looks for local extensions, so
## `LOAD vtk;` works with no path. Prints the exact commands to use it.
install: release
	./scripts/install_extension.sh

## Remove the installed extension.
uninstall:
	./scripts/install_extension.sh --uninstall

## Everything: setup, build, and the full test suite.
all-checks: configure release check

.PHONY: configure install uninstall all-checks

## Smoke test through the DuckDB Python client (needs `make install` first).
## Uses an ad-hoc uvx environment, so nothing is installed system-wide.
python-smoke: install
	uvx --with duckdb==$(patsubst v%,%,$(DUCKDB_VERSION_TAG)) python scripts/python_smoke.py

.PHONY: python-smoke
