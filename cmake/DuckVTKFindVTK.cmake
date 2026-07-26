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
# 9.1 rather than 9.0 because 9.1 is the oldest version actually validated: Ubuntu
# 24.04 ships it and CI builds against it. The binding constraint is
# vtkXMLReader's in-memory API — 9.1/9.3 expose only
# SetInputString(const std::string &), while SetInputArray arrived in 9.6 — so
# src/vtk/vtk_dataset.cpp deliberately restricts itself to the 9.1 surface.
# Claiming 9.0 would be asserting something untested.
set(DUCK_VTK_MIN_VERSION 9.1)

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
)

# Optional components. Phase 4 formats. Requested separately so a VTK build
# lacking them degrades to "that format is unsupported" rather than failing the
# whole configure step.
# NOTE: IOGeometry is deliberately absent. It requires FiltersHybrid ->
# RenderingCore, so it cannot exist in a rendering-free VTK build; requesting it
# makes VTK's own configure step fail. See scripts/build_minimal_vtk.sh.
set(DUCK_VTK_OPTIONAL_COMPONENTS
    IOEnSight             # EnSight Gold — common in CFD
    IOExodus              # ExodusII (.ex2) — common in FEA
    IOCGNS                # CGNS — CFD standard
    IOHDF                 # VTKHDF
    IOParallelXML
    FiltersCore           # only if a derived-quantity feature needs it
)

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
    "  * Debian/Ubuntu system package (needs root, VTK 9.3):\n"
    "        sudo apt install libvtk9-dev\n"
    "  * Homebrew bottle (large: pulls Qt/mesa/llvm; carries an ABI risk):\n"
    "        brew install vtk\n"
    "\n"
    "If VTK is installed somewhere unusual, pass it explicitly:\n"
    "  make release EXT_FLAGS='-DVTK_DIR=/path/to/lib/cmake/vtk-9.6'\n")
endif()

# Probe the optional components one at a time. find_package with a failing
# component in the main call would abort even though these are non-essential.
set(DUCK_VTK_ENABLED_OPTIONAL "")
foreach(_comp ${DUCK_VTK_OPTIONAL_COMPONENTS})
  find_package(VTK ${DUCK_VTK_MIN_VERSION} QUIET COMPONENTS ${_comp})
  if(TARGET VTK::${_comp})
    list(APPEND DUCK_VTK_ENABLED_OPTIONAL ${_comp})
  endif()
endforeach()

# Re-run the find with the full resolved set so VTK_LIBRARIES contains
# everything we intend to link and hand to vtk_module_autoinit.
find_package(VTK ${DUCK_VTK_MIN_VERSION} REQUIRED
  COMPONENTS ${DUCK_VTK_REQUIRED_COMPONENTS} ${DUCK_VTK_ENABLED_OPTIONAL})

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
message(STATUS "duck_vtk: optional components on = ${DUCK_VTK_ENABLED_OPTIONAL}")

# Record which optional formats are available so the C++ can #ifdef the reader
# factory entries and report honestly in vtk_info / error messages.
foreach(_comp ${DUCK_VTK_ENABLED_OPTIONAL})
  string(TOUPPER "${_comp}" _comp_uc)
  add_compile_definitions(DUCK_VTK_HAVE_${_comp_uc}=1)
endforeach()
