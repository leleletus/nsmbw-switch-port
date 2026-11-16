# construir_nro: cadena de herramientas de Switch (copia de r9/r17pgo/cmake/nsmbw-switch.cmake, la de m5_pgo/m6q).
if(NOT DEFINED ENV{DEVKITPRO})
  set(ENV{DEVKITPRO} /opt/devkitpro)
endif()
include($ENV{DEVKITPRO}/cmake/Switch.cmake)

set(NWII_NX_OPT "-O2 -DNDEBUG -DNWII_FASTMEM=1 -ffp-contract=off -fomit-frame-pointer -fvisibility=hidden")
set(CMAKE_C_FLAGS_RELEASE "${NWII_NX_OPT}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "${NWII_NX_OPT}" CACHE STRING "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
add_compile_definitions(NWII_PLATFORM_SWITCH=1)
