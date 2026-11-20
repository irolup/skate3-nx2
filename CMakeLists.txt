cmake_minimum_required(VERSION 3.25)
# Nintendo Switch 2 build of the Skate 3 static recompilation (skate3recomp + rexglue SDK).
# Normally built through ./build.sh (ounce-devkit Docker image). By hand:
#   source $DEVKITPRO/switch2vars.sh
#   aarch64-none-elf-cmake -S . -B build -G Ninja && cmake --build build
project(skate3_ns2 LANGUAGES C CXX ASM)

# skate3recomp (unmodified; holds the generated/ recompiled code) and the rexglue SDK with
# Switch 2 support. Both are git submodules in third_party/.
set(SKATE3_ROOT ${CMAKE_CURRENT_LIST_DIR}/third_party/skate3recomp CACHE PATH "skate3recomp checkout")
set(REXGLUE_SDK_ROOT ${CMAKE_CURRENT_LIST_DIR}/third_party/rexglue-sdk CACHE PATH
    "rexglue SDK checkout with Switch 2 support")
set(SDK ${REXGLUE_SDK_ROOT})
if(NOT EXISTS ${SKATE3_ROOT}/generated/sources.cmake)
    message(FATAL_ERROR "No recompiled code in ${SKATE3_ROOT}/generated: run the code generation "
                        "first (see README.md).")
endif()
set(TP  ${SDK}/thirdparty)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_compile_options(
    -isystem ${CMAKE_CURRENT_LIST_DIR}/compat/include
    -fno-strict-aliasing -ffp-contract=off -fno-char8_t
    -ffunction-sections -fdata-sections
    -include ${CMAKE_CURRENT_LIST_DIR}/compat/switch2_compat.h
    -w)
add_compile_definitions(
    REXGLUE_BUILD_CONFIG="Release" SPDLOG_FMT_EXTERNAL=1 FMT_HEADER_ONLY=1
    SPDLOG_NO_TZ_OFFSET=1 SPDLOG_NO_TLS=0 REXGLUE_ENABLE_PERF_COUNTERS)

set(REX_INCLUDES
    ${SDK}/include
    ${CMAKE_CURRENT_LIST_DIR}/gen/include
    ${SDK}
    ${TP}/simde ${TP}/simde/simde
    ${TP}/fmt/include ${TP}/spdlog/include
    ${TP}/tomlplusplus/include ${TP}/disruptorplus/include
    ${TP}/utfcpp/source ${TP}/cli11/include ${TP})

# --- rexcore -------------------------------------------------------------
set(CORE_SRC
    arena bit_map bit_stream byte_stream clock cvar exception_handler filesystem
    filesystem_wildcard guest_presence logging memory mutex ring_buffer sha256
    string string_buffer threading timer_queue utf8 vec128 perf/counter
    atomic_posix dbg_posix
    math_gcc string_posix system_posix
    )
list(TRANSFORM CORE_SRC REPLACE "dynlib_posix" "")
list(TRANSFORM CORE_SRC PREPEND ${SDK}/src/core/)
list(TRANSFORM CORE_SRC APPEND .cpp)
add_library(rexcore STATIC ${CORE_SRC})
target_include_directories(rexcore PUBLIC ${REX_INCLUDES})
target_sources(rexcore PRIVATE platform/dynlib_switch2.cpp)
target_sources(rexcore PRIVATE
    platform/clock_switch2.cpp platform/threading_switch2.cpp platform/memory_switch2.cpp platform/fiber_switch2.cpp
    platform/seh_switch2.cpp platform/exception_handler_switch2.cpp
    platform/mapped_memory_switch2.cpp platform/socket_switch2.cpp platform/filesystem_switch2.cpp)

# --- the rest of the runtime ---------------------------------------------
function(rex_glob out dir)
    file(GLOB_RECURSE _f ${SDK}/src/${dir}/*.cpp)
    foreach(x ${ARGN})
        list(FILTER _f EXCLUDE REGEX "${x}")
    endforeach()
    set(${out} ${_f} PARENT_SCOPE)
endfunction()

# glslang (runtime GLSL->SPIR-V for the emulated GPU) and its SPIR-V builder
set(GLS ${TP}/glslang)
file(GLOB GLS_MI ${GLS}/glslang/MachineIndependent/*.cpp ${GLS}/glslang/MachineIndependent/preprocessor/*.cpp)
file(GLOB GLS_GCG ${GLS}/glslang/GenericCodeGen/*.cpp)
file(GLOB GLS_SPV ${GLS}/SPIRV/*.cpp)
set(GLS_SRC ${GLS_MI} ${GLS_GCG} ${GLS_SPV} ${GLS}/OGLCompilersDLL/InitializeDll.cpp ${GLS}/glslang/OSDependent/Unix/ossource.cpp)
add_library(glslang_ns2 STATIC ${GLS_SRC})
target_include_directories(glslang_ns2 PUBLIC ${GLS} ${CMAKE_CURRENT_LIST_DIR}/gen/include)
target_compile_definitions(glslang_ns2 PRIVATE ENABLE_OPT=0)

rex_glob(FS_SRC filesystem "vfs_dump")
rex_glob(SYS_SRC system)
rex_glob(KERN_SRC kernel "xboxkrnl_usbcam" "cert_monitor" "debug_monitor")
rex_glob(AUD_SRC audio "audio/sdl/")
list(APPEND AUD_SRC ${CMAKE_CURRENT_LIST_DIR}/platform/audio_switch2.cpp)
rex_glob(IN_SRC input "input/sdl/" "input/xinput/" "input_system.cpp" "mnk/")
list(APPEND IN_SRC ${CMAKE_CURRENT_LIST_DIR}/platform/input_system_switch2.cpp)
rex_glob(GFX_SRC graphics "graphics/d3d12/" "dxbc" "trace_dump" "trace_reader" "trace_viewer" "trace_player")
rex_glob(UI_SRC ui "ui/d3d12/" "window_gtk" "windowed_app_context_gtk" "surface_gnulinux" "window_sdl" "windowed_app_context_sdl" "surface_sdl" "ui/shaders/" "_win\\.cpp" "windowed_app_main")
list(APPEND UI_SRC ${CMAKE_CURRENT_LIST_DIR}/platform/ui_switch2.cpp)

add_library(rexruntime STATIC ${FS_SRC} ${SYS_SRC} ${KERN_SRC} ${AUD_SRC} ${IN_SRC} ${GFX_SRC} ${UI_SRC}
    ${TP}/o1heap/o1heap/o1heap.c ${TP}/aes_128/aes.c
    ${TP}/libmspack/libmspack/mspack/lzxd.c
    ${TP}/imgui/imgui.cpp ${TP}/imgui/imgui_draw.cpp ${TP}/imgui/imgui_tables.cpp
    ${TP}/imgui/imgui_widgets.cpp ${TP}/imgui/misc/freetype/imgui_freetype.cpp)
target_include_directories(rexruntime PUBLIC ${REX_INCLUDES}
    ${SDK}/src ${TP}/o1heap/o1heap ${TP}/aes_128 ${TP}/libmspack/libmspack/mspack ${TP}/imgui ${TP}/xxHash ${TP}/snappy
    ${TP}/volk ${TP}/vulkan-headers/include ${TP}/vulkan-memory-allocator/include
    ${TP}/spirv-headers/include ${TP}/glslang ${TP}/renderdoc ${TP}/spirv-tools/include)
target_compile_definitions(rexruntime PUBLIC VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL=0 VK_USE_PLATFORM_VI_NN=1 REXGLUE_USE_VULKAN=1 REX_HAS_VULKAN=1)
target_link_libraries(rexruntime PUBLIC rexcore glslang_ns2)
target_compile_definitions(rexruntime PUBLIC IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
target_compile_definitions(rexruntime PUBLIC IMGUI_ENABLE_FREETYPE)
target_include_directories(rexruntime PUBLIC /opt/devkitpro/portlibs/switch/include/freetype2)

# --- FFmpeg subset (XMA via WMA Pro) --------------------------------------
include(${CMAKE_CURRENT_LIST_DIR}/gen/ffmpeg_sources.cmake)
add_library(ffavutil STATIC ${FFMPEG_UTIL_SRC})
add_library(ffavcodec STATIC ${FFMPEG_CODEC_SRC})
foreach(t ffavutil ffavcodec)
    target_include_directories(${t} BEFORE PRIVATE ${CMAKE_CURRENT_LIST_DIR}/ffmpeg-cfg)
    target_include_directories(${t} PUBLIC ${TP}/FFmpeg PRIVATE ${TP}/FFmpeg/compat/atomics/gcc)
    target_compile_definitions(${t} PRIVATE HAVE_AV_CONFIG_H _USE_MATH_DEFINES)
endforeach()
target_link_libraries(ffavcodec PUBLIC ffavutil)
target_link_libraries(rexruntime PUBLIC ffavcodec ffavutil)
target_include_directories(rexruntime PUBLIC ${TP}/FFmpeg)

# --- the game -------------------------------------------------------------
include(${SKATE3_ROOT}/generated/sources.cmake)
set(GEN_MAIN ${GENERATED_SOURCES})
include(${SKATE3_ROOT}/generated/eawebkit/sources.cmake)
set(GEN_WEB ${GENERATED_SOURCES})
option(NS2_WITH_GENERATED "Link the 287 MB of recompiled code" ON)
# The app: the SDK's ReXApp + emulated Xenos GPU on Vulkan.
set(APP_SRC src/ns2_app.cpp src/skate3_vfs.cpp ${CMAKE_CURRENT_LIST_DIR}/platform/windowed_app_main_switch2.cpp)
if(NS2_WITH_GENERATED)
    list(APPEND APP_SRC ${GEN_MAIN} ${GEN_WEB})
endif()
add_executable(skate3 ${APP_SRC})
target_include_directories(skate3 PRIVATE ${SKATE3_ROOT} ${SKATE3_ROOT}/generated ${SKATE3_ROOT}/generated/eawebkit ${CMAKE_CURRENT_LIST_DIR}/gen/include ${TP}/xxHash)
target_compile_definitions(skate3 PRIVATE SKATE3_BUILD_CONFIG="Release" SKATE3_HAS_TITLE_UPDATE=1)
target_sources(skate3 PRIVATE compat/libc_compat.c compat/crash_hooks.cpp)
target_link_options(skate3 PRIVATE -Wl,--wrap=abort -Wl,--wrap=__cxa_throw -Wl,--wrap=pthread_detach -Wl,--wrap=pthread_mutex_lock)
target_link_libraries(skate3 PRIVATE rexruntime ffavcodec ffavutil freetype harfbuzz png16 bz2 z vulkan nx)
set_target_properties(skate3 PROPERTIES CXX_STANDARD 23)
# NRO metadata shown in the homebrew menu. Icon: baseline (not progressive) JPEG, 256x256 pixels,
# small (< ~100 KB), at icon.jpg next to this file; without it the toolchain's default icon is used.
set(NRO_TITLE "Skate 3")
set(NRO_AUTHOR "irolup")
set(NRO_VERSION "1.0.0")
nx_generate_nacp(skate3.nacp NAME "${NRO_TITLE}" AUTHOR "${NRO_AUTHOR}" VERSION "${NRO_VERSION}")
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/icon.jpg)
  nx_create_nro(skate3 NACP skate3.nacp ICON ${CMAKE_CURRENT_LIST_DIR}/icon.jpg)
else()
  nx_create_nro(skate3 NACP skate3.nacp)
endif()
