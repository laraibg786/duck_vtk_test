#!/usr/bin/env bash
# Run the SQL invariant suite against EVERY corpus file.
#
# WHY THIS EXISTS
# ---------------
# sqllogictest pins specific values for specific files. Invariants are different:
# they are properties that must hold for *any* mesh, so they keep working as the
# corpus grows and they catch whole classes of bug rather than one value.
#
# The bounds invariant is the standout: it cross-checks our coordinate-reading path
# against a completely separate VTK code path (GetBounds), which catches x/y/z
# transposition — a bug no per-value spot check on a symmetric mesh would find.
#
# Usage: ./scripts/run_invariants.sh [glob-filter]

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

DUCKDB="${DUCKDB:-./build/release/duckdb}"
EXT="${EXT:-build/release/extension/vtk/vtk.duckdb_extension}"
FILTER="${1:-}"

[[ -f "$EXT" ]] || { echo "FATAL: $EXT not found — run 'make release'" >&2; exit 2; }
[[ -x "$DUCKDB" ]] || { echo "FATAL: $DUCKDB not found" >&2; exit 2; }

pass_n=0; fail_n=0; skip_n=0

# Each invariant is "name|SQL returning exactly one value|expected".
# Written so a violation returns a non-zero count, making the expectation uniform.
run_invariants_for() {
  local file="$1" label="$2"
  local prelude="LOAD '$EXT'; ATTACH '$file' AS m (TYPE vtk);"
  local failed=0 first_err=""

  # Skip empty datasets: several invariants are vacuous or undefined at 0 points,
  # and degenerate.test covers them properly.
  local npts
  npts=$("$DUCKDB" -noheader -list -c "$prelude SELECT num_points FROM m.vtk_info;" 2>/dev/null) || {
    printf '  \033[1;31mERR \033[0m %s (ATTACH failed)\n' "$label"; return 1; }
  if [[ "$npts" == "0" ]]; then
    printf '  skip  %s (empty dataset)\n' "$label"; return 2
  fi

  local -a checks=(
    # --- point id integrity -------------------------------------------------
    "points row count matches vtk_info|SELECT (SELECT count(*) FROM m.points) - (SELECT num_points FROM m.vtk_info);|0"
    "point_id is 0-based and dense|SELECT count(*) FROM (SELECT point_id FROM m.points EXCEPT SELECT range FROM range((SELECT num_points FROM m.vtk_info)));|0"
    "point_id is unique|SELECT count(*) - count(DISTINCT point_id) FROM m.points;|0"

    # --- cell id integrity --------------------------------------------------
    "cells row count matches vtk_info|SELECT (SELECT count(*) FROM m.cells) - (SELECT num_cells FROM m.vtk_info);|0"
    "cell_id is unique|SELECT count(*) - count(DISTINCT cell_id) FROM m.cells;|0"

    # --- connectivity -------------------------------------------------------
    "no connectivity dangles outside the point set|SELECT count(*) FROM m.cell_points WHERE point_id < 0 OR point_id >= (SELECT num_points FROM m.vtk_info);|0"
    "cell_points cardinality equals sum(num_points)|SELECT (SELECT count(*) FROM m.cell_points) - COALESCE((SELECT sum(num_points) FROM m.cells),0);|0"
    "num_points agrees with len(point_ids)|SELECT count(*) FROM m.cells WHERE num_points <> len(point_ids);|0"
    # cell_points must be exactly the flattening of cells.point_ids, including order
    "cell_points is the flattening of point_ids|SELECT count(*) FROM (SELECT cell_id, vertex_index, point_id FROM m.cell_points EXCEPT SELECT cell_id, generate_subscripts(point_ids,1)-1, unnest(point_ids) FROM m.cells);|0"

    # --- cross-check against a separate VTK code path -----------------------
    "SQL-computed bounds match VTK GetBounds|SELECT count(*) FROM m.vtk_info i, (SELECT min(x) a, max(x) b, min(y) c, max(y) d, min(z) e, max(z) f FROM m.points) p WHERE i.bounds_x_min <> p.a OR i.bounds_x_max <> p.b OR i.bounds_y_min <> p.c OR i.bounds_y_max <> p.d OR i.bounds_z_min <> p.e OR i.bounds_z_max <> p.f;|0"

    # --- catalogue self-consistency ----------------------------------------
    "every POINT array appears as a points column|SELECT count(*) FROM m.vtk_arrays a WHERE a.association='POINT' AND a.column_name NOT IN (SELECT column_name FROM information_schema.columns WHERE table_catalog='m' AND table_name='points');|0"
    "every CELL array appears as a cells column|SELECT count(*) FROM m.vtk_arrays a WHERE a.association='CELL' AND a.column_name NOT IN (SELECT column_name FROM information_schema.columns WHERE table_catalog='m' AND table_name='cells');|0"
    "vtk_info array counts match vtk_arrays|SELECT (SELECT num_point_arrays FROM m.vtk_info) - (SELECT count(*) FROM m.vtk_arrays WHERE association='POINT');|0"
    "cell_type_name is never NULL|SELECT count(*) FROM m.cells WHERE cell_type_name IS NULL;|0"
    "vtk_info has exactly one row|SELECT count(*) FROM m.vtk_info;|1"
  )

  for check in "${checks[@]}"; do
    local name="${check%%|*}"; local rest="${check#*|}"
    local sql="${rest%|*}"; local want="${rest##*|}"
    local got
    got=$("$DUCKDB" -noheader -list -c "$prelude $sql" 2>&1)
    if [[ "$got" != "$want" ]]; then
      failed=$((failed+1))
      [[ -z "$first_err" ]] && first_err="$name: got '$got' want '$want'"
    fi
  done

  if (( failed == 0 )); then
    printf '  \033[1;32m ok \033[0m %s (%s invariants)\n' "$label" "${#checks[@]}"
    return 0
  fi
  printf '  \033[1;31mFAIL\033[0m %s (%s/%s failed) %s\n' "$label" "$failed" "${#checks[@]}" "$first_err"
  return 1
}

echo "Running SQL invariants against the corpus"
echo "  duckdb:    $DUCKDB"
echo "  extension: $EXT"
echo

# Phase 1 formats only. Composite/CAE formats are Phase 4 and are expected to be
# rejected at ATTACH, so running data invariants on them would be meaningless.
while IFS= read -r f; do
  [[ -n "$FILTER" && "$f" != *"$FILTER"* ]] && continue
  case "$(basename "$f")" in
    truncated.vtk|not_really.vtk|bad_type.vtu|bad_ascii_nan.vtk|bad_xml_junk.vtu) continue ;;  # expected-error fixtures
  esac
  run_invariants_for "$f" "$f"
  case $? in
    0) pass_n=$((pass_n+1)) ;;
    2) skip_n=$((skip_n+1)) ;;
    *) fail_n=$((fail_n+1)) ;;
  esac
done < <(find test/data -type f \( -name '*.vtk' -o -name '*.vtu' -o -name '*.vtp' \
                                  -o -name '*.vti' -o -name '*.vtr' -o -name '*.vts' \) | sort)

echo
echo "$pass_n files passed, $fail_n failed, $skip_n skipped"
(( fail_n == 0 )) || exit 1
