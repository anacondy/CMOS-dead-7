# toolchain-mingw-x86_64.cmake — 64-bit Windows cross build from Linux/macOS.
#
#   cmake -B build/x64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-x86_64.cmake
#   cmake --build build/x64
#
# SPDX-License-Identifier: MIT
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32-    CACHE STRING "MinGW triple prefix")

set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}g++)
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}windres)
set(CMAKE_STRIP        ${TOOLCHAIN_PREFIX}strip)

set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
