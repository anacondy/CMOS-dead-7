# CMakeLists.txt — TimeKeeper, for MSVC (cl + rc) and MinGW-w64 (gcc + windres).
#
#   # on Windows with Visual Studio:
#   cmake -B build/x86 -A Win32 && cmake --build build/x86 --config Release
#   cmake -B build/x64 -A Win64 && cmake --build build/x64 --config Release
#
#   # cross from Linux/macOS (toolchain files live in cmake/):
#   cmake -B build/x86 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-i386.cmake
#   cmake --build build/x86 && ctest --test-dir build/x86 -V    # incl. Wine /test
#
#   # unit tests only, natively, no cross tools:
#   cmake -B build/tests -DTK_ONLY_TESTS=ON && cmake --build build/tests
#   ctest --test-dir build/tests -V
#
# Kept deliberately flat: one early `return()` for the tests-only mode instead of
# nesting every block in an `if()`. Someone auditing "does the build really pass
# /O1 /Os /GL /MT and /SUBSYSTEM:WINDOWS" should be able to read this top to
# bottom without tracking three levels of conditionals.
#
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.15)
project(TimeKeeper C)

# ---------------------------------------------------------------- options ----
option(TK_NO_CRT           "Skip the C runtime start-up objects (MinGW only; needed for the size target)" ON)
option(TK_COMPACT_SELFTEST "Build the self-test without verbose prose (~38 KB)" ON)
option(TK_LTCG             "Link-time code generation (small win, upsets some RC/link combos)" OFF)
option(TK_STRIP            "Strip the release binary (matches the Makefile recipe)" ON)
option(TK_TESTS            "Register tests with ctest" ON)
option(TK_ONLY_TESTS       "Native configure that builds nothing but the unit tests" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ------------------------------------------------------------- definitions ----
# 0x0601 = Windows 7 SP1 / Server 2008 R2. Setting it makes reaching for a
# Windows 8+ API a compile error here, instead of an "entry point not found"
# dialog on the machine you are trying to rescue.
add_compile_definitions(
  _WIN32_WINNT=0x0601 WINVER=0x0601 NTDDI_VERSION=0x06010000
  WIN32_LEAN_AND_MEAN NOMINMAX STRICT UNICODE _UNICODE)
if(TK_COMPACT_SELFTEST)
  # See src/selftest.c: identical vectors and assertions, no prose. The product
  # reports FAIL@<line> (and logs it), the host build prints the full message.
  add_compile_definitions(TK_SELFTEST_COMPACT)
endif()
if(MSVC)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# ------------------------------------------------------------- source sets ----
set(TK_CORE_SRC
  src/timeutil.c src/verdict.c src/selftest.c src/sntp_proto.c
  src/fallback_proto.c src/log_rotate.c src/tklibc.c)
set(TK_WIN_SRC
  src/crt_start.c src/main.c src/sntp.c src/http_date.c src/fallback.c
  src/privilege.c src/log.c src/winutil.c src/install.c)
set(TK_HEADERS
  src/config.h src/timeutil.h src/verdict.h src/selftest.h src/sntp.h src/sntp_proto.h
  src/http_date.h src/fallback.h src/fallback_proto.h src/privilege.h src/log.h
  src/winutil.h src/install.h src/tklibc.h tests/vectors.h)
set(TK_TEST_SRC tests/host_test.c ${TK_CORE_SRC})

# The entry point is part of the program when the CRT's is not: without
# src/crt_start.c and -nostartfiles, the link still succeeds and produces a small,
# inert stub. cmake/check_artifact.cmake exists because a size gate alone cannot
# notice that (see the comment there).
if(TK_NO_CRT AND NOT MSVC)
  list(FIND TK_WIN_SRC "src/crt_start.c" _tk_entry)
  if(_tk_entry LESS 0)
    message(FATAL_ERROR "TK_NO_CRT is ON but src/crt_start.c is not in TK_WIN_SRC.")
  endif()
endif()

# ------------------------------------------------ tests-only native branch ----
if(TK_ONLY_TESTS)
  enable_testing()
  add_executable(tk_unit_tests ${TK_TEST_SRC})
  target_include_directories(tk_unit_tests PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(tk_unit_tests PRIVATE
    -std=c11 -O1 -g -Wall -Wextra -Wshadow -fno-tree-loop-distribute-patterns)
  if(NOT MSVC)
    target_compile_options(tk_unit_tests PRIVATE
      -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(tk_unit_tests PRIVATE -fsanitize=address,undefined)
  endif()
  add_test(NAME unit_tests COMMAND tk_unit_tests)
  return()
endif()

# ---------------------------------------------------------- Windows target ----
if(NOT WIN32)
  message(FATAL_ERROR
    "TimeKeeper builds for Windows. Use one of:\n"
    "  cmake -B build/x86 -A Win32                                                (MSVC)\n"
    "  cmake -B build/x86 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-i386.cmake (cross)\n"
    "  make -f Makefile.mingw                                                     (cross, no cmake)\n"
    "For the unit tests alone on this host, add -DTK_ONLY_TESTS=ON.")
endif()

add_executable(TimeKeeper WIN32 ${TK_WIN_SRC} ${TK_CORE_SRC} ${TK_HEADERS})
target_include_directories(TimeKeeper PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR})

# version.rc references app.manifest by relative path. windres resolves it against
# the current directory, rc.exe against the .rc's directory, so the manifest is
# staged into the binary directory too and both find it.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/app.manifest
               ${CMAKE_CURRENT_BINARY_DIR}/app.manifest COPYONLY)
target_sources(TimeKeeper PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/version.rc)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/version.rc
  PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/app.manifest)

# ------------------------------------------------------------------- flags ----
if(MSVC)
  # /O1 /Os /GL /MT /SUBSYSTEM:WINDOWS as specified. MSVC has no safe equivalent
  # of -nostartfiles, so an MSVC build carries the static CRT and lands nearer
  # 120 KB; use MinGW if the 80 KB target matters (README section 4).
  target_compile_options(TimeKeeper PRIVATE
    /O1 /Os /Gy /GR- /EHs-c- /W4 /utf-8 /wd4996 /wd4668)
  if(TK_LTCG)
    target_compile_options(TimeKeeper PRIVATE /GL)
  endif()
  if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_compile_options(TimeKeeper PRIVATE /arch:SSE2)
  endif()
  set_target_properties(TimeKeeper PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_link_options(TimeKeeper PRIVATE
    /SUBSYSTEM:WINDOWS,6.01 /OPT:REF /OPT:ICF /INCREMENTAL:NO /MANIFEST:NO
    /STACK:0x10000,0x2000)
  if(TK_LTCG)
    target_link_options(TimeKeeper PRIVATE /LTCG)
  endif()
  # /MANIFEST:NO: we embed our own through version.rc. Letting mt.exe merge a
  # second one is the classic cause of "this app can't run".
else()
  target_compile_options(TimeKeeper PRIVATE
    -Os -fno-ident -fno-unwind-tables -fno-asynchronous-unwind-tables
    -ffunction-sections -fdata-sections -fno-stack-protector
    -fno-tree-loop-distribute-patterns
    -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-align -Wwrite-strings
    -Wconversion -Wno-sign-conversion -Wstrict-prototypes -Wmissing-prototypes
    -Wold-style-definition -Wundef -Wunreachable-code -Winit-self)
  if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_compile_options(TimeKeeper PRIVATE -march=i686)
  endif()
  if(TK_LTCG)
    target_compile_options(TimeKeeper PRIVATE -flto)
    target_link_options(TimeKeeper PRIVATE -flto)
  endif()
  if(TK_NO_CRT)
    target_compile_definitions(TimeKeeper PRIVATE TK_NOCRT)
    target_link_options(TimeKeeper PRIVATE -nostartfiles)
  endif()
  target_link_options(TimeKeeper PRIVATE
    -static -static-libgcc
    -Wl,--gc-sections -Wl,--build-id=none
    -Wl,--nxcompat -Wl,--dynamicbase -Wl,--file-alignment,512
    -Wl,--subsystem,windows -Wl,--major-os-version,6 -Wl,--minor-os-version,1
    -Wl,--major-subsystem-version,6 -Wl,--minor-subsystem-version,1)
  target_link_libraries(TimeKeeper PRIVATE kernel32 advapi32 ws2_32 wininet user32)
endif()

# ------------------------------------------------------- post-build gates ----
if(NOT MSVC)
  if(TK_STRIP)
    # Unstripped, the COFF symbol table adds ~31 KB of file bytes while the
    # program itself (section sizes) is identical. Either "103 KB" or "71 KB"
    # would be true depending on who built it; one measured number is worth more
    # than the symbols, and /test still reports FAIL@<line> without them.
    add_custom_command(TARGET TimeKeeper POST_BUILD
      COMMAND ${CMAKE_STRIP} --strip-all $<TARGET_FILE:TimeKeeper>
      VERBATIM COMMENT "stripping symbols")
  endif()
  add_custom_command(TARGET TimeKeeper POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DTARGET_FILE=$<TARGET_FILE:TimeKeeper>
            -DLIMIT=153600
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_size.cmake
    VERBATIM COMMENT "checking the 150 KB hard size limit")
  add_custom_command(TARGET TimeKeeper POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DTARGET_FILE=$<TARGET_FILE:TimeKeeper>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_artifact.cmake
    VERBATIM COMMENT "checking the artifact is not an empty stub")
endif()

# ---------------------------------------------------------------- tests ----
if(TK_TESTS)
  enable_testing()

  if(NOT CMAKE_CROSSCOMPILING)
    add_executable(tk_unit_tests ${TK_TEST_SRC})
    target_include_directories(tk_unit_tests PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(tk_unit_tests PRIVATE
      -std=c11 -O1 -g -Wall -Wextra -Wshadow -fno-tree-loop-distribute-patterns)
    if(NOT MSVC)
      target_compile_options(tk_unit_tests PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(tk_unit_tests PRIVATE -fsanitize=address,undefined)
    endif()
    add_test(NAME unit_tests COMMAND tk_unit_tests)
  endif()

  # Cross-built artifact: run the real PE's own self-test under Wine. This is the
  # check that catches a stub binary, a bad entry point or a missing import -
  # things a size or compile check cannot see.
  find_program(TK_WINE NAMES wine wine64)
  if(TK_WINE)
    add_test(NAME artifact_selftest_wine
      COMMAND ${TK_WINE} $<TARGET_FILE:TimeKeeper> /test)
    set_tests_properties(artifact_selftest_wine PROPERTIES TIMEOUT 300
      ENVIRONMENT "WINEDEBUG=-all")
  endif()

  find_program(TK_PYTHON NAMES python3 python)
  if(TK_PYTHON)
    add_test(NAME pe_audit
      COMMAND ${TK_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/tests/pe_audit.py
              $<TARGET_FILE:TimeKeeper> --require-manifest --require-versioninfo
              --max-size 153600)
  endif()
endif()

install(TARGETS TimeKeeper RUNTIME DESTINATION .)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/TimeKeeper.dat DESTINATION . OPTIONAL)
