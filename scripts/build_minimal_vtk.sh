#!/usr/bin/env bash
# Build a minimal VTK from source, containing only the modules duck_vtk needs.
#
# WHY THIS EXISTS
# ---------------
# `brew install vtk` works but is a poor fit for this project:
#   * The bottle is built with rendering support, so it drags in qtbase,
#     qtdeclarative, gtk+3, mesa, gcc and llvm — measured at ~2.8 GB of downloads,
#     of which llvm alone is 550 MB. We link five small modules.
#   * ghcr.io returned repeated `HTTP/2 stream not closed cleanly (PROTOCOL_ERROR)`
#     failures on the large blobs, and resumed downloads corrupted the cache.
#   * Homebrew's Linux bottles are built with Homebrew's own toolchain, creating a
#     libstdc++ ABI mismatch risk against Debian's system GCC. Building from
#     source with the SAME compiler that builds the extension removes that risk
#     entirely rather than merely testing for it.
#
# This build is ~54 MB of source and enables no rendering, no Qt, no Python.
#
# DO NOT substitute the distro package. `apt install libvtk9-dev` on Ubuntu 24.04
# gives VTK 9.1, which cannot read XML files with an <AppendedData> section: it
# fails to parse them, still reports success, and yields an empty mesh. The build
# now requires VTK >= 9.6 and will refuse 9.1 outright — see
# cmake/DuckVTKFindVTK.cmake for the measurement.
#
# OUTPUT CONTRACT
# --------------
# stdout carries EXACTLY ONE line: the cmake config directory (the value to use as
# VTK_DIR). Every progress message, the library listing and the closing hint go to
# stderr. Callers can therefore write
#     VTK_DIR=$(./scripts/build_minimal_vtk.sh)
# and, under `set -e`, a failed build aborts the caller instead of silently
# assigning whatever happened to be printed last. The previous contract was
# "the last line of stdout", which meant a build that died early handed the caller
# a progress message as its VTK_DIR and failed much later with a confusing
# find_package error.
#
# Usage:  ./scripts/build_minimal_vtk.sh [version] [install_prefix]

set -euo pipefail

VTK_VERSION="${1:-9.6.2}"
VTK_SERIES="$(echo "$VTK_VERSION" | cut -d. -f1,2)"
PREFIX="${2:-$HOME/.local/vtk-$VTK_VERSION}"
WORK="${VTK_BUILD_WORKDIR:-/tmp/vtk-build-$VTK_VERSION}"
JOBS="${JOBS:-$(nproc)}"

# stderr, so stdout stays reserved for the single config-dir line. See the output
# contract above.
info() { printf '\033[1;34m==>\033[0m %s\n' "$*" >&2; }

# NOTE: this script used to PREPEND /home/linuxbrew/.linuxbrew/bin to PATH. That
# silently decided which cmake, ninja, curl, git and duckdb ran on any machine with
# Linuxbrew installed, overriding the system toolchain. If you want a Homebrew
# toolchain, put it on PATH yourself.

if [[ -f "$PREFIX/lib/cmake/vtk-$VTK_SERIES/vtk-config.cmake" ]] \
   || [[ -f "$PREFIX/lib/cmake/vtk-$VTK_SERIES/VTKConfig.cmake" ]]; then
  info "VTK $VTK_VERSION already installed at $PREFIX — nothing to do"
  echo "$PREFIX/lib/cmake/vtk-$VTK_SERIES"
  exit 0
fi

mkdir -p "$WORK"
cd "$WORK"

TARBALL="VTK-$VTK_VERSION.tar.gz"
if [[ ! -f "$TARBALL" ]]; then
  info "Downloading VTK $VTK_VERSION source (~54 MB)"
  # --http1.1 avoids the HTTP/2 PROTOCOL_ERROR seen against ghcr.io/vtk.org here.
  # -C - resumes a partial download rather than restarting.
  # --no-progress-meter: the meter is not a TTY-aware thing in curl, so without it
  # every CI log (and every captured stderr) gets thousands of progress lines.
  curl -fSL --http1.1 --no-progress-meter -C - -o "$TARBALL.part" \
    "https://www.vtk.org/files/release/$VTK_SERIES/$TARBALL"
  mv "$TARBALL.part" "$TARBALL"
fi

if [[ ! -d "VTK-$VTK_VERSION" ]]; then
  info "Extracting"
  tar xzf "$TARBALL"
fi

info "Configuring minimal build (no rendering / Qt / Python / testing)"
# Module selection strategy: turn everything off, then explicitly request the
# five modules we link. VTK resolves their dependencies automatically (IOXML
# pulls IOXMLParser, expat, etc.), so this yields the smallest closure that can
# still read every serial VTK mesh format.
# Route VTK's own compilation through ccache when available. VTK is ~1275
# translation units, so on a second build (a repeated container run, a CI cache hit,
# a version bump that touches little) this is the difference between ~20 minutes and
# under one. Deliberately not forced if the caller already set a launcher.
CCACHE_ARG=""
if [[ -z "${CMAKE_CXX_COMPILER_LAUNCHER:-}" ]] && command -v ccache >/dev/null 2>&1; then
  CCACHE_ARG="-DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache"
  info "using ccache for the VTK build"
fi

cmake -S "VTK-$VTK_VERSION" -B build -G Ninja \
  $CCACHE_ARG \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DBUILD_SHARED_LIBS=ON \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DVTK_BUILD_TESTING=OFF \
  -DVTK_BUILD_EXAMPLES=OFF \
  -DVTK_BUILD_DOCUMENTATION=OFF \
  -DVTK_WRAP_PYTHON=OFF \
  -DVTK_WRAP_JAVA=OFF \
  -DVTK_USE_64BIT_IDS=ON \
  -DVTK_GROUP_ENABLE_Rendering=NO \
  -DVTK_GROUP_ENABLE_Qt=NO \
  -DVTK_GROUP_ENABLE_Views=NO \
  -DVTK_GROUP_ENABLE_Web=NO \
  -DVTK_GROUP_ENABLE_Imaging=NO \
  -DVTK_GROUP_ENABLE_MPI=NO \
  -DVTK_GROUP_ENABLE_StandAlone=DONT_WANT \
  -DVTK_MODULE_ENABLE_VTK_CommonCore=YES \
  -DVTK_MODULE_ENABLE_VTK_CommonDataModel=YES \
  -DVTK_MODULE_ENABLE_VTK_CommonExecutionModel=YES \
  -DVTK_MODULE_ENABLE_VTK_CommonMisc=YES \
  -DVTK_MODULE_ENABLE_VTK_IOLegacy=YES \
  -DVTK_MODULE_ENABLE_VTK_IOXML=YES \
  -DVTK_MODULE_ENABLE_VTK_FiltersCore=YES
# Deliberately NOT enabled:
#   VTK_MODULE_ENABLE_VTK_IOParallelXML=YES — writers only, and we are read-only.
#   VTK_MODULE_ENABLE_VTK_IOGeometry=YES
#     IOGeometry (OBJ/STL/PLY readers) depends on FiltersHybrid, which depends on
#     RenderingCore. With VTK_GROUP_ENABLE_Rendering=NO the configure step fails:
#       "The VTK::IOGeometry module ... requires the disabled module
#        VTK::FiltersHybrid (disabled due to the VTK::RenderingCore module not
#        being available)"
#     Those formats are out of Phase 1 scope. If they are ever wanted, either
#     enable RenderingCore (dragging in OpenGL) or read them via a different
#     module — do not "fix" this by turning rendering back on casually.
#   IOExodus / IOCGNS / IOHDF
#     Need external netcdf / cgns / hdf5. Phase 4 concerns; adding them means
#     installing those libraries first. cmake/DuckVTKFindVTK.cmake probes for
#     them as OPTIONAL components, so their absence degrades gracefully to
#     "that format is unsupported" rather than breaking the build.

info "Building with $JOBS jobs"
cmake --build build --parallel "$JOBS"

info "Installing to $PREFIX"
cmake --install build

CFG_DIR="$(ls -d "$PREFIX"/lib/cmake/vtk-* 2>/dev/null | head -1 || true)"
if [[ -z "$CFG_DIR" ]]; then
  echo "ERROR: install finished but no lib/cmake/vtk-* directory found under $PREFIX" >&2
  exit 1
fi

info "Done. VTK_DIR=$CFG_DIR"
info "Libraries:"
ls "$PREFIX"/lib/libvtk*.so* 2>/dev/null | head -20 >&2 || true
cat >&2 <<EOF

Build the extension against it with:
    make release VTK_DIR=$CFG_DIR
or export it once:
    export VTK_DIR=$CFG_DIR
EOF
echo "$CFG_DIR"
