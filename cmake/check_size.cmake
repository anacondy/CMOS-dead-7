# check_size.cmake — fail the build when the artifact exceeds the size budget.
#
#   cmake -DTARGET_FILE=bin/TimeKeeper32.exe -DLIMIT=153600 -P cmake/check_size.cmake
#
# SPDX-License-Identifier: MIT
if(NOT TARGET_FILE)
  message(FATAL_ERROR "usage: -DTARGET_FILE=<exe> [-DLIMIT=bytes] -P check_size.cmake")
endif()
if(NOT LIMIT)
  set(LIMIT 153600)   # hard limit from the specification
endif()
file(SIZE "${TARGET_FILE}" sz)
if(sz GREATER LIMIT)
  message(FATAL_ERROR
    "${TARGET_FILE} is ${sz} bytes, over the ${LIMIT} byte budget.\n"
    "Check: TK_NO_CRT=ON, -Os, --gc-sections, and that the self-test was built\n"
    "with -DTK_SELFTEST_COMPACT. See TESTING.md 'size' for the measurement recipe.")
endif()
get_filename_component(n "${TARGET_FILE}" NAME)
message(STATUS "size budget: ${n} = ${sz} bytes (limit ${LIMIT}) -> OK")
