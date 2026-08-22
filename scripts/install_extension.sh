#!/usr/bin/env bash
# Install the built extension into DuckDB's local extension directory so that
# `LOAD vtk;` works without a filesystem path.
#
# DuckDB looks for locally installed extensions at:
#     <extension_directory>/<duckdb_version>/<platform>/<name>.duckdb_extension
# defaulting to ~/.duckdb/extensions. Both the version and the platform must match
# the CLI exactly, so they are queried from the target binary rather than guessed.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# NOTE: this script used to PREPEND /home/linuxbrew/.linuxbrew/bin to PATH. That
# silently decided which cmake, ninja, curl, git and duckdb ran on any machine with
# Linuxbrew installed, overriding the system toolchain. If you want a Homebrew
# toolchain, put it on PATH yourself.

EXT_NAME="vtk"
EXT_BUILT="build/release/extension/${EXT_NAME}/${EXT_NAME}.duckdb_extension"
DUCKDB_BIN="${DUCKDB_BIN:-$(command -v duckdb || echo ./build/release/duckdb)}"
UNINSTALL=0
[[ "${1:-}" == "--uninstall" ]] && UNINSTALL=1

info() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m  ok\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[fail]\033[0m %s\n' "$*" >&2; exit 1; }

[[ -x "$DUCKDB_BIN" ]] || die "no duckdb binary found (set DUCKDB_BIN=...)"

# Ask the target binary, do not assume. An extension installed under the wrong
# version or platform directory is silently ignored by LOAD, which is a
# maddening failure mode.
VERSION="$("$DUCKDB_BIN" -noheader -list -c "SELECT version();" 2>/dev/null | tr -d '[:space:]')"
PLATFORM="$("$DUCKDB_BIN" -noheader -list -c "SELECT platform FROM pragma_platform();" 2>/dev/null | tr -d '[:space:]')"
[[ -n "$VERSION" && -n "$PLATFORM" ]] || die "could not determine duckdb version/platform from $DUCKDB_BIN"

EXT_DIR="${DUCKDB_EXTENSION_DIRECTORY:-$HOME/.duckdb/extensions}/${VERSION}/${PLATFORM}"
TARGET="${EXT_DIR}/${EXT_NAME}.duckdb_extension"

if (( UNINSTALL )); then
  if [[ -f "$TARGET" ]]; then
    rm -f "$TARGET"
    ok "removed $TARGET"
  else
    ok "nothing installed at $TARGET"
  fi
  exit 0
fi

[[ -f "$EXT_BUILT" ]] || die "$EXT_BUILT not found — run 'make release' first"

info "duckdb   : $DUCKDB_BIN ($VERSION, $PLATFORM)"
info "installing to $TARGET"
mkdir -p "$EXT_DIR"
cp -f "$EXT_BUILT" "$TARGET"
ok "installed ($(stat -c%s "$TARGET") bytes)"

# Verify by actually loading it by NAME, which is the thing the user will do and
# the only check that proves the version/platform directory is right.
info "verifying 'LOAD vtk' by name"
if out=$("$DUCKDB_BIN" -unsigned -noheader -list -c "LOAD ${EXT_NAME}; SELECT vtk_version();" 2>&1); then
  ok "LOAD ${EXT_NAME} works; VTK ${out}"
else
  printf '%s\n' "$out" >&2
  die "installed, but 'LOAD ${EXT_NAME}' failed"
fi

cat <<EOF

$(printf '\033[1;32m=== installed ===\033[0m')

Because locally built extensions are unsigned, DuckDB needs to be told to allow
them. Either start the CLI with -unsigned:

    duckdb -unsigned
    D LOAD vtk;
    D ATTACH 'mesh.vtu' AS m (TYPE vtk);
    D SELECT cell_type_name, count(*) FROM m.cells GROUP BY 1;

or set it per-session before loading:

    SET allow_unsigned_extensions = true;
    LOAD vtk;

From Python:

    import duckdb
    con = duckdb.connect(config={'allow_unsigned_extensions': True})
    con.load_extension('vtk')

Uninstall with:  make uninstall
EOF
