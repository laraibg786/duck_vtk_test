#!/usr/bin/env bash
# Build-and-load smoke test for duck_vtk.
#
# This is deliberately more than "it compiled". The checks escalate:
#   1. the artefact exists
#   2. its dynamic dependencies actually resolve (catches missing RPATH)
#   3. it loads into our OWN duckdb build
#   4. it loads into the SYSTEM duckdb CLI  <-- the one users actually have
#   5. VTK is genuinely callable from inside the loaded module
#   6. ATTACH works end to end on a real file
#
# Check 4 only runs when the installed CLI is the SAME DuckDB version this was
# built for; otherwise it skips with an explanation, because an extension refusing
# to load into a different version is the design rather than a defect. When the
# versions DO match and the load fails, that is a real problem — see the message
# there. Use `make check-pin` to see the three versions that must agree.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

BUILD_MODE="${BUILD_MODE:-release}"
EXT="build/${BUILD_MODE}/extension/vtk/vtk.duckdb_extension"
OWN_DUCKDB="build/${BUILD_MODE}/duckdb"

pass() { printf '\033[1;32m  ok  \033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m FAIL \033[0m %s\n' "$*" >&2; exit 1; }
step() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }

# ---------------------------------------------------------------------------
step "1. Extension artefact exists"
# ---------------------------------------------------------------------------
[[ -f "$EXT" ]] || fail "missing $EXT — run 'make ${BUILD_MODE}' first"
pass "$EXT ($(wc -c < "$EXT" | tr -d " ") bytes)"

# ---------------------------------------------------------------------------
step "2. Dynamic dependencies resolve"
# ---------------------------------------------------------------------------
# A 'not found' here means the RPATH is wrong and the module will fail to
# dlopen with a message that does not mention VTK at all — worth catching
# explicitly rather than debugging via a confusing LOAD error.
if command -v ldd >/dev/null 2>&1; then
  if ldd "$EXT" 2>/dev/null | grep -q "not found"; then
    ldd "$EXT" | grep "not found" >&2
    fail "unresolved shared libraries; check DUCK_VTK_LIBRARY_DIR / RPATH in cmake/DuckVTKFindVTK.cmake"
  fi
  vtk_libs=$(ldd "$EXT" 2>/dev/null | grep -c "libvtk" || true)
  pass "all shared libs resolve (${vtk_libs} VTK libraries linked)"
else
  printf '  skip  ldd unavailable\n'
fi

# ---------------------------------------------------------------------------
step "3. Loads into our own duckdb build"
# ---------------------------------------------------------------------------
[[ -x "$OWN_DUCKDB" ]] || fail "missing $OWN_DUCKDB"
own_ver=$("$OWN_DUCKDB" -noheader -list -c "LOAD '${EXT}'; SELECT vtk_version();" 2>&1) \
  || fail "LOAD failed in own build: $own_ver"
[[ -n "$own_ver" ]] || fail "vtk_version() returned empty"
pass "own build loads; vtk_version() = ${own_ver}"

# ---------------------------------------------------------------------------
step "4. Loads into the SYSTEM duckdb CLI"
# ---------------------------------------------------------------------------
if command -v duckdb >/dev/null 2>&1; then
  sys_ver=$(duckdb --version)
  # An extension records the DuckDB version it was built for and LOAD refuses a
  # mismatch. That is not a defect — it is the design — so when the system CLI is a
  # DIFFERENT version from the one we targeted, skip with an explanation instead of
  # reporting a failure. Only a mismatch on the SAME version is a real problem.
  built_ver=$("$OWN_DUCKDB" -noheader -list -c "SELECT version();" 2>/dev/null || echo "?")
  if [[ "$sys_ver" != *"$built_ver"* ]]; then
    printf '  skip  system duckdb is %s but this extension was built for %s\n' "$sys_ver" "$built_ver"
    printf '        (expected: the two only match when DUCKDB_VERSION_TAG equals your\n'
    printf '         installed CLI. To test the system load, build with\n'
    printf '         DUCKDB_VERSION_TAG=<your CLI version> or install a matching CLI.)\n'
  elif sys_out=$(duckdb -unsigned -noheader -list \
        -c "LOAD '${PWD}/${EXT}'; SELECT vtk_version();" 2>&1); then
    pass "system duckdb (${sys_ver}) loads it; vtk_version() = ${sys_out}"
  else
    printf '%s\n' "$sys_out" >&2
    fail "system duckdb (${sys_ver}) is the same version we built for but could NOT load
      the extension. This is a real problem: check the metadata footer via
      OVERRIDE_GIT_DESCRIBE and 'make check-pin'."
  fi
else
  printf '  skip  no system duckdb on PATH\n'
fi

# ---------------------------------------------------------------------------
step "5. VTK is callable from inside the module"
# ---------------------------------------------------------------------------
# vtk_version() returning a real VTK version string proves the VTK libraries are
# not merely linked but initialised and callable. A missing vtk_module_autoinit
# does not break this, so check 6 is what actually covers autoinit.
case "$own_ver" in
  9.*) pass "VTK runtime reports ${own_ver}" ;;
  *)   fail "vtk_version() = '${own_ver}', expected something like 9.6.2" ;;
esac

# ---------------------------------------------------------------------------
step "6. ATTACH round-trip on a real file"
# ---------------------------------------------------------------------------
# This is the autoinit canary: without vtk_module_autoinit the readers are never
# registered and this returns 0 rows / null output while everything above passes.
SAMPLE=""
for cand in \
    test/data/legacy/uGridEx.vtk \
    test/data/legacy/VTKCellTypes.vtk \
    test/data/legacy/*.vtk \
    test/data/xml/*.vtu ; do
  if [[ -f "$cand" ]]; then SAMPLE="$cand"; break; fi
done

if [[ -z "$SAMPLE" ]]; then
  printf '  skip  no corpus file found\n'
  printf '\033[1;33m[warn]\033[0m smoke test incomplete: the ATTACH path was NOT exercised.\n'
  exit 0
fi

# The storage extension is registered as of Phase 3, so an unrecognised TYPE here
# is a real failure — the earlier "skip if not implemented" escape hatch has been
# removed deliberately, since keeping it would let a broken ATTACH pass silently.

npoints=$("$OWN_DUCKDB" -noheader -list -c "
  LOAD '${EXT}';
  ATTACH '${SAMPLE}' AS m (TYPE vtk);
  SELECT count(*) FROM m.points;
" 2>&1) || fail "ATTACH failed on ${SAMPLE}: ${npoints}"

case "$npoints" in
  ''|*[!0-9]*) fail "expected a row count from m.points, got: '${npoints}'" ;;
  0) fail "m.points returned 0 rows for ${SAMPLE}.
      If the file is genuinely non-empty this is the classic missing
      vtk_module_autoinit symptom: readers are not registered, so the reader
      produces empty output instead of data." ;;
  *) pass "ATTACH ${SAMPLE} -> ${npoints} points" ;;
esac

ncells=$("$OWN_DUCKDB" -noheader -list -c "
  LOAD '${EXT}';
  ATTACH '${SAMPLE}' AS m (TYPE vtk);
  SELECT count(*) FROM m.cells;
" 2>&1) || fail "cells scan failed: ${ncells}"
pass "ATTACH ${SAMPLE} -> ${ncells} cells"

printf '\n\033[1;32m=== smoke test passed ===\033[0m\n'
