#!/usr/bin/env bash
# Cold-boot build + test of duck_vtk inside the CI container.
#
# Mirrors what the DuckDB community-extensions pipeline does, minus vcpkg (which
# needs network access to a pinned vcpkg checkout; `make ci-verify-vcpkg` covers
# that separately). What this proves:
#
#   * the repo builds from a CLONE, not from the developer's working tree, so an
#     uncommitted file cannot make the build look healthy
#   * with NO preinstalled VTK, NO warm ccache, NO checked-out submodules
#   * `make release` alone is sufficient — the bootstrap fetches what it needs
#   * the full test suite passes in that environment
#
# Every step is loud about which stage failed, because "the Docker build broke" is
# useless on its own.

set -uo pipefail

STAGE=""
stage() { STAGE="$1"; printf '\n\033[1;34m########## %s ##########\033[0m\n' "$1"; }
die() { printf '\n\033[1;31m[FAILED at: %s]\033[0m %s\n' "$STAGE" "${1:-}" >&2; exit 1; }

: "${DUCKDB_VERSION_TAG:=v1.5.5}"
: "${VTK_MODE:=source}"
: "${JOBS:=$(nproc)}"

export CCACHE_DIR="${CCACHE_DIR:-/ccache}"
mkdir -p "$CCACHE_DIR"

stage "0. Environment"
echo "cmake   : $(cmake --version | head -1)"
echo "ninja   : $(ninja --version)"
echo "gcc     : $(gcc --version | head -1)"
echo "duckdb  : $DUCKDB_VERSION_TAG"
echo "vtk mode: $VTK_MODE"
echo "jobs    : $JOBS"
# Deliberately assert the absence of developer state, so this cannot silently
# become a warm build that proves nothing.
if [[ -n "$(ls -A "$HOME"/.local/vtk-* 2>/dev/null)" ]]; then
  die "a prebuilt VTK is present in \$HOME/.local — this is not a cold boot"
fi

stage "1. Clone the repository (not a copy of the working tree)"
rm -rf /work/duck_vtk
git clone --quiet "${REPO_URL:-/src}" /work/duck_vtk || die "clone failed"
cd /work/duck_vtk
if [[ -n "${REPO_REF:-}" && "${REPO_REF}" != "HEAD" ]]; then
  git checkout --quiet "$REPO_REF" || die "could not check out $REPO_REF"
fi
echo "at $(git rev-parse --short HEAD)"
echo "submodule dirs after a plain clone:"
printf '  duckdb/            : %s entries\n' "$(ls -A duckdb 2>/dev/null | wc -l)"
printf '  extension-ci-tools/: %s entries\n' "$(ls -A extension-ci-tools 2>/dev/null | wc -l)"

stage "2. VTK"
if [[ "$VTK_MODE" == "apt" ]]; then
  VTK_CMAKE_DIR="$(ls -d /usr/lib/*/cmake/vtk-* 2>/dev/null | head -1)"
  [[ -n "$VTK_CMAKE_DIR" ]] || die "VTK_MODE=apt but no packaged VTK found"
  echo "using packaged VTK: $VTK_CMAKE_DIR"
  export VTK_DIR="$VTK_CMAKE_DIR"
else
  echo "building minimal VTK from source (this is the slow step)"
  JOBS="$JOBS" ./scripts/build_minimal_vtk.sh >/tmp/vtk_build.log 2>&1 \
    || { tail -40 /tmp/vtk_build.log; die "minimal VTK build failed"; }
  VTK_CMAKE_DIR="$(ls -d "$HOME"/.local/vtk-*/lib/cmake/vtk-* 2>/dev/null | head -1)"
  [[ -n "$VTK_CMAKE_DIR" ]] || die "VTK built but no cmake config found"
  echo "built: $VTK_CMAKE_DIR"
  export VTK_DIR="$VTK_CMAKE_DIR"
fi

stage "3. Build (bootstrap must fetch duckdb + ci-tools by itself)"
# No `make configure` on purpose: `make release` alone has to work, because that is
# all the community-extensions CI runs.
make release -j"$JOBS" >/tmp/build.log 2>&1 || { tail -60 /tmp/build.log; die "make release failed"; }
grep -E "duck_vtk: (VTK_VERSION|StorageExtension::Register|extra flags)" /tmp/build.log || true
EXT=build/release/extension/vtk/vtk.duckdb_extension
[[ -f "$EXT" ]] || die "extension artefact missing"
echo "artefact: $(stat -c%s "$EXT") bytes"

stage "4. Version stamp matches what we built against"
built_for=$(./build/release/duckdb -noheader -list -c "SELECT version();" 2>/dev/null)
echo "duckdb built: $built_for (expected $DUCKDB_VERSION_TAG)"
[[ "$built_for" == "$DUCKDB_VERSION_TAG" ]] || die "version mismatch: $built_for != $DUCKDB_VERSION_TAG"

stage "5. Load + ATTACH"
./build/release/duckdb -unsigned -noheader -list -c "
  LOAD '$PWD/$EXT';
  SELECT vtk_build_info();
  ATTACH 'test/data/legacy/VTKCellTypes.vtk' AS m (TYPE vtk);
  SELECT num_points || '/' || num_cells FROM m.vtk_info;
" || die "load/ATTACH failed"

stage "6. sqllogictest"
./build/release/test/unittest --test-dir . "[sql]" 2>&1 | tail -4
./build/release/test/unittest --test-dir . "[sql]" >/dev/null 2>&1 || die "sqllogictest failed"

stage "7. SQL invariants"
./scripts/run_invariants.sh 2>&1 | tail -2
./scripts/run_invariants.sh >/dev/null 2>&1 || die "invariants failed"

stage "8. API compatibility shim"
./scripts/check_api_compat.sh ./duckdb 2>&1 | tail -3

printf '\n\033[1;32m########## COLD-BOOT BUILD AND TEST SUCCEEDED ##########\033[0m\n'
