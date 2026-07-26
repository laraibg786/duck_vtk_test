# Compiler/optimisation settings for duck_vtk.
#
# Philosophy: default to fast-and-portable. Every flag that trades portability or
# build robustness for speed is opt-in and documented, because a database
# extension that crashes or refuses to load on a slightly different machine is
# worth far less than one that is 5% slower.
#
# Options (all -D on the cmake line, or via `make release DUCK_VTK_...=ON`):
#   DUCK_VTK_NATIVE_ARCH   OFF  -march=native. Fastest, NOT portable.
#   DUCK_VTK_LTO           OFF  Link-time optimisation.
#   DUCK_VTK_WERROR        OFF  Warnings as errors (CI).
#   DUCK_VTK_ASAN          OFF  Address+UB sanitizer (debug builds).

include(CheckCXXCompilerFlag)

option(DUCK_VTK_NATIVE_ARCH "Optimise for the building machine's CPU (-march=native). Not portable." OFF)
option(DUCK_VTK_LTO "Enable link-time optimisation." OFF)
option(DUCK_VTK_WERROR "Treat warnings as errors." OFF)
option(DUCK_VTK_ASAN "Enable Address/UB sanitizers (use with a Debug build)." OFF)

set(DUCK_VTK_EXTRA_FLAGS "")
set(DUCK_VTK_EXTRA_LINK_FLAGS "")

# ---------------------------------------------------------------------------
# Always-on, safe optimisations
# ---------------------------------------------------------------------------
# CMAKE_BUILD_TYPE=Release already supplies -O3 -DNDEBUG. These add to it without
# affecting portability or numerical results.
function(duck_vtk_try_flag FLAG VAR)
  string(MAKE_C_IDENTIFIER "HAVE${FLAG}" _probe)
  check_cxx_compiler_flag("${FLAG}" ${_probe})
  if(${_probe})
    set(${VAR} "${${VAR}};${FLAG}" PARENT_SCOPE)
  endif()
endfunction()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # Devirtualise and inline across the shared-library boundary within our own
  # module. Safe, and worth a few percent given how many small virtual calls the
  # catalog layer makes.
  duck_vtk_try_flag(-fno-semantic-interposition DUCK_VTK_EXTRA_FLAGS)

  # Let the linker drop unreferenced code/data. DuckDB already passes
  # -Wl,--gc-sections for the loadable module, so giving it per-function sections
  # actually lets that work.
  duck_vtk_try_flag(-ffunction-sections DUCK_VTK_EXTRA_FLAGS)
  duck_vtk_try_flag(-fdata-sections DUCK_VTK_EXTRA_FLAGS)

  # NOT enabled, deliberately:
  #   -ffast-math / -Ofast : would licence the compiler to reorder floating-point
  #     arithmetic and assume no NaN/Inf. This extension must pass NaN and ±Inf
  #     through unchanged (design §8, verified by nan_inf.vtu), so fast-math would
  #     turn a correctness requirement into undefined behaviour. Never enable it.
  #   -fvisibility=hidden : DuckDB's build_loadable_extension already applies it.
endif()

if(DUCK_VTK_NATIVE_ARCH)
  duck_vtk_try_flag(-march=native DUCK_VTK_EXTRA_FLAGS)
  duck_vtk_try_flag(-mtune=native DUCK_VTK_EXTRA_FLAGS)
  message(WARNING "duck_vtk: DUCK_VTK_NATIVE_ARCH=ON — the resulting extension may "
                  "crash with SIGILL on machines with an older CPU. Do not ship it.")
endif()

if(DUCK_VTK_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_msg)
  if(_ipo_ok)
    set(DUCK_VTK_ENABLE_IPO ON)
    message(STATUS "duck_vtk: LTO enabled")
  else()
    message(WARNING "duck_vtk: LTO requested but unsupported: ${_ipo_msg}")
  endif()
endif()

if(DUCK_VTK_WERROR)
  duck_vtk_try_flag(-Werror DUCK_VTK_EXTRA_FLAGS)
  duck_vtk_try_flag(-Wall DUCK_VTK_EXTRA_FLAGS)
  duck_vtk_try_flag(-Wextra DUCK_VTK_EXTRA_FLAGS)
  # VTK's own headers emit deprecation notices; failing our build over a
  # third-party header would be self-defeating.
  duck_vtk_try_flag(-Wno-deprecated-declarations DUCK_VTK_EXTRA_FLAGS)
  duck_vtk_try_flag(-Wno-unused-parameter DUCK_VTK_EXTRA_FLAGS)
endif()

if(DUCK_VTK_ASAN)
  duck_vtk_try_flag(-fsanitize=address DUCK_VTK_EXTRA_FLAGS)
  duck_vtk_try_flag(-fsanitize=undefined DUCK_VTK_EXTRA_FLAGS)
  duck_vtk_try_flag(-fno-omit-frame-pointer DUCK_VTK_EXTRA_FLAGS)
  set(DUCK_VTK_EXTRA_LINK_FLAGS "${DUCK_VTK_EXTRA_LINK_FLAGS};-fsanitize=address;-fsanitize=undefined")
endif()

# ---------------------------------------------------------------------------
# ccache
# ---------------------------------------------------------------------------
# DuckDB is a large build; ccache turns a 10-minute rebuild into seconds. Applied
# only if the user has not already configured a launcher.
if(NOT CMAKE_CXX_COMPILER_LAUNCHER)
  find_program(DUCK_VTK_CCACHE ccache)
  if(DUCK_VTK_CCACHE)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${DUCK_VTK_CCACHE}" CACHE STRING "" FORCE)
    set(CMAKE_C_COMPILER_LAUNCHER "${DUCK_VTK_CCACHE}" CACHE STRING "" FORCE)
    message(STATUS "duck_vtk: using ccache at ${DUCK_VTK_CCACHE}")
  endif()
endif()

message(STATUS "duck_vtk: compiler            = ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "duck_vtk: build type          = ${CMAKE_BUILD_TYPE}")
message(STATUS "duck_vtk: extra flags         = ${DUCK_VTK_EXTRA_FLAGS}")

#! Applies the resolved settings to one target.
function(duck_vtk_apply_optimisations TARGET)
  if(DUCK_VTK_EXTRA_FLAGS)
    target_compile_options(${TARGET} PRIVATE ${DUCK_VTK_EXTRA_FLAGS})
  endif()
  if(DUCK_VTK_EXTRA_LINK_FLAGS)
    target_link_options(${TARGET} PRIVATE ${DUCK_VTK_EXTRA_LINK_FLAGS})
  endif()
  if(DUCK_VTK_ENABLE_IPO)
    set_property(TARGET ${TARGET} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  endif()
endfunction()
