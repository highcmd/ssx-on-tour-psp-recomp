cmake_minimum_required(VERSION 3.20)
project(ssx_recomp CXX)
if(WIN32)
  enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(SSX_NATIVE_ARCH "Optimise the software renderer for this machine's CPU (-march=native)" ON)
# Windows builds are self-contained folders (ssx.exe next to image.bin, assets/ and the DLLs).
if(WIN32)
  set(SSX_PORTABLE_DEFAULT ON)
else()
  set(SSX_PORTABLE_DEFAULT OFF)
endif()
option(SSX_PORTABLE "Find image.bin and assets/ next to the executable" ${SSX_PORTABLE_DEFAULT})

find_package(SDL2 REQUIRED)
# FFmpeg: pkg-config, or (Windows) a prebuilt tree with include/ and lib/ given as SSX_FFMPEG_ROOT.
set(SSX_FFMPEG_ROOT "" CACHE PATH "FFmpeg dev tree (include/, lib/) when pkg-config is not used")
if(SSX_FFMPEG_ROOT)
  add_library(ffmpeg INTERFACE)
  target_include_directories(ffmpeg INTERFACE ${SSX_FFMPEG_ROOT}/include)
  foreach(lib avcodec avutil swscale)
    find_library(FFMPEG_${lib}_LIB NAMES ${lib} lib${lib}.dll.a PATHS ${SSX_FFMPEG_ROOT}/lib NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(ffmpeg INTERFACE ${FFMPEG_${lib}_LIB})
  endforeach()
  add_library(PkgConfig::FFMPEG ALIAS ffmpeg)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
endif()
file(GLOB SSX_ISO_CANDIDATES ${CMAKE_SOURCE_DIR}/iso/*.iso)
list(GET SSX_ISO_CANDIDATES 0 SSX_ISO_DEFAULT)
set(SSX_ISO "${SSX_ISO_DEFAULT}" CACHE FILEPATH "UMD image the game reads (non-portable builds)")

set(GEN_DIR ${CMAKE_SOURCE_DIR}/build/gen)
if(NOT EXISTS ${GEN_DIR}/sources.txt)
  message(FATAL_ERROR "Run tools/recomp.py first (build/gen is missing)")
endif()
file(STRINGS ${GEN_DIR}/sources.txt GEN_SOURCES_REL)
# Re-run this configuration when the recompiler writes a different set of files.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${GEN_DIR}/sources.txt)
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
  runtime/src/hle_intr.cpp
  runtime/src/hle_ge.cpp
  runtime/src/ge_render.cpp
  runtime/src/hle_misc.cpp
  runtime/src/hle_mpeg.cpp
  runtime/src/hle_audio.cpp
  runtime/src/hle_savedata.cpp
  runtime/src/settings.cpp
  runtime/src/game_hooks.cpp
  runtime/src/gl_funcs.cpp
  runtime/src/ge_gl.cpp
  runtime/src/platform.cpp)
if(WIN32)
  # UTF-8 code page (non-ASCII paths), DPI awareness, no console window.
  target_sources(ssx PRIVATE runtime/win/ssx.rc)
  set_target_properties(ssx PROPERTIES WIN32_EXECUTABLE ON)
  target_link_libraries(ssx PRIVATE winmm)
  target_link_options(ssx PRIVATE -static)
  # The mingw SDL2 package exports include/SDL2; the sources include <SDL2/SDL.h>.
  get_target_property(_sdl_inc SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
  foreach(_d ${_sdl_inc})
    get_filename_component(_p ${_d} DIRECTORY)
    target_include_directories(ssx PRIVATE ${_p})
  endforeach()   # no libc++/libunwind DLLs to ship
endif()
target_include_directories(ssx PRIVATE runtime/src runtime/vfpu/shim ${GEN_DIR})
target_compile_options(ssx PRIVATE -Wall -Wno-unused-function -fno-strict-aliasing -fwrapv)
if(SSX_NATIVE_ARCH)
  set_source_files_properties(runtime/src/ge_render.cpp PROPERTIES COMPILE_OPTIONS "-O3;-march=native")
else()
  set_source_files_properties(runtime/src/ge_render.cpp PROPERTIES COMPILE_OPTIONS "-O3")
endif()
target_compile_definitions(ssx PRIVATE
  SSX_DEFAULT_IMAGE="${GEN_DIR}/image.bin"
  SSX_DEFAULT_ISO="${SSX_ISO}"
  SSX_DEFAULT_ASSETS="${CMAKE_SOURCE_DIR}/runtime/assets")
if(SSX_PORTABLE)
  target_compile_definitions(ssx PRIVATE SSX_PORTABLE=1)
endif()
# HLE functions self-register through static constructors: keep every object.
target_link_libraries(ssx PRIVATE -Wl,--whole-archive game_code vfpu -Wl,--no-whole-archive SDL2::SDL2 PkgConfig::FFMPEG)
