cmake_minimum_required(VERSION 3.20)
project(okgf VERSION 0.1.0 LANGUAGES C)
set(OKGF_GAME_RELEASE "SRHD" CACHE STRING "Original game behavior: SRHD or SR1")
set_property(CACHE OKGF_GAME_RELEASE PROPERTY STRINGS SRHD SR1)
if(NOT OKGF_GAME_RELEASE MATCHES "^(SRHD|SR1)$")
  message(FATAL_ERROR "OKGF_GAME_RELEASE must be SRHD or SR1")
endif()
set(OKGF_MATH_BACKEND "NATIVE" CACHE STRING "Numerical implementation: COMPATIBLE, EXACT, or NATIVE")
set_property(CACHE OKGF_MATH_BACKEND PROPERTY STRINGS COMPATIBLE EXACT NATIVE)
if(NOT OKGF_MATH_BACKEND MATCHES "^(COMPATIBLE|EXACT|NATIVE)$")
  message(FATAL_ERROR "OKGF_MATH_BACKEND must be COMPATIBLE, EXACT, or NATIVE")
endif()
option(OKGF_PORTABLE_MATH "Force software trigonometry and portable integer primitives" OFF)
include(CheckCSourceCompiles)
check_c_source_compiles("
  #if (defined(__GNUC__) || defined(__clang__)) && (defined(__i386__) || defined(__x86_64__))
  int main(void) { __asm__ volatile(\"fnop\"); return 0; }
  #elif defined(_MSC_VER) && defined(_M_IX86)
  int main(void) { __asm { fnop } return 0; }
  #else
  #error Native x87 instructions are unavailable
  #endif
" OKGF_HAVE_X87)
if(OKGF_MATH_BACKEND STREQUAL "EXACT" AND (NOT OKGF_HAVE_X87 OR OKGF_PORTABLE_MATH))
  message(FATAL_ERROR "EXACT math requires native x87 FSIN/FCOS; use COMPATIBLE for the portable model or NATIVE for host arithmetic")
endif()
if(OKGF_MATH_BACKEND STREQUAL "NATIVE" AND OKGF_PORTABLE_MATH)
  message(FATAL_ERROR "OKGF_PORTABLE_MATH applies to COMPATIBLE, not NATIVE")
endif()
message(STATUS "OKGF math backend: ${OKGF_MATH_BACKEND}")
if(EMSCRIPTEN)
  set(OKGF_LIBRARY_TYPE STATIC)
else()
  set(OKGF_LIBRARY_TYPE SHARED)
endif()
add_library(okgf ${OKGF_LIBRARY_TYPE}
  src/math/precision.c
  src/cpu.c
  src/pixels.c
  src/copy.c
  src/lines.c
  src/alpha_buffers.c
  src/rle.c
  src/delta.c
  src/indexed.c
  src/lighting.c
  src/rotation.c
  src/rotation_build.c
  src/planet.c
  src/planet_build.c
  src/shapes.c
  src/gradients.c
  src/rescale.c
  src/bmp_read.c
  src/image_write.c
  src/png_read.c
  src/jpeg_read.c
  src/psd_read.c
  src/image_read.c
  src/platform.c
)
target_compile_definitions(okgf PUBLIC OKGF_GAME_RELEASE=OKGF_GAME_${OKGF_GAME_RELEASE})
if(WIN32)
  set_target_properties(okgf PROPERTIES PREFIX "")
  target_sources(okgf PRIVATE src/platform_win32.c)
  target_link_libraries(okgf PRIVATE gdi32 user32 ole32 oleaut32 version)
endif()

if(EMSCRIPTEN)
  target_compile_options(okgf PRIVATE -sUSE_LIBJPEG=1 -sUSE_LIBPNG=1 -sUSE_ZLIB=1)
  target_link_options(okgf INTERFACE -sUSE_LIBJPEG=1 -sUSE_LIBPNG=1 -sUSE_ZLIB=1)
else()
  find_package(JPEG REQUIRED)
  target_link_libraries(okgf PRIVATE JPEG::JPEG)
  find_package(PNG REQUIRED)
  target_link_libraries(okgf PRIVATE PNG::PNG)
endif()
if(OKGF_MATH_BACKEND STREQUAL "NATIVE")
  target_compile_definitions(okgf PRIVATE OKGF_NATIVE_MATH=1)
else()
  add_subdirectory(vendor/softfloat)
  target_sources(okgf PRIVATE src/math/trig_x87.c src/math/trig_emulated.c)
  target_link_libraries(okgf PRIVATE okgf_softfloat)
endif()
target_include_directories(okgf PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(okgf PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(okgf PUBLIC c_std_11)
if(MSVC)
  target_compile_options(okgf PRIVATE /W4 /fp:strict)
  set_target_properties(okgf PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
else()
  target_compile_options(okgf PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-fast-math -ffp-contract=off)
  target_link_libraries(okgf PRIVATE m)
endif()
