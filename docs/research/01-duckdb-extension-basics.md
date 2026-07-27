# DuckDB v1.5.4 Out-of-Tree C++ Extension Reference

> **VERSION SCOPE — read before trusting a line number.**
>
> This document was researched against DuckDB **v1.5.4** (`08e34c447b`), which was
> the pin at the time. The project now targets **v1.5.5** on the default line and
> **v1.4.5** on the LTS line; `git ls-tree HEAD duckdb` is the live pin.
>
> It is kept at v1.5.4 deliberately rather than rewritten. Every fact here was read
> from real headers at that tag, and the API facts have held: the `ExtensionLoader` /
> `DUCKDB_CPP_EXTENSION_ENTRY` entrypoint, the removal of `ExtensionUtil`, and the
> `LookupSchema`/`LookupEntry` pure virtuals are all unchanged in v1.5.5. Rewriting
> the citations to a new tag would cost the one thing that makes this useful — that
> every claim was verified against a specific, checkable revision.
>
> What that means in practice: trust the **API shapes**, re-check **`file:line`
> citations** against the tag you are actually building. The one API that genuinely
> differs between the 1.4 and 1.5 lines is storage-extension registration, and
> `CMakeLists.txt` handles it by probing the header rather than by version string —
> `make check-api-compat` proves both branches compile.


Target: DuckDB **v1.5.4** ("Variegata", commit `08e34c447bae34eaee3723cac61f2878b6bdf787`) — matches the
brew-installed CLI on this machine (`duckdb --version` → `v1.5.4 (Variegata) 08e34c447b`; verified live
with `pragma_version()` below).

Sources used (see bottom of each section for exact paths/lines):

- `github.com/duckdb/extension-template` — cloned shallow (`--depth 1`) to
  `/tmp/.../scratchpad/research-b/extension-template`. HEAD at time of research was commit `cfaf3e2`
  ("Bump to v1.5.4"), i.e. **the template's `main` branch already targets our exact DuckDB version.**
- `github.com/duckdb/duckdb` — NOT fully cloned (repo is large and the sandbox's link was saturated by a
  concurrent Homebrew VTK install, ~870 KB/s cap). Instead, individual files were fetched with
  `curl https://raw.githubusercontent.com/duckdb/duckdb/v1.5.4/<path>` (HTTP 200 verified for every file
  quoted below) into `/tmp/.../scratchpad/raw/`. This is fully equivalent to reading the tagged source
  tree since `raw.githubusercontent.com` serves exact tag contents.
- `github.com/duckdb/extension-ci-tools` — cloned shallow at branch `v1.5-variegata` (the branch the
  template's workflow pins), to `/tmp/.../scratchpad/research-b/extension-ci-tools`.
- `github.com/duckdb/duckdb-web` (the source of duckdb.org) — sparse-cloned to
  `/tmp/.../scratchpad/research-b/duckdb-web`, read `docs/current/**/*.md` directly (the site's
  `/docs/stable/...` URLs are redirects to `/docs/current/...`).
- Live verification on the installed `v1.5.4` CLI (`duckdb`, `duckdb -unsigned`) for `duckdb_extensions()`
  columns, `PRAGMA platform`, `current_setting('allow_unsigned_extensions')`, and error text.
- Context7 MCP was attempted but returned "Invalid API key" in this environment — not used.

All local scratch paths referenced below are under
`/tmp/claude-1000/-home-ali-Documents-repos-duck-vtk/7db86a63-923b-4e59-836b-d77f492103a4/scratchpad/`
(shortened to `.../scratchpad/` in citations). GitHub paths are given relative to the repo root, at tag
`v1.5.4` for `duckdb/duckdb`, and current `main`/`v1.5-variegata` for the template/ci-tools repos, unless
noted otherwise.

## Table of Contents

1. [Repo layout & build system](#1-repo-layout--build-system)
2. [The extension entrypoint C++ API for v1.5.4](#2-the-extension-entrypoint-c-api-for-v154)
3. [Registering things (ExtensionLoader API)](#3-registering-things-extensionloader-api)
4. [Loading a locally built extension](#4-loading-a-locally-built-extension)
5. [Testing](#5-testing)
6. [Third-party dependency linking](#6-third-party-dependency-linking)
7. [Common pitfalls](#7-common-pitfalls)
8. [Uncertainties / verify at build time](#8-uncertainties--verify-at-build-time)

---

## 1. Repo layout & build system

### 1.1 Directory layout

`extension-template` (root of a new out-of-tree extension repo):

```
.
├── CMakeLists.txt
├── Makefile
├── extension_config.cmake
├── vcpkg.json
├── LICENSE
├── .gitignore
├── .gitmodules
├── docs/
│   ├── README.md            # this becomes README.md after bootstrap
│   ├── NEXT_README.md        # template for the NEW repo's README.md
│   └── UPDATING.md
├── scripts/
│   ├── bootstrap-template.py # one-shot renamer: waddle -> your_extension_name
│   └── extension-upload.sh
├── src/
│   ├── waddle_extension.cpp
│   └── include/
│       └── waddle_extension.hpp
├── test/
│   ├── README.md
│   └── sql/
│       └── waddle.test
├── .github/workflows/
│   ├── ExtensionTemplate.yml       # CI for testing the template itself only
│   └── MainDistributionPipeline.yml
├── duckdb/                # git submodule -> github.com/duckdb/duckdb
└── extension-ci-tools/    # git submodule -> github.com/duckdb/extension-ci-tools
```

`waddle` is the template's placeholder extension name (renamed via `scripts/bootstrap-template.py
<name>`, which also renames `src/waddle_extension.cpp`/`.hpp`, `test/sql/waddle.test`, and deletes
itself + `ExtensionTemplate.yml`).

Source: directory listing of the clone, `.../scratchpad/research-b/extension-template/` (all files
enumerated via `find . -not -path '*/.git/*' -type f`).

### 1.2 Submodule pins (verified exact match to installed DuckDB)

`.gitmodules` (`.../scratchpad/research-b/extension-template/.gitmodules`):

```ini
[submodule "duckdb"]
	path = duckdb
	url = https://github.com/duckdb/duckdb
	branch = main
[submodule "extension-ci-tools"]
	path = extension-ci-tools
	url = https://github.com/duckdb/extension-ci-tools
	branch = main
```

`branch = main` is misleading — the actual **pinned commit** (from `git ls-tree HEAD duckdb
extension-ci-tools` inside the template clone) is what matters:

```
160000 commit 08e34c447bae34eaee3723cac61f2878b6bdf787	duckdb
160000 commit b777c70d30942cca5bef62d6d4fa23a13362f398	extension-ci-tools
```

`08e34c447bae34eaee3723cac61f2878b6bdf787` is **exactly** the `v1.5.4` tag
(`git ls-remote --tags https://github.com/duckdb/duckdb | grep 1.5.4` →
`08e34c447bae34eaee3723cac61f2878b6bdf787	refs/tags/v1.5.4`), and exactly the commit reported by the
installed CLI (`v1.5.4 (Variegata) 08e34c447b`). The template's git log also confirms this explicitly:
`git log -1` on the template clone shows HEAD = `cfaf3e2 "Merge pull request #172 from
carlopi/bump_v154 — Bump to v1.5.4"`.

**Conclusion: cloning `extension-template` fresh today and running `git submodule update --init
--recursive` (no manual repinning needed) gets you a `duckdb/` submodule identical to the installed
CLI's source.** `extension-ci-tools` is pinned to the `v1.5.4`-targeting branch `v1.5-variegata` (see
`extension-ci-tools/config/distribution_matrix.json`'s sibling `README.md` versioning table below).

### 1.3 Root `CMakeLists.txt` (full content)

`.../scratchpad/research-b/extension-template/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME waddle)

# DuckDB's extension distribution supports vcpkg. As such, dependencies can be added in ./vcpkg.json and then
# used in cmake with find_package. Feel free to remove or replace with other dependencies.
# Note that it should also be removed from vcpkg.json to prevent needlessly installing it..
find_package(OpenSSL REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

set(EXTENSION_SOURCES src/waddle_extension.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Link OpenSSL in both the static library as the loadable extension
target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
```

Key mechanics:

- `TARGET_NAME` = the extension's short name (`waddle`). `EXTENSION_NAME` =
  `${TARGET_NAME}_extension` (the **static** library target). `LOADABLE_EXTENSION_NAME` =
  `${TARGET_NAME}_loadable_extension` (the **shared/loadable** `.duckdb_extension` target). Both names
  are conventions defined by this file itself, not by DuckDB core — but `build_static_extension` and
  `build_loadable_extension` (called below) are **DuckDB-core-provided CMake functions**, not defined
  anywhere in this repo. They only exist because this `CMakeLists.txt` is never configured standalone —
  it is pulled in via `add_subdirectory()` from inside `duckdb`'s own root `CMakeLists.txt`, triggered by
  `duckdb_extension_load(waddle SOURCE_DIR ...)` in `extension_config.cmake` (§1.5). This is **the**
  central out-of-tree mechanism: your extension's `CMakeLists.txt` is a *subdirectory* of the DuckDB
  project, not an independent CMake project with `find_package(DuckDB)`.
- `build_static_extension(NAME FILES...)` and `build_loadable_extension(NAME PARAMETERS FILES...)` are
  defined in DuckDB core at `extension/extension_build_tools.cmake` (fetched verbatim from
  `raw.githubusercontent.com/duckdb/duckdb/v1.5.4/extension/extension_build_tools.cmake`, saved to
  `.../scratchpad/raw/extension_build_tools.cmake`) — see §6 for the full function bodies (visibility,
  linkage, the `.duckdb_extension` suffix, metadata-footer injection).
- `find_package(OpenSSL REQUIRED)` is a plain CMake `find_package` call — nothing vcpkg-specific about
  it syntactically. vcpkg only participates by being passed as `-DCMAKE_TOOLCHAIN_FILE=.../vcpkg.cmake`
  (via `VCPKG_TOOLCHAIN_PATH`, see §1.4/§6), which makes `find_package` resolve against vcpkg-installed
  packages instead of system ones. **This means substituting `find_package(VTK REQUIRED)` (found via a
  system/Homebrew VTK) is a drop-in replacement** — no vcpkg machinery is required to use it (§6, §7).

### 1.4 Root `Makefile` (full content)

`.../scratchpad/research-b/extension-template/Makefile`:

```makefile
PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=waddle
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile
```

Everything (targets `debug`, `release`, `test`, etc.) comes from the included
`extension-ci-tools/makefiles/duckdb_extension.Makefile` — full content and explanation in §1.6.

### 1.5 `extension_config.cmake` (full content)

`.../scratchpad/research-b/extension-template/extension_config.cmake`:

```cmake
# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(waddle
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)

# Any extra extensions that should be built
# e.g.: duckdb_extension_load(json)
```

`duckdb_extension_load(NAME SOURCE_DIR <path> [DONT_LINK] [DONT_BUILD] [INCLUDE_DIR <path>] [GIT_URL
<url> GIT_TAG <ref>] ...)` is defined in DuckDB core, `extension/extension_build_tools.cmake:414-497`
(function `duckdb_extension_load`). With `SOURCE_DIR` given (our case), it takes the
"local extension, custom path" branch (lines 445-470): it auto-derives `INCLUDE_DIR` =
`<SOURCE_DIR>/src/include` and `TEST_DIR` = `<SOURCE_DIR>/test/sql` if not given, computes the extension
version via `git describe` inside `SOURCE_DIR` (`duckdb_extension_generate_version`,
`extension_build_tools.cmake:371-412`), and calls `register_extension(...)`
(`extension_build_tools.cmake:266-319`), which:
- appends the extension name to the global `DUCKDB_EXTENSION_NAMES` list,
- sets `DUCKDB_EXTENSION_<NAME>_SHOULD_LINK` (whether it's statically linked into the `duckdb`/`unittest`
  binaries — true unless `DONT_LINK`), `..._SHOULD_BUILD`, `..._PATH`, `..._INCLUDE_PATH`,
  `..._TEST_PATH`, `..._EXT_VERSION`.

Later, `extension_build_tools.cmake:582-613` iterates `DUCKDB_EXTENSION_NAMES` and does
`add_subdirectory(${DUCKDB_EXTENSION_<NAME>_PATH} extension/<name>)` — this is the literal moment your
extension's root `CMakeLists.txt` gets configured as a DuckDB subdirectory.

### 1.6 `extension-ci-tools/makefiles/duckdb_extension.Makefile` (full content)

Fetched from `raw.githubusercontent.com/duckdb/duckdb/...` — no wait, this file lives in the
**extension-ci-tools** repo, cloned to
`.../scratchpad/research-b/extension-ci-tools/makefiles/duckdb_extension.Makefile` (branch
`v1.5-variegata`, the branch pinned by the template's `MainDistributionPipeline.yml`, see §1.8). Full
content (310 lines):

```makefile
# Reusable makefile for building out-of-tree extension with the DuckDB C++ based extension template
#
# Inputs
#   EXT_NAME          : Upper case string describing the name of the out-of-tree extension
#   EXT_CONFIG        : Path to the extension config file specifying how to build the extension
#   EXT_FLAGS         : Extra CMake flags to pass to the build
#   EXT_RELEASE_FLAGS : Extra CMake flags to pass to the release build
#   EXT_DEBUG_FLAGS   : Extra CMake flags to pass to the debug build
#   SKIP_TESTS        : Replaces all test targets with a NOP step
#
# 	BUILD_EXTENSION_TEST_DEPS   : Can be set to either `default`, `full`, or `none`. Toggles which extension dependencies are built
#	DEFAULT_TEST_EXTENSION_DEPS : `;`-separated list of extensions that are built in `default` and `full` mode
#	FULL_TEST_EXTENSION_DEPS    : `;`-separated list of extensions that are built in `full` mode

.PHONY: all clean clean-python clangd format debug release pull update wasm_mvp wasm_eh wasm_threads test test_release test_debug test_reldebug test_release_internal test_debug_internal test_reldebug_internal set_duckdb_version set_duckdb_tag  output_distribution_matrix

all: release

TEST_PATH="/test/unittest"
DUCKDB_PATH="/duckdb"

DUCKDB_SRCDIR ?= "./duckdb/"

TESTS_BASE_DIRECTORY = "test/"

ifeq (${SUBSET_EXTENSIONS_TESTS},complete)
	TESTS_BASE_DIRECTORY=""
endif

#### Extension test dependency code
ifeq (${BUILD_EXTENSION_TEST_DEPS},)
	BUILD_EXTENSION_TEST_DEPS:=default
endif

ifeq (${BUILD_EXTENSION_TEST_DEPS},default)
	ifneq (${DEFAULT_TEST_EXTENSION_DEPS},)
		CORE_EXTENSIONS:=${CORE_EXTENSIONS};${DEFAULT_TEST_EXTENSION_DEPS}
	endif
else ifeq (${BUILD_EXTENSION_TEST_DEPS},full)
	ifneq (${DEFAULT_TEST_EXTENSION_DEPS},)
		CORE_EXTENSIONS:=${CORE_EXTENSIONS};${DEFAULT_TEST_EXTENSION_DEPS}
	endif
	ifneq (${FULL_TEST_EXTENSION_DEPS},)
		CORE_EXTENSIONS:=${CORE_EXTENSIONS};${FULL_TEST_EXTENSION_DEPS}
	endif
else ifneq (${BUILD_EXTENSION_TEST_DEPS}, none)
$(error Unknown option passed to BUILD_EXTENSION_TEST_DEPS variable: ${BUILD_EXTENSION_TEST_DEPS})
endif

#### Core extensions, allows easily building one of the core extensions
ifneq ($(CORE_EXTENSIONS),)
	CORE_EXTENSION_VAR:=-DCORE_EXTENSIONS="$(CORE_EXTENSIONS)"
endif

#### OSX config
OSX_BUILD_FLAG=
ifneq (${OSX_BUILD_ARCH}, "")
	OSX_BUILD_FLAG=-DOSX_BUILD_ARCH=${OSX_BUILD_ARCH}
endif

ifeq ("${OSX_BUILD_ARCH}", "arm64")
	RUST_FLAGS=-DRust_CARGO_TARGET=aarch64-apple-darwin
else ifeq ("${OSX_BUILD_ARCH}", "x86_64")
	RUST_FLAGS=-DRust_CARGO_TARGET=x86_64-apple-darwin
endif

#### Windows config
ifeq ($(DUCKDB_PLATFORM),windows_amd64_mingw)
	RUST_FLAGS=-DRust_CARGO_TARGET=x86_64-pc-windows-gnu
else ifeq ($(DUCKDB_PLATFORM),windows_amd64_rtools)
	RUST_FLAGS=-DRust_CARGO_TARGET=x86_64-pc-windows-gnu
endif

#### VCPKG config
EXTENSION_CONFIG_STEP ?=
EXTENSION_CONFIG_STEP_WASM ?=

# Set the toolchain
VCPKG_TOOLCHAIN_PATH?=
ifneq ("${VCPKG_TOOLCHAIN_PATH}", "")
	TOOLCHAIN_FLAGS:=${TOOLCHAIN_FLAGS} -DVCPKG_BUILD=1 -DCMAKE_TOOLCHAIN_FILE='${VCPKG_TOOLCHAIN_PATH}'
endif

# Add the extension config step which ensures the vcpkg dependencies of all extensions get merged properly
ifeq (${USE_MERGED_VCPKG_MANIFEST}, 1)
	EXTENSION_CONFIG_STEP= build/extension_configuration/vcpkg.json
	EXTENSION_CONFIG_STEP_WASM=extension_configuration_wasm
	VCPKG_MANIFEST_FLAGS:=-DVCPKG_MANIFEST_DIR='${PROJ_DIR}build/extension_configuration'
else ifneq ("${VCPKG_TOOLCHAIN_PATH}", "")
	VCPKG_MANIFEST_FLAGS:=-DVCPKG_MANIFEST_DIR='${PROJ_DIR}'
endif

ifneq ("${VCPKG_TARGET_TRIPLET}", "")
	TOOLCHAIN_FLAGS:=${TOOLCHAIN_FLAGS} -DVCPKG_TARGET_TRIPLET='${VCPKG_TARGET_TRIPLET}'
endif
ifneq ("${VCPKG_HOST_TRIPLET}", "")
	TOOLCHAIN_FLAGS:=${TOOLCHAIN_FLAGS} -DVCPKG_HOST_TRIPLET='${VCPKG_HOST_TRIPLET}'
endif

#### Enable Ninja as generator
ifeq ($(GEN),ninja)
	GENERATOR=-G "Ninja" -DFORCE_COLORED_OUTPUT=1
endif

### Extension configs
ifneq ("${EXTRA_EXTENSION_CONFIGS}", "")
	EXTENSION_CONFIGS:=${EXTRA_EXTENSION_CONFIGS};${EXT_CONFIG}
else
	EXTENSION_CONFIGS:=${EXT_CONFIG}
endif
EXTENSION_CONFIG_FLAG=-DDUCKDB_EXTENSION_CONFIGS='${EXTENSION_CONFIGS}'

#### Configuration for this extension

# This setting controls how DuckDB is linked into the loadable extensions.
# Setting this to 0 will speed up linking and reduce binary size, but may render your extension binaries unloadable on some platforms.
EXTENSION_STATIC_BUILD ?= 1
ENABLE_EXTENSION_AUTOLOADING ?= 0
ENABLE_EXTENSION_AUTOINSTALL ?= 0

BUILD_FLAGS=-DEXTENSION_STATIC_BUILD=$(EXTENSION_STATIC_BUILD) $(EXTENSION_FLAGS) $(EXTENSION_CONFIG_FLAG) ${EXT_FLAGS} $(CORE_EXTENSION_VAR) $(OSX_BUILD_FLAG) $(RUST_FLAGS) $(TOOLCHAIN_FLAGS) -DDUCKDB_EXPLICIT_PLATFORM='${DUCKDB_PLATFORM}' -DCUSTOM_LINKER=${CUSTOM_LINKER} -DOVERRIDE_GIT_DESCRIBE="${OVERRIDE_GIT_DESCRIBE}" -DUNITTEST_ROOT_DIRECTORY="$(PROJ_DIR)" -DBENCHMARK_ROOT_DIRECTORY="$(PROJ_DIR)" -DENABLE_UNITTEST_CPP_TESTS=FALSE -DENABLE_EXTENSION_AUTOLOADING=$(ENABLE_EXTENSION_AUTOLOADING) -DENABLE_EXTENSION_AUTOINSTALL=$(ENABLE_EXTENSION_AUTOINSTALL)

#### Extra Flags
ifeq (${CRASH_ON_ASSERT}, 1)
	BUILD_FLAGS += -DCRASH_ON_ASSERT=1
endif
ifeq ($(BUILD_BENCHMARK), 1)
	BUILD_FLAGS += -DBUILD_BENCHMARKS=1
endif
ifeq (${TREAT_WARNINGS_AS_ERRORS}, 1)
	BUILD_FLAGS += -DTREAT_WARNINGS_AS_ERRORS=1
endif
ifeq (${DISABLE_SANITIZER}, 1)
	BUILD_FLAGS += -DENABLE_SANITIZER=FALSE -DENABLE_UBSAN=0
endif
ifeq (${DISABLE_UBSAN}, 1)
	BUILD_FLAGS += -DENABLE_UBSAN=0
endif
ifeq (${THREADSAN}, 1)
	BUILD_FLAGS += -DENABLE_THREAD_SANITIZER=1
endif
ifneq (${BUILD_EXTENSION_TEST_DEPS}, )
	BUILD_FLAGS += -DBUILD_EXTENSION_TEST_DEPS=${BUILD_EXTENSION_TEST_DEPS}
endif

#### Clang Tidy
ifneq ($(TIDY_THREADS),)
	TIDY_THREAD_PARAMETER := -j ${TIDY_THREADS}
endif
ifneq ($(TIDY_BINARY),)
	TIDY_BINARY_PARAMETER := -clang-tidy-binary ${TIDY_BINARY}
endif
ifneq ($(TIDY_CHECKS),)
        TIDY_PERFORM_CHECKS := '-checks=${TIDY_CHECKS}'
endif

clangd: ${EXTENSION_CONFIG_STEP}
	cmake $(GENERATOR) $(BUILD_FLAGS) $(EXT_DEBUG_FLAGS) $(VCPKG_MANIFEST_FLAGS) -DCMAKE_BUILD_TYPE=Debug -S $(DUCKDB_SRCDIR) -B .cache/clangd/debug

debug: ${EXTENSION_CONFIG_STEP}
	mkdir -p build/debug
	cmake $(GENERATOR) $(BUILD_FLAGS) $(EXT_DEBUG_FLAGS) $(VCPKG_MANIFEST_FLAGS) -DCMAKE_BUILD_TYPE=Debug -S $(DUCKDB_SRCDIR) -B build/debug
	cmake --build build/debug --config Debug

release: ${EXTENSION_CONFIG_STEP}
	mkdir -p build/release
	cmake $(GENERATOR) $(BUILD_FLAGS) $(EXT_RELEASE_FLAGS) $(VCPKG_MANIFEST_FLAGS) -DCMAKE_BUILD_TYPE=Release -S $(DUCKDB_SRCDIR) -B build/release
	cmake --build build/release --config Release

relassert: ${EXTENSION_CONFIG_STEP}
	mkdir -p build/relassert
	cmake $(GENERATOR) $(BUILD_FLAGS) $(EXT_RELEASE_FLAGS) $(VCPKG_MANIFEST_FLAGS) -DCMAKE_BUILD_TYPE=RelWithDebInfo -S $(DUCKDB_SRCDIR) -DFORCE_ASSERT=1 -B build/relassert
	cmake --build build/relassert --config RelWithDebInfo

reldebug: ${EXTENSION_CONFIG_STEP}
	mkdir -p build/reldebug
	cmake $(GENERATOR) $(BUILD_FLAGS) $(EXT_RELEASE_FLAGS) $(VCPKG_MANIFEST_FLAGS) -DCMAKE_BUILD_TYPE=RelWithDebInfo -S $(DUCKDB_SRCDIR) -B build/reldebug
	cmake --build build/reldebug

# Main tests
test: test_release

TEST_RELEASE_TARGET=test_release_internal
TEST_DEBUG_TARGET=test_debug_internal
TEST_RELDEBUG_TARGET=test_reldebug_internal

# Disable testing outside docker: the unittester is currently dynamically linked by default
ifeq ($(LINUX_CI_IN_DOCKER),0)
	SKIP_TESTS=1
endif

ifeq ($(SKIP_TESTS),1)
	TEST_RELEASE_TARGET=tests_skipped
	TEST_DEBUG_TARGET=tests_skipped
	TEST_RELDEBUG_TARGET=tests_skipped
endif

test_release: $(TEST_RELEASE_TARGET)
test_debug: $(TEST_DEBUG_TARGET)
test_reldebug: $(TEST_RELDEBUG_TARGET)

test_release_internal:
	./build/release/$(TEST_PATH) "$(TESTS_BASE_DIRECTORY)*"
test_debug_internal:
	./build/debug/$(TEST_PATH) "$(TESTS_BASE_DIRECTORY)*"
test_reldebug_internal:
	./build/reldebug/$(TEST_PATH) "$(TESTS_BASE_DIRECTORY)*"

tests_skipped:
	@echo "Tests are skipped in this run..."

# ... (WASM targets, extension_configuration targets for merged vcpkg manifests, format/tidy targets,
#      submodule helpers `update`/`pull`, `clean`, `set_duckdb_repository`, `set_duckdb_version`,
#      `set_duckdb_tag`, `output_distribution_matrix` — see full file for these; omitted here as
#      not relevant to a non-WASM, non-multi-extension-vcpkg build)
```

(Full, unabridged file is at `.../scratchpad/research-b/extension-ci-tools/makefiles/duckdb_extension.Makefile`,
310 lines — the elided portion above is WASM (`wasm_mvp`/`wasm_eh`/`wasm_threads` targets),
`format`/`tidy-check`, and submodule/version helper targets not needed for a native Linux build.)

**Note on `TEST_PATH`:** the literal string is `TEST_PATH="/test/unittest"` (line 19) — note the
leading `/`. Combined with `${DUCKDB_SRCDIR}` this is *not* directly concatenated for the test targets;
instead the test targets hard-code `./build/release/$(TEST_PATH)` which — because `TEST_PATH` itself
starts with `/` — expands to `./build/release//test/unittest` (harmless double-slash on POSIX).

**How to inject extra CMake flags (e.g. `-DVTK_DIR=...`):** the Makefile's `BUILD_FLAGS` always
includes `${EXT_FLAGS}` (both debug and release), and additionally `${EXT_RELEASE_FLAGS}` (release-only)
/ `${EXT_DEBUG_FLAGS}` (debug-only) are appended per-build-type (see `debug:`/`release:` targets above).
These are Make variables read from the environment or `make` command line, e.g.:

```sh
EXT_FLAGS="-DVTK_DIR=/opt/homebrew/lib/cmake/vtk-9.3" GEN=ninja make
# or release-only:
EXT_RELEASE_FLAGS="-DVTK_DIR=/opt/homebrew/lib/cmake/vtk-9.3" make release
```

**vcpkg is fully optional.** `VCPKG_TOOLCHAIN_PATH` is only consulted if non-empty (lines 79-82); if
unset, no `-DCMAKE_TOOLCHAIN_FILE` flag is passed at all, and CMake's `find_package(VTK)` in the
extension's `CMakeLists.txt` falls through to normal system/Homebrew CMake package search paths. This is
also explicit in `duckdb/extension/README.md` (§7): *"VCPKG is only required for extensions that want to
rely on it for dependency management."* Deleting `vcpkg.json` from a copy of the template removes the
vcpkg manifest entirely; `duckdb/extension/extension_build_tools.cmake:596-598` only *warns* (does not
error) if a `vcpkg.json` is present but `VCPKG_BUILD` isn't set — the inverse (no `vcpkg.json` at all) is
not warned about and works cleanly, confirmed by the extension-ci-tools Makefile's `VCPKG_TOOLCHAIN_PATH`
gate being the sole determinant of whether the toolchain file flag is added.

### 1.7 Output paths of `make debug` / `make release`

From `duckdb_extension.Makefile:160-168` (§1.6) plus `docs/README.md` in the template
(`.../scratchpad/research-b/extension-template/docs/README.md:61-69`):

```
./build/release/duckdb                                        # CLI with extension statically linked & pre-loaded
./build/release/test/unittest                                 # unittest runner, extension linked in
./build/release/extension/<extension_name>/<extension_name>.duckdb_extension   # loadable binary
```

(`build/debug/...` for `make debug`.) The `extension/<name>/<name>.duckdb_extension` path comes from
`build_loadable_extension_directory`'s third argument `OUTPUT_DIRECTORY` =
`"extension/${NAME}"` — see `extension_build_tools.cmake:226` (`build_loadable_extension` calls
`build_loadable_extension_directory(${NAME} "CPP" "extension/${NAME}" ...)`).

### 1.8 `.github/workflows/*`

`MainDistributionPipeline.yml` (full content,
`.../scratchpad/research-b/extension-template/.github/workflows/MainDistributionPipeline.yml`):

```yaml
#
# This workflow calls the main distribution pipeline from DuckDB to build, test and (optionally) release the extension
#
name: Main Extension Distribution Pipeline
on:
  push:
  pull_request:
  workflow_dispatch:

concurrency:
  group: ${{ github.workflow }}-${{ github.ref }}-${{ github.head_ref || '' }}-${{ github.base_ref || '' }}-${{ github.ref != 'refs/heads/main' && github.sha || '' }}
  cancel-in-progress: true

jobs:
  duckdb-stable-build:
    name: Build extension binaries
    uses: duckdb/extension-ci-tools/.github/workflows/_extension_distribution.yml@v1.5-variegata
    with:
      duckdb_version: v1.5.4
      ci_tools_version: v1.5-variegata
      extension_name: waddle

  code-quality-check:
    name: Code Quality Check
    uses: duckdb/extension-ci-tools/.github/workflows/_extension_code_quality.yml@v1.5-variegata
    with:
      duckdb_version: v1.5.4
      ci_tools_version: v1.5-variegata
      extension_name: waddle
      format_checks: 'format;tidy'
```

This explicitly pins `duckdb_version: v1.5.4` and `ci_tools_version: v1.5-variegata` — reusable workflow
calls into `extension-ci-tools`, which is why the extension repo itself needs almost no CI logic of its
own.

`ExtensionTemplate.yml` (only relevant to testing the template repo itself; deleted by
`bootstrap-template.py`) matrix-builds against `duckdb_version: ['<submodule_version>']` for
linux/macos/windows, runs `python3 scripts/bootstrap-template.py ext_1_a_123b_b11`, then `make` and
`make test`. Not something a downstream extension author needs to keep.

### 1.9 `extension-ci-tools` versioning table

`.../scratchpad/research-b/extension-ci-tools/README.md`:

```
| Extension-ci-tools Branch | DuckDB target version | Actively maintained? |
|---------------------------|-----------------------|----------------------|
| main                      | main                  | yes                  |
| v1.5.4                    | v1.5.4                | yes                  |
| v1.5.3                    | v1.5.3                | no                   |
...
```

So `extension-ci-tools` also has a branch literally named `v1.5.4` (in addition to `v1.5-variegata`,
which the template's workflow actually references) — both target the same DuckDB version; use whichever
the template pins (`v1.5-variegata`) for consistency with the template's own Makefile submodule.

---

## 2. The extension entrypoint C++ API for v1.5.4

### 2.1 Definitive answer: v1.5.4 uses the **new `ExtensionLoader` / `DUCKDB_CPP_EXTENSION_ENTRY`** mechanism, NOT the old `duckdb::DatabaseInstance`/`<name>_init` free-function style

Proof, from the template's actual source (which — per §1.2 — is pinned to our exact DuckDB version),
`.../scratchpad/research-b/extension-template/src/waddle_extension.cpp` (full file, 65 lines):

```cpp
#define DUCKDB_EXTENSION_MAIN

#include "waddle_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

// OpenSSL linked through vcpkg
#include <openssl/opensslv.h>

namespace duckdb {

inline void WaddleScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "...........🦆 " + name.GetString());
	});
}

inline void WaddleOpenSSLVersionScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "Waddle " + name.GetString() + ", my linked OpenSSL version is " +
		                                           OPENSSL_VERSION_TEXT);
	});
}

static void LoadInternal(ExtensionLoader &loader) {
	// Register a scalar function
	auto waddle_scalar_function =
	    ScalarFunction("waddle", {LogicalType::VARCHAR}, LogicalType::VARCHAR, WaddleScalarFun);

	loader.RegisterFunction(waddle_scalar_function);

	// Register another scalar function
	auto waddle_openssl_version_scalar_function = ScalarFunction("waddle_openssl_version", {LogicalType::VARCHAR},
	                                                             LogicalType::VARCHAR, WaddleOpenSSLVersionScalarFun);
	loader.RegisterFunction(waddle_openssl_version_scalar_function);
}

void WaddleExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string WaddleExtension::Name() {
	return "waddle";
}

std::string WaddleExtension::Version() const {
#ifdef EXT_VERSION_WADDLE
	return EXT_VERSION_WADDLE;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(waddle, loader) {
	duckdb::LoadInternal(loader);
}
}
```

and header, `.../scratchpad/research-b/extension-template/src/include/waddle_extension.hpp` (full file):

```cpp
#pragma once

#include "duckdb.hpp"

namespace duckdb {

class WaddleExtension : public Extension {
public:
	void Load(ExtensionLoader &db) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
```

### 2.2 The `DUCKDB_CPP_EXTENSION_ENTRY` macro, verbatim

`src/include/duckdb/main/extension/extension_loader.hpp` at tag `v1.5.4` (fetched via
`raw.githubusercontent.com/duckdb/duckdb/v1.5.4/src/include/duckdb/main/extension/extension_loader.hpp`,
saved to `.../scratchpad/raw/loader.hpp`), lines 115-124:

```cpp
//! Helper macro to define the entrypoint for a C++ extension
//! Usage:
//!
//!		DUCKDB_CPP_EXTENSION_ENTRY(my_extension, loader) {
//!			loader.RegisterFunction(...);
//!		}
//!
#define DUCKDB_CPP_EXTENSION_ENTRY(EXTENSION_NAME, LOADER_NAME)                                                        \
	DUCKDB_EXTENSION_API void EXTENSION_NAME##_duckdb_cpp_init(duckdb::ExtensionLoader &LOADER_NAME)
```

So `DUCKDB_CPP_EXTENSION_ENTRY(waddle, loader) { ... }` expands to a function named
**`waddle_duckdb_cpp_init(duckdb::ExtensionLoader &loader)`**, marked `DUCKDB_EXTENSION_API`. This is
the exact, load-bearing symbol name DuckDB's loader `dlopen`s/`dlsym`s for (the `<name>_duckdb_cpp_init`
convention; confirmed independently by `extension/extension_build_tools.cmake:145` and `:189`, which
build a macOS linker symbol whitelist / an Emscripten `EXPORTED_FUNCTIONS` list respectively both
referencing literally `${NAME}_duckdb_cpp_init`).

`DUCKDB_EXTENSION_API` is defined in `src/include/duckdb/common/winapi.hpp` (fetched verbatim, full file,
`.../scratchpad/raw/src_include_duckdb_common_winapi.hpp`):

```cpp
#ifndef DUCKDB_EXTENSION_API
#ifdef _WIN32
#ifdef DUCKDB_STATIC_BUILD
#define DUCKDB_EXTENSION_API
#else
#define DUCKDB_EXTENSION_API __declspec(dllexport)
#endif
#else
#define DUCKDB_EXTENSION_API __attribute__((visibility("default")))
#endif
#endif
```

On Linux (our target platform), `DUCKDB_EXTENSION_API` = `__attribute__((visibility("default")))` — this
is exactly what must survive even when the rest of the extension is compiled with
`-fvisibility=hidden`/`CXX_VISIBILITY_PRESET hidden` (§6/§7).

### 2.3 The `Extension` base class

`src/include/duckdb/main/extension.hpp` at tag `v1.5.4` (full file,
`.../scratchpad/raw/main_extension.hpp`):

```cpp
#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/winapi.hpp"

namespace duckdb {
class ExtensionLoader;

//! The Extension class is the base class used to define extensions
class Extension {
public:
	DUCKDB_API virtual ~Extension();

	DUCKDB_API virtual void Load(ExtensionLoader &db) = 0;
	DUCKDB_API virtual std::string Name() = 0;
	DUCKDB_API virtual std::string Version() const {
		return "";
	}
	DUCKDB_API static const char *DefaultVersion();
};

enum class ExtensionABIType : uint8_t {
	UNKNOWN = 0,
	//! Uses C++ ABI, version needs to match precisely
	CPP = 1,
	//! Uses C ABI using the duckdb_ext_api_v1 struct, version needs to be equal or higher
	C_STRUCT = 2,
	//! Uses C ABI using the duckdb_ext_api_v1 struct including "unstable" functions, version needs to match precisely
	C_STRUCT_UNSTABLE = 3
};

//! The parsed extension metadata footer
struct ParsedExtensionMetaData {
	static constexpr const idx_t FOOTER_SIZE = 512;
	static constexpr const idx_t SIGNATURE_SIZE = 256;
	static constexpr const char *EXPECTED_MAGIC_VALUE = {
	    "4\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"};

	string magic_value;

	ExtensionABIType abi_type;

	string platform;
	// (For ExtensionABIType::CPP or ExtensionABIType::C_STRUCT_UNSTABLE) the DuckDB version this extension is compiled
	// for
	string duckdb_version;
	// (only for ExtensionABIType::C_STRUCT) the CAPI version of the C_STRUCT (Currently interpreted as the minimum
	// DuckDB version)
	string duckdb_capi_version;
	string extension_version;
	string signature;
	string extension_abi_metadata;

	bool AppearsValid() {
		return magic_value == EXPECTED_MAGIC_VALUE;
	}

	// Returns an error string describing which parts of the metadata are mismatcheds
	string GetInvalidMetadataError();
};

struct VersioningUtils {
	//! Note: only supports format v{major}.{minor}.{patch}
	static bool ParseSemver(string &semver, idx_t &major_out, idx_t &minor_out, idx_t &patch_out);

	//! Note: only supports format v{major}.{minor}.{patch}
	static bool IsSupportedCAPIVersion(string &capi_version_string);
	static bool IsSupportedCAPIVersion(idx_t major, idx_t minor, idx_t patch);
};

} // namespace duckdb
```

This confirms: `ExtensionABIType::CPP` is what our template uses (the whole point of
`DUCKDB_CPP_EXTENSION_ENTRY`), as opposed to the pure-C ABI (`duckdb_ext_api_v1` struct,
`C_STRUCT`/`C_STRUCT_UNSTABLE`) used by extensions built against the stable **C API**
(`src/include/duckdb_extension.h`, an autogenerated header — see §2.5) rather than the C++ headers. Both
exist in v1.5.4; **the extension-template / C++-header approach uses `CPP`**, which is version-locked
("needs to match precisely" — i.e., a `CPP`-ABI extension built against v1.5.4 will refuse to load into
any other DuckDB version, confirmed again in §4).

### 2.4 `ExtensionUtil` is REMOVED in v1.5.4 — do not use it

Some older tutorials/blog posts (and DuckDB's own docs prior to this removal) show
`ExtensionUtil::RegisterFunction(...)` as a static-class API. **This class no longer exists as of
v1.5.4.** Full content of `src/include/duckdb/main/extension_util.hpp` at tag `v1.5.4` (fetched verbatim,
`.../scratchpad/raw/extension_util.hpp`):

```cpp
#pragma once

namespace duckdb {

#ifndef DUCKDB_CLANG_TIDY
// NOLINTBEGIN
static_assert(false, "The DuckDB 'ExtensionUtil' class has been removed, see this PR for more details: "
                     "https://github.com/duckdb/duckdb/pull/17772");
// NOLINTEND
#endif

} // namespace duckdb
```

Including this header at all is now a **hard compile error** by design (`static_assert(false, ...)`).
The replacement is the `ExtensionLoader` instance passed into your `DUCKDB_CPP_EXTENSION_ENTRY` /
`Extension::Load` — see §3.

### 2.5 (Context only) the pure-C ABI alternative — not used by the template, out of scope for this doc's implementation

`src/include/duckdb_extension.h` (autogenerated, "WARNING: this file is autogenerated by
scripts/generate_c_api.py") defines a versioned function-pointer struct (`duckdb_ext_api_v1`) and macros
`DUCKDB_EXTENSION_API_VERSION_MAJOR/MINOR/PATCH` (defaults 1.2.0 unless overridden) for extensions that
want to be forward/backward compatible across DuckDB versions without recompilation (`C_STRUCT`/
`C_STRUCT_UNSTABLE` ABI types, §2.3). This is the mechanism the DuckDB *C client* and Rust/other-language
extension bindings use. **It is not what `extension-template` uses and not recommended for a fresh C++
extension** — the C++ `ExtensionLoader` path is simpler and is what this whole document (and the
template) is built around. Full struct is ~1450 lines; not reproduced here as it's not on our critical
path (see `.../scratchpad/raw/duckdb_extension_macro.h` if needed for cross-reference).

---

## 3. Registering things (`ExtensionLoader` API)

### 3.1 Full public interface, verbatim

`src/include/duckdb/main/extension/extension_loader.hpp` at tag `v1.5.4`, lines 19-113 (class body):

```cpp
namespace duckdb {

class DatabaseInstance;
struct CreateMacroInfo;
struct CreateCollationInfo;
struct CreateAggregateFunctionInfo;
struct CreateScalarFunctionInfo;
struct CreateTableFunctionInfo;

class ExtensionLoader {
	friend class DuckDB;
	friend class ExtensionHelper;

public:
	explicit ExtensionLoader(ExtensionActiveLoad &load_info);
	ExtensionLoader(DatabaseInstance &db, const string &extension_name);

	//! Returns the DatabaseInstance associated with this extension loader
	DUCKDB_API DatabaseInstance &GetDatabaseInstance();

public:
	//! Set the description of the extension
	DUCKDB_API void SetDescription(const string &description);

public:
	//! Register a new scalar function - merge overloads if the function already exists
	DUCKDB_API void RegisterFunction(ScalarFunction function);
	DUCKDB_API void RegisterFunction(ScalarFunctionSet function);
	DUCKDB_API void RegisterFunction(CreateScalarFunctionInfo info);

	//! Register a new aggregate function - merge overloads if the function already exists
	DUCKDB_API void RegisterFunction(AggregateFunction function);
	DUCKDB_API void RegisterFunction(AggregateFunctionSet function);
	DUCKDB_API void RegisterFunction(CreateAggregateFunctionInfo info);

	//! Register a new table function - merge overloads if the function already exists
	DUCKDB_API void RegisterFunction(TableFunction function);
	DUCKDB_API void RegisterFunction(TableFunctionSet function);
	DUCKDB_API void RegisterFunction(CreateTableFunctionInfo info);

	//! Register a new pragma function - throw an exception if the function already exists
	DUCKDB_API void RegisterFunction(PragmaFunction function);

	//! Register a new pragma function set - throw an exception if the function already exists
	DUCKDB_API void RegisterFunction(PragmaFunctionSet function);

	//! Register a CreateSecretFunction
	DUCKDB_API void RegisterFunction(CreateSecretFunction function);

	//! Register a new copy function - throw an exception if the function already exists
	DUCKDB_API void RegisterFunction(CopyFunction function);
	//! Register a new macro function - throw an exception if the function already exists
	DUCKDB_API void RegisterFunction(CreateMacroInfo &info);

	//! Register a new collation
	DUCKDB_API void RegisterCollation(CreateCollationInfo &info);

	//! Register a new coordinate system
	DUCKDB_API void RegisterCoordinateSystem(CreateCoordinateSystemInfo &info);

	//! Returns a reference to the function in the catalog - throws an exception if it does not exist
	DUCKDB_API ScalarFunctionCatalogEntry &GetFunction(const string &name);
	DUCKDB_API TableFunctionCatalogEntry &GetTableFunction(const string &name);
	DUCKDB_API optional_ptr<CatalogEntry> TryGetFunction(const string &name);
	DUCKDB_API optional_ptr<CatalogEntry> TryGetTableFunction(const string &name);

	//! Add a function overload
	DUCKDB_API void AddFunctionOverload(ScalarFunction function);
	DUCKDB_API void AddFunctionOverload(ScalarFunctionSet function);
	DUCKDB_API void AddFunctionOverload(TableFunctionSet function);

	//! Registers a new type
	DUCKDB_API void RegisterType(string type_name, LogicalType type,
	                             bind_logical_type_function_t bind_function = nullptr);

	//! Registers a new secret type
	DUCKDB_API void RegisterSecretType(SecretType secret_type);

	//! Registers a cast between two types
	DUCKDB_API void RegisterCastFunction(const LogicalType &source, const LogicalType &target,
	                                     bind_cast_function_t function, int64_t implicit_cast_cost = -1);
	DUCKDB_API void RegisterCastFunction(const LogicalType &source, const LogicalType &target, BoundCastInfo function,
	                                     int64_t implicit_cast_cost = -1);

private:
	void FinalizeLoad();

private:
	DatabaseInstance &db;
	string extension_name;
	string extension_description;
	optional_ptr<ExtensionInfo> extension_info;
};

} // namespace duckdb
```

`GetDatabaseInstance()` is how you reach the `DatabaseInstance &` if you need lower-level config access
(e.g. `DBConfig::GetConfig(loader.GetDatabaseInstance())`, the same pattern the implementation itself
uses internally — see below).

### 3.2 Implementation detail worth knowing (what each `RegisterFunction` overload actually does)

`src/main/extension/extension_loader.cpp` at tag `v1.5.4` (full file, 240 lines, fetched verbatim to
`.../scratchpad/raw/src_main_extension_extension_loader.cpp`) shows each overload boils down to building
a `CreateXxxFunctionInfo` and calling into the system catalog, e.g.:

```cpp
void ExtensionLoader::RegisterFunction(ScalarFunction function) {
	ScalarFunctionSet set(function.name);
	set.AddFunction(std::move(function));
	RegisterFunction(std::move(set));
}

void ExtensionLoader::RegisterFunction(ScalarFunctionSet function) {
	CreateScalarFunctionInfo info(std::move(function));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	RegisterFunction(std::move(info));
}

void ExtensionLoader::RegisterFunction(CreateScalarFunctionInfo function) {
	D_ASSERT(!function.functions.name.empty());
	auto &system_catalog = Catalog::GetSystemCatalog(db);
	auto data = CatalogTransaction::GetSystemTransaction(db);
	system_catalog.CreateFunction(data, function);
}
```

and identically for `TableFunction`/`TableFunctionSet`/`CreateTableFunctionInfo`
(`extension_loader.cpp:90-108`). Note `OnCreateConflict::ALTER_ON_CONFLICT` for scalar/aggregate/table
functions — registering a function with a name that already exists **merges as an overload** rather than
erroring (this is how the same catalog name can have multiple type signatures).

`RegisterCastFunction` reaches `DBConfig::GetConfig(db).GetCastFunctions().RegisterCastFunction(...)`
(`extension_loader.cpp:226-238`) — confirming the pattern for reaching `DBConfig` from inside the loader:
`DBConfig::GetConfig(loader.GetDatabaseInstance())`.

### 3.3 Minimal scalar function registration (already shown in full in §2.1) — key signature

`ScalarFunction` constructor used, from `src/include/duckdb/function/scalar_function.hpp`
(`.../scratchpad/raw/src_include_duckdb_function_scalar_function.hpp`), line 127:

```cpp
DUCKDB_API ScalarFunction(string name, vector<LogicalType> arguments, LogicalType return_type,
                          scalar_function_t function, ...);
```
where `scalar_function_t = std::function<void(DataChunk &, ExpressionState &, Vector &)>`
(`scalar_function.hpp:98`).

### 3.4 Minimal table function registration — key signatures

From `src/include/duckdb/function/table_function.hpp`
(`.../scratchpad/raw/src_include_duckdb_function_table_function.hpp`), lines 288-380:

```cpp
typedef unique_ptr<FunctionData> (*table_function_bind_t)(ClientContext &context, TableFunctionBindInput &input,
                                                          vector<LogicalType> &return_types, vector<string> &names);
typedef unique_ptr<GlobalTableFunctionState> (*table_function_init_global_t)(ClientContext &context,
                                                                             TableFunctionInitInput &input);
typedef unique_ptr<LocalTableFunctionState> (*table_function_init_local_t)(ExecutionContext &context,
                                                                           TableFunctionInitInput &input,
                                                                           GlobalTableFunctionState *global_state);
typedef void (*table_function_t)(ClientContext &context, TableFunctionInput &data, DataChunk &output);
...
class TableFunction : public SimpleNamedParameterFunction {
public:
	DUCKDB_API TableFunction();
	TableFunction(string name, const vector<LogicalType> &arguments, table_function_t function,
	              table_function_bind_t bind = nullptr, table_function_init_global_t init_global = nullptr,
	              table_function_init_local_t init_local = nullptr);
	...
```

So a minimal table function registration looks like:

```cpp
static unique_ptr<FunctionData> MyBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	names.push_back("result");
	return_types.push_back(LogicalType::VARCHAR);
	return make_uniq<MyBindData>(...);
}

static void MyFunc(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	// fill `output`, set output.SetCardinality(n) when done / 0 to signal EOF
}

// in LoadInternal(ExtensionLoader &loader):
TableFunction my_table_func("my_table_func", {LogicalType::VARCHAR}, MyFunc, MyBind);
loader.RegisterFunction(my_table_func);
```

### 3.5 ATTACH / storage extensions (bonus — reaching `DBConfig` for `StorageExtension::Register`)

Not part of the base `ExtensionLoader` API but reachable through it. `src/include/duckdb/storage/storage_extension.hpp`
(fetched verbatim):

```cpp
class StorageExtension {
public:
	attach_function_t attach;
	create_transaction_manager_t create_transaction_manager;
	shared_ptr<StorageExtensionInfo> storage_info;
	...
	static optional_ptr<StorageExtension> Find(const DBConfig &config, const string &extension_name);
	static void Register(DBConfig &config, const string &extension_name, shared_ptr<StorageExtension> extension);
};
```

Pattern to register one from inside `LoadInternal(ExtensionLoader &loader)`:

```cpp
auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
auto storage_extension = make_shared_ptr<MyStorageExtension>();
StorageExtension::Register(config, "my_db_type", std::move(storage_extension));
```

(Not exercised/verified beyond reading the header — flagged in §8.)

---

## 4. Loading a locally built extension

### 4.1 Commands

```sh
# Start the CLI allowing unsigned extensions:
duckdb -unsigned

# Then, from SQL:
LOAD '/abs/path/to/build/release/extension/my_ext/my_ext.duckdb_extension';

# Equivalent, for client APIs that don't have an -unsigned CLI flag (Python/R/etc.):
#   set the `allow_unsigned_extensions` config option to true instead, e.g. Python:
#     con = duckdb.connect(':memory:', config={'allow_unsigned_extensions': 'true'})
```

Source: `duckdb-web` docs, `docs/current/extensions/extension_distribution.md:43-63` (redirect target of
`/docs/stable/extensions/advanced_installation_methods` and `/docs/stable/extensions/overview`):

> "If you wish to load your own extensions or extensions from third-parties you will need to enable the
> `allow_unsigned_extensions` flag. To load unsigned extensions using the CLI client, pass the
> `-unsigned` flag to it on startup: `duckdb -unsigned` ... For client APIs, the
> `allow_unsigned_extensions` database configuration options needs to be set."

### 4.2 `-unsigned` only — no `--allow-unsigned` alias

The full CLI argument table, `docs/current/clients/cli/arguments.md:56` (only match for "unsigned" in
that file):

```
| `-unsigned`            | Allow loading of unsigned extensions ... This option is intended to be used for developing extensions. ...
```

**Single-dash `-unsigned` is the only spelling** — there is no `--allow-unsigned` variant. (DuckDB CLI
flags throughout this doc's table are consistently single-dash, e.g. `-json`, `-readonly`, `-version`.)

Live-verified on the installed `v1.5.4` CLI:

```
$ echo "SELECT current_setting('allow_unsigned_extensions');" | duckdb
┌──────────────────────────────────────────────┐
│ current_setting('allow_unsigned_extensions') │
│                    boolean                    │
├──────────────────────────────────────────────┤
│ false                                          │
└──────────────────────────────────────────────┘
```
confirming the setting name and default (`false`) exactly, and:
```
$ echo "LOAD '/tmp/nonexistent.duckdb_extension';" | duckdb
IO Error: Extension "/tmp/nonexistent.duckdb_extension" not found.
Candidate extensions: "motherduck", "inet", "fts", "excel", "https"
```
showing the exact error text/format for a missing local path (useful for `statement error` sqllogictests
around bad `LOAD` paths).

### 4.3 Platform string / `duckdb_platform`

Confirmed platform table, `docs/current/extensions/extension_distribution.md:16-23`:

| Platform name   | Operating system | Architecture    |
| --------------- | ---------------- | --------------- |
| `linux_amd64`   | Linux            | x86_64 (AMD64)  |
| `linux_arm64`   | Linux            | AArch64 (ARM64) |
| `osx_amd64`     | macOS            | x86_64 (Intel)  |
| `osx_arm64`     | macOS            | AArch64 (Apple Silicon) |
| `windows_amd64` | Windows          | x86_64          |
| `windows_arm64` | Windows          | AArch64         |

plus `windows_amd64_mingw`, `wasm_eh`, `wasm_mvp` for some extensions
(`extension_distribution.md:25-28`), matching `extension-ci-tools/config/distribution_matrix.json`
exactly (`.../scratchpad/research-b/extension-ci-tools/config/distribution_matrix.json`, which enumerates
`linux_amd64`, `linux_arm64`, `linux_amd64_musl`, `linux_arm64_musl`, `osx_amd64`, `osx_arm64`,
`windows_amd64`, `windows_arm64`, `windows_amd64_mingw`, `wasm_mvp`, `wasm_eh`, `wasm_threads`).

Live-verified on this machine:

```
$ echo "PRAGMA platform;" | duckdb
┌─────────────┐
│  platform   │
│   varchar   │
├─────────────┤
│ linux_amd64 │
└─────────────┘
```

So a `.duckdb_extension` built on/for this machine must embed `linux_amd64` in its metadata footer
platform field (§4.4) — this happens automatically via the build's `duckdb_platform` CMake target/file
(`extension_build_tools.cmake:212`, `-DPLATFORM_FILE=${DuckDB_BINARY_DIR}/duckdb_platform_out`), no
manual action needed as long as you build on the same OS/arch you intend to load on.

### 4.4 ABI / metadata-footer compatibility mechanics

> "To avoid binary compatibility issues, the binary extensions distributed by DuckDB are tied both to a
> specific DuckDB version and a platform. ... When trying to load an extension that was compiled for a
> different version or platform, DuckDB will throw an error and refuse to load the extension."
> — `docs/current/extensions/extension_distribution.md:65-69`

Mechanically, every `.duckdb_extension` file has a **512-byte footer** appended post-build
(`ParsedExtensionMetaData::FOOTER_SIZE = 512`, `extension.hpp` — see §2.3), built by
`scripts/append_metadata.cmake` (full file fetched, `.../scratchpad/raw/scripts_append_metadata.cmake`),
invoked from `extension_build_tools.cmake:208-213`:

```cmake
add_custom_command(
        TARGET ${TARGET_NAME}
        POST_BUILD
        COMMAND
        ${CMAKE_COMMAND} -DABI_TYPE=${ABI_TYPE} -DEXTENSION=$<TARGET_FILE:${TARGET_NAME}>${EXTENSION_POSTFIX} -DPLATFORM_FILE=${DuckDB_BINARY_DIR}/duckdb_platform_out -DVERSION_FIELD="${FOOTER_VERSION_VALUE}" -DEXTENSION_VERSION="${EXTENSION_VERSION}" -DNULL_FILE=${DUCKDB_MODULE_BASE_DIR}/scripts/null.txt -P ${DUCKDB_MODULE_BASE_DIR}/scripts/append_metadata.cmake
)
```

The footer layout, from `append_metadata.cmake` itself (comments + code, lines 24-68): 8 metadata fields
of **32 bytes each** (256 bytes total) followed by a **256-byte signature** block (`EMPTY_256` when
unsigned) = 512 bytes total, matching `FOOTER_SIZE=512`/`SIGNATURE_SIZE=256`:

```
META1 = "4"                         (magic/schema marker, matches ParsedExtensionMetaData::EXPECTED_MAGIC_VALUE = "4\0\0...")
META2 = contents of duckdb_platform_out file, e.g. "linux_amd64"
META3 = VERSION_FIELD  (DuckDB version this ext targets, e.g. "v1.5.4" for CPP ABI)
META4 = EXTENSION_VERSION (the extension's own version, e.g. its git describe)
META5 = ABI_TYPE  ("CPP" for the template)
META6-8 = reserved / empty
[256-byte signature]
```

At load time, DuckDB reads this footer and compares platform + version against the running instance;
mismatch → refusal to load (exact error text not captured live in this research pass — flagged in §8).
Because our `CPP` ABI type requires an **exact** version match (`ExtensionABIType::CPP` doc-comment: "Uses
C++ ABI, version needs to match precisely" — `extension.hpp:32-33`), **an extension built against the
`v1.5.4` submodule will only load into a `v1.5.4` DuckDB — which is exactly the brew-installed CLI on
this machine.** This directly answers "can the brew CLI load an extension built from this source
checkout": **yes, byte-for-byte, since both report `08e34c447b`.**

### 4.5 Verifying with `duckdb_extensions()`

Live output on this machine (columns confirmed exactly — the docs page's example table was slightly
stale/abbreviated, so the live run is the authoritative source here):

```
$ echo "SELECT * FROM duckdb_extensions() LIMIT 3;" | duckdb -unsigned
┌────────────────┬─────────┬───────────┬──────────────┬────────────────────────────────────────────┬───────────┬────────────────────┬────────────────────┬─────────────────┐
│ extension_name │ loaded  │ installed │ install_path │                 description                  │  aliases  │ extension_version  │   install_mode     │ installed_from  │
│    varchar     │ boolean │  boolean  │   varchar    │                   varchar                    │ varchar[] │      varchar       │      varchar       │     varchar     │
├────────────────┼─────────┼───────────┼──────────────┼────────────────────────────────────────────┼───────────┼────────────────────┼────────────────────┼─────────────────┤
│ autocomplete   │ true    │ true      │ (BUILT-IN)   │ Adds support for autocomplete in the shell  │ []        │ v1.5.4             │ STATICALLY_LINKED  │                 │
│ avro           │ false   │ false     │              │ Adds support for reading Avro files         │ []        │                    │ NOT_INSTALLED      │                 │
│ aws            │ false   │ false     │              │ Provides features that depend on the AWS SDK│ []        │                    │ NOT_INSTALLED      │                 │
└────────────────┴─────────┴───────────┴──────────────┴────────────────────────────────────────────┴───────────┴────────────────────┴────────────────────┴─────────────────┘
```

So the actual column set (in order) is: `extension_name, loaded, installed, install_path, description,
aliases, extension_version, install_mode, installed_from`. After `LOAD '/path/to/my_ext.duckdb_extension'`
you'd expect `my_ext` to show `loaded = true`, `install_path` = the path you loaded (or blank, since
`LOAD` with an explicit path doesn't go through the installed-extensions directory — this specific
detail was **not** independently re-verified with a real custom extension in this research pass; flagged
in §8).

---

## 5. Testing

### 5.1 sqllogictest format — directives

Primary source: `docs/current/dev/sqllogictest/{intro,writing_tests,result_verification,test_configuration,catch,debugging}.md`
in the `duckdb-web` repo (all fetched verbatim; paths as sparse-checked into
`.../scratchpad/research-b/duckdb-web/docs/current/dev/sqllogictest/`).

**File header & basic structure** (`intro.md:14-42`):

```sql
# name: test/sql/projection/test_simple_projection.test
# group [projection]

# enable query verification
statement ok
PRAGMA enable_verification

# create table
statement ok
CREATE TABLE a (i INTEGER, j INTEGER);

# insertion: 1 affected row
statement ok
INSERT INTO a VALUES (42, 84);

query II
SELECT * FROM a;
----
42	84
```

- Statements/queries **must be separated by blank lines** (parser is whitespace-sensitive this way).
- `# name:` = relative path of the test file itself; `# group:` = its containing directory. Both are
  used by `unittest <path>` / `unittest "[group]"` selection.
- `.test` = fast set (run by default); `.test_slow` = only run when `unittest *` runs everything
  explicitly (`intro.md:42`).

**`statement ok` / `statement error`** (`result_verification.md:37-46`):

```sql
statement error
SELECT * FROM non_existent_table;
----
Table with name non_existent_table does not exist!
```

> "The `statement error` also takes an optional expected result – which is interpreted as the *expected
> error message*. ... The test passes if the error message *contains* the text under `statement error` –
> the entire error message does not need to be provided."

**`query` result verification** (`result_verification.md:11-23`):

```sql
query II
SELECT 42, 84 UNION ALL SELECT 10, 20;
----
42	84
10	20
```

- The letters after `query` (`I`, `II`, `III`, ...) indicate **column count**, one `I` per column — *not*
  a type code (legacy `R`/`T` letters are still accepted but deprecated/ignored:
  "DuckDB deprecated the usage of types in the sqllogictest ... only `I` should be used").
- `----` separates the query text from expected results.
- Row-wise: tab-separated values per line, one line per row. Value-wise (alternate): one value per line,
  in row-major order (`result_verification.md:72-92`).
- `NULL` values → literal string `NULL`; empty strings → literal string `(empty)`
  (`result_verification.md:25-35`), because blank lines are structurally significant (end-of-block
  marker).
- Regex assertions: `<REGEX>:<pattern>` (match) / `<!REGEX>:<pattern>` (must-not-match) per value
  (`result_verification.md:48-59`) — used heavily for `EXPLAIN` plan checks.
- `<FILE>:relative/path/to/expected.csv` — read expected result from a file
  (`result_verification.md:61-70`).
- Result sorting modifier goes right after the type string: `query I rowsort` / `nosort` / `valuesort`
  (`result_verification.md:132-146`).
- Query labels (`query I nosort r43`) let two different queries assert mutual equality instead of a
  literal expected value (`result_verification.md:148-162`).
- MD5-hash based verification: `mode output_hash` computes hashes to paste in as `N values hashing to
  <md5>` (`result_verification.md:94-129`) — used to keep large-result tests compact.

**`require <extension>`** (`intro.md:74-78`, confirmed exactly against the real parser/runner source
`test/sqlite/sqllogic_parser.cpp:258-259` token `"require"` → `SQLLOGIC_REQUIRE`, and
`test/sqlite/sqllogic_test_runner.cpp:1082-1090`):

```sql
require parquet
```

> "If the extension is not loaded, any statements that occur after the require field will be skipped."

Also supports `require vector_size <N>` to gate on the compiled vector size (`intro.md:78`).

**`require-env <VAR> [expected_value]`** — **not documented on duckdb.org**, but confirmed as a real,
implemented directive by reading the actual test runner source directly,
`test/sqlite/sqllogic_test_runner.cpp:1105-1148` (fetched verbatim to
`.../scratchpad/raw/chk_test_sqlite_sqllogic_test_runner.cpp`):

```cpp
} else if (token.type == SQLLogicTokenType::SQLLOGIC_REQUIRE_ENV) {
	if (InLoop()) {
		parser.Fail("require-env cannot be called in a loop");
	}
	if (token.parameters.size() != 1 && token.parameters.size() != 2) {
		parser.Fail("require-env requires 1 argument: <env name> [optional: <expected env val>]");
	}
	...
	if (env_actual == nullptr) {
		// Environment variable was not found, this test should not be run
		SKIP_TEST("require-env " + token.parameters[0]);
		return;
	}
	if (token.parameters.size() == 2) {
		// Check that the value is the same as the expected value
		auto env_value = token.parameters[1];
		if (std::strcmp(env_actual, env_value.c_str()) != 0) {
			SKIP_TEST("require-env " + token.parameters[0] + " " + token.parameters[1]);
			return;
		}
		...
```

Usage: `require-env MY_VAR` (skip test if `MY_VAR` unset) or `require-env MY_VAR expected_value` (skip
unless it equals `expected_value`). Token recognized in
`test/sqlite/sqllogic_parser.cpp:258-262` (`"require"` → `SQLLOGIC_REQUIRE`, `"require-env"` →
`SQLLOGIC_REQUIRE_ENV`, `"test-env"` → `SQLLOGIC_TEST_ENV`).

**`mode skip` / `mode unskip`** — confirmed both in docs (`docs/current/dev/sqllogictest/debugging.md:22`:
"You can also skip certain queries from executing by placing `mode skip` in the file, followed by an
optional `mode unskip`. Any queries between the two statements will not be executed.") and in the runner
source, `test/sqlite/sqllogic_test_runner.cpp:963-975`:

```cpp
} else if (token.type == SQLLogicTokenType::SQLLOGIC_MODE) {
	...
	string parameter = token.parameters[0];
	if (parameter == "skip") {
		skip_level++;
	} else if (parameter == "unskip") {
		skip_level--;
	} else {
		auto command = make_uniq<ModeCommand>(*this, std::move(parameter));
		ExecuteCommand(std::move(command));
	}
```
(`skip_level` is a counter, so `mode skip`/`mode unskip` pairs can nest.)

**Temp files**: `__TEST_DIR__` placeholder is substituted with the test's scratch directory path
(`intro.md:65-72`), e.g.:
```sql
statement ok
COPY csv_data TO '__TEST_DIR__/output_file.csv.gz' (COMPRESSION gzip);
```

### 5.2 `make test` wiring

From `duckdb_extension.Makefile` (§1.6), the `test`/`test_release`/`test_debug` targets ultimately run:

```sh
./build/release/test/unittest "test/*"
# (or build/debug/... for test_debug; TESTS_BASE_DIRECTORY defaults to "test/")
```

i.e. it invokes the DuckDB core `unittest` binary — which has your extension's tests linked in/available
— restricted (by the `"test/*"` glob argument, from `TESTS_BASE_DIRECTORY`) to just this repo's `test/`
directory. Per `writing_tests.md:15`, `unittest` itself lives at `build/release/test/unittest` /
`build/debug/test/unittest` regardless of which repo built it (out-of-tree extensions get their own
copy because the whole DuckDB CMake project — including `test/`  — is rebuilt as part of `make`).

Test selection details (`test_configuration.md`, full option list quoted): `--test-dir <path>` overrides
the working directory root the runner resolves relative test paths against (used when running the
`unittest` binary from somewhere other than the extension repo root: `unittest --test-dir . "[sql]"`
matches the task prompt's expectation) — `unittest --test-dir . "[sql]"` runs every sqllogictest tagged
group `[sql]` (the group tag on `# group: [projection]` etc.) rooted at `.`.

Single-test invocation, `docs/current/dev/sqllogictest/debugging.md:29-31`:
```sh
build/debug/test/unittest test/sql/projection/test_simple_projection.test
```

Config surface, `test_configuration.md` full option table (abridged to the ones relevant to extension
authors):

| Option | Description |
|---|---|
| `--test-dir <path>` | Override default working directory of test runner |
| `--require <extension_name>` | If the extension is missing: **fail** rather than skip tests that require it (useful in CI to catch accidental extension-not-built regressions) |
| `--statically-loaded-extensions <comma-separated strings>` | Extensions to be loaded (from the statically available ones) |
| `--skip-error-messages <comma-separated strings>` | Skip rather than fail tests whose error message contains any of these substrings |
| `--select-tag` / `--select-tag-set` / `--skip-tag` / `--skip-tag-set` | Include/exclude by tag |

Environment variable form: any option can also be set via `DUCKDB_TEST_<OPTION_UPPERCASED>=1`, e.g.
`DUCKDB_TEST_CHECKPOINT_ON_SHUTDOWN=1 unittest` (`test_configuration.md:76-84`).

### 5.3 The template's real test file (concrete worked example)

`.../scratchpad/research-b/extension-template/test/sql/waddle.test` (full file):

```sql
# name: test/sql/waddle.test
# description: test waddle extension
# group: [sql]

# Before we load the extension, this will fail
statement error
SELECT waddle('Sam');
----
Catalog Error: Scalar Function with name waddle does not exist!

# Require statement will ensure this test is run with this extension loaded
require waddle

# Confirm the extension works
query I
SELECT waddle('Sam');
----
...........🦆 Sam

query I
SELECT waddle_openssl_version('Michael') ILIKE 'Waddle Michael, my linked OpenSSL version is OpenSSL%';
----
true
```

Note the pattern: test the "not-loaded" failure mode *before* `require <ext>`, then everything after
`require` only runs once the (statically-linked, in this build) extension is available.

### 5.4 C++ Catch2 unit tests (`test/cpp/`)

`docs/current/dev/sqllogictest/catch.md` (full content):

> "While we prefer the sqllogic tests for testing most functionality, for certain tests only SQL is not
> sufficient. This typically happens when you want to test the C++ API."

```cpp
#include "catch.hpp"
#include "test_helpers.hpp"

TEST_CASE("Test simple storage", "[storage]") {
	auto config = GetTestConfig();
	unique_ptr<QueryResult> result;
	auto storage_database = TestCreatePath("storage_test");

	DeleteDatabase(storage_database);
	{
		DuckDB db(storage_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE test (a INTEGER, b INTEGER);"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO test VALUES (11, 22), (13, 22), (12, 21), (NULL, NULL)"));
	}
	for (idx_t i = 0; i < 2; i++) {
		DuckDB db(storage_database, config.get());
		Connection con(db);
		result = con.Query("SELECT * FROM test ORDER BY a");
		REQUIRE(CHECK_COLUMN(result, 0, {Value(), 11, 12, 13}));
	}
	DeleteDatabase(storage_database);
}
```

> "The test uses the `TEST_CASE` wrapper to create each test. ... Results are checked using either
> `REQUIRE_FAIL` / `REQUIRE_NO_FAIL` (corresponding to statement ok and statement error) or
> `REQUIRE(CHECK_COLUMN(...))` (corresponding to query with a result check). **Every test that is created
> in this way needs to be added to the corresponding `CMakeLists.txt`.**"

The extension-template does not ship a `test/cpp/` example or a pre-wired `CMakeLists.txt` entry for
Catch tests — this is a DuckDB-core-testing convention (`ENABLE_UNITTEST_CPP_TESTS` build flag exists,
default `FALSE` for extensions per `duckdb_extension.Makefile:121`
`-DENABLE_UNITTEST_CPP_TESTS=FALSE`); if you want C++ Catch tests in an out-of-tree extension you'd add
your own `test/cpp/*.cpp` sources to an `add_executable`/test target in your extension's
`CMakeLists.txt` and link them against `duckdb_static` + Catch (flagged in §8 as not exercised end-to-end
by this research — the template gives you sqllogictest wiring for free but not C++ unit test wiring).

---

## 6. Third-party dependency linking

### 6.1 The template builds TWO targets — here's exactly how each links dependencies

Recall §1.3's root `CMakeLists.txt`:

```cmake
find_package(OpenSSL REQUIRED)
set(EXTENSION_NAME ${TARGET_NAME}_extension)               # STATIC target
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)  # SHARED/loadable target

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)
```

**Both** `target_link_libraries` calls are required — the static target (linked into `duckdb`/`unittest`
binaries) and the loadable target (`.duckdb_extension` file) are compiled from the *same* sources but are
two completely separate CMake targets, and each needs its own dependency links. For a VTK-based
extension this means:

```cmake
find_package(VTK REQUIRED COMPONENTS CommonCore ...)
...
target_link_libraries(${EXTENSION_NAME} ${VTK_LIBRARIES})
target_link_libraries(${LOADABLE_EXTENSION_NAME} ${VTK_LIBRARIES})
```//(or the modern `VTK::CommonCore` etc. imported-target form, both work with target_link_libraries)

### 6.2 `build_static_extension` (full function body)

`extension/extension_build_tools.cmake:257-263`:

```cmake
function(build_static_extension NAME PARAMETERS)
    # all parameters after name
    set(FILES "${ARGV}")
    list(REMOVE_AT FILES 0)
    add_library(${NAME}_extension STATIC ${FILES})
    target_link_libraries(${NAME}_extension duckdb_static)
endfunction()
```

Trivial: a plain `STATIC` library linked against `duckdb_static` (DuckDB's own static core lib).
`CMAKE_POSITION_INDEPENDENT_CODE ON` is set **globally** at the top of DuckDB's own root `CMakeLists.txt`
(`.../scratchpad/raw/CMakeLists.txt:60`: `set(CMAKE_POSITION_INDEPENDENT_CODE ON)`), so `duckdb_static`
itself (and, transitively, your `_extension` static target sources) are already compiled `-fPIC` — no
extra action needed to make the static target embeddable into the shared loadable target later.

### 6.3 `build_loadable_extension_directory` (full function body — the important one)

`extension/extension_build_tools.cmake:100-218` (fetched verbatim,
`.../scratchpad/raw/extension_build_tools.cmake`):

```cmake
function(build_loadable_extension_directory NAME ABI_TYPE OUTPUT_DIRECTORY EXTENSION_VERSION CAPI_VERSION PARAMETERS)
    set(TARGET_NAME ${NAME}_loadable_extension)
    ...
    if(EMSCRIPTEN)
        add_library(${TARGET_NAME} STATIC ${FILES})
    else()
        add_library(${TARGET_NAME} SHARED ${FILES})
    endif()
    set_target_properties(${TARGET_NAME} PROPERTIES DEFINE_SYMBOL "")
    set_target_properties(${TARGET_NAME} PROPERTIES OUTPUT_NAME ${NAME})
    set_target_properties(${TARGET_NAME} PROPERTIES PREFIX "")
    ...
    # loadable extension binaries can be built two ways:
    # 1. EXTENSION_STATIC_BUILD=1
    #    DuckDB is statically linked into each extension binary. This increases portability because in several situations
    #    DuckDB itself may have been loaded with RTLD_LOCAL. This is currently the main way we distribute the loadable
    #    extension binaries
    # 2. EXTENSION_STATIC_BUILD=0
    #    The DuckDB symbols required by the loadable extensions are left unresolved. This will reduce the size of the binaries
    #    and works well when running the DuckDB cli directly. For windows this uses delay loading. For MacOS and linux the
    #    dynamic loader will look up the missing symbols when the extension is dlopen-ed.
    if(WASM_LOADABLE_EXTENSIONS)
        set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -sSIDE_MODULE=1 -DWASM_LOADABLE_EXTENSIONS")
    elseif(${ABI_TYPE} STREQUAL "C_STRUCT" OR ${ABI_TYPE} STREQUAL "C_STRUCT_UNSTABLE")
        # TODO strip all symbols except the capi init
    elseif (EXTENSION_STATIC_BUILD)
        if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU" OR "${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
            if (APPLE)
                set_target_properties(${TARGET_NAME} PROPERTIES CXX_VISIBILITY_PRESET hidden)
                set(WHITELIST "-Wl,-exported_symbol,_${NAME}_duckdb_cpp_init")
                target_link_libraries(${TARGET_NAME} duckdb_static dummy_static_extension_loader ${DUCKDB_EXTRA_LINK_FLAGS} -Wl,-dead_strip ${WHITELIST})
            elseif (ZOS)
                target_link_libraries(${TARGET_NAME} duckdb_static dummy_static_extension_loader ${DUCKDB_EXTRA_LINK_FLAGS})
            else()
                # For GNU we rely on fvisibility=hidden to hide the extension symbols and use -exclude-libs to hide the duckdb symbols
                set_target_properties(${TARGET_NAME} PROPERTIES CXX_VISIBILITY_PRESET hidden)
                target_link_libraries(${TARGET_NAME} duckdb_static dummy_static_extension_loader ${DUCKDB_EXTRA_LINK_FLAGS} -Wl,--gc-sections -Wl,--exclude-libs,ALL)
            endif()
        elseif (WIN32)
            target_link_libraries(${TARGET_NAME} duckdb_static dummy_static_extension_loader ${DUCKDB_EXTRA_LINK_FLAGS})
        else()
            message(FATAL_ERROR, "EXTENSION static build is only intended for Linux and Windows on MVSC")
        endif()
    else()
        if (WIN32)
            target_link_libraries(${TARGET_NAME} duckdb ${DUCKDB_EXTRA_LINK_FLAGS})
        elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang$")
            if (APPLE)
                set_target_properties(${TARGET_NAME} PROPERTIES LINK_FLAGS "-undefined dynamic_lookup")
            endif()
        endif()
    endif()

    target_compile_definitions(${TARGET_NAME} PUBLIC -DDUCKDB_BUILD_LOADABLE_EXTENSION)
    set_target_properties(${TARGET_NAME} PROPERTIES SUFFIX ".duckdb_extension")
    ...
    add_custom_command(
            TARGET ${TARGET_NAME}
            POST_BUILD
            COMMAND
            ${CMAKE_COMMAND} -DABI_TYPE=${ABI_TYPE} -DEXTENSION=$<TARGET_FILE:${TARGET_NAME}>${EXTENSION_POSTFIX} -DPLATFORM_FILE=${DuckDB_BINARY_DIR}/duckdb_platform_out -DVERSION_FIELD="${FOOTER_VERSION_VALUE}" -DEXTENSION_VERSION="${EXTENSION_VERSION}" -DNULL_FILE=${DUCKDB_MODULE_BASE_DIR}/scripts/null.txt -P ${DUCKDB_MODULE_BASE_DIR}/scripts/append_metadata.cmake
    )
    add_dependencies(${TARGET_NAME} duckdb_platform)
    ...
endfunction()

function(build_loadable_extension NAME PARAMETERS)
    set(FILES "${ARGV}")
    list(REMOVE_AT FILES 0 1)
    string(TOUPPER ${NAME} EXTENSION_NAME_UPPERCASE)
    build_loadable_extension_directory(${NAME} "CPP" "extension/${NAME}" "${DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_EXT_VERSION}" "" "${PARAMETERS}" ${FILES})
endfunction()
```

**On Linux with GCC/Clang and the default `EXTENSION_STATIC_BUILD=1`** (the default per
`duckdb_extension.Makefile:117`, `EXTENSION_STATIC_BUILD ?= 1`):

- `${NAME}_loadable_extension` is an `add_library(... SHARED ...)`.
- `CXX_VISIBILITY_PRESET hidden` is set on it — **your code's non-`DUCKDB_EXTENSION_API`-marked symbols
  are hidden by default.** Only the entrypoint macro's `DUCKDB_EXTENSION_API` (=
  `__attribute__((visibility("default")))`, §2.2) symbol is exported.
- It links `duckdb_static` (the *entire* DuckDB core, statically) **into** the shared object, plus a
  helper target `dummy_static_extension_loader`.
- `-Wl,--gc-sections` (drop unused sections) and **`-Wl,--exclude-libs,ALL`** (hide all symbols
  originating from statically-linked libraries, i.e. hide `duckdb_static`'s own symbols from the final
  `.so`'s dynamic symbol table) are added.
- `SUFFIX ".duckdb_extension"` renames the output file's extension from the platform default (`.so`) to
  `.duckdb_extension`.
- A `POST_BUILD` custom command appends the metadata footer (§4.4) via `append_metadata.cmake`.

This explains why `EXTENSION_STATIC_BUILD=1` produces large, fully self-contained `.duckdb_extension`
files (each one embeds its own private copy of DuckDB core) — the doc-comment's rationale: it's more
portable because DuckDB itself might have been `dlopen`'d with `RTLD_LOCAL`, meaning a dynamically-linked
extension couldn't find DuckDB's symbols via normal symbol resolution.

**With `EXTENSION_STATIC_BUILD=0`**: DuckDB symbols are left **unresolved** in the `.so`; the dynamic
linker resolves them against the already-loaded `duckdb` process's symbols at `dlopen` time (works when
running the actual `duckdb` CLI directly; smaller binaries; not the default distribution mode).

### 6.4 C++ standard: **17** for the extension itself, but core DuckDB's own default is **11** — flagged as needing verification

- Extension-template's own `CMakeLists.txt:16-17`: `set(CMAKE_CXX_STANDARD "17" CACHE STRING ...)`,
  `set(CMAKE_CXX_STANDARD_REQUIRED ON)`.
- DuckDB core's own root `CMakeLists.txt:54-57` (fetched verbatim,
  `.../scratchpad/raw/CMakeLists.txt`):
  ```cmake
  set(CMAKE_CXX_STANDARD "11" CACHE STRING "C++ standard to enforce")
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  ```

Both are `CACHE` variable sets **without `FORCE`**. Per CMake's documented `set(... CACHE ...)` semantics,
if a cache entry already exists, a subsequent non-`FORCE` `CACHE` set for the same variable is a no-op.
Because DuckDB's top-level `CMakeLists.txt` runs (and sets the `CMAKE_CXX_STANDARD` cache entry to `"11"`)
**before** `add_subdirectory()`-ing into the extension's own `CMakeLists.txt` (extension subdirectories
are only added near the end of `extension_build_tools.cmake`, itself `include()`'d near the end of the
core `CMakeLists.txt`), the extension's later attempt to CACHE-set `"17"` runs against an
**already-populated** cache entry. Whether this means the extension's targets actually compile with
`-std=gnu++11` or `-std=gnu++17` was **not conclusively resolved by reading source alone** — see §8. In
practice, real extensions built from this exact template do use C++14/17 features successfully, which
suggests either (a) CMake's per-directory variable scoping resolves this more subtly than the abstract
CACHE rule implies, or (b) something else (e.g. explicit `target_compile_features` somewhere, or
`CMAKE_CXX_STANDARD` being read fresh as a directory-scoped normal variable at each `add_library()` call
in a way that doesn't strictly follow the "CACHE set once" mental model). **Recommendation: after
configuring, inspect `build/release/CMakeCache.txt` for `CMAKE_CXX_STANDARD:STRING=` and/or
`build/release/compile_commands.json` for the actual `-std=` flag passed to your extension's `.cpp`
files, to confirm which standard is actually in effect before relying on C++17-only syntax.**

### 6.5 `-fvisibility=hidden` concerns — summary

- Applies automatically via `CXX_VISIBILITY_PRESET hidden` target property on the `_loadable_extension`
  target when `EXTENSION_STATIC_BUILD=1` (default) on Linux/macOS (§6.3).
- The **only** symbol that must remain exported is the entrypoint,
  `<name>_duckdb_cpp_init`, and DuckDB's own build machinery already guarantees this
  (`DUCKDB_EXTENSION_API` = default-visibility attribute on that one function, §2.2; plus, on macOS, an
  explicit linker whitelist `-Wl,-exported_symbol,_${NAME}_duckdb_cpp_init`).
- Any symbols your own code needs to export beyond that (there normally are none — everything is reached
  through the `ExtensionLoader`/catalog registration, not by the host looking up further symbols) would
  need explicit `DUCKDB_EXTENSION_API`/`__attribute__((visibility("default")))` annotations.
- Practical implication for linking VTK: **VTK's own shared libraries (`libvtkCommonCore-9.3.so` etc.)
  are separate `.so` files dynamically linked by your loadable extension `.so`** — `-fvisibility=hidden`
  and `--exclude-libs,ALL` only affect symbols originating from **statically-linked** archives compiled
  into your extension's `.so` (i.e., `duckdb_static` and your own `.o`s); they do **not** hide or strip
  symbols from a dynamically-linked shared library dependency like VTK. VTK's public API remains
  reachable from your extension's own code at compile/link time as normal; only its symbols don't leak
  into your `.duckdb_extension`'s exported dynamic symbol table (which is what you want — no symbol
  clashes with a different VTK version the host process might separately load).

---

## 7. Common pitfalls

1. **Exceptions across the ABI boundary.** DuckDB's own code throws C++ exceptions
   (`duckdb::Exception` and subclasses, see `src/include/duckdb/common/exception.hpp`) internally and
   expects extensions to do the same — since `ExtensionABIType::CPP` requires an exact-version C++ ABI
   match (§2.3/§4.4), the extension is compiled against the *same* C++ standard library / exception
   personality as the host, so throwing `duckdb::Exception` (or any `std::exception`) from within your
   registered function callbacks and letting it propagate back into DuckDB's query executor is the normal
   and supported pattern (DuckDB core itself catches exceptions at the query-execution boundary and turns
   them into `ErrorData`/query failures). This is **not** the same as the C-API extensions
   (`C_STRUCT`/`C_STRUCT_UNSTABLE` ABI, §2.5), where the boundary is a plain-C function-pointer struct and
   error propagation instead goes through explicit `duckdb_scalar_function_set_error`/
   `duckdb_bind_set_error`-style calls — not applicable here since we're using the C++ `ExtensionLoader`
   path.

2. **`duckdb::` symbol visibility / one-definition-rule risk with `EXTENSION_STATIC_BUILD=1`.** Because
   your loadable `.so` statically embeds a **private copy of the entire DuckDB core** (`duckdb_static`),
   and `-Wl,--exclude-libs,ALL` hides those symbols from the dynamic symbol table, you generally cannot
   have two different extensions built this way both `dlopen`'d that expect to *share* DuckDB internal
   state through global/static symbols — each extension gets its own private DuckDB core instance
   embedded, only interacting with the "real" host DuckDB through the registered catalog entries /
   `ExtensionLoader` calls, not through raw symbol sharing. This is by design and not something you need
   to work around, but it does mean **don't assume RTTI/`dynamic_cast` or `typeid` comparisons of DuckDB
   core types will necessarily identify as "the same type"** across the host/extension boundary if
   built with mismatched hidden-visibility settings — stick to the `ExtensionLoader` API surface.

3. **`-DEXTENSION_STATIC_BUILD=1`** is the Makefile default (`EXTENSION_STATIC_BUILD ?= 1`,
   `duckdb_extension.Makefile:117`) and is what makes the resulting `.duckdb_extension` file large but
   portable/self-contained. Setting `EXTENSION_STATIC_BUILD=0` shrinks the binary but produces a file
   that only works when `dlopen`'d by a process that already has the matching DuckDB symbols loaded
   (i.e., the actual `duckdb` binary/library, not some other embedding context) — the doc-comment
   explicitly flags portability risk with `RTLD_LOCAL`-loaded hosts (§6.3). **Recommendation: leave the
   default (`1`) unless you have a specific reason not to.**

4. **`OVERRIDE_GIT_DESCRIBE`.** Used to fake a git version string when building from a source tree that
   isn't a git checkout (e.g. a tarball) — passed straight through as a CMake define
   (`-DOVERRIDE_GIT_DESCRIBE="${OVERRIDE_GIT_DESCRIBE}"`, `duckdb_extension.Makefile:121`). Example from
   the docs (`docs/current/dev/building/build_configuration.md:105-120`):
   ```sh
   OVERRIDE_GIT_DESCRIBE=v0.10.0-843-g09ea97d0a9 GEN=ninja make
   ```
   Not needed for a normal git-checkout-based build (our case, since we're building from the pinned
   `duckdb` submodule checkout) — only relevant if the build environment strips `.git` metadata.

5. **vcpkg is fully avoidable.** Confirmed in three independent places: (a) `duckdb/extension/README.md`
   ("VCPKG is only required for extensions that want to rely on it for dependency management"), (b) the
   `duckdb_extension.Makefile`'s `VCPKG_TOOLCHAIN_PATH?=` gate (only adds
   `-DCMAKE_TOOLCHAIN_FILE=...`/`-DVCPKG_BUILD=1` if the variable is non-empty — §1.6), and (c) live
   knowledge that `find_package(...)` is a plain CMake call that works against system/Homebrew-installed
   CMake config packages (e.g. `VTK-config.cmake`) with **no** vcpkg toolchain file present at all. The
   only friction point is a **build-time warning** (not an error) if a `vcpkg.json` manifest exists in
   your extension dir but `VCPKG_BUILD` isn't defined (`extension_build_tools.cmake:596-598`) — simply
   don't ship a `vcpkg.json` (or delete the template's) if you intend to rely on system-installed VTK via
   `find_package(VTK REQUIRED)`/`-DVTK_DIR=...`, and there is no warning at all.

6. **Extra CMake flags for a system library** (e.g. Homebrew-installed VTK's `VTK_DIR`) are passed through
   `EXT_FLAGS` / `EXT_RELEASE_FLAGS` / `EXT_DEBUG_FLAGS` Make variables (§1.6):
   ```sh
   EXT_FLAGS="-DVTK_DIR=$(brew --prefix vtk)/lib/cmake/vtk-9.3" GEN=ninja make
   ```

7. **Static-vs-shared linking of the extension's *own* dependency (VTK) is a separate concern from
   `EXTENSION_STATIC_BUILD`.** `EXTENSION_STATIC_BUILD` only controls whether **DuckDB itself** is
   statically embedded into your `.duckdb_extension`. Whether VTK is linked statically or dynamically
   into your extension is controlled entirely by whatever `VTK::*` imported targets / `.a`/`.so` files
   `find_package(VTK)` resolves to on the build machine — Homebrew VTK ships shared `.dylib`/`.so`
   libraries by default, meaning your `.duckdb_extension` will have a **runtime dependency on
   libvtk*.so being locatable via the dynamic linker** (rpath/`LD_LIBRARY_PATH`) wherever the extension is
   loaded — this is a real portability constraint worth flagging for distribution (not an issue for local
   dev on the same machine that built it).

---

## 8. Uncertainties / verify at build time

1. **C++ standard actually applied to the extension's own sources (§6.4).** Source reading shows a
   `CACHE`-variable interaction between DuckDB core's `CMAKE_CXX_STANDARD "11"` (root `CMakeLists.txt:54`)
   and the extension-template's own `CMAKE_CXX_STANDARD "17"` (its `CMakeLists.txt:16`), both set without
   `FORCE`. I could not conclusively determine from source alone which value wins for the extension
   target's actual compile flags, though real-world usage of the template with C++14/17 code strongly
   suggests 17 (or at least something newer than 11) does take effect. **Verify by inspecting
   `build/release/CMakeCache.txt` (`CMAKE_CXX_STANDARD:STRING=`) and/or
   `build/release/compile_commands.json` for the `-std=` flag on your extension's `.cpp` files after
   first configure.** If it resolves to 11, explicitly pass `-DCMAKE_CXX_STANDARD=17` via `EXT_FLAGS` (or
   `EXT_RELEASE_FLAGS`) as a safe override — since that's passed as a top-level `cmake -D...` argument
   (not a nested `CACHE` set inside a subdirectory's `CMakeLists.txt`), it is guaranteed to populate the
   cache before *any* subdirectory processing, avoiding the ambiguity entirely.

2. **Exact `LOAD`-time error text/behavior for version/platform mismatches.** I confirmed the *mechanism*
   (`ExtensionHelper::CheckExtensionSignature`/`ParsedExtensionMetaData`, §4.4) and the *general policy*
   ("DuckDB will throw an error and refuse to load the extension" per docs), and confirmed the error text
   for a **missing file path** (`IO Error: Extension "..." not found. Candidate extensions: ...`), but did
   **not** build a real mismatched-version/platform `.duckdb_extension` to capture the exact error string
   DuckDB emits in that case. Worth doing once a real extension binary exists, to write accurate
   `statement error` sqllogictests around it.

3. **`duckdb_extensions()`'s `install_path`/`installed_from` values after a bare `LOAD '/abs/path...'`**
   (as opposed to `INSTALL`+`LOAD`). I confirmed the column set live (§4.5) but did not load a real custom
   extension by explicit path to see exactly what appears in `install_path`/`installed`/`installed_from`
   for that specific flow (docs strongly imply `LOAD` by explicit path bypasses the installed-extensions
   registry entirely, but this wasn't independently re-verified with a toy extension binary).

4. **`test/cpp/` Catch2 wiring in an out-of-tree extension's own `CMakeLists.txt`.** The Catch-test
   *pattern* (`TEST_CASE`, `REQUIRE_NO_FAIL`, `CHECK_COLUMN`) is confirmed from DuckDB core's own docs,
   but the extension-template does not ship a worked example of adding a `test/cpp/*.cpp` Catch test
   target to an out-of-tree extension's `CMakeLists.txt` (only sqllogictest wiring is automatic via
   `TEST_DIR`/the `require <ext>` mechanism). If C++ unit tests are wanted, expect to hand-roll an
   `add_executable`/`target_link_libraries(... duckdb_static)` block and confirm it actually gets built
   and run by `make test` (not verified end-to-end here).

5. **`StorageExtension`/ATTACH registration from `ExtensionLoader`** (§3.5) — the header-level API
   (`StorageExtension::Register(DBConfig&, ...)`) was read and cross-referenced with how `DBConfig` is
   reached elsewhere in `extension_loader.cpp` (`DBConfig::GetConfig(db)`), but no real extension
   exercising ATTACH was built/tested in this research pass.

6. **DUCKDB_EXTENSION_MAIN macro.** The template's `waddle_extension.cpp` still does `#define
   DUCKDB_EXTENSION_MAIN` before including `duckdb.hpp` (§2.1), but grepping every DuckDB v1.5.4 header
   fetched in this research (including the master `duckdb.hpp`, `winapi.hpp`, `extension.hpp`,
   `extension_loader.hpp`, `extension_helper.hpp`, `extension_util.hpp`) found **zero** references to
   `DUCKDB_EXTENSION_MAIN` anywhere. It appears to be a vestigial define from older extension-template
   generations (likely once relevant to the amalgamated single-header DuckDB distribution) that is now a
   harmless no-op. Safe to keep (matches upstream template) or drop; not confirmed to matter either way.

7. **`extension-ci-tools` `v1.5.4` branch vs `v1.5-variegata` branch** — both exist and both target
   DuckDB v1.5.4 per the README table (§1.9); the template's own workflow uses `v1.5-variegata`
   specifically. Not confirmed whether the two branches are identical in content or `v1.5-variegata` is a
   superset/rename — used `v1.5-variegata` throughout this doc since that's what the template's checked-in
   workflow actually references.

8. **Full, unabridged content of `duckdb_extension.Makefile`'s WASM/format/tidy/submodule-helper targets**
   was fetched and exists at `.../scratchpad/research-b/extension-ci-tools/makefiles/duckdb_extension.Makefile`
   but is elided from §1.6 above as out of scope for a native Linux build with a system VTK dependency;
   read directly from that file if WASM or clang-tidy CI wiring is needed later.

9. **A full, from-scratch local build was not performed in this research pass** (out of scope — this is a
   research task, not an implementation task, per instructions). All CMake/Makefile logic above is traced
   through source reading and cross-referenced against live `duckdb` CLI behavior where possible (§4), but
   the actual `make debug`/`make release` invocation with a real VTK dependency has not been dry-run.
   Recommend the implementation phase do a `make debug` with a trivial `find_package(VTK REQUIRED)` early,
   before writing substantial extension code, to shake out any of the above uncertainties concretely.
