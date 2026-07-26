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
# scripts/configure.sh), so it carries NO TAGS and git describe yields
# nothing — the build then stamps the fallback "v0.0.1" and the extension fails
# to load with:
#
#   Invalid Input Error: Failed to load '...vtk.duckdb_extension', The file was
#   built specifically for DuckDB version 'v0.0.1' and can only be loaded with
#   that version of DuckDB. (this version of DuckDB is 'v1.5.4')
#
# MUST be kept in step with the duckdb submodule pin (08e34c447b == v1.5.4).
# `make check-pin` verifies the two agree.
# Derived from the duckdb submodule's ACTUAL commit rather than hardcoded, because
# a hardcoded tag silently drifts from the submodule and the only symptom is an
# extension that refuses to load. `git describe` cannot be used: the submodule is
# fetched shallow and carries no tags, which is the very reason
# OVERRIDE_GIT_DESCRIBE is needed in the first place.
#
# Add a line here when bumping the submodule. `make check-pin` fails loudly if the
# checked-out commit is not in this table, so an unrecorded bump cannot slip
# through as a mystery load error.
DUCKDB_KNOWN_VERSIONS := \
	d8cdaa33fda8df955cc76ef58a280f68f4cd43fa=v1.5.5 \
	08e34c447bae34eaee3723cac61f2878b6bdf787=v1.5.4 \
	f31be57c1845a8895169fd58142040be26d433cf=v1.4.5

DUCKDB_SUBMODULE_SHA = $(shell git -C duckdb rev-parse HEAD 2>/dev/null)
DUCKDB_DERIVED_TAG = $(strip $(patsubst $(DUCKDB_SUBMODULE_SHA)=%,%,\
	$(filter $(DUCKDB_SUBMODULE_SHA)=%,$(DUCKDB_KNOWN_VERSIONS))))

# Overridable, e.g. `make release DUCKDB_VERSION_TAG=v1.4.5 DUCKDB_SRCDIR=...`.
DUCKDB_VERSION_TAG ?= $(if $(DUCKDB_DERIVED_TAG),$(DUCKDB_DERIVED_TAG),v1.5.5)
OVERRIDE_GIT_DESCRIBE ?= $(DUCKDB_VERSION_TAG)

# What the DuckDB community-extensions pipeline actually builds against. Recorded
# separately from DUCKDB_VERSION_TAG on purpose: their CI checks DuckDB out itself
# (`cd duckdb && git checkout <version>`), so our submodule pin does NOT decide
# what they build — it only decides what a LOCAL build produces. These are the
# versions that must compile; `make check-api-compat` is what proves it.
COMMUNITY_DUCKDB_VERSIONS := v1.5.5 v1.4.5

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
# Drop a stale CMake cache when the DuckDB source tree changes.
#
# Switching versions (e.g. `make release DUCKDB_SRCDIR=/tmp/duckdb-1.4.5/`) otherwise
# fails with:
#   CMake Error: The source ".../duckdb/CMakeLists.txt" does not match the source
#   "/tmp/duckdb-1.4.5/CMakeLists.txt" used to generate cache.
# which is an unhelpful wall to hit when testing LTS compatibility. Detect the
# mismatch and re-configure automatically instead of requiring `rm -rf build/`.
# Recursively expanded (=, not :=) on purpose: DUCKDB_SRCDIR is set by the
# extension-ci-tools makefile, which is included BELOW this point. With := it
# would evaluate to empty here and the guard would clear the cache on every build.
DUCKDB_SRC_ABS = $(abspath $(patsubst "%",%,$(DUCKDB_SRCDIR)))
define _duck_vtk_guard_cache
	@for d in build/release build/debug build/relassert build/reldebug; do 		if [ -f "$$d/CMakeCache.txt" ]; then 			cached=$$(grep -m1 '^CMAKE_HOME_DIRECTORY:INTERNAL=' "$$d/CMakeCache.txt" | cut -d= -f2-); 			if [ -n "$$cached" ] && [ "$$cached" != "$(DUCKDB_SRC_ABS)" ]; then 				echo "duck_vtk: DuckDB source changed ($$cached -> $(DUCKDB_SRC_ABS)); clearing $$d"; 				rm -rf "$$d"; 			fi; 		fi; 	done
endef

# Bootstrap the build dependencies if this is a non-recursive clone.
#
# GNU make, when an `include`d file is missing, looks for a rule that can create
# it, runs that rule, and then re-executes itself. So declaring a rule for the
# included makefile makes `make release` work on a plain `git clone` with no
# `--recursive` and no separate setup step. See scripts/bootstrap_deps.sh for why
# this cannot be a CMake FetchContent.
extension-ci-tools/makefiles/duckdb_extension.Makefile duckdb/CMakeLists.txt:
	@./scripts/bootstrap_deps.sh

include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Run the guard before any configure-and-build target.
release debug relassert reldebug: | duckdb/CMakeLists.txt guard-duckdb-src
guard-duckdb-src:
	$(_duck_vtk_guard_cache)
.PHONY: guard-duckdb-src

# ---------------------------------------------------------------------------
# duck_vtk-specific convenience targets
# ---------------------------------------------------------------------------

EXT_RELEASE_PATH := build/release/extension/$(EXT_NAME)/$(EXT_NAME).duckdb_extension
EXT_DEBUG_PATH   := build/debug/extension/$(EXT_NAME)/$(EXT_NAME).duckdb_extension

.PHONY: setup data smoke oracle invariants check phase0 print-vtk check-pin

## Alias for `configure`, kept because `make setup` is a common reflex.
## There is deliberately only ONE setup implementation (scripts/configure.sh);
## the previous scripts/configure.sh was a second, slowly diverging copy.
setup: configure

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

## Elementwise diff of every corpus file against Python VTK (the real oracle).
## Prefers uvx (nothing installed system-wide, globally cached) and falls back to
## a .venv. Keep this in step with what scripts/configure.sh reports.
ORACLE_ARGS = scripts/validate_against_vtk.py \
	--data test/data \
	--duckdb ./build/release/duckdb \
	--ext $(EXT_RELEASE_PATH)

oracle: release
	@if command -v uvx >/dev/null 2>&1; then \
		echo "using uvx"; uvx --with vtk --with numpy python $(ORACLE_ARGS); \
	elif [ -x .venv/bin/python ]; then \
		echo "using .venv"; .venv/bin/python $(ORACLE_ARGS); \
	else \
		echo "FATAL: no uvx and no .venv — run 'make configure'" >&2; exit 2; \
	fi

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

## Verify the DuckDB 1.4/1.5 version shim against one or more source trees.
## Seconds instead of the ~30 minutes a full build per version would take, and it
## also proves each branch is REJECTED by the other version — without which the
## shim could be silently redundant.
##   make check-api-compat EXTRA_DUCKDB_SRC=/path/to/duckdb-1.4.5
check-api-compat:
	./scripts/check_api_compat.sh ./duckdb $(EXTRA_DUCKDB_SRC)

## Verify DUCKDB_VERSION_TAG matches the duckdb submodule pin, and that the
## installed CLI is the same build. A drift here produces an extension that
## silently refuses to load, so it is worth an explicit check.
check-pin:
	@sub="$(DUCKDB_SUBMODULE_SHA)"; \
	if [ -z "$$sub" ]; then \
	  echo "duckdb submodule not present; run 'make configure' or './scripts/bootstrap_deps.sh'"; exit 1; \
	fi; \
	echo "duckdb submodule    : $$sub"; \
	if [ -z "$(DUCKDB_DERIVED_TAG)" ]; then \
	  echo "UNRECORDED PIN: that commit is not in DUCKDB_KNOWN_VERSIONS in the Makefile."; \
	  echo "  The build would stamp the fallback version into the extension's metadata"; \
	  echo "  footer, and LOAD would fail with a confusing version-mismatch error."; \
	  echo "  Add '<sha>=<vX.Y.Z>' to DUCKDB_KNOWN_VERSIONS."; \
	  exit 1; \
	fi; \
	echo "derived version tag : $(DUCKDB_VERSION_TAG)"; \
	echo "community targets   : $(COMMUNITY_DUCKDB_VERSIONS)"; \
	cli=$$(duckdb --version 2>/dev/null || echo missing); \
	echo "system duckdb CLI   : $$cli"; \
	case "$$cli" in \
	  *"$(DUCKDB_VERSION_TAG) "*) echo "OK: the installed CLI can load a local build" ;; \
	  missing) echo "note: no duckdb on PATH; cannot cross-check the system load" ;; \
	  *) echo "note: the installed CLI is a different version from this build, so it"; \
	     echo "      cannot load the artefact. Not an error - see smoke.sh step 4." ;; \
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

.PHONY: configure install uninstall all-checks check-api-compat

## Smoke test through the DuckDB Python client (needs `make install` first).
## Uses an ad-hoc uvx environment, so nothing is installed system-wide.
python-smoke: install
	uvx --with duckdb==$(patsubst v%,%,$(DUCKDB_VERSION_TAG)) python scripts/python_smoke.py

.PHONY: python-smoke

# ---------------------------------------------------------------------------
# Cold-boot verification in Docker
# ---------------------------------------------------------------------------
# Reproducible, isolated build with none of this machine's state: no preinstalled
# VTK, no warm ccache, no checked-out submodules, and the repo CLONED rather than
# copied so an uncommitted file cannot make the build look healthy.

DOCKER_IMAGE_CI ?= duck-vtk-ci
DOCKER_CCACHE   ?= duck-vtk-ccache

## Build the CI container image.
ci-image:
	docker build -f docker/Dockerfile.ci -t $(DOCKER_IMAGE_CI) \
		--build-arg DUCKDB_VERSION_TAG=$(DUCKDB_VERSION_TAG) .

## Cold-boot build + full test suite in Docker, building minimal VTK from source.
## This is the check to run before submitting to community-extensions.
# When the local submodules are populated they are mounted as read-only git mirrors,
# so the container clones the pinned commit locally instead of pulling ~500 MB from
# GitHub every run. Set CI_VERIFY_NO_MIRROR=1 to force the real network fetch (which
# is what CI does, since a GitHub runner has the bandwidth for it).
CI_MIRROR_ARGS = $(if $(CI_VERIFY_NO_MIRROR),,\
	$(if $(wildcard duckdb/CMakeLists.txt),-v $(PROJ_DIR)duckdb:/mirror/duckdb:ro -e DUCKDB_GIT_MIRROR=/mirror/duckdb,) \
	$(if $(wildcard extension-ci-tools/makefiles/duckdb_extension.Makefile),-v $(PROJ_DIR)extension-ci-tools:/mirror/citools:ro -e CITOOLS_GIT_MIRROR=/mirror/citools,))

ci-verify: ci-image
	docker volume create $(DOCKER_CCACHE) >/dev/null
	docker run --rm \
		-v $(PROJ_DIR):/src:ro \
		-v $(DOCKER_CCACHE):/ccache \
		-e DUCKDB_VERSION_TAG=$(DUCKDB_VERSION_TAG) \
		-e DUCKDB_SHA=$(DUCKDB_SUBMODULE_SHA) \
		-e VTK_MODE=source \
		$(CI_MIRROR_ARGS) \
		$(DOCKER_IMAGE_CI)

## Same, but against the distro's packaged VTK. Proves we are not secretly tied to
## VTK 9.6 — Ubuntu's libvtk9-dev is an older 9.x.
ci-verify-apt:
	docker build -f docker/Dockerfile.ci -t $(DOCKER_IMAGE_CI)-apt \
		--build-arg VTK_MODE=apt --build-arg DUCKDB_VERSION_TAG=$(DUCKDB_VERSION_TAG) .
	docker volume create $(DOCKER_CCACHE) >/dev/null
	docker run --rm \
		-v $(PROJ_DIR):/src:ro \
		-v $(DOCKER_CCACHE):/ccache \
		-e DUCKDB_VERSION_TAG=$(DUCKDB_VERSION_TAG) \
		-e VTK_MODE=apt \
		$(DOCKER_IMAGE_CI)-apt

## Build through vcpkg exactly as community-extensions does, using our vtk-minimal
## overlay port. THE most important pre-submission check: it is the only thing that
## exercises the vcpkg plumbing around the port. Slow (clones vcpkg, builds VTK).
ci-verify-vcpkg:
	./scripts/verify_vcpkg_port.sh $(VCPKG_COMMIT)

## Drop into a shell in the CI container to debug a failure.
ci-shell: ci-image
	docker run --rm -it --entrypoint /bin/bash \
		-v $(PROJ_DIR):/src:ro -v $(DOCKER_CCACHE):/ccache $(DOCKER_IMAGE_CI)

.PHONY: ci-image ci-verify ci-verify-apt ci-verify-vcpkg ci-shell

## Pre-submission paperwork gate for DuckDB community extensions. Runs offline in
## about a second; does not build. Pair with `make ci-verify` and `make check`.
submit-check:
	./scripts/submit_check.sh

.PHONY: submit-check

## Re-run the SQL suite and invariants with every read routed through the
## in-memory parse path used for remote files, proving it produces identical
## results to reading a local path directly.
test-memory-reads: release
	@echo "=== sqllogictest via the in-memory parse path ==="
	DUCK_VTK_FORCE_MEMORY_READ=1 ./build/release/test/unittest --test-dir . "[sql]"
	@echo "=== invariants via the in-memory parse path ==="
	DUCK_VTK_FORCE_MEMORY_READ=1 ./scripts/run_invariants.sh

.PHONY: test-memory-reads
