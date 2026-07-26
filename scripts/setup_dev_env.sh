#!/usr/bin/env bash
# Install everything duck_vtk needs to build and to validate itself.
#
# Two separate concerns:
#   1. The C++ toolchain and VTK, needed to BUILD the extension.
#   2. A Python environment with VTK bindings, needed to VALIDATE it. This is the
#      independent oracle from docs/design/02-validation-and-testing.md — the
#      extension must never be checked against itself, so this is not optional.
#
# Idempotent: safe to re-run.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

info()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn()  { printf '\033[1;33m[warn]\033[0m %s\n' "$*"; }
die()   { printf '\033[1;31m[fail]\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 1. Build toolchain
# ---------------------------------------------------------------------------
if command -v brew >/dev/null 2>&1; then
  info "Installing build toolchain via Homebrew"
  # ccache is not strictly required but DuckDB is a large build and it turns a
  # 10-minute rebuild into a 30-second one; well worth the disk.
  brew install cmake ninja ccache || warn "brew toolchain install reported errors"

  if ! brew list --versions vtk >/dev/null 2>&1; then
    info "Installing VTK via Homebrew (large: pulls Qt/mesa/llvm as bottle deps)"
    # Homebrew's ghcr.io downloads intermittently fail with HTTP/2 PROTOCOL_ERROR.
    # Downloads resume, so retrying is effective.
    HOMEBREW_CURL_RETRIES=5 brew install vtk \
      || HOMEBREW_CURL_RETRIES=5 brew install vtk \
      || warn "brew install vtk failed; see the VTK fallbacks below"
  else
    info "VTK already installed: $(brew list --versions vtk)"
  fi
else
  warn "Homebrew not found; skipping toolchain install"
fi

# ---------------------------------------------------------------------------
# 2. Verify VTK is discoverable, and explain the alternatives if not
# ---------------------------------------------------------------------------
VTK_CMAKE_DIR="$(ls -d /home/linuxbrew/.linuxbrew/opt/vtk/lib/cmake/vtk-* \
                    /opt/homebrew/opt/vtk/lib/cmake/vtk-* \
                    /usr/lib/*/cmake/vtk-* \
                    /usr/local/lib/cmake/vtk-* 2>/dev/null | head -1 || true)"

if [[ -n "$VTK_CMAKE_DIR" ]]; then
  info "Found VTK CMake config: $VTK_CMAKE_DIR"
else
  warn "No VTK CMake config found. Options, in order of preference:"
  cat <<'EOF'
    a) Homebrew (no root required, VTK 9.6.x):
         HOMEBREW_CURL_RETRIES=5 brew install vtk
       Note this pulls Qt, mesa and llvm as bottle dependencies (~2 GB).

    b) Debian/Ubuntu system package (VTK 9.3, ABI-matched to system GCC —
       this is the safest choice if you hit a libstdc++ mismatch with the
       Homebrew bottle). Requires root:
         sudo apt install libvtk9-dev

    c) Minimal source build (smallest footprint, no Qt/mesa/llvm, and the
       right choice if you later want a statically linked, distributable
       extension). See docs/design/03-architecture-and-roadmap.md §5.

    Then pass the location explicitly if auto-detection still fails:
         make release EXT_FLAGS='-DVTK_DIR=/path/to/lib/cmake/vtk-9.6'
EOF
fi

# ---------------------------------------------------------------------------
# 3. Submodules
# ---------------------------------------------------------------------------
if [[ -f .gitmodules ]]; then
  info "Initialising submodules (duckdb, extension-ci-tools)"
  # --depth 1 keeps this to a few hundred MB instead of a couple of GB.
  git submodule update --init --recursive --depth 1 \
    || die "submodule init failed"
  if [[ -d duckdb ]]; then
    info "duckdb submodule at $(git -C duckdb rev-parse --short HEAD)"
    # This pin must match the duckdb the extension will be LOADED into.
    # 08e34c447b == v1.5.4 == the Homebrew duckdb CLI build.
    if ! git -C duckdb rev-parse HEAD | grep -q '^08e34c447bae34eaee3723cac61f2878b6bdf787'; then
      warn "duckdb submodule is NOT at 08e34c447b (v1.5.4)."
      warn "The built extension may fail to load in the system 'duckdb' CLI."
    fi
  fi
else
  warn "No .gitmodules yet — see docs/plan for the Phase 1 bootstrap steps."
fi

# ---------------------------------------------------------------------------
# 4. Python oracle environment
# ---------------------------------------------------------------------------
info "Creating Python venv for the validation oracle"
if [[ ! -d .venv ]]; then
  python3 -m venv .venv || die "could not create venv"
fi
./.venv/bin/python -m pip install --quiet --upgrade pip

# vtk from PyPI is a self-contained wheel: it does NOT reuse the C++ VTK we build
# against. That independence is exactly what makes it a valid oracle — a shared
# bug in one VTK build cannot mask itself on both sides of the comparison.
# meshio is a genuinely separate implementation, so it serves as a second opinion
# on the formats it supports.
info "Installing vtk + meshio into .venv (this is the independent test oracle)"
./.venv/bin/python -m pip install --quiet vtk meshio numpy \
  || warn "oracle install failed — 'make oracle' will not work until this succeeds"

if ./.venv/bin/python -c "import vtk; print('python vtk', vtk.VTK_VERSION)" 2>/dev/null; then
  info "Oracle ready: $(./.venv/bin/python -c 'import vtk; print(vtk.VTK_VERSION)')"
else
  warn "Python VTK not importable; the L3 oracle layer will be unavailable."
  warn "Do NOT commit test expectations that were never verified against an oracle."
fi

# ---------------------------------------------------------------------------
# 5. Report
# ---------------------------------------------------------------------------
info "Environment summary"
printf '  %-22s %s\n' "cmake"        "$(command -v cmake  >/dev/null && cmake --version | head -1 || echo MISSING)"
printf '  %-22s %s\n' "ninja"        "$(command -v ninja  >/dev/null && ninja --version   || echo MISSING)"
printf '  %-22s %s\n' "ccache"       "$(command -v ccache >/dev/null && ccache --version | head -1 || echo MISSING)"
printf '  %-22s %s\n' "c++"          "$( (c++ --version 2>/dev/null || g++ --version 2>/dev/null) | head -1 || echo MISSING)"
printf '  %-22s %s\n' "system duckdb" "$(command -v duckdb >/dev/null && duckdb --version || echo MISSING)"
printf '  %-22s %s\n' "VTK cmake dir" "${VTK_CMAKE_DIR:-MISSING}"
printf '  %-22s %s\n' "python oracle" "$(./.venv/bin/python -c 'import vtk;print(vtk.VTK_VERSION)' 2>/dev/null || echo MISSING)"

info "Next: 'make phase0' to validate the VTK ABI, then 'make data', then 'make release'."
