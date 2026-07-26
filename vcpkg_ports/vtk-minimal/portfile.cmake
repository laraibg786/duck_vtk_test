# vtk-minimal — VTK with only the modules duck_vtk needs to read mesh files.
#
# Design notes, because a vcpkg port is easy to get subtly wrong:
#
#  * NO SOURCE PATCHES. Every reduction below uses an upstream, documented VTK
#    CMake option. That matters for maintainability: upgrading VTK is a version +
#    hash change, with no patch to re-base and no risk of a patch silently
#    applying to the wrong lines.
#
#  * The module set is exactly what src/ links, and is kept in step with
#    cmake/DuckVTKFindVTK.cmake. If you add a component there, add it here too or
#    CI will fail at link time while local builds keep working.
#
#  * Static by default. DuckDB loadable extensions must be self-contained: a
#    .duckdb_extension that needs libvtkCommonCore-9.6.so on the user's machine is
#    not distributable, which is the whole point of building for the community repo.

#  * RELEASE ONLY. Nothing links a debug VTK: the extension ships release, and a
#    debug VTK would only ever be built to be thrown away. Skipping it halves both
#    build time and disk.
#
#    This is load-bearing, not an optimisation. Most triplets community-extensions
#    uses are already release-only (x64-linux-release,
#    x64-windows-static-md-release-vs2019comp), but `x64-mingw-static` is NOT, so
#    it built VTK twice and exhausted the runner's disk mid-compile:
#      Fatal error: can't write 11 bytes to section .text of Common/Core/...
#    which reads like a compiler bug rather than ENOSPC and cost real time to
#    diagnose. Setting it in the portfile covers every triplet regardless.
#
#    Setting VCPKG_BUILD_TYPE in a portfile is an established upstream pattern —
#    ports/cgal, ports/si and ports/nonstd-bit-lite all do exactly this.
set(VCPKG_BUILD_TYPE release)

vcpkg_download_distfile(ARCHIVE
    # vtk.org is the canonical source. A GitHub mirror of the same release is
    # listed second so a vtk.org outage does not break every build.
    URLS
        "https://www.vtk.org/files/release/9.6/VTK-9.6.2.tar.gz"
        "https://github.com/Kitware/VTK/releases/download/v9.6.2/VTK-9.6.2.tar.gz"
    FILENAME "VTK-9.6.2.tar.gz"
    SHA512 1a8d6c89ab03961f59181b12d06d9baca09445485e1cb5ac78a81ec4a2f9d8a25e87d6112f932d856782d0fcab23880295a95fd38e7f9b0b9592138fb2e2e671
)

vcpkg_extract_source_archive(SOURCE_PATH ARCHIVE "${ARCHIVE}")

# DuckDB extensions are shared modules, so everything linked in must be PIC even
# when VTK itself is built static.
string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "dynamic" VTK_BUILD_SHARED)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DBUILD_SHARED_LIBS=${VTK_BUILD_SHARED}
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON

        # --- what we do NOT build -------------------------------------------
        # Rendering is the big one: it drags in OpenGL, glew, freetype, gl2ps and
        # X11. A SQL engine never renders anything.
        -DVTK_GROUP_ENABLE_Rendering=NO
        -DVTK_GROUP_ENABLE_Qt=NO
        -DVTK_GROUP_ENABLE_Views=NO
        -DVTK_GROUP_ENABLE_Web=NO
        -DVTK_GROUP_ENABLE_Imaging=NO
        -DVTK_GROUP_ENABLE_MPI=NO
        # DONT_WANT rather than NO: StandAlone contains modules that our IO
        # modules legitimately depend on, so let VTK pull in what it must and
        # nothing more.
        -DVTK_GROUP_ENABLE_StandAlone=DONT_WANT
        -DVTK_BUILD_ALL_MODULES=OFF
        -DVTK_BUILD_TESTING=OFF
        -DVTK_BUILD_EXAMPLES=OFF
        -DVTK_BUILD_DOCUMENTATION=OFF
        -DVTK_WRAP_PYTHON=OFF
        -DVTK_WRAP_JAVA=OFF
        -DVTK_ENABLE_WRAPPING=OFF
        # No CLI tools; we only want libraries and headers.
        -DVTK_INSTALL_SDK=ON

        # --- what we DO build ------------------------------------------------
        # Keep in step with DUCK_VTK_REQUIRED_COMPONENTS in
        # cmake/DuckVTKFindVTK.cmake.
        -DVTK_MODULE_ENABLE_VTK_CommonCore=YES
        -DVTK_MODULE_ENABLE_VTK_CommonDataModel=YES
        -DVTK_MODULE_ENABLE_VTK_CommonExecutionModel=YES
        # CommonMisc holds vtkErrorCode. Omitting it gives a "DSO missing from
        # command line" link error that names no obvious culprit.
        -DVTK_MODULE_ENABLE_VTK_CommonMisc=YES
        -DVTK_MODULE_ENABLE_VTK_IOLegacy=YES
        -DVTK_MODULE_ENABLE_VTK_IOXML=YES
        -DVTK_MODULE_ENABLE_VTK_FiltersCore=YES

        # IOParallelXML is deliberately NOT enabled. All 20 of its classes are
        # WRITERS (vtkXMLP*Writer and writer helpers) — every vtkXMLP*Reader lives
        # in IOXML — and we are read-only. Verified: nothing links it
        # (`objdump -p libvtkIOXML` does not list it), and the .pvtu/.pvti
        # rejection in src/vtk/vtk_dataset.cpp is a pure string sniff on the XML
        # type name, so it needs no parallel module at all.

        # NOT enabled: VTK_MODULE_ENABLE_VTK_IOGeometry. It requires
        # FiltersHybrid, which requires RenderingCore, so with rendering disabled
        # VTK's own configure step FAILS on it. Do not "fix" a missing OBJ/STL
        # reader by re-enabling rendering.

        # --- use vcpkg's copies of shared third-party libs -------------------
        # Anything VTK vendors that vcpkg already provides should come from vcpkg,
        # so we do not ship two copies of zlib in one process.
        -DVTK_MODULE_USE_EXTERNAL_VTK_expat=ON
        -DVTK_MODULE_USE_EXTERNAL_VTK_lz4=ON
        -DVTK_MODULE_USE_EXTERNAL_VTK_zlib=ON
        # Deliberately left vendored: doubleconversion, lzma, utf8, pugixml,
        # token, fast_float, nlohmann_json, verdict, exprtk, loguru, kissfft.
        # Adding each as a vcpkg dependency buys nothing here and multiplies the
        # ways a build can break, which is the opposite of the point.
    MAYBE_UNUSED_VARIABLES
        VTK_GROUP_ENABLE_Qt
        VTK_GROUP_ENABLE_Web
        VTK_GROUP_ENABLE_MPI
        VTK_WRAP_JAVA
        VTK_ENABLE_WRAPPING
)

vcpkg_cmake_install()

# VTK installs its config as lib/cmake/vtk-9.6/. vcpkg_cmake_config_fixup moves it
# to share/vtk-minimal/ and rewrites the paths, which is what makes
# find_package(VTK) work through the vcpkg toolchain.
vcpkg_cmake_config_fixup(PACKAGE_NAME VTK CONFIG_PATH lib/cmake/vtk-9.6)

vcpkg_copy_pdbs()

# Strip everything that is not a library, header or CMake config.
file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/debug/bin"
    "${CURRENT_PACKAGES_DIR}/bin"
    "${CURRENT_PACKAGES_DIR}/share/licenses"
    "${CURRENT_PACKAGES_DIR}/share/vtk/doxygen"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/Copyright.txt")
