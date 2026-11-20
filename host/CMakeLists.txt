# PC build of the Skate 3 boot test with the Switch 2 memory model (one flat arena,
# folded aliases, no page protection). Lets the runtime, VFS, XEX load and the first
# guest code be debugged on the PC.
cmake_minimum_required(VERSION 3.25)
project(skate3_host LANGUAGES C CXX ASM)

get_filename_component(NS2 ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
set(SKATE3_ROOT ${NS2}/third_party/skate3recomp CACHE PATH "skate3recomp checkout")
set(REXSDK_DIR ${NS2}/third_party/rexglue-sdk CACHE PATH "rexglue SDK checkout")
set(REXGLUE_USE_VULKAN ON)
set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "" FORCE)
set(REXGLUE_ENABLE_PERF_COUNTERS ON CACHE BOOL "" FORCE)
option(HOST_ARENA "Use the Switch 2 flat-arena memory model" ON)
if(HOST_ARENA)
    set(REX_SIM_ARENA ON)
    set(REX_SIM_MEMORY_FILE ${NS2}/platform/memory_switch2.cpp)
    add_compile_definitions(REX_SIM_ARENA=1)
endif()

include(${SKATE3_ROOT}/cmake/RexGlue.cmake)

include(${SKATE3_ROOT}/generated/sources.cmake)
set(GEN_MAIN ${GENERATED_SOURCES})
include(${SKATE3_ROOT}/generated/eawebkit/sources.cmake)
set(GEN_WEB ${GENERATED_SOURCES})

add_executable(skate3_host main.cpp ${NS2}/src/skate3_vfs.cpp ${GEN_MAIN} ${GEN_WEB})
target_include_directories(skate3_host PRIVATE ${SKATE3_ROOT} ${SKATE3_ROOT}/generated
    ${SKATE3_ROOT}/generated/eawebkit ${REXSDK_DIR}/thirdparty/xxHash)
target_link_libraries(skate3_host PRIVATE rex::runtime)
rexglue_apply_target_settings(skate3_host)
set_target_properties(skate3_host PROPERTIES INSTALL_RPATH "$ORIGIN" BUILD_WITH_INSTALL_RPATH ON)
target_compile_features(skate3_host PRIVATE cxx_std_23)
