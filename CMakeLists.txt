cmake_minimum_required(VERSION 3.20)
project(ssx_recomp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(SDL2 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
file(GLOB SSX_ISO_CANDIDATES ${CMAKE_SOURCE_DIR}/iso/*.iso)
list(GET SSX_ISO_CANDIDATES 0 SSX_ISO_DEFAULT)
set(SSX_ISO "${SSX_ISO_DEFAULT}" CACHE FILEPATH "Raw UMD image (for umd0: block reads)")

set(GEN_DIR ${CMAKE_SOURCE_DIR}/build/gen)
if(NOT EXISTS ${GEN_DIR}/sources.txt)
  message(FATAL_ERROR "Run tools/recomp.py first (build/gen is missing)")
endif()
file(STRINGS ${GEN_DIR}/sources.txt GEN_SOURCES_REL)
list(TRANSFORM GEN_SOURCES_REL PREPEND ${GEN_DIR}/ OUTPUT_VARIABLE GEN_SOURCES)

# Recompiled game code: huge, goto-heavy functions. -O1 keeps build times sane;
# no strict aliasing / wrapping arithmetic to match the guest's semantics.
add_library(game_code STATIC ${GEN_SOURCES})
target_include_directories(game_code PUBLIC runtime/src runtime/vfpu/shim ${GEN_DIR})
target_compile_options(game_code PRIVATE -O1 -fno-strict-aliasing -fwrapv -w -fno-exceptions)

add_library(vfpu STATIC
  runtime/vfpu/MIPSIntVFPU.cpp
  runtime/vfpu/MIPSVFPUUtils.cpp
  runtime/vfpu/MIPSVFPUFallbacks.cpp
  runtime/vfpu/vfpu_glue.cpp)
target_include_directories(vfpu PUBLIC runtime/vfpu/shim runtime/src)
target_compile_options(vfpu PRIVATE -O2 -w)

add_executable(ssx
  runtime/src/main.cpp
  runtime/src/core.cpp
  runtime/src/kernel_thread.cpp
  runtime/src/kernel_misc.cpp
  runtime/src/hle_io.cpp
  runtime/src/hle_display.cpp
  runtime/src/hle_ge.cpp
  runtime/src/ge_render.cpp
  runtime/src/hle_misc.cpp
  runtime/src/hle_mpeg.cpp)
target_include_directories(ssx PRIVATE runtime/src runtime/vfpu/shim ${GEN_DIR})
target_compile_options(ssx PRIVATE -Wall -Wno-unused-function -fno-strict-aliasing -fwrapv)
target_compile_definitions(ssx PRIVATE
  SSX_DEFAULT_IMAGE="${GEN_DIR}/image.bin"
  SSX_DEFAULT_UMD="${CMAKE_SOURCE_DIR}/extracted"
  SSX_DEFAULT_ISO="${SSX_ISO}"
  SSX_DEFAULT_ASSETS="${CMAKE_SOURCE_DIR}/runtime/assets")
# HLE functions self-register through static constructors: keep every object.
target_link_libraries(ssx PRIVATE -Wl,--whole-archive game_code vfpu -Wl,--no-whole-archive SDL2::SDL2 PkgConfig::FFMPEG)
