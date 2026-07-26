#!/usr/bin/env bash
# Materialise the duckdb / extension-ci-tools build dependencies if they are absent.
#
# WHY THIS IS NOT A CMake FetchContent
# ------------------------------------
# It is tempting to fetch DuckDB with FetchContent and keep the repo submodule-free.
# That cannot work here: DuckDB is the TOP-LEVEL CMake project. The build runs
# `cmake -S ./duckdb -B build/release` with our extension pulled in via
# extension_config.cmake, so `duckdb/CMakeLists.txt` must already exist before CMake
# is invoked at all. FetchContent runs *during* configure — too late to provide the
# thing being configured. Likewise `extension-ci-tools/makefiles/duckdb_extension.Makefile`
# is `include`d by our Makefile, so it must exist before make finishes parsing.
#
# So the earliest correct hook is a make prerequisite, which is what this is.
#
# WHY THE SUBMODULES STILL EXIST
# ------------------------------
# The DuckDB community-extensions CI clones the extension repo with
# `submodules: recursive` and then runs `cd duckdb && git checkout <version>`
# (extension-ci-tools' `set_duckdb_version` target). A `duckdb` git submodule is
# therefore required for submission — without it that step fails outright.
#
# It costs almost nothing: a plain `git clone` of this repo leaves `duckdb/` and
# `extension-ci-tools/` EMPTY. Measured: 26 MB clone, 0 entries in both directories,
# 218 bytes of .gitmodules. This script is what makes such a clone still build with
# a single command, so nobody has to know about `--recursive`.
#
# Idempotent. Safe to run on every build; does nothing once the deps are present.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# Keep in step with .gitmodules, the Makefile's DUCKDB_VERSION_TAG, and
# community-extension/description.yml.
DUCKDB_SHA="${DUCKDB_SHA:-d8cdaa33fda8df955cc76ef58a280f68f4cd43fa}"        # v1.5.5
CITOOLS_SHA="${CITOOLS_SHA:-72e76e99cd7fee45a99739cd118ec2db64e034ec}"      # v1.5-variegata

# Optional local mirrors, used INSTEAD of GitHub when set.
#
# Useful in three situations: a slow or metered link (a duckdb fetch is ~500 MB), an
# air-gapped build, and the Docker cold-boot check, where re-downloading half a
# gigabyte per run makes the check too expensive to run often. Any path git can clone
# from works, including another checkout on the same machine.
#
# This does not weaken the pin: the requested SHA must still exist in the mirror, and
# the checkout is verified afterwards exactly as for a network fetch.
DUCKDB_GIT_MIRROR="${DUCKDB_GIT_MIRROR:-}"
CITOOLS_GIT_MIRROR="${CITOOLS_GIT_MIRROR:-}"

info() { printf '\033[1;34m==>\033[0m %s\n' "$*" >&2; }
ok()   { printf '\033[1;32m  ok\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[fail]\033[0m %s\n' "$*" >&2; exit 1; }

bootstrap() {
  local dir="$1" url="$2" sha="$3" marker="$4"

  if [[ -e "$marker" ]]; then
    return 0 # already usable; say nothing, this runs on every build
  fi

  # Prefer the submodule machinery when this is a git checkout that knows about it:
  # it respects the recorded gitlink, so the version cannot drift from what the
  # repo pins.
  # With an explicit mirror, go straight to the direct fetch: `git submodule update`
  # would use the URL recorded in .gitmodules and hit the network anyway.
  local mirror_in_use=0
  case "$url" in
    http*|git://) ;;
    *) mirror_in_use=1 ;;
  esac

  if [[ $mirror_in_use -eq 0 ]] && [[ -f .gitmodules ]] && git rev-parse --git-dir >/dev/null 2>&1; then
    info "bootstrapping $dir via git submodule (shallow)"
    if git submodule update --init --depth 1 -- "$dir" >&2 2>/dev/null && [[ -e "$marker" ]]; then
      ok "$dir at $(git -C "$dir" rev-parse --short HEAD 2>/dev/null || echo '?')"
      return 0
    fi
    info "submodule update did not produce $marker; falling back to a direct fetch"
  fi

  # Fallback: a plain shallow fetch of the pinned commit. Covers a tarball export,
  # a shallow clone that cannot resolve submodules, and CI that copied the tree.
  info "fetching $dir @ ${sha:0:10} (shallow, single commit)"
  rm -rf "$dir"
  mkdir -p "$dir"
  git -C "$dir" init -q                                  || die "git init failed in $dir"
  git -C "$dir" remote add origin "$url"                 || die "could not add remote for $dir"
  # Fetch only the pinned commit: duckdb's full history is ~1 GB.
  git -C "$dir" fetch -q --depth 1 origin "$sha"         || die "could not fetch $sha from $url"
  git -C "$dir" checkout -q FETCH_HEAD                   || die "could not check out $sha in $dir"
  [[ -e "$marker" ]] || die "$dir was fetched but $marker is still missing"
  ok "$dir at $(git -C "$dir" rev-parse --short HEAD)"
}

bootstrap extension-ci-tools \
         "${CITOOLS_GIT_MIRROR:-https://github.com/duckdb/extension-ci-tools}" \
         "$CITOOLS_SHA" extension-ci-tools/makefiles/duckdb_extension.Makefile

bootstrap duckdb \
         "${DUCKDB_GIT_MIRROR:-https://github.com/duckdb/duckdb}" \
         "$DUCKDB_SHA" duckdb/CMakeLists.txt

# The formatting configs are symlinks into the duckdb submodule and only resolve
# once it is present.
for f in .clang-format .clang-tidy .editorconfig; do
  if [[ ! -e "$f" && -f "duckdb/$f" ]]; then
    ln -sf "duckdb/$f" "$f"
  fi
done
