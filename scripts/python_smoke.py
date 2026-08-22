#!/usr/bin/env python3
"""Smoke test via the DuckDB PYTHON client, not just the CLI.

Run after `make install` (which puts the extension where DuckDB finds it by name):

    make python-smoke      # pins the duckdb wheel to DUCKDB_VERSION_TAG

Worth having separately from the CLI tests: the Python client is how most users
will actually reach this extension, it takes a different code path to load
extensions, and `allow_unsigned_extensions` has to be passed as connection config
rather than a command-line flag.
"""

import duckdb, sys
con = duckdb.connect(config={"allow_unsigned_extensions": True})
con.load_extension("vtk")                      # installed by name — no path
print("vtk_version       :", con.sql("SELECT vtk_version()").fetchone()[0])
con.sql("ATTACH 'test/data/legacy/VTKCellTypes.vtk' AS m (TYPE vtk)")
print("tables            :", [r[0] for r in con.sql(
    "SELECT table_name FROM duckdb_tables() WHERE database_name='m' ORDER BY table_name").fetchall()])
print("points/cells      :", con.sql("SELECT num_points, num_cells FROM m.vtk_info").fetchone())
print("cell types        :", con.sql(
    "SELECT count(DISTINCT cell_type_name) FROM m.cells").fetchone()[0])
print("centroid of cell 0:", con.sql("""
    SELECT round(avg(p.x),6), round(avg(p.y),6), round(avg(p.z),6)
    FROM m.cell_points cp JOIN m.points p USING (point_id) WHERE cp.cell_id = 0
""").fetchone())
print("vector components :", con.sql(
    "SELECT vectors[1], vectors[2], vectors[3] FROM m.points WHERE point_id=1").fetchone())
# Pandas/Arrow interop is the reason many users want this in Python at all.
df = con.sql("SELECT cell_type_name, count(*) AS n FROM m.cells GROUP BY 1 ORDER BY 1").df()
print("to pandas         :", df.shape, list(df.columns))
arrow = con.sql("SELECT point_id, x, y, z, scalars FROM m.points").arrow()
print("to arrow          :", arrow.num_rows, "rows,", arrow.num_columns, "cols")
print("read-only enforced:", end=" ")
try:
    con.sql("CREATE TABLE m.foo(i INT)"); print("NO — that is a bug"); sys.exit(1)
except Exception as e:
    print(type(e).__name__, "-", str(e).split("\n")[0][:70])
