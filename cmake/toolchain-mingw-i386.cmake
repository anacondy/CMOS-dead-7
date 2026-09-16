# toolchain-mingw-i386.cmake — 32-bit Windows cross build from Linux/macOS.
#
#   cmake -B build/x86 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-i386.cmake
#   cmake --build build/x86
#
# The x64 twin is toolchain-mingw-x86_64.cmake. On Windows with MSVC you do not
# need either: `cmake -B build -A Win32` is the equivalent.
#
# SPDX-License-Identifier: MIT
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

set(TOOLCHAIN_PREFIX i686-w64-mingw32-    CACHE STRING "MinGW triple prefix")

set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}g++)
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}windres)
set(CMAKE_STRIP        ${TOOLCHAIN_PREFIX}strip)

# Static CRT: the artifact must run on a clean Win7 box with no redistributable.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc")
# Find programs (gcc, windres) on the host, libraries in the target sysroot.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
