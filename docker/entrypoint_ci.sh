#!/usr/bin/env bash
# Cold-boot build + test of duck_vtk inside the CI container.
#
# Mirrors what the DuckDB community-extensions pipeline does, minus vcpkg — that is
# covered by `make ci-verify-vcpkg`, which clones the pinned vcpkg and builds VTK
# through our overlay port. What THIS proves:
#
#   * the repo builds from a CLONE, not from the developer's working tree, so an
#     uncommitted file cannot make the build look healthy
#   * with NO preinstalled VTK, NO warm ccache, NO checked-out submodules
#   * `make release` alone is sufficient — the bootstrap fetches what it needs
#   * the full test suite passes in that environment
#
# Every step is loud about which stage failed, because "the Docker build broke" is
# useless on its own.

# -e matters here. Without it, every unguarded command merely printed its error and
# the script carried on to announce success — which is how stage 8 came to be unable
# to fail at all.
set -euo pipefail

# Logs go to mktemp files rather than fixed /tmp names: this script also runs on
# developer workstations, where a predictable /tmp/build.log is a symlink-attack
# target.
# One temp DIRECTORY, removed wholesale. The previous version kept an array and
# appended to it from newlog() — but newlog() is always called as `x=$(newlog)`, and
# a command substitution runs in a SUBSHELL, so the parent's array stayed empty and
# nothing was ever cleaned up.
_LOGDIR=$(mktemp -d)
trap 'rm -rf "$_LOGDIR"' EXIT
newlog() { printf '%s/%s.log' "$_LOGDIR" "$1"; }

STAGE=""
stage() { STAGE="$1"; printf '\n\033[1;34m########## %s ##########\033[0m\n' "$1"; }
die() { printf '\n\033[1;31m[FAILED at: %s]\033[0m %s\n' "$STAGE" "${1:-}" >&2; exit 1; }

: "${DUCKDB_VERSION_TAG:=v1.5.5}"
: "${JOBS:=$(nproc)}"

export CCACHE_DIR="${CCACHE_DIR:-/ccache}"
mkdir -p "$CCACHE_DIR"

stage "0. Environment"
echo "cmake   : $(cmake --version | head -1)"
echo "ninja   : $(ninja --version)"
echo "gcc     : $(gcc --version | head -1)"
echo "duckdb  : $DUCKDB_VERSION_TAG"
echo "jobs    : $JOBS"
# Deliberately assert the absence of developer state, so this cannot silently
# become a warm build that proves nothing.
if [[ -n "$(ls -A "$HOME"/.local/vtk-* 2>/dev/null)" ]]; then
  die "a prebuilt VTK is present in \$HOME/.local — this is not a cold boot"
fi

stage "1. Clone the repository (not a copy of the working tree)"
# The source is bind-mounted from the host, so its files are owned by a UID that
# does not exist in this container and git refuses to touch it ("detected dubious
# ownership"). Marking just the mount read-safe is the narrow fix; the container is
# ephemeral and the mount is read-only.
git config --global --add safe.directory "${REPO_URL:-/src}" 2>/dev/null || true
git config --global --add safe.directory "${REPO_URL:-/src}/.git" 2>/dev/null || true
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

stage "2. Build minimal VTK from source"
# There is deliberately no "use the distro package" mode. Ubuntu ships VTK 9.1,
# which parses no <AppendedData> section yet reports success — the silent-empty-mesh
# trap documented in cmake/DuckVTKFindVTK.cmake. The 9.6 floor rejects it outright,
# so such a mode could only ever fail; it was removed rather than left to rot.
echo "building minimal VTK from source (this is the slow step)"
_vtk_log=$(newlog vtk)
if ! VTK_DIR=$(JOBS="$JOBS" ./scripts/build_minimal_vtk.sh 2>"$_vtk_log"); then
  tail -40 "$_vtk_log"
  die "minimal VTK build failed"
fi
export VTK_DIR
echo "built: $VTK_DIR"

stage "3. Build (bootstrap must obtain duckdb + ci-tools by itself)"
# A mirror only changes WHERE the pinned commit comes from; the SHA and the
# post-checkout verification are unchanged. It is opt-in because it means this run
# no longer exercises the network fetch — the build and tests are still fully cold.
if [[ -n "${DUCKDB_GIT_MIRROR:-}" ]]; then
  echo "duckdb source   : local mirror ${DUCKDB_GIT_MIRROR} (network fetch NOT exercised)"
  # The mirror is a bare-style git directory, so mark it (not a .git child) safe.
  git config --global --add safe.directory "${DUCKDB_GIT_MIRROR}" 2>/dev/null || true
else
  echo "duckdb source   : github (full network fetch, ~500 MB)"
fi
if [[ -n "${CITOOLS_GIT_MIRROR:-}" ]]; then
  git config --global --add safe.directory "${CITOOLS_GIT_MIRROR}" 2>/dev/null || true
fi
# A bind-mounted git directory carries the host's UID, which git refuses to read.
git config --global --add safe.directory '*' 2>/dev/null || true
export DUCKDB_GIT_MIRROR CITOOLS_GIT_MIRROR DUCKDB_SHA CITOOLS_SHA
# No `make configure` on purpose: `make release` alone has to work, because that is
# all the community-extensions CI runs.
_build_log=$(newlog build)
make release -j"$JOBS" >"$_build_log" 2>&1 || { tail -60 "$_build_log"; die "make release failed"; }
grep -E "duck_vtk: (VTK_VERSION|StorageExtension::Register|extra flags)" "$_build_log" || true
EXT=build/release/extension/vtk/vtk.duckdb_extension
[[ -f "$EXT" ]] || die "extension artefact missing"
echo "artefact: $(stat -c%s "$EXT") bytes"

stage "4. Version stamp matches what we built against"
built_for=$(./build/release/duckdb -noheader -list -c "SELECT version();" 2>/dev/null) \
  || die "could not query the freshly built duckdb"
echo "duckdb built: $built_for (expected $DUCKDB_VERSION_TAG)"
[[ "$built_for" == "$DUCKDB_VERSION_TAG" ]] || die "version mismatch: $built_for != $DUCKDB_VERSION_TAG"

stage "5. Load + ATTACH"
./build/release/duckdb -unsigned -noheader -list -c "
  LOAD '$PWD/$EXT';
  SELECT vtk_build_info();
  ATTACH 'test/data/legacy/VTKCellTypes.vtk' AS m (TYPE vtk);
  SELECT num_points || '/' || num_cells FROM m.vtk_info;
" || die "load/ATTACH failed"

# Each suite below runs EXACTLY ONCE. Previously each ran twice — once piped to
# `tail` for the summary (which discards the exit status) and once to /dev/null
# purely to recover that status. That doubled the cold-boot test time, and worse,
# the run whose output you read was not the run that decided pass/fail.
stage "6. Corpus integrity"
# The cold boot works from a fresh clone, so this also proves the corpus survived
# git transfer intact.
(cd test/data && sha256sum -c --quiet MANIFEST.sha256) || die "corpus checksum mismatch"
echo "corpus verified"

stage "7. sqllogictest"
_sql_log=$(newlog sql)
if ! ./build/release/test/unittest --test-dir . "[sql]" >"$_sql_log" 2>&1; then
  tail -40 "$_sql_log"; die "sqllogictest failed"
fi
tail -4 "$_sql_log"

stage "8. SQL invariants"
_inv_log=$(newlog invariants)
if ! ./scripts/run_invariants.sh >"$_inv_log" 2>&1; then
  tail -40 "$_inv_log"; die "invariants failed"
fi
tail -2 "$_inv_log"

stage "9. In-memory parse path (the route remote files take)"
# Not covered by stage 7: DUCK_VTK_FORCE_MEMORY_READ reroutes local reads through
# the in-memory parser, which is otherwise only reachable with httpfs loaded and so
# went entirely untested.
_mem_log=$(newlog memory)
if ! DUCK_VTK_FORCE_MEMORY_READ=1 ./build/release/test/unittest --test-dir . "[sql]" >"$_mem_log" 2>&1; then
  tail -40 "$_mem_log"; die "in-memory read path failed"
fi
tail -4 "$_mem_log"

stage "10. API compatibility shim"
# This used to be an unchecked `| tail -3`, so the cold boot announced success even
# when the 1.4/1.5 shim no longer compiled — in the one job that gates submission.
_api_log=$(newlog apicompat)
if ! ./scripts/check_api_compat.sh ./duckdb >"$_api_log" 2>&1; then
  tail -20 "$_api_log"; die "API compatibility shim failed"
fi
tail -3 "$_api_log"

printf '\n\033[1;32m########## COLD-BOOT BUILD AND TEST SUCCEEDED ##########\033[0m\n'
