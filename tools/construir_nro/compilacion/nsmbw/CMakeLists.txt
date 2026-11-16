# construir_nro: el NRO de Switch = código generado (export/) + runtime (nWiiRuntime/).
# Copia de switch/nsmbw/CMakeLists.txt del proyecto (la que compiló m5_pgo/m6q), con las rutas como variables.
# Las opciones de PGO/flags van en ../cmake/pgo.cmake (CMAKE_PROJECT_nsmbw_switch_INCLUDE).
cmake_minimum_required(VERSION 3.20)
project(nsmbw_switch C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NSMBW_EXPORT_DIR "" CACHE PATH "carpeta export/ (código generado)")
set(NWII_RUNTIME_DIR "" CACHE PATH "carpeta nWiiRuntime/")
set(NSMBW_NACP_NAME "NSMBW Recomp" CACHE STRING "")
set(NSMBW_NACP_AUTHOR "nsmbw-port" CACHE STRING "")
set(NSMBW_NACP_VERSION "0.2" CACHE STRING "")

# nwiiruntime + glad + SDL2-static/tomlplusplus para libnx.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../runtime runtime)

file(GLOB NSMBW_GENERATED CONFIGURE_DEPENDS
     ${NSMBW_EXPORT_DIR}/output_*.cpp
     ${NSMBW_EXPORT_DIR}/main_output.cpp
     ${NSMBW_EXPORT_DIR}/fastdispatch.cpp)

add_executable(nsmbw ${NWII_RUNTIME_DIR}/src/core/main.cpp ${NSMBW_GENERATED})
target_include_directories(nsmbw PRIVATE
    ${NSMBW_EXPORT_DIR}
    ${NWII_RUNTIME_DIR}/src
    ${NWII_RUNTIME_DIR}/include)
target_link_libraries(nsmbw PRIVATE nwiiruntime)
# El código generado usa _setjmp/_longjmp de glibc; en newlib son equivalentes.
target_compile_definitions(nsmbw PRIVATE _setjmp=setjmp _longjmp=longjmp)

nx_generate_nacp(nsmbw.nacp NAME "${NSMBW_NACP_NAME}" AUTHOR "${NSMBW_NACP_AUTHOR}" VERSION "${NSMBW_NACP_VERSION}")
nx_create_nro(nsmbw NACP nsmbw.nacp)
