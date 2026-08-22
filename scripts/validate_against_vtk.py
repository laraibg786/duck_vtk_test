#!/usr/bin/env python3
"""Elementwise validation of duck_vtk against Python VTK — the independent oracle.

WHY THIS EXISTS
---------------
sqllogictest pins a handful of sampled values. That is necessary but nowhere near
sufficient: a bug that reads a float32 array through a double accessor produces
plausible-looking numbers, and a bug that reads int64 through
vtkDataArray::GetComponent is *exactly right* below 2^53 and wrong above it.
Only comparison against a reader that shares no code with ours catches those.

The PyPI `vtk` wheel is a self-contained build — it does not reuse the C++ VTK the
extension links against. That independence is the whole point: a shared bug cannot
hide itself on both sides of the comparison.

USAGE
-----
    .venv/bin/python scripts/validate_against_vtk.py \
        --data test/data \
        --duckdb ./build/release/duckdb \
        --ext build/release/extension/vtk/vtk.duckdb_extension

Exits non-zero if ANY file mismatches. Prints a per-file table and, for failures,
the first --max-diffs differing elements with indices.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

try:
    import vtk
    from vtk.util.numpy_support import vtk_to_numpy
except ImportError:
    sys.exit(
        "FATAL: python 'vtk' not importable.\n"
        "This harness IS the correctness oracle; without it there is nothing to\n"
        "validate against. Install it with:  make configure\n"
        "Do NOT commit test expectations that were never checked against an oracle."
    )

# Formats duck_vtk Phase 1 is expected to read. Others are skipped with a reason
# rather than counted as failures, so the report stays honest.
PHASE1_SUFFIXES = {".vtk", ".vtu", ".vtp", ".vti", ".vtr", ".vts"}
SKIP_SUFFIXES = {
    ".vtm": "multiblock — Phase 4",
    ".pvd": "time series — Phase 4",
    ".pvtu": "parallel pieces — Phase 4",
    ".ex2": "ExodusII — Phase 4 (needs IOExodus module)",
    ".e": "ExodusII — Phase 4 (needs IOExodus module)",
    ".cgns": "CGNS — Phase 4 (needs IOCGNS module)",
    ".vtkhdf": "VTKHDF — Phase 4 (needs IOHDF module)",
    ".sha256": "manifest",
}
# Synthetic fixtures whose expected outcome is an ERROR, not data. Comparing them
# elementwise is meaningless; test/sql/degenerate.test asserts their behaviour.
#
# NOTE these are files duck_vtk MUST reject. Python VTK silently accepts the
# truncated ones (it reports Success and returns uninitialised coordinates — the
# same behaviour documented in docs/PHASE0_RESULTS.md §4), so the oracle cannot be
# used to judge them. duck_vtk is deliberately STRICTER than raw VTK here.
# test/sql/attach_errors.test asserts the rejections instead.
EXPECT_ERROR = {
    "truncated.vtk",
    "not_really.vtk",
    "bad_type.vtu",
    "bad_ascii_nan.vtk",
    # Added late: this fixture was registered in run_invariants.sh and MANIFEST.sha256
    # but not here, so `make oracle` failed on it from the day it landed. Nothing
    # noticed, because no CI job runs the oracle.
    "bad_xml_junk.vtu",
}


@dataclass
class Result:
    path: str
    status: str  # ok | fail | skip | error
    detail: str = ""
    diffs: list[str] = field(default_factory=list)


# --------------------------------------------------------------------------
# Oracle side: read the file with Python VTK
# --------------------------------------------------------------------------


# Concrete XML reader per extension.
#
# The oracle may dispatch on the FILE EXTENSION even though duck_vtk itself must
# dispatch on content (a .vtk-named text file has to be rejected). Two reasons it
# is fine here: the corpus filenames are known-correct, and CanReadFile is
# reliable on the CONCRETE readers — it is only broken on
# vtkXMLGenericDataObjectReader. ReadOutputType would be the content-based
# alternative, but its `bool &parallel` out-parameter does not wrap into Python
# ("ReadOutputType argument 2"), so it is unusable from here.
def _xml_reader_for(suffix: str):
    import vtk as _v
    return {
        ".vtu": _v.vtkXMLUnstructuredGridReader,
        ".vtp": _v.vtkXMLPolyDataReader,
        ".vti": _v.vtkXMLImageDataReader,
        ".vtr": _v.vtkXMLRectilinearGridReader,
        ".vts": _v.vtkXMLStructuredGridReader,
    }.get(suffix)


def read_with_vtk(path: Path):
    """Return a vtkDataSet, or raise."""
    suffix = path.suffix.lower()
    if suffix == ".vtk":
        reader = vtk.vtkGenericDataObjectReader()
        reader.SetFileName(str(path))
        # Match duck_vtk: every ReadAll* flag defaults to OFF, so without these the
        # oracle would see only the FIRST array of each attribute kind and would
        # "confirm" a truncated array list.
        for setter in ("ReadAllScalarsOn", "ReadAllVectorsOn", "ReadAllNormalsOn",
                       "ReadAllTensorsOn", "ReadAllColorScalarsOn", "ReadAllTCoordsOn",
                       "ReadAllFieldsOn"):
            getattr(reader, setter)()
        reader.Update()
        obj = reader.GetOutput()
    else:
        cls = _xml_reader_for(suffix)
        if cls is None:
            raise RuntimeError(f"no oracle reader for {suffix}")
        reader = cls()
        if not reader.CanReadFile(str(path)):
            raise RuntimeError(f"{cls.__name__} cannot read {path}")
        reader.SetFileName(str(path))
        reader.Update()
        obj = reader.GetOutput()
    if obj is None:
        raise RuntimeError(f"reader produced no output for {path}")
    ds = obj if isinstance(obj, vtk.vtkDataSet) else None
    if ds is None:
        raise RuntimeError(f"{path}: not a vtkDataSet ({obj.GetClassName()})")
    return ds


def oracle_points(ds):
    return [tuple(ds.GetPoint(i)) for i in range(ds.GetNumberOfPoints())]


def oracle_cells(ds):
    """(cell_type, [point_ids]) per cell.

    Uses GetCellPoints with a caller-owned vtkIdList rather than GetCell(), for the
    same reason the C++ side must: GetCell returns a shared scratch object.
    """
    out = []
    ids = vtk.vtkIdList()
    for i in range(ds.GetNumberOfCells()):
        ds.GetCellPoints(i, ids)
        out.append(
            (int(ds.GetCellType(i)), [int(ids.GetId(k)) for k in range(ids.GetNumberOfIds())])
        )
    return out


def oracle_arrays(ds, association: str):
    """{name: {'ncomp': n, 'dtype': int, 'values': [[c0, c1, ...], ...]}}"""
    fd = ds.GetPointData() if association == "POINT" else ds.GetCellData()
    result = {}
    for i in range(fd.GetNumberOfArrays()):
        # GetAbstractArray, not GetArray: GetArray() returns None for vtkStringArray,
        # which would silently drop string columns from the comparison.
        arr = fd.GetAbstractArray(i)
        if arr is None:
            continue
        name = arr.GetName() or f"unnamed_{association.lower()}_{i}"
        ncomp = arr.GetNumberOfComponents()
        ntup = arr.GetNumberOfTuples()
        vals = []
        if isinstance(arr, vtk.vtkStringArray):
            # vtkStringArray indexes by VALUE, so tuple t component c is value
            # t*ncomp + c. Flattening one value per row (the previous behaviour)
            # contradicted the ncomp reported alongside it, which made any
            # multi-component string array impossible to compare correctly.
            nvals = arr.GetNumberOfValues()
            ntuples = nvals // ncomp if ncomp else 0
            vals = [[arr.GetValue(t * ncomp + c) for c in range(ncomp)]
                    for t in range(ntuples)]
        else:
            # Integer arrays must be read EXACTLY. arr.GetComponent() returns a
            # double, so anything above 2^53 is silently rounded on the oracle side —
            # 9007199254740993 comes back as 9007199254740992.0. That is precisely
            # what test/data/synthetic/int64_precision.vtu exists to catch, and this
            # harness could not catch it: the old comparison coerced BOTH sides
            # through float(), so the extension's exact value was rounded to match the
            # oracle's rounded one and the check passed vacuously.
            np_arr = None
            try:
                np_arr = vtk_to_numpy(arr)
            except Exception:  # noqa: BLE001  (not a vtkDataArray, or unsupported type)
                np_arr = None
            if np_arr is not None and np_arr.dtype.kind in "iu":
                flat = np_arr.reshape(ntup, ncomp)
                vals = [[int(x) for x in row] for row in flat]
            else:
                for t in range(ntup):
                    vals.append([arr.GetComponent(t, c) for c in range(ncomp)])
        result[name] = {"ncomp": ncomp, "dtype": arr.GetDataType(), "values": vals}
    return result


# --------------------------------------------------------------------------
# Extension side: query through the DuckDB CLI
# --------------------------------------------------------------------------


def duck_json(duckdb_bin: str, ext: str, sql: str, timeout: int = 300):
    """Run SQL with the extension loaded and return parsed JSON rows.

    JSON, not CSV: a text round-trip through CSV loses float precision and mangles
    LIST columns, which would make exact comparison impossible for the very data we
    most need to check exactly.
    """
    script = f"LOAD '{ext}';\n.mode json\n{sql}\n"
    proc = subprocess.run(
        [duckdb_bin, "-unsigned", "-batch", "-init", "/dev/null"],
        input=script,
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"duckdb failed: {proc.stderr.strip()[:2000]}")
    out = proc.stdout.strip()
    if not out:
        return []
    # The CLI can emit several JSON arrays if the script had multiple statements;
    # take the last non-empty one.
    chunks = [c for c in out.split("\n[") if c.strip()]
    text = ("[" + chunks[-1]) if len(chunks) > 1 else out
    # DuckDB emits bare `nan`, `inf` and `-inf`, which are not valid JSON. Python's
    # parser accepts the capitalised `NaN`/`Infinity`/`-Infinity` spellings, so
    # normalise rather than losing the very values nan_inf.vtu exists to check.
    text = re.sub(r"(?<=[:\[,\s])-inf(?=[,\]}\s])", "-Infinity", text)
    text = re.sub(r"(?<=[:\[,\s])inf(?=[,\]}\s])", "Infinity", text)
    text = re.sub(r"(?<=[:\[,\s])nan(?=[,\]}\s])", "NaN", text)
    try:
        return json.loads(text)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"could not parse duckdb JSON output: {exc}\n{out[:1000]}") from exc


# --------------------------------------------------------------------------
# Comparison
# --------------------------------------------------------------------------


VTK_FLOAT = 10  # vtkType.h — float32

def _as_float32(x):
    """Round a Python float to the nearest float32, exactly as hardware would."""
    import struct
    return struct.unpack("<f", struct.pack("<f", float(x)))[0]


def floats_equal(a, b, float32: bool = False) -> bool:
    """Exact, with NaN==NaN and signed-infinity matching.

    Exact rather than tolerant: values reach us via JSON (no lossy text round-trip),
    float32 widened to double is exact, and the oracle reads the same bytes. A
    tolerance here would mask precisely the precision bugs this harness exists to
    find. Only fall back to a tolerance if a specific, understood reason emerges —
    and document it.
    """
    if a is None or b is None:
        return a is None and b is None
    try:
        fa, fb = float(a), float(b)
    except (TypeError, ValueError):
        return a == b
    if math.isnan(fa) and math.isnan(fb):
        return True
    if math.isinf(fa) or math.isinf(fb):
        return fa == fb
    if float32:
        # The array's own type is float32, so float32 IS full precision for it.
        #
        # Comparing as double would fail spuriously: the oracle reads via
        # GetComponent, which widens float32 to double and prints
        # 71.7343978881836, while DuckDB's JSON writer emits a FLOAT column at
        # float32 precision as 71.7344. Both are the identical 4-byte value.
        # Rounding both sides to float32 compares what actually exists.
        return _as_float32(fa) == _as_float32(fb)
    return fa == fb


def values_equal(got, exp, is_f32: bool) -> bool:
    """Exact for strings, tolerance-aware for floats.

    floats_equal() coerces through float(), so a string array compared with it either
    raised or silently compared as NaN. String arrays are compared verbatim.
    """
    if isinstance(exp, int) and not isinstance(exp, bool):
        # Exact. DuckDB's JSON output renders a 64-bit integer beyond JSON's safe
        # range as a quoted string, so parse rather than compare representations.
        try:
            return int(str(got)) == exp
        except (TypeError, ValueError):
            return False
    if isinstance(exp, str) or isinstance(got, str):
        return got == exp
    return floats_equal(got, exp, is_f32)


def compare_file(path: Path, duckdb_bin: str, ext: str, max_diffs: int) -> Result:
    rel = str(path)
    if path.name in EXPECT_ERROR:
        return Result(rel, "skip", "expected-error fixture; asserted in attach_errors.test")
    try:
        ds = read_with_vtk(path)
    except Exception as exc:  # noqa: BLE001
        if path.name in EXPECT_ERROR:
            return Result(rel, "skip", "expected-error fixture; asserted in degenerate.test")
        return Result(rel, "skip", f"oracle could not read it: {exc}")

    diffs: list[str] = []
    posix = path.as_posix()

    # ---- counts -----------------------------------------------------------
    try:
        info = duck_json(duckdb_bin, ext, f"SELECT * FROM vtk_info('{posix}');")
    except Exception as exc:  # noqa: BLE001
        return Result(rel, "error", str(exc))
    if not info:
        return Result(rel, "fail", "vtk_info returned no rows")
    got = info[0]
    exp_np, exp_nc = ds.GetNumberOfPoints(), ds.GetNumberOfCells()
    if int(got.get("num_points", -1)) != exp_np:
        diffs.append(f"num_points: got {got.get('num_points')} expected {exp_np}")
    if int(got.get("num_cells", -1)) != exp_nc:
        diffs.append(f"num_cells: got {got.get('num_cells')} expected {exp_nc}")
    if diffs:
        return Result(rel, "fail", "count mismatch", diffs)

    # ---- point coordinates -----------------------------------------------
    exp_pts = oracle_points(ds)
    rows = duck_json(duckdb_bin, ext, f"SELECT point_id, x, y, z FROM vtk_points('{posix}') ORDER BY point_id;")
    if len(rows) != len(exp_pts):
        diffs.append(f"points row count: got {len(rows)} expected {len(exp_pts)}")
    else:
        for i, (row, exp) in enumerate(zip(rows, exp_pts)):
            for axis, e in zip(("x", "y", "z"), exp):
                if not floats_equal(row.get(axis), e):
                    diffs.append(f"points[{i}].{axis}: got {row.get(axis)!r} expected {e!r}")
                    if len(diffs) >= max_diffs:
                        return Result(rel, "fail", "coordinate mismatch", diffs)

    # ---- cells: type + connectivity (order is semantically load-bearing) --
    exp_cells = oracle_cells(ds)
    rows = duck_json(
        duckdb_bin, ext,
        f"SELECT cell_id, cell_type, num_points, point_ids FROM vtk_cells('{posix}') ORDER BY cell_id;",
    )
    if len(rows) != len(exp_cells):
        diffs.append(f"cells row count: got {len(rows)} expected {len(exp_cells)}")
    else:
        for i, (row, (etype, epts)) in enumerate(zip(rows, exp_cells)):
            if int(row.get("cell_type", -1)) != etype:
                diffs.append(f"cells[{i}].cell_type: got {row.get('cell_type')} expected {etype}")
            got_pts = row.get("point_ids") or []
            if [int(v) for v in got_pts] != epts:
                # Never sorted: vertex order defines face/normal orientation.
                diffs.append(f"cells[{i}].point_ids: got {got_pts} expected {epts}")
            if int(row.get("num_points", -1)) != len(epts):
                diffs.append(f"cells[{i}].num_points: got {row.get('num_points')} expected {len(epts)}")
            if len(diffs) >= max_diffs:
                return Result(rel, "fail", "cell mismatch", diffs)

    # ---- arrays -----------------------------------------------------------
    # Map array name -> the column name duck_vtk actually exposes.
    try:
        name_map = {
            (r["association"], r["name"]): r["column_name"]
            for r in duck_json(duckdb_bin, ext,
                               f"SELECT association, name, column_name FROM vtk_arrays('{posix}');")
        }
    except Exception as exc:  # noqa: BLE001
        return Result(rel, "error", f"could not read vtk_arrays: {exc}")

    for assoc, table, key in (("POINT", "vtk_points", "point_id"), ("CELL", "vtk_cells", "cell_id")):
        expected = oracle_arrays(ds, assoc)
        for name, meta in expected.items():
            column = name_map.get((assoc, name), name)
            quoted = column.replace('"', '""')
            is_string = bool(meta["values"]) and isinstance(meta["values"][0][0], str)
            try:
                if is_string and meta["ncomp"] > 1:
                    # DuckDB's `.mode json` renders a VARCHAR[] as [alpha, beta] —
                    # the elements are NOT quoted, so the output is not valid JSON and
                    # json.loads rejects it. Expanding the list into one row per
                    # component emits each value as a proper JSON string instead.
                    expanded = duck_json(
                        duckdb_bin, ext,
                        f'SELECT {key} AS k, t.i AS c, "{quoted}"[t.i] AS v '
                        f'FROM {table}(\'{posix}\'), generate_series(1,{meta["ncomp"]}) AS t(i) '
                        f'ORDER BY k, c;',
                    )
                    grouped: dict = {}
                    for r in expanded:
                        grouped.setdefault(r["k"], []).append(r["v"])
                    rows = [{"v": grouped[k]} for k in sorted(grouped)]
                else:
                    rows = duck_json(
                        duckdb_bin, ext,
                        f'SELECT {key}, "{quoted}" AS v FROM {table}(\'{posix}\') ORDER BY {key};',
                    )
            except Exception as exc:  # noqa: BLE001
                diffs.append(f"{assoc} array {name!r} (column {column!r}): query failed: {exc}")
                continue
            is_f32 = meta["dtype"] == VTK_FLOAT
            n = min(len(rows), len(meta["values"]))
            for t in range(n):
                got_v = rows[t].get("v")
                exp_v = meta["values"][t]
                if meta["ncomp"] == 1:
                    # ncomp==1 must be a scalar column, never a 1-element list.
                    if isinstance(got_v, list):
                        diffs.append(f"{assoc} {name}[{t}]: got list {got_v!r}, expected scalar")
                    elif not values_equal(got_v, exp_v[0], is_f32):
                        diffs.append(f"{assoc} {name}[{t}]: got {got_v!r} expected {exp_v[0]!r}")
                else:
                    if not isinstance(got_v, list):
                        diffs.append(f"{assoc} {name}[{t}]: got {got_v!r}, expected {meta['ncomp']}-list")
                    elif len(got_v) != meta["ncomp"]:
                        diffs.append(f"{assoc} {name}[{t}]: got {len(got_v)} components expected {meta['ncomp']}")
                    else:
                        for c, (g, e) in enumerate(zip(got_v, exp_v)):
                            if not values_equal(g, e, is_f32):
                                diffs.append(f"{assoc} {name}[{t}][{c}]: got {g!r} expected {e!r}")
                if len(diffs) >= max_diffs:
                    return Result(rel, "fail", "array value mismatch", diffs)

    if diffs:
        return Result(rel, "fail", "mismatch", diffs)
    return Result(rel, "ok", f"{exp_np} pts, {exp_nc} cells, "
                             f"{len(oracle_arrays(ds, 'POINT'))}+{len(oracle_arrays(ds, 'CELL'))} arrays")


# --------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--data", default="test/data")
    ap.add_argument("--duckdb", default="./build/release/duckdb")
    ap.add_argument("--ext", default="build/release/extension/vtk/vtk.duckdb_extension")
    ap.add_argument("--max-diffs", type=int, default=10)
    ap.add_argument("--filter", default="", help="only files whose path contains this")
    args = ap.parse_args()

    if not Path(args.ext).exists():
        return int(bool(print(f"FATAL: extension not found at {args.ext} — run 'make release'", file=sys.stderr)) or 2)

    print(f"oracle: python vtk {vtk.VTK_VERSION}")
    print(f"extension: {args.ext}\n")

    results: list[Result] = []
    for path in sorted(Path(args.data).rglob("*")):
        if not path.is_file() or args.filter and args.filter not in str(path):
            continue
        suffix = path.suffix.lower()
        if suffix in SKIP_SUFFIXES:
            results.append(Result(str(path), "skip", SKIP_SUFFIXES[suffix]))
            continue
        if suffix not in PHASE1_SUFFIXES:
            continue
        results.append(compare_file(path, args.duckdb, args.ext, args.max_diffs))

    width = max((len(r.path) for r in results), default=20)
    for r in results:
        mark = {"ok": "\033[1;32m ok \033[0m", "fail": "\033[1;31mFAIL\033[0m",
                "skip": "skip", "error": "\033[1;31mERR \033[0m"}[r.status]
        print(f"{mark}  {r.path:<{width}}  {r.detail}")
        for d in r.diffs:
            print(f"        {d}")

    n_ok = sum(r.status == "ok" for r in results)
    n_fail = sum(r.status in ("fail", "error") for r in results)
    n_skip = sum(r.status == "skip" for r in results)
    print(f"\n{n_ok} ok, {n_fail} failed, {n_skip} skipped")
    if n_skip:
        print("NOTE: skipped files are NOT validated. Do not treat them as passing,\n"
              "and do not commit expectations for them derived from duck_vtk itself.")
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
