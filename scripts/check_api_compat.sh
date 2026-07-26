#!/usr/bin/env bash
# Verify the DuckDB version shim in VtkRegisterStorageExtension, cheaply.
#
# WHY THIS EXISTS
# ---------------
# duck_vtk supports both the DuckDB 1.4 LTS line and 1.5.x. They differ in exactly
# one API — storage-extension registration — selected at configure time by a probe
# that greps the real DuckDB header:
#
#   1.4.x : DBConfig::storage_extensions["vtk"] = make_uniq<...>()   (public map)
#   1.5.x : StorageExtension::Register(config, "vtk", shared_ptr)    (static helper)
#
# A full build against each version takes ~30 minutes. A `-fsyntax-only` compile of
# the catalog translation unit answers the same question in seconds, and does it
# more rigorously: it checks the full 2x2 matrix, proving each branch compiles with
# its own version AND is REJECTED by the other. Without that second half, a shim
# could be silently redundant (both branches valid everywhere) and nobody would know.
#
# Usage:
#   ./scripts/check_api_compat.sh <duckdb-src-A> [<duckdb-src-B> ...]
#   ./scripts/check_api_compat.sh ./duckdb /tmp/duckdb-1.4.5

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

TARGET_SRC="src/catalog/vtk_catalog.cpp"
CXX_BIN="${CXX:-g++}"

find_vtk_include() {
  local d
  for d in "$HOME"/.local/vtk-*/include/vtk-* \
           /home/linuxbrew/.linuxbrew/opt/vtk/include/vtk-* \
           /usr/include/vtk-*; do
    [[ -d "$d" ]] && { echo "$d"; return 0; }
  done
  return 1
}

VTK_INC="$(find_vtk_include)" || { echo "FATAL: no VTK include dir found" >&2; exit 2; }

includes_for() {
  local d="$1"
  printf -- "-Isrc/include -I%s/src/include -I%s" "$d" "$VTK_INC"
  # DuckDB's public headers pull in several vendored third-party headers.
  for tp in fmt/include re2 utf8proc/include concurrentqueue fastpforlib fast_float mbedtls/include; do
    [[ -d "$d/third_party/${tp%%/*}" ]] && printf -- " -I%s/third_party/%s" "$d" "$tp"
  done
}

# Reports whether the header actually has the 1.5-style static Register, i.e. what
# CMakeLists.txt's probe would conclude.
probe_expected() {
  local d="$1"
  if grep -qE "static[[:space:]]+void[[:space:]]+Register[[:space:]]*\(" \
       "$d/src/include/duckdb/storage/storage_extension.hpp" 2>/dev/null; then
    echo 1
  else
    echo 0
  fi
}

compiles() { # dir, flag
  $CXX_BIN -fsyntax-only -std=c++17 $(includes_for "$1") \
    "-DDUCK_VTK_HAS_STORAGE_EXTENSION_REGISTER=$2" '-DDUCK_VTK_VERSION="compat-check"' \
    "$TARGET_SRC" >/dev/null 2>&1
}

(( $# )) || { echo "usage: $0 <duckdb-src-dir> [...]" >&2; exit 2; }

echo "compiler : $($CXX_BIN --version | head -1)"
echo "vtk      : $VTK_INC"
echo "target   : $TARGET_SRC"
echo

failures=0
for src in "$@"; do
  if [[ ! -f "$src/src/include/duckdb/storage/storage_extension.hpp" ]]; then
    echo "SKIP $src (not a DuckDB source tree)"; continue
  fi
  expected="$(probe_expected "$src")"
  style=$([[ "$expected" == 1 ]] && echo "1.5-style static Register" || echo "1.4-style storage_extensions map")
  echo "=== $src"
  echo "    probe says: DUCK_VTK_HAS_STORAGE_EXTENSION_REGISTER=$expected ($style)"

  other=$(( 1 - expected ))

  if compiles "$src" "$expected"; then
    echo "    ok       matching branch ($expected) compiles"
  else
    echo "    FAIL     matching branch ($expected) does NOT compile"
    $CXX_BIN -fsyntax-only -std=c++17 $(includes_for "$src") \
      "-DDUCK_VTK_HAS_STORAGE_EXTENSION_REGISTER=$expected" '-DDUCK_VTK_VERSION="x"' \
      "$TARGET_SRC" 2>&1 | grep -E "error:" | head -5
    failures=$((failures+1))
  fi

  if compiles "$src" "$other"; then
    # Not fatal, but it means the shim is doing nothing for this version and the
    # probe is untested — worth knowing rather than assuming.
    echo "    WARN     the OTHER branch ($other) also compiles; the shim is redundant here"
  else
    echo "    ok       the other branch ($other) is correctly rejected"
  fi
  echo
done

if (( failures )); then
  echo "$failures version(s) failed"; exit 1
fi
echo "all versions OK"
