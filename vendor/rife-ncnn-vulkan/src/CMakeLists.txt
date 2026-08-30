cmake_policy(SET CMP0091 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(rife)

cmake_minimum_required(VERSION 3.24)

option(USE_STATIC_MOLTENVK "link moltenvk static library" OFF)

find_package(Threads)
find_package(OpenMP)
find_package(Vulkan REQUIRED)

macro(rife_add_shader SHADER_SRC)
    get_filename_component(SHADER_SRC_NAME_WE ${SHADER_SRC} NAME_WE)
    set(SHADER_COMP_HEADER ${CMAKE_CURRENT_BINARY_DIR}/${SHADER_SRC_NAME_WE}.comp.hex.h)

    add_custom_command(
        OUTPUT ${SHADER_COMP_HEADER}
        COMMAND ${CMAKE_COMMAND} -DSHADER_SRC=${CMAKE_CURRENT_SOURCE_DIR}/${SHADER_SRC} -DSHADER_COMP_HEADER=${SHADER_COMP_HEADER} -P "${CMAKE_CURRENT_SOURCE_DIR}/generate_shader_comp_header.cmake"
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER_SRC}
        COMMENT "Preprocessing shader source ${SHADER_SRC_NAME_WE}.comp"
        VERBATIM
    )
    set_source_files_properties(${SHADER_COMP_HEADER} PROPERTIES GENERATED TRUE)

    list(APPEND SHADER_SPV_HEX_FILES ${SHADER_COMP_HEADER})
endmacro()

include_directories(${CMAKE_CURRENT_BINARY_DIR})

if(OPENMP_FOUND)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${OpenMP_EXE_LINKER_FLAGS}")
endif()

# enable global link time optimization
cmake_policy(SET CMP0069 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)

rife_add_shader(rife_preproc.comp)
rife_add_shader(rife_postproc.comp)
rife_add_shader(rife_preproc_tta.comp)
rife_add_shader(rife_postproc_tta.comp)
rife_add_shader(rife_flow_tta_avg.comp)
rife_add_shader(rife_v2_flow_tta_avg.comp)
rife_add_shader(rife_v4_flow_tta_avg.comp)
rife_add_shader(rife_flow_tta_temporal_avg.comp)
rife_add_shader(rife_v2_flow_tta_temporal_avg.comp)
rife_add_shader(rife_v4_flow_tta_temporal_avg.comp)
rife_add_shader(rife_out_tta_temporal_avg.comp)
rife_add_shader(rife_v4_timestep.comp)
rife_add_shader(rife_v4_timestep_tta.comp)
rife_add_shader(warp.comp)
rife_add_shader(warp_pack4.comp)
rife_add_shader(warp_pack8.comp)

add_custom_target(rife-spirv DEPENDS ${SHADER_SPV_HEX_FILES})

add_library(rife
    rife.cpp
    warp.cpp
)

add_dependencies(rife rife-spirv)

set(RIFE_LINK_LIBRARIES ncnn ${Vulkan_LIBRARY})

if(USE_STATIC_MOLTENVK)
    find_library(CoreFoundation NAMES CoreFoundation)
    find_library(Foundation NAMES Foundation)
    find_library(Metal NAMES Metal)
    find_library(QuartzCore NAMES QuartzCore)
    find_library(CoreGraphics NAMES CoreGraphics)
    find_library(Cocoa NAMES Cocoa)
    find_library(IOKit NAMES IOKit)
    find_library(IOSurface NAMES IOSurface)

    list(APPEND RIFE_LINK_LIBRARIES
        ${Metal}
        ${QuartzCore}
        ${CoreGraphics}
        ${Cocoa}
        ${IOKit}
        ${IOSurface}
        ${Foundation}
        ${CoreFoundation}
    )
endif()

target_link_libraries(rife ${RIFE_LINK_LIBRARIES})
