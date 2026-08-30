cmake_minimum_required(VERSION 3.24)
project(main LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
find_package(ncnn REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libswscale libavutil)

add_subdirectory(vendor/realcugan-ncnn-vulkan/src)
add_subdirectory(vendor/rife-ncnn-vulkan/src)

# FetchContent
include(FetchContent)

add_executable(main
    src/main.cpp
)

target_include_directories(main PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/realcugan-ncnn-vulkan/src
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/rife-ncnn-vulkan/src
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/readerwriterqueue
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/concurrentqueue
)

target_link_libraries(main PRIVATE
    PkgConfig::FFMPEG
    ncnn
    realcugan
    rife
)
