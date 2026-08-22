#!/usr/bin/env bash
# Build duck_vtk through vcpkg, exactly as the community-extensions CI does.
#
# WHY THIS IS THE MOST IMPORTANT REMAINING CHECK
#   Everything else verifies the extension against a VTK we built ourselves with
#   plain CMake. Their CI instead builds VTK through our vcpkg overlay port
#   (vcpkg_ports/vtk-minimal) under a vcpkg triplet. The CMake FLAGS in that port
#   are the ones already proven locally, but the vcpkg plumbing around them —
#   vcpkg_cmake_configure, vcpkg_cmake_config_fixup's CONFIG_PATH, the triplet's
#   static/dynamic linkage, MAYBE_UNUSED_VARIABLES — has no other coverage.
#
#   If the submission is going to fail, this is where it fails.
#
# COST
#   Clones vcpkg (~200 MB) and builds VTK from source under it. Tens of minutes on a
#   good connection, and it needs a lot of disk. Not part of `make check`.
#
# Usage:
#   ./scripts/verify_vcpkg_port.sh [vcpkg-commit]

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# Default matches what community-extensions pins for DuckDB v1.5.5. See
# duckdb/community-extensions/.github/workflows/build.yml.
VCPKG_COMMIT="${1:-84bab45d415d22042bd0b9081aea57f362da3f35}"
VCPKG_ROOT="${VCPKG_ROOT:-$HOME/.cache/duck-vtk-vcpkg}"
TRIPLET="${VCPKG_TARGET_TRIPLET:-x64-linux}"

info() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m  ok\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[fail]\033[0m %s\n' "$*" >&2; exit 1; }

# NOTE: this script used to PREPEND /home/linuxbrew/.linuxbrew/bin to PATH. That
# silently decided which cmake, ninja, curl, git and duckdb ran on any machine with
# Linuxbrew installed, overriding the system toolchain. If you want a Homebrew
# toolchain, put it on PATH yourself.

info "vcpkg commit : $VCPKG_COMMIT"
info "vcpkg root   : $VCPKG_ROOT"
info "triplet      : $TRIPLET"

# ---------------------------------------------------------------------------
info "1/4  vcpkg checkout"
# ---------------------------------------------------------------------------
if [[ ! -d "$VCPKG_ROOT/.git" ]]; then
  mkdir -p "$VCPKG_ROOT"
  git -C "$VCPKG_ROOT" init -q
  git -C "$VCPKG_ROOT" remote add origin https://github.com/microsoft/vcpkg.git
fi
if [[ "$(git -C "$VCPKG_ROOT" rev-parse HEAD 2>/dev/null)" != "$VCPKG_COMMIT" ]]; then
  info "fetching the pinned commit (shallow)"
  git -C "$VCPKG_ROOT" fetch -q --depth 1 origin "$VCPKG_COMMIT" || die "vcpkg fetch failed"
  git -C "$VCPKG_ROOT" checkout -q FETCH_HEAD || die "vcpkg checkout failed"
fi
ok "vcpkg at $(git -C "$VCPKG_ROOT" rev-parse --short HEAD)"

if [[ ! -x "$VCPKG_ROOT/vcpkg" ]]; then
  info "bootstrapping vcpkg"
  "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics >/tmp/vcpkg_bootstrap.log 2>&1 \
    || { tail -20 /tmp/vcpkg_bootstrap.log; die "vcpkg bootstrap failed"; }
fi
ok "vcpkg binary ready"

# ---------------------------------------------------------------------------
info "2/4  build the vtk-minimal overlay port on its own"
# ---------------------------------------------------------------------------
# Installing the port alone first means a port failure is reported as a port
# failure, instead of surfacing later as a confusing find_package error.
"$VCPKG_ROOT/vcpkg" install "vtk-minimal:${TRIPLET}" \
  --overlay-ports=./vcpkg_ports \
  --x-install-root="$VCPKG_ROOT/installed" \
  >/tmp/vcpkg_vtk.log 2>&1 \
  || { echo "--- last 60 lines ---"; tail -60 /tmp/vcpkg_vtk.log; die "vtk-minimal failed to build under vcpkg"; }
ok "vtk-minimal built"

inst="$VCPKG_ROOT/installed/${TRIPLET}"
cfg=$(ls -d "$inst"/share/vtk 2>/dev/null | head -1)
[[ -n "$cfg" ]] || cfg=$(ls -d "$inst"/share/vtk* 2>/dev/null | head -1)
if [[ -z "$cfg" ]]; then
  echo "installed tree:"; find "$inst/share" -maxdepth 1 -type d | head -20
  die "no VTK cmake config in the installed tree — check vcpkg_cmake_config_fixup's CONFIG_PATH"
fi
ok "VTK cmake config at $cfg"
info "libraries installed: $(ls "$inst"/lib/*vtk* 2>/dev/null | wc -l)"
info "rendering libs present (must be 0): $(ls "$inst"/lib 2>/dev/null | grep -ci 'Rendering\|OpenGL')"

# ---------------------------------------------------------------------------
info "3/4  build the extension through the vcpkg toolchain"
# ---------------------------------------------------------------------------
export VCPKG_TOOLCHAIN_PATH="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
export VCPKG_TARGET_TRIPLET="$TRIPLET"
[[ -f "$VCPKG_TOOLCHAIN_PATH" ]] || die "toolchain file missing at $VCPKG_TOOLCHAIN_PATH"

# Unset VTK_DIR so the build MUST resolve VTK through vcpkg. Leaving it set would
# make this check silently pass against the locally built VTK instead.
unset VTK_DIR
make release >/tmp/vcpkg_ext.log 2>&1 \
  || { echo "--- last 60 lines ---"; tail -60 /tmp/vcpkg_ext.log; die "extension build under vcpkg failed"; }
grep -E "duck_vtk: (vcpkg detected|VTK_VERSION|VTK_DIR)" /tmp/vcpkg_ext.log || true
ok "extension built against the vcpkg-provided VTK"

# ---------------------------------------------------------------------------
info "4/4  test"
# ---------------------------------------------------------------------------
./build/release/test/unittest --test-dir . "[sql]" 2>&1 | tail -3
./build/release/test/unittest --test-dir . "[sql]" >/dev/null 2>&1 || die "tests failed"
./scripts/run_invariants.sh 2>&1 | tail -1

printf '\n\033[1;32m=== vcpkg path verified: the submission build should succeed ===\033[0m\n'
