# VTK discovery for duck_vtk.
#
# Isolated from the main CMakeLists so that:
#   * the component list has one authoritative home;
#   * alternate VTK installations (Homebrew bottle, Debian libvtk9-dev, a local
#     minimal source build) can be selected with -DVTK_DIR=... ;
#   * failures produce an actionable message instead of CMake's default
#     "Could not find a package configuration file provided by VTK".
#
# Exports to the caller:
#   VTK_LIBRARIES         — the module targets to link and to pass to autoinit
#   DUCK_VTK_LIBRARY_DIR  — directory holding the VTK shared libraries, for RPATH

# Minimum VTK.
#
# 9.6, and this floor is evidence-based rather than cautious. It was 9.1 — on the
# reasoning that Ubuntu 24.04 ships 9.1 and CI built against it — and that was
# WRONG: it compiles there and then silently returns empty meshes.
#
# ROOT CAUSE, corrected. This was originally recorded as "VTK 9.1 cannot parse
# appended data", which is not quite right and would have misled the next person.
# The real defect is an incompatibility between VTK older than 9.3.1 and expat
# >= 2.6.0: VTK deliberately ends the XML document early at <AppendedData> and then
# keeps feeding the appended bytes to expat, which expat 2.6.0 (Feb 2024) began
# rejecting. Upstream fixed it in commit db8f9efca220 ("vtkXMLDataParser: track
# AppendedData state explicitly"), first released in VTK 9.3.1.
#   https://gitlab.kitware.com/vtk/vtk/-/issues/19258
# Ubuntu 24.04 ships expat 2.6.1 and builds VTK against system expat, which is why
# it reproduces there. An upstream 9.1 build with VTK's own vendored expat 2.4.1
# does NOT reproduce it.
#
# This matters for us specifically: the vcpkg port sets
# VTK_MODULE_USE_EXTERNAL_VTK_expat=ON, so we are always on the external-expat path
# where the bug bites.
#
# What was measured, on Ubuntu 24.04's libvtk9.1t64 (9.1.0+dfsg2):
#   every XML file containing an <AppendedData> section fails to parse —
#     vtkXMLDataParser: Error parsing XML in stream at line 32, byte index 2123:
#         junk after document element
#     vtkXMLReader:     Error parsing input file.  ReadXMLInformation aborting.
#   while GetErrorCode() stays at Success and the reader hands back a valid but
#   EMPTY dataset. test/data/xml/cow.vtp reported 0 points instead of 2903.
#   Files without an appended section (e.g. vase.vti) read correctly, which is
#   why this hid for so long: it looks like a per-file problem, not a per-version
#   one.
#
# Appended data is not an edge case — it is what most real XML writers emit — so
# a VTK that cannot read it is not usable for this extension.
#
# The floor stays at 9.6 rather than dropping to 9.3.1. 9.3.1 through 9.5.x contain
# the fix and are therefore not known-bad — they are simply UNTESTED here, and a
# floor should assert what has been verified. 9.1, 9.2 and 9.3.0 are genuinely
# excluded: they predate the fix. If you validate a version in the 9.3.1-9.5 range,
# lower this and record the evidence.
#
# Note also that src/vtk/vtk_dataset.cpp still restricts itself to the 9.1 API
# surface for in-memory reads (SetInputString(const std::string &) rather than
# SetInputArray, which arrived in 9.6). That is deliberate: it costs nothing and
# keeps the door open to lowering this floor later.
set(DUCK_VTK_MIN_VERSION 9.6)

# ---------------------------------------------------------------------------
# Component set
# ---------------------------------------------------------------------------
# Deliberately minimal: reading meshes and their attribute arrays. We do NOT
# request any Rendering*, Interaction*, Views*, or GUISupport* modules — pulling
# those in would link OpenGL/Qt into a database extension for no benefit, and
# would make the module fail to load on headless machines.
#
# Note the Homebrew bottle is *built* with rendering support (hence its mesa/Qt
# dependencies), but we only need to LINK the modules listed here.
set(DUCK_VTK_REQUIRED_COMPONENTS
    CommonCore            # vtkObject, vtkDataArray, vtkSmartPointer, type constants
    CommonDataModel       # vtkDataSet, vtkUnstructuredGrid, vtkPolyData, vtkCellArray
    CommonExecutionModel  # vtkAlgorithm, the pipeline (Update/UpdateInformation)
    CommonMisc            # vtkErrorCode. NOT in CommonCore — omitting it gives
                          # a 'DSO missing from command line' link error.
    IOLegacy              # legacy .vtk readers
    IOXML                 # .vtu/.vtp/.vts/.vtr/.vti and the parallel/multiblock variants
    FiltersCore           # not optional: IOLegacy -> IOCellGrid -> FiltersCellGrid
                          # -> FiltersCore, so it is linked whatever we ask for.
                          # Both vcpkg_ports/vtk-minimal and build_minimal_vtk.sh
                          # enable it explicitly; listing it here keeps the three in
                          # step and lets submit_check.sh verify that.
)

# There is deliberately NO optional-component list any more.
#
# There used to be one (IOEnSight, IOExodus, IOCGNS, IOHDF) with a per-component
# probe loop and DUCK_VTK_HAVE_<NAME> compile definitions, described as letting a
# VTK without them "degrade gracefully". All of that was dead:
#
#   * neither supported VTK source enables them — not vcpkg_ports/vtk-minimal, not
#     scripts/build_minimal_vtk.sh — so DUCK_VTK_HAVE_* was never defined in any
#     shipping build;
#   * nothing in src/ ever referenced DUCK_VTK_HAVE_* (grep: zero hits), so the
#     definitions had no effect even where they would have been set;
#   * src/vtk/vtk_dataset.cpp only ever instantiates vtkXMLGenericDataObjectReader,
#     vtkGenericDataObjectReader and vtkDataObjectReader, so an .ex2/.cgns/.vtkhdf
#     file fails with "Unrecognized file type" whether or not the module is linked.
#
# It also cost three extra find_package invocations on every configure, and would
# have pulled netCDF/HDF5/CGNS into a database extension for no benefit.
#
# NOTE: IOGeometry is deliberately absent too, for a different reason. It requires
# FiltersHybrid -> RenderingCore, so it cannot exist in a rendering-free VTK build;
# requesting it makes VTK's own configure step fail. See scripts/build_minimal_vtk.sh.
#
# When these formats are actually implemented, add the module to the required list,
# to the vcpkg port and to the build script together, and make the reader factory
# instantiate it.

# ---------------------------------------------------------------------------
# Locate a VTK config directory if the user did not specify one
# ---------------------------------------------------------------------------
# When building through vcpkg (which is how the DuckDB community-extensions CI
# builds every extension), VTK comes from our vtk-minimal overlay port and the
# vcpkg toolchain already puts it on CMAKE_PREFIX_PATH. Probing local prefixes in
# that case would be actively harmful: it could bind against a system VTK built by
# a different compiler than the one vcpkg used, which is the ABI hazard this
# project went out of its way to eliminate. So when vcpkg is in play, let
# find_package resolve it and do not guess.
if(DEFINED VCPKG_TOOLCHAIN OR DEFINED ENV{VCPKG_TOOLCHAIN_PATH} OR DEFINED VCPKG_TARGET_TRIPLET)
  set(DUCK_VTK_VIA_VCPKG TRUE)
  message(STATUS "duck_vtk: vcpkg detected — VTK will be resolved by the toolchain (vtk-minimal port)")
endif()

if(NOT DUCK_VTK_VIA_VCPKG AND (NOT DEFINED VTK_DIR OR VTK_DIR STREQUAL ""))
  # Candidate prefixes in priority order. The minimal source build from
  # scripts/build_minimal_vtk.sh comes FIRST because it is this project's
  # documented default (see architecture doc §5): it is ABI-matched to the
  # compiler building the extension, whereas a Homebrew bottle is not.
  file(GLOB _duck_vtk_local_prefixes "$ENV{HOME}/.local/vtk-*")
  list(SORT _duck_vtk_local_prefixes)
  list(REVERSE _duck_vtk_local_prefixes)      # newest version first
  set(_duck_vtk_prefixes
      ${_duck_vtk_local_prefixes}
      "$ENV{HOMEBREW_PREFIX}/opt/vtk"
      "/home/linuxbrew/.linuxbrew/opt/vtk"
      "/opt/homebrew/opt/vtk"
      "/usr/local"
      "/usr")
  foreach(_prefix ${_duck_vtk_prefixes})
    if(_prefix AND EXISTS "${_prefix}")
      file(GLOB _cfg_dirs
           "${_prefix}/lib/cmake/vtk-*"
           "${_prefix}/lib/cmake/vtk"
           "${_prefix}/lib/*/cmake/vtk-*")   # Debian multiarch: lib/x86_64-linux-gnu/cmake
      if(_cfg_dirs)
        list(SORT _cfg_dirs)
        list(REVERSE _cfg_dirs)             # prefer the highest version
        list(GET _cfg_dirs 0 VTK_DIR)
        message(STATUS "duck_vtk: auto-detected VTK_DIR=${VTK_DIR}")
        break()
      endif()
    endif()
  endforeach()
endif()

find_package(VTK ${DUCK_VTK_MIN_VERSION} QUIET COMPONENTS ${DUCK_VTK_REQUIRED_COMPONENTS})

if(NOT VTK_FOUND)
  message(FATAL_ERROR
    "duck_vtk: could not find VTK >= ${DUCK_VTK_MIN_VERSION} with the required components.\n"
    "  Required: ${DUCK_VTK_REQUIRED_COMPONENTS}\n"
    "  Tried VTK_DIR='${VTK_DIR}'\n"
    "\n"
    "  (vcpkg in use: ${DUCK_VTK_VIA_VCPKG} — if TRUE, check that vtk-minimal built)\n"
    "\n"
    "Install one of the following, then re-run:\n"
    "  * RECOMMENDED, no root needed, ABI-matched to your compiler:\n"
    "        ./scripts/build_minimal_vtk.sh\n"
    "  * Homebrew bottle (large: pulls Qt/mesa/llvm; carries an ABI risk):\n"
    "        brew install vtk\n"
    "\n"
    "NOT the distro package: Ubuntu/Debian's libvtk9-dev is older than this floor,\n"
    "and silently returns EMPTY meshes for XML files with an <AppendedData>\n"
    "section. This message used to recommend it, which could not have worked.\n"
    "\n"
    "If VTK is installed somewhere unusual, pass it explicitly:\n"
    "  make release EXT_FLAGS='-DVTK_DIR=/path/to/lib/cmake/vtk-9.6'\n")
endif()

# Re-run as REQUIRED so the failure mode is a clear CMake error rather than a
# half-populated VTK_LIBRARIES that fails at link time.
find_package(VTK ${DUCK_VTK_MIN_VERSION} REQUIRED COMPONENTS ${DUCK_VTK_REQUIRED_COMPONENTS})

# ---------------------------------------------------------------------------
# Derive the library directory for RPATH purposes
# ---------------------------------------------------------------------------
# VTK_PREFIX_PATH is set by VTKConfig.cmake. Fall back to walking up from
# VTK_DIR (…/lib/cmake/vtk-9.6 → …/lib) if it is unavailable.
set(DUCK_VTK_LIBRARY_DIR "")
if(DEFINED VTK_PREFIX_PATH AND EXISTS "${VTK_PREFIX_PATH}/lib")
  set(DUCK_VTK_LIBRARY_DIR "${VTK_PREFIX_PATH}/lib")
else()
  get_filename_component(_d "${VTK_DIR}" DIRECTORY)   # …/lib/cmake
  get_filename_component(_d "${_d}" DIRECTORY)        # …/lib
  if(EXISTS "${_d}")
    set(DUCK_VTK_LIBRARY_DIR "${_d}")
  endif()
endif()

# ---------------------------------------------------------------------------
# Report, loudly. These values matter when diagnosing a load failure, and
# scripts/smoke.sh greps the build log for them.
# ---------------------------------------------------------------------------
message(STATUS "duck_vtk: VTK_VERSION            = ${VTK_VERSION}")
message(STATUS "duck_vtk: VTK_DIR                = ${VTK_DIR}")
message(STATUS "duck_vtk: VTK library dir        = ${DUCK_VTK_LIBRARY_DIR}")
message(STATUS "duck_vtk: required components    = ${DUCK_VTK_REQUIRED_COMPONENTS}")
