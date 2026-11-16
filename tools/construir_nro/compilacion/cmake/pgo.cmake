# construir_nro: copia de r9/r17pgo/cmake/pgo.cmake (opciones del código generado: PGO, flags, bibliotecas extra).
# r17pgo: PGO / flag experiments for the Switch build, without touching the shared
# switch/nsmbw/CMakeLists.txt. build_pgo.sh passes this file as
# -DCMAKE_PROJECT_nsmbw_switch_INCLUDE=... so it runs right after project(nsmbw_switch);
# the targets do not exist yet, so the real work is deferred to the end of the top directory.
#
# Cache variables (set by build_pgo.sh):
#   NWII_PGO          off | gen | use
#   NWII_PGO_DIR      use: folder with the .gcda files (names = object path relative to the
#                     build dir, '/' -> '#', as -fprofile-prefix-path makes them)
#   NWII_GEN_FLAGS    extra flags for the generated code (target nsmbw: output_*.cpp,
#                     main_output.cpp, fastdispatch.cpp and the runtime's main.cpp)
#   NWII_RT_FLAGS     extra flags for the runtime library (target nwiiruntime)
#   NWII_LINK_FLAGS   extra link flags (e.g. -Wl,--section-ordering-file=...)
#   NWII_PGO_SRC      gen: pgo/gcov_nx.cpp (dump thread + TLS-safe indirect-call profiler)
set(NWII_PGO "off" CACHE STRING "off|gen|use")
set(NWII_PGO_DIR "" CACHE PATH "")
set(NWII_GEN_FLAGS "" CACHE STRING "")
set(NWII_RT_FLAGS "" CACHE STRING "")
set(NWII_LINK_FLAGS "" CACHE STRING "")
set(NWII_PGO_SRC "" CACHE FILEPATH "")
# Extra flags only for some generated sources (e.g. -O3 only where it helps: output_24.cpp has the AX mixer).
set(NWII_SRC_RE "" CACHE STRING "regex of generated sources that also get NWII_SRC_FLAGS")
set(NWII_SRC_FLAGS "" CACHE STRING "")
# use: also optimize the runtime library (+ main.cpp) with the profile? Default OFF (9-nov): the runtime's
# render worker / handoff / write-gather paths depend on timing, and the instrumented NRO runs the game at
# 5-30 fps, so those paths are trained in the wrong state (idle worker, empty queues): on m1 the render
# worker came out 36 % slower per GX write (map 60 -> 45 fps). The generated code (the game itself) is
# deterministic per frame and is what PGO is for.
set(NWII_PGO_RUNTIME OFF CACHE BOOL "")
set(NWII_EXTRA_LIBS "" CACHE STRING "static libs linked after the runtime (m1: uamlib + deko3d)")
# Sources of the generated code that get NO profile flags (neither instrumented nor optimized
# with the profile; compiled exactly as the normal build). main_output.cpp = run_game, one
# function with a 168 000-case switch and a setjmp: 3.5 GB of RAM to compile normally and well
# over 5 GB instrumented (8-nov: that took the PC down). Its hot part is only the dispatch loop.
set(NWII_PGO_SKIP "main_output\\.cpp$" CACHE STRING "regex")

function(nwii_pgo_apply)
  separate_arguments(_gen UNIX_COMMAND "${NWII_GEN_FLAGS}")
  separate_arguments(_rt UNIX_COMMAND "${NWII_RT_FLAGS}")
  separate_arguments(_ln UNIX_COMMAND "${NWII_LINK_FLAGS}")
  # The .gcda name is <object path relative to this build dir> with '/' -> '#', so the
  # instrumented build and the optimized build (different build dirs) agree on it.
  set(_pp "-fprofile-prefix-path=${CMAKE_BINARY_DIR}")
  if(NWII_PGO STREQUAL "gen")
    # Generated code only runs on the game (cpu) thread: plain counters (fast).
    # The runtime runs on main/render, the render worker, AX and audio threads: atomic
    # counters (LL/SC on the A57) so concurrent updates are not lost or torn.
    list(PREPEND _gen -fprofile-generate=/pgo ${_pp} -fprofile-update=single)
    list(PREPEND _rt -fprofile-generate=/pgo ${_pp} -fprofile-update=atomic)
    list(APPEND _ln -fprofile-generate=/pgo)
    add_library(nwii_pgo_rt OBJECT ${NWII_PGO_SRC})
    target_compile_options(nwii_pgo_rt PRIVATE -O2 -fno-profile-generate)
    target_include_directories(nwii_pgo_rt PRIVATE $ENV{DEVKITPRO}/libnx/include)
    target_sources(nsmbw PRIVATE $<TARGET_OBJECTS:nwii_pgo_rt>)
  elseif(NWII_PGO STREQUAL "use")
    set(_use -fprofile-use=${NWII_PGO_DIR} ${_pp} -fprofile-partial-training -fprofile-correction
        -Wno-missing-profile)
    list(PREPEND _gen ${_use})
    if(NWII_PGO_RUNTIME)
      list(PREPEND _rt ${_use})
    else()
      set(NWII_PGO_SKIP "${NWII_PGO_SKIP}|src/core/main\\.cpp$")
    endif()
  endif()
  if(_gen)
    # Per source (not per target) so NWII_PGO_SKIP can leave some out.
    get_target_property(_srcs nsmbw SOURCES)
    set(_n 0)
    foreach(_s IN LISTS _srcs)
      if(_s MATCHES "\\.cpp$" AND NOT _s MATCHES "${NWII_PGO_SKIP}")
        set_property(SOURCE ${_s} APPEND PROPERTY COMPILE_OPTIONS ${_gen})
        math(EXPR _n "${_n} + 1")
      endif()
    endforeach()
    message(STATUS "r17pgo: generated-code flags on ${_n} sources (skip: ${NWII_PGO_SKIP})")
  endif()
  if(NWII_SRC_RE AND NWII_SRC_FLAGS)
    separate_arguments(_sf UNIX_COMMAND "${NWII_SRC_FLAGS}")
    get_target_property(_srcs nsmbw SOURCES)
    foreach(_s IN LISTS _srcs)
      if(_s MATCHES "${NWII_SRC_RE}")
        set_property(SOURCE ${_s} APPEND PROPERTY COMPILE_OPTIONS ${_sf})
        message(STATUS "r17pgo: + [${_sf}] on ${_s}")
      endif()
    endforeach()
  endif()
  if(_rt)
    target_compile_options(nwiiruntime PRIVATE ${_rt})
  endif()
  if(_ln)
    target_link_options(nsmbw PRIVATE ${_ln})
  endif()
  if(NWII_EXTRA_LIBS)
    separate_arguments(_xl UNIX_COMMAND "${NWII_EXTRA_LIBS}")
    target_link_libraries(nsmbw PRIVATE ${_xl})
  endif()
  message(STATUS "r17pgo: NWII_PGO=${NWII_PGO} gen=[${_gen}] rt=[${_rt}] link=[${_ln}]")
endfunction()
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL nwii_pgo_apply)
