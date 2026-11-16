# construir_nro: compilación de nWiiRuntime para Switch (copia de switch/runtime/CMakeLists.txt del proyecto).
# Da los objetivos que el runtime espera de su proyecto padre (SDL2-static, tomlplusplus) con las portlibs de
# devkitPro en lugar del SDL de FetchContent.
cmake_minimum_required(VERSION 3.20)
project(nwii_switch_runtime C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NWII_RUNTIME_DIR "" CACHE PATH "")
set(NWII_UAMLIB_INCLUDE "" CACHE PATH "carpeta con uam_api.h (backend deko3d)")

# toml++: -DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=<copia local> evita la descarga.
include(FetchContent)
FetchContent_Declare(tomlplusplus
  GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
  GIT_TAG v3.4.0)
FetchContent_MakeAvailable(tomlplusplus)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
add_library(SDL2-static INTERFACE)
target_link_libraries(SDL2-static INTERFACE PkgConfig::SDL2)

add_subdirectory(${NWII_RUNTIME_DIR} nwiiruntime)

# glad.c incluye <dlfcn.h> para su autocargador; libnx no lo tiene.
target_include_directories(glad PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../compat)
if(NWII_UAMLIB_INCLUDE)
  target_include_directories(nwiiruntime PRIVATE ${NWII_UAMLIB_INCLUDE})
endif()

# Fuentes solo de Switch (viven en el árbol del runtime; la compilación de PC no las usa).
set(_nx_platform ${NWII_RUNTIME_DIR}/src/platform/switch/switch_platform.cpp)
target_sources(nwiiruntime PRIVATE
    ${NWII_RUNTIME_DIR}/src/platform/switch/switch_input.cpp
    ${_nx_platform})

# El código nativo sale de un main.dol exacto (NSMBW EE. UU. rev 1); el NRO rechaza (con un mensaje) otras
# revisiones/regiones por su SHA-256.
set(NSMBW_GAME_DOL "" CACHE FILEPATH "main.dol del usuario")
if(EXISTS "${NSMBW_GAME_DOL}")
  file(SHA256 "${NSMBW_GAME_DOL}" _nx_dol_sha256)
  set_source_files_properties(${_nx_platform} TARGET_DIRECTORY nwiiruntime
      PROPERTIES COMPILE_DEFINITIONS "NWII_NX_EXPECTED_DOL_SHA256=\"${_nx_dol_sha256}\"")
  message(STATUS "main.dol SHA-256 check: ${_nx_dol_sha256}")
endif()
