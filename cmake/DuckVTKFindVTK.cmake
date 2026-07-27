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
# 9.6.2 is correct: verified locally, and green through the community-extensions
# pipeline on linux_amd64 and osx_arm64. 9.2 through 9.5 are UNTESTED; they are
# excluded because there is no evidence for them, not because they are known bad.
# If you validate one, lower this and say so here.
#
# Note also that src/vtk/vtk_dataset.cpp still restricts itself to the 9.1 API
# surface for in-memory reads (SetInputString(const std::string &) rather than
# SetInputArray, which arrived in 9.6). That is deliberate: it costs nothing and
# keeps the door open to lowering this floor later.
#
# ---------------------------------------------------------------------------
# WHY 9.6.2 AND NOT SOME OTHER RELEASE — the version policy
# ---------------------------------------------------------------------------
# Recorded because "9.6.2" looks like an arbitrary tag picked off a download page,
# and it is not. Kitware's actual 9.x release history (tag dates from the VTK
# repository):
#
#   v9.4.0  2024-11-22    v9.5.0  2025-06-20    v9.6.0  2026-02-09
#   v9.4.1  2024-12-26    v9.5.1  2025-08-28    v9.6.1  2026-03-24
#   v9.4.2  2025-03-27    v9.5.2  2025-09-16    v9.6.2  2026-05-15
#   v9.7.0.rc0  2026-07-08 … rc3  2026-07-27
#
# Read off that table:
#   * A new MINOR line lands roughly every 7-8 months, with 2-3 patch releases
#     settling it over the following ~3 months.
#   * 9.6.2 is the THIRD patch of the current line — i.e. the newest stable
#     release, and a settled one, not a .0.
#   * 9.7.0 is in release-candidate as of this writing. Do NOT pin an rc: it is
#     explicitly not a stable release, and a community extension that ships one
#     inherits every bug found between rc and final.
#
# The alternative worth considering is 9.5.2, the last patch of the previous line
# and ~8 months more settled. It is deliberately NOT chosen: it has never been
# built or tested by this project, so switching to it would trade a configuration
# that is green across every platform in the community pipeline for one that is
# merely older. Age is not evidence.
#
# POLICY when 9.7.0 final ships:
#   1. Bump vcpkg_ports/vtk-minimal (version, URL, SHA512) and the default in
#      scripts/build_minimal_vtk.sh. Those two must move together —
#      `make submit-check` compares the module lists but NOT the versions.
#   2. Leave this floor at 9.6 unless a 9.7 API is actually needed. The floor and
#      the pin answer different questions: the pin is what we build and ship, the
#      floor is what we refuse to build against. Raising the floor in lockstep
#      with the pin needlessly breaks anyone building against a system VTK.
#   3. Re-run `make ci-verify` and the full `make check` before trusting it. VTK
#      minor releases have changed module dependency edges before.
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
    FiltersCore           # pulled in transitively by IOLegacy and IOHDF; named
                          # explicitly so this list and the vcpkg port's
                          # VTK_MODULE_ENABLE_VTK_* set are literally the same set,
                          # which is what scripts/submit_check.sh compares.
)

# ---------------------------------------------------------------------------
# There is deliberately NO optional-component probe.
# ---------------------------------------------------------------------------
# There used to be one: a DUCK_VTK_OPTIONAL_COMPONENTS list (IOEnSight, IOExodus,
# IOCGNS, IOHDF) probed one at a time, with a DUCK_VTK_HAVE_<COMP>=1 compile
# definition emitted for each one found. It was removed, and the reasons are worth
# keeping so it does not come back by accident:
#
#  1. It did nothing. Nothing in src/ ever referenced a DUCK_VTK_HAVE_* macro, and
#     src/vtk/vtk_dataset.cpp registers no reader for any of those formats. The
#     only observable effect was linking extra VTK libraries that were never called.
#
#  2. `IOCGNS` is not a VTK component. The module is VTK::IOCGNSReader (see
#     VTK's IO/CGNS/vtk.module). So that probe could never succeed and had been
#     silently failing for its whole existence — the exact failure mode a probe
#     that "degrades gracefully" is guaranteed to hide.
#
#  3. It made the build environment-dependent in a way nothing tested. Against the
#     vtk-minimal vcpkg port none of those modules exist, so nothing linked.
#     Against a distro or Homebrew VTK they all exist, so four extra libraries
#     linked and four extra macros were defined. Same source, same command, two
#     different binaries depending on which VTK happened to be installed. For a
#     project whose whole dependency argument is "eliminate the ABI/config variable
#     rather than test for it", that is the wrong default.
#
# The right shape when one of these formats is actually implemented: add the
# component to DUCK_VTK_REQUIRED_COMPONENTS above AND enable it in
# vcpkg_ports/vtk-minimal/portfile.cmake. submit_check.sh already fails if those two
# disagree, so support becomes a declared, verified fact instead of a property of
# the build machine. docs/ROADMAP.md records the per-format cost of doing that.
#
# NOTE while reading that roadmap: IOGeometry (OBJ/STL/OpenFOAM) cannot simply be
# added. It PRIVATE_DEPENDS on VTK::RenderingCore, so VTK's own configure step fails
# for it in a rendering-free build. That is a real upstream constraint, not a flag we
# are missing.

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

# Re-run the find as REQUIRED so VTK_LIBRARIES is populated with exactly the set we
# intend to link and hand to vtk_module_autoinit.
#
# One set, not "required plus whatever else happened to be installed": the module
# set the extension links must be a property of this file and the vcpkg port, never
# of the build machine. See the note above the component list.
find_package(VTK ${DUCK_VTK_MIN_VERSION} REQUIRED
  COMPONENTS ${DUCK_VTK_REQUIRED_COMPONENTS})

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
message(STATUS "duck_vtk: linked VTK modules     = ${VTK_LIBRARIES}")
