cmake_minimum_required(VERSION 3.20)

include(FetchContent)

# GLAD GL loader: use the vendored copy when present, otherwise fetch the
# generator and produce the same GL 3.3 core loader (glad1 keeps the
# glad/glad.h + gladLoadGLLoader API this code uses). Either way the result is
# a `glad` target nwiiruntime links against.
set(NWII_VENDORED_GLAD "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/glad")
if(EXISTS "${NWII_VENDORED_GLAD}/src/glad.c")
    add_library(glad STATIC "${NWII_VENDORED_GLAD}/src/glad.c")
    target_include_directories(glad PUBLIC "${NWII_VENDORED_GLAD}/include")
else()
    message(STATUS "vendored GLAD not found - fetching Dav1dde/glad v0.1.36")
    set(GLAD_PROFILE "core" CACHE STRING "" FORCE)
    set(GLAD_API "gl=3.3" CACHE STRING "" FORCE)
    set(GLAD_GENERATOR "c" CACHE STRING "" FORCE)
    FetchContent_Declare(glad
        GIT_REPOSITORY https://github.com/Dav1dde/glad.git
        GIT_TAG        v0.1.36
    )
    FetchContent_MakeAvailable(glad)
endif()

add_library(nwiiruntime STATIC
    src/core/loader.cpp
    src/core/config.cpp
    src/core/os_hle_stubs.cpp
    src/core/event_scheduler.cpp
    src/core/force_demo.cpp
    src/core/tprof.cpp
    src/core/s2.cpp
    src/input/input_manager.cpp
    src/input/wii_remotes.cpp
    src/input/sources/mouse_keyboard_source.cpp
    src/input/sources/gamepad_source.cpp
    src/input/sources/phone_source.cpp
    src/hle/memory.cpp
    src/hle/gx.cpp
    src/hle/pad.cpp
    src/hle/wpad_hle.cpp
    src/hle/dvd.cpp
    src/hle/gx/fifo_parser.cpp
    src/hle/gx/gx_matrix_seam.cpp
    src/hle/gx/gx_state_seam.cpp
    src/hle/gx/gx_state_seam.cpp
    src/hle/gx/renderer_gl.cpp
    src/hle/gx/renderer_factory.cpp
    src/hle/gx/tev_shader_gen.cpp
    src/hle/texture_cache_gl.cpp
    src/hle/ax.cpp
    src/hle/ax/ax_hle.cpp
    src/hle/ax/ax_hle_ref.cpp
    src/hle/ax/ax_async.cpp
    src/hle/ios.cpp
    src/hle/ios_kernel.cpp
    src/hle/virtual_disc.cpp
    src/hw/mmio_dispatcher.cpp
    src/platform/platform.cpp
    src/platform/wii_platform.cpp
    src/platform/gc_platform.cpp
    src/hw/hw_pi.cpp
    src/hw/hw_pe.cpp
    src/hw/hw_vi.cpp
    src/hw/hw_dsp.cpp
    src/hw/hw_ai.cpp
    src/hw/hw_di.cpp
    src/hw/hw_ipc.cpp
    src/hw/hw_all.cpp
    src/hw/hw_cp.cpp
    src/hle/devices/di_device.cpp
    src/hle/devices/fs_device.cpp
    src/hle/devices/stm_device.cpp
    src/hle/devices/usb_device.cpp
    src/hle/devices/es_device.cpp
    src/hle/interpreter.cpp
    src/hle/gx/dk/dk_glsl.cpp
    src/hle/gx/dk/dkgl_select.cpp
)

target_link_libraries(nwiiruntime PUBLIC SDL2-static tomlplusplus::tomlplusplus glad)

# Switch-only: native audio output and the debug sampling profiler (the rest of
# src/platform/switch is added by the Switch project).
if(NINTENDO_SWITCH)
    target_sources(nwiiruntime PRIVATE
        src/platform/switch/switch_audio.cpp
        src/platform/switch/switch_profiler.cpp
        src/platform/switch/switch_alloc.cpp
        src/hle/gx/dk/dkgl.cpp)
    # r15dk: backend deko3d (uamlib = uam dentro de la NRO; las bibliotecas se añaden al enlazar:
    # r9/r15dk/tools/variant_build_dk.sh)
    target_include_directories(nwiiruntime PRIVATE /home/djvemo/nsmbw-port/r9/r15dk/ext/uamlib)
endif()

if(APPLE)
    find_library(COCOA_LIBRARY Cocoa)
    find_library(METAL_LIBRARY Metal)
    find_library(QUARTZCORE_LIBRARY QuartzCore)
    find_library(IOKIT_LIBRARY IOKit)
    find_library(COREVIDEO_LIBRARY CoreVideo)
    target_link_libraries(nwiiruntime PUBLIC 
        ${COCOA_LIBRARY} 
        ${METAL_LIBRARY} 
        ${QUARTZCORE_LIBRARY}
        ${IOKIT_LIBRARY} 
        ${COREVIDEO_LIBRARY}
    )
    target_sources(nwiiruntime PRIVATE src/hle/gx/renderer_metal.mm)
endif()

target_include_directories(nwiiruntime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# add_executable(nwiiruntime_exe src/main.cpp)
# target_link_libraries(nwiiruntime_exe PRIVATE nwiiruntime)

# target_link_libraries(nwiiruntime_exe PRIVATE nwiirecomp_lib)
