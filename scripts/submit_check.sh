#!/usr/bin/env bash
# Pre-submission gate for DuckDB community extensions.
#
# Checks the things that are cheap to get wrong and expensive to discover in a
# review cycle: an unfilled TODO in the descriptor, an excluded-platform list that
# has drifted between the descriptor and CI, a module enabled in our CMake but not
# in the vcpkg port, an unrecorded submodule bump.
#
# Deliberately does NOT build. `make ci-verify` does the cold-boot build in Docker
# and `make check` runs the test suite; this is the paperwork check that runs in a
# second and can be run offline.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

DESC="community-extension/description.yml"
CI=".github/workflows/ci.yml"

fails=0
warns=0
ok()   { printf '\033[1;32m  ok  \033[0m %s\n' "$*"; }
bad()  { printf '\033[1;31m FAIL \033[0m %s\n' "$*"; fails=$((fails+1)); }
warn() { printf '\033[1;33m warn \033[0m %s\n' "$*"; warns=$((warns+1)); }
step() { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }

PY=python3
command -v "$PY" >/dev/null 2>&1 || { echo "python3 required"; exit 2; }

# A python that can actually import yaml. Debian's python3 has no pyyaml, so fall
# back to an ad-hoc uvx environment — the same escalation the descriptor parse does
# below, hoisted here so later checks can reuse it. Empty if neither works, in which
# case YAML-dependent checks warn rather than fail.
YAML_PY=""
if "$PY" -c "import yaml" 2>/dev/null; then
  YAML_PY="$PY"
elif command -v uvx >/dev/null 2>&1 \
     && uvx --quiet --with pyyaml python -c "import yaml" 2>/dev/null; then
  YAML_PY="uvx --quiet --with pyyaml python"
fi

step "Descriptor exists and parses"
[[ -f "$DESC" ]] || { bad "$DESC missing"; exit 1; }
# NOTE: capture the status explicitly rather than `if ! cmd`. With `!`, bash
# replaces $? with the inverted value (0/1), which would swallow the exit code 3
# this uses to mean "no YAML parser available" — and report a perfectly valid file
# as malformed.
"$PY" - "$DESC" <<'EOF' 2>/dev/null
import sys
try:
    import yaml
except ImportError:
    sys.exit(3)   # distinguish "no parser" from "bad file"
yaml.safe_load(open(sys.argv[1]))
EOF
yaml_rc=$?
case "$yaml_rc" in
  0) ok "$DESC parses" ;;
  3) # Retry through uvx if available: worth doing, because "cannot check" is a
     # much weaker result than an actual parse.
     if command -v uvx >/dev/null 2>&1 \
        && uvx --quiet --with pyyaml python -c "import yaml,sys;yaml.safe_load(open(sys.argv[1]))" "$DESC" 2>/dev/null; then
       ok "$DESC parses (via uvx)"
     else
       warn "no YAML parser available; skipped the parse check"
     fi ;;
  *) bad "$DESC is not valid YAML" ;;
esac

step "No unfilled TODOs in the descriptor"
# The anchor must account for the "NN:" prefix that `grep -n` adds, or the
# comment filter matches nothing and every explanatory comment mentioning TODO
# counts as an unfilled placeholder. It was `^\s*#`, which meant this check
# would have stayed RED even after all three values were filled in — while the
# display line below (correctly anchored) printed nothing, making the failure
# look like a bug in this script.
if grep -nE "TODO" "$DESC" | grep -vE "^[0-9]+:\s*#" | grep -q .; then
  grep -nE "TODO" "$DESC" | grep -vE "^[0-9]+:\s*#" | sed 's/^/       /'
  bad "the descriptor still has TODO placeholders (repo.github, repo.ref, maintainers)"
else
  ok "no TODO placeholders"
fi

step "Required descriptor fields present"
for key in "name:" "description:" "version:" "language:" "build:" "license:" "maintainers:"; do
  grep -qE "^\s+${key}" "$DESC" && ok "extension.${key%:}" || bad "missing extension.${key%:}"
done
grep -qE "^\s+github:" "$DESC" && ok "repo.github" || bad "missing repo.github"
grep -qE "^\s+ref:" "$DESC"    && ok "repo.ref"    || bad "missing repo.ref"

step "Excluded platforms agree between descriptor and CI"
# The descriptor must match the DEFAULT DuckDB line's exclude_archs, because that is
# the only line community-extensions builds for a submission (their build_andium.yml
# is `if: false`). Other matrix entries may legitimately exclude MORE — the LTS line
# excludes windows_amd64 because DuckDB v1.4.5's own sqlite3_api_wrapper does not
# compile under the current MSVC. An earlier version of this check unioned every
# exclude_archs in the file, which cannot express that and would fail here.
# NOTE: sed, not `grep -oP`. BSD grep (macOS) has no -P, so every -oP in this
# script used to yield an empty string there — and an empty-vs-empty comparison
# PASSES. The script reported success on macOS while checking nothing at all.
desc_ex=$(sed -n 's/.*excluded_platforms:[[:space:]]*"\([^"]*\)".*/\1/p' "$DESC" \
          | tr ';' '\n' | sort -u)
ci_default_ex=$(${YAML_PY:-false} - "$CI" <<'PY' 2>/dev/null
import sys, yaml
DEFAULT_LINE = "v1.5.5"
with open(sys.argv[1]) as fh:
    wf = yaml.safe_load(fh)
entries = wf["jobs"]["distribution"]["strategy"]["matrix"]["include"]
for e in entries:
    if e.get("duckdb_version") == DEFAULT_LINE:
        print("\n".join(sorted(set(e["exclude_archs"].split(";")))))
        break
PY
)
if [[ -z "$desc_ex" ]]; then
  bad "could not read excluded_platforms from $DESC"
elif [[ -z "$YAML_PY" ]]; then
  warn "no YAML parser available; skipped the descriptor/CI exclusion comparison"
elif [[ "$desc_ex" == "$ci_default_ex" ]]; then
  ok "$(wc -l <<<"$desc_ex") platforms excluded, descriptor matches the default line"
  # Report per-line extras so a divergence is visible rather than silent.
  ${YAML_PY:-false} - "$CI" <<'PY' 2>/dev/null
import sys, yaml
with open(sys.argv[1]) as fh:
    wf = yaml.safe_load(fh)
base = None
for e in wf["jobs"]["distribution"]["strategy"]["matrix"]["include"]:
    if e.get("duckdb_version") == "v1.5.5":
        base = set(e["exclude_archs"].split(";"))
for e in wf["jobs"]["distribution"]["strategy"]["matrix"]["include"]:
    extra = set(e["exclude_archs"].split(";")) - (base or set())
    if extra:
        print(f"       note: {e['duckdb_version']} also excludes {';'.join(sorted(extra))}")
PY
else
  bad "excluded_platforms and the default line's exclude_archs differ:"
  diff <(echo "$desc_ex") <(echo "$ci_default_ex") | sed 's/^/       /'
fi

step "vcpkg manifest and overlay port"
[[ -f vcpkg.json ]] && ok "vcpkg.json present" || bad "vcpkg.json missing"
"$PY" -c "import json;json.load(open('vcpkg.json'))" 2>/dev/null \
  && ok "vcpkg.json is valid JSON" || bad "vcpkg.json is not valid JSON"
for f in vcpkg_ports/vtk-minimal/vcpkg.json vcpkg_ports/vtk-minimal/portfile.cmake; do
  [[ -f "$f" ]] && ok "$f" || bad "$f missing"
done
grep -q '"vtk-minimal"' vcpkg.json \
  && ok "vcpkg.json depends on vtk-minimal" \
  || bad "vcpkg.json does not depend on vtk-minimal"
grep -q '"./vcpkg_ports"' vcpkg.json \
  && ok "overlay-ports includes ./vcpkg_ports" \
  || bad "vcpkg.json does not list ./vcpkg_ports in overlay-ports"

step "Every VTK module our CMake requires is enabled in the port"
# A module enabled in DuckVTKFindVTK.cmake but not in the port links fine locally
# (system VTK has everything) and fails only in their CI. Cheap to check here.
missing=""
while read -r mod; do
  grep -q "VTK_MODULE_ENABLE_VTK_${mod}=YES" vcpkg_ports/vtk-minimal/portfile.cmake \
    || missing="$missing $mod"
done < <(sed -n '/set(DUCK_VTK_REQUIRED_COMPONENTS/,/^)/p' cmake/DuckVTKFindVTK.cmake \
         | sed -n 's/^[[:space:]][[:space:]]*\([A-Z][A-Za-z]*\).*/\1/p')
if [[ -n "$missing" ]]; then
  bad "required components not enabled in the vcpkg port:$missing"
else
  ok "all required components enabled in the port"
fi

step "vcpkg port and local build script enable the same VTK modules"
# These two must agree or "works locally, fails in their CI" becomes possible in
# either direction. CommonMisc was already out of step once: the port enabled it
# explicitly while the script relied on VTK pulling it in transitively.
# Require the -D prefix: both files also NAME modules in comments explaining why
# they are deliberately not enabled (IOGeometry), and matching those would report a
# difference that does not exist.
vtk_modules() {
  sed -n 's/.*-DVTK_MODULE_ENABLE_VTK_\([A-Za-z]*\)=YES.*/\1/p' "$1" | sort -u
}
port_mods=$(vtk_modules vcpkg_ports/vtk-minimal/portfile.cmake)
script_mods=$(vtk_modules scripts/build_minimal_vtk.sh)
if [[ "$port_mods" == "$script_mods" ]]; then
  ok "$(wc -l <<<"$port_mods") modules, identical in both"
else
  bad "module lists differ between the vcpkg port and build_minimal_vtk.sh:"
  diff <(echo "$port_mods") <(echo "$script_mods") | sed 's/^/       /'
fi

step "VTK build options that must agree, do"
# The module-enable comparison above misses options that change ABI or semantics.
# VTK_USE_64BIT_IDS is the one that matters most: sizeof(vtkIdType) underpins the
# exact-integer behaviour and is reported by vtk_build_info(). It was set in
# build_minimal_vtk.sh but left to VTK's default in the port that actually ships.
opt_mismatch=""
for opt in VTK_USE_64BIT_IDS VTK_GROUP_ENABLE_Rendering VTK_GROUP_ENABLE_Qt \
           VTK_BUILD_TESTING VTK_WRAP_PYTHON; do
  p=$(sed -n "s/.*-D${opt}=\([A-Za-z0-9]*\).*/\1/p" vcpkg_ports/vtk-minimal/portfile.cmake | head -1)
  s=$(sed -n "s/.*-D${opt}=\([A-Za-z0-9]*\).*/\1/p" scripts/build_minimal_vtk.sh | head -1)
  if [[ "$p" != "$s" ]]; then
    opt_mismatch="$opt_mismatch $opt(port='${p:-unset}' script='${s:-unset}')"
  fi
done
if [[ -n "$opt_mismatch" ]]; then
  bad "VTK options differ between the port and build_minimal_vtk.sh:$opt_mismatch"
else
  ok "VTK_USE_64BIT_IDS and the group toggles agree"
fi

step "Submodule pin is recorded"
# Guarded on duckdb/ already existing. `make check-pin` includes
# extension-ci-tools/makefiles/duckdb_extension.Makefile, and on a fresh clone that
# include triggers scripts/bootstrap_deps.sh — a ~500 MB fetch, from a check that
# advertises itself as offline and instant.
PIN_LOG="$(mktemp)"
trap 'rm -f "$PIN_LOG"' EXIT
if [[ ! -f duckdb/CMakeLists.txt ]]; then
  warn "duckdb/ is not materialised; skipped the pin check (run 'make configure' first)"
elif make -s check-pin >"$PIN_LOG" 2>&1; then
  ok "$(grep -m1 'derived version tag' "$PIN_LOG" || echo 'pin recorded')"
else
  sed 's/^/       /' "$PIN_LOG"
  bad "check-pin failed — the submodule commit is not in DUCKDB_KNOWN_VERSIONS"
fi

step "Extension name is not already taken upstream"
if command -v curl >/dev/null 2>&1; then
  name=$(sed -n 's/^[[:space:]]*name:[[:space:]]*\([^[:space:]]*\).*/\1/p' "$DESC" | head -1)
  code=$(curl -sS -o /dev/null -m 15 -w "%{http_code}" \
    "https://raw.githubusercontent.com/duckdb/community-extensions/main/extensions/${name}/description.yml" 2>/dev/null || echo "000")
  case "$code" in
    404) ok "name '${name}' is unclaimed upstream" ;;
    200) bad "name '${name}' is ALREADY TAKEN upstream" ;;
    *)   warn "could not check name availability (HTTP ${code:-none}); re-check before submitting" ;;
  esac
else
  warn "curl unavailable; cannot check name availability"
fi

printf '\n'
if (( fails )); then
  printf '\033[1;31m%s check(s) failed\033[0m'"$( ((warns)) && printf ', %s warning(s)' "$warns" )"'\n' "$fails"
  printf 'Not ready to submit.\n'
  exit 1
fi
printf '\033[1;32mAll pre-submission checks passed\033[0m'
(( warns )) && printf ' (%s warning(s))' "$warns"
printf '\n\nStill required before opening the PR:\n'
printf '  make ci-verify   # cold-boot build in Docker\n'
printf '  make check       # full test suite\n'
printf '  fill in repo.github / repo.ref / maintainers in %s\n' "$DESC"
