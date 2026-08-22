# `vtk-minimal` — local overlay port

## What this is

A vcpkg port of [VTK](https://vtk.org) that builds **only** the modules duck_vtk
links, with no rendering stack. It exists because the DuckDB community-extensions
pipeline resolves every dependency through vcpkg, and vcpkg's official `vtk` port
cannot be used here.

## Why not the official `vtk` port

Its portfile **hardcodes** `VTK_GROUP_ENABLE_Rendering=YES` with no feature to turn
it off, and its manifest carries 26 base dependencies including `glew`, `gl2ps` and
`freetype`, plus a second VTK-derived host build (`vtk-compile-tools`). A SQL engine
renders nothing, and dragging OpenGL into a database extension makes it fail to load
on headless machines.

(Qt is an *opt-in feature* of that port, not a default dependency. An earlier version
of this note said otherwise; it was wrong.)

## Provenance

| | |
|---|---|
| Upstream | `https://www.vtk.org/files/release/<series>/VTK-<version>.tar.gz` |
| Version | see `vcpkg.json` (`version`) and `portfile.cmake` (`URLS`, `SHA512`) |
| Patches | **none** |

**There are no patches, deliberately.** Every reduction uses a documented upstream
VTK CMake option, so a version bump is a version + SHA512 change with no patch to
re-base and no risk of a patch silently applying to the wrong lines.

> Note: vtk.org is the only working source. Kitware/VTK on GitHub publishes **tags
> only and zero releases**, so a `github.com/Kitware/VTK/releases/download/...`
> mirror URL 404s. One was listed here as a fallback for a while; it never worked.

## Bumping VTK

1. Update `version` in `vcpkg.json`.
2. Update the `URLS`, `FILENAME` and `SHA512` in `portfile.cmake`
   (`curl -fsSL <url> | sha512sum`).
3. Update `CONFIG_PATH lib/cmake/vtk-<series>` in `portfile.cmake` — this is
   **series-sensitive** and a stale value fails late, after the whole VTK build.
4. Update the default `VTK_VERSION` in `scripts/build_minimal_vtk.sh` and the cache
   `path`/`key` in `.github/workflows/ci.yml`.
5. Update `DUCK_VTK_MIN_VERSION` in `cmake/DuckVTKFindVTK.cmake` if the floor moves.
6. Run `make ci-verify-vcpkg` — nothing else exercises this port.

`scripts/submit_check.sh` cross-checks that the module set here matches
`scripts/build_minimal_vtk.sh` and `cmake/DuckVTKFindVTK.cmake`.

## Module set

Enabled: `CommonCore`, `CommonDataModel`, `CommonExecutionModel`, `CommonMisc`,
`IOLegacy`, `IOXML`, `FiltersCore`. Everything else is off; `StandAlone` is
`DONT_WANT` so VTK still resolves what those modules genuinely need.

Deliberately **not** enabled:

- `IOParallelXML` — all 20 of its classes are writers; every `vtkXMLP*Reader` lives
  in `IOXML`, and duck_vtk is read-only.
- `IOGeometry` (OBJ/STL/PLY) — requires `FiltersHybrid` → `RenderingCore`, so VTK's
  own configure step *fails* with rendering disabled. Do not "fix" a missing OBJ
  reader by re-enabling rendering.

## Removing this port

If vcpkg's official `vtk` port ever grows a feature to disable rendering, delete this
directory and drop `./vcpkg_ports` from `overlay-ports` in `../../vcpkg.json`.
