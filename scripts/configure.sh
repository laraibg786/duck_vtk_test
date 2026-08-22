#!/usr/bin/env bash
# One-command setup for duck_vtk. Run `make configure`.
#
# Everything here is idempotent and safe to re-run. The goal is that a new
# contributor types two commands — `make configure && make release` — and has a
# working extension, with no manual submodule wrangling and no hunting for VTK.
#
# What it does, in order:
#   1. Checks/installs the build toolchain (cmake, ninja, ccache).
#   2. Fetches the duckdb + extension-ci-tools submodules AT THEIR PINNED COMMITS,
#      shallowly. A plain `git submodule update --init` pulls ~1 GB of duckdb
#      history; fetching the single pinned commit is a fraction of that.
#   3. Ensures a usable VTK exists, building a minimal one if necessary.
#   4. Creates the Python venv used as the independent test oracle.
#   5. Prints a summary and the next command.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# NOTE: no hardcoded dependency SHAs live here any more.
#
# There used to be two, and one had rotted: CITOOLS_SHA said b777c70d while the
# recorded gitlink said 72e76e99. Because fetch_pinned() below did `rm -rf` on any
# tree whose HEAD differed from its hardcoded value, `make configure` DELETED a
# correctly-pinned extension-ci-tools and replaced it with a stale one — so the
# documented setup command left you building with a different
# duckdb_extension.Makefile than CI uses.
#
# scripts/bootstrap_deps.sh already derives both SHAs from the gitlinks
# (`git ls-tree HEAD`), which is the one source of truth. This script now calls it
# instead of keeping a second, drifting copy of the same logic.

info() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m  ok\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[warn]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[fail]\033[0m %s\n' "$*" >&2; exit 1; }

# Only the user-scoped bin dir. A previous version also PREPENDED
# /home/linuxbrew/.linuxbrew/bin, which silently decided which cmake, ninja, git and
# duckdb the setup used on any machine that happens to have Linuxbrew — including
# picking a Homebrew toolchain over the system one. That is precisely the libstdc++
# ABI mismatch that building VTK from source exists to avoid.
export PATH="$HOME/.local/bin:$PATH"

# ---------------------------------------------------------------------------
info "1/5  Build toolchain"
# ---------------------------------------------------------------------------
missing=()
for tool in cmake ninja; do
  command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
command -v ccache >/dev/null 2>&1 || missing+=(ccache)

if ((${#missing[@]})); then
  if command -v brew >/dev/null 2>&1; then
    info "installing: ${missing[*]}"
    brew install "${missing[@]}" || warn "brew install reported errors"
  elif command -v apt-get >/dev/null 2>&1; then
    warn "Missing ${missing[*]}. Install with:  sudo apt install cmake ninja-build ccache"
  else
    warn "Missing ${missing[*]} and no known package manager found."
  fi
fi
for tool in cmake ninja; do
  command -v "$tool" >/dev/null 2>&1 && ok "$tool $($tool --version 2>&1 | head -1 | tr -d '\n')" \
                                     || die "$tool is required"
done
command -v ccache >/dev/null 2>&1 && ok "ccache present (large speedup on rebuilds)" \
                                  || warn "ccache absent; rebuilds will be slow"

CXX_BIN="${CXX:-c++}"
ok "compiler: $($CXX_BIN --version 2>&1 | head -1)"

# ---------------------------------------------------------------------------
info "2/5  Submodules at pinned commits"
# ---------------------------------------------------------------------------
./scripts/bootstrap_deps.sh || die "bootstrap_deps.sh failed"

# bootstrap_deps.sh returns early once the marker file exists, so it will not correct
# a dependency that is PRESENT but checked out at the wrong commit. `make configure`
# is the "put my checkout into the right state" command, so verify against the
# gitlink and correct if needed.
gitlink_sha() { git ls-tree HEAD "$1" 2>/dev/null | awk '$2 == "commit" { print $3 }'; }

verify_pin() {
  local dir="$1" url="$2" want have
  want="$(gitlink_sha "$dir")"
  if [[ -z "$want" ]]; then
    warn "$dir has no recorded gitlink; leaving it alone"
    return 0
  fi
  have="$(git -C "$dir" rev-parse HEAD 2>/dev/null || echo none)"
  if [[ "$have" == "$want" ]]; then
    ok "$dir at ${want:0:10} (matches the gitlink)"
    return 0
  fi
  info "$dir is at ${have:0:10} but the repo pins ${want:0:10}; correcting"
  git -C "$dir" fetch -q --depth 1 origin "$want" \
    || die "could not fetch $want for $dir from $url"
  git -C "$dir" checkout -q FETCH_HEAD || die "could not check out $want in $dir"
  ok "$dir now at $(git -C "$dir" rev-parse --short HEAD)"
}

verify_pin duckdb https://github.com/duckdb/duckdb
verify_pin extension-ci-tools https://github.com/duckdb/extension-ci-tools

# The formatting configs are symlinks into the submodule; they only resolve once
# it is present.
for f in .clang-format .clang-tidy .editorconfig; do
  [[ -e "$f" ]] || [[ ! -f "duckdb/$f" ]] || ln -sf "duckdb/$f" "$f"
done

# ---------------------------------------------------------------------------
info "3/5  VTK"
# ---------------------------------------------------------------------------
find_vtk() {
  local d
  for d in "$HOME"/.local/vtk-*/lib/cmake/vtk-* \
           /home/linuxbrew/.linuxbrew/opt/vtk/lib/cmake/vtk-* \
           /opt/homebrew/opt/vtk/lib/cmake/vtk-* \
           /usr/lib/*/cmake/vtk-* \
           /usr/local/lib/cmake/vtk-*; do
    [[ -d "$d" ]] && { echo "$d"; return 0; }
  done
  return 1
}

if VTK_CMAKE_DIR="$(find_vtk)"; then
  ok "found VTK: $VTK_CMAKE_DIR"
else
  warn "no VTK found. Building a minimal one from source (~54 MB download)."
  warn "This is the recommended path: it is ABI-matched to your compiler, which"
  warn "avoids the libstdc++ mismatch risk of a prebuilt binary."
  ./scripts/build_minimal_vtk.sh || die "minimal VTK build failed — see architecture doc §5 for alternatives"
  VTK_CMAKE_DIR="$(find_vtk)" || die "VTK still not found after building"
  ok "built VTK: $VTK_CMAKE_DIR"
fi

# ---------------------------------------------------------------------------
info "4/5  Python oracle"
# ---------------------------------------------------------------------------
# The oracle compares duck_vtk's output against an INDEPENDENT VTK build (the
# PyPI wheel). Without it, correctness can only be spot-checked by hand.
#
# uv/uvx is preferred: no venv to manage and it caches globally.
if command -v uvx >/dev/null 2>&1; then
  ok "uvx present — 'make oracle' will use: uvx --with vtk --with numpy python"
  info "warming the uvx cache in the background (the vtk wheel is large)"
  (uvx --quiet --with vtk --with numpy python -c "import vtk" >/dev/null 2>&1 &) || true
elif [[ -d .venv ]]; then
  ok ".venv already present"
else
  info "creating .venv for the oracle"
  python3 -m venv .venv 2>/dev/null && \
    ./.venv/bin/python -m pip install --quiet --upgrade pip && \
    ./.venv/bin/python -m pip install --quiet vtk numpy \
    && ok "oracle venv ready" \
    || warn "could not create the oracle venv; 'make oracle' will be unavailable"
fi

# ---------------------------------------------------------------------------
info "5/5  Test corpus"
# ---------------------------------------------------------------------------
if [[ -f test/data/MANIFEST.sha256 ]]; then
  # shasum on macOS, which has no sha256sum.
  if command -v sha256sum >/dev/null 2>&1; then SHA256SUM="sha256sum"; else SHA256SUM="shasum -a 256"; fi
  if (cd test/data && $SHA256SUM -c MANIFEST.sha256 >/dev/null 2>&1); then
    ok "corpus verified ($(grep -c . test/data/MANIFEST.sha256) files)"
  else
    warn "corpus checksum mismatch — run: cd test/data && $SHA256SUM -c MANIFEST.sha256"
  fi
else
  warn "no test/data/MANIFEST.sha256 found"
fi

# ---------------------------------------------------------------------------
printf '\n\033[1;32m=== configure complete ===\033[0m\n\n'
printf '  VTK_DIR is auto-detected; override with:  make release VTK_DIR=%s\n' "$VTK_CMAKE_DIR"
printf '\nNext:\n'
printf '  make release          # build the extension (first build takes a while)\n'
printf '  make smoke            # verify it loads, including into the system duckdb\n'
printf '  make check            # full test suite\n'
printf '  make install          # install so `LOAD vtk;` works with no path\n'
