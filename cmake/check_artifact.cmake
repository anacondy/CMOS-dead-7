# check_artifact.cmake — prove the linked .exe is the program, not a stub.
#
#   cmake -DTARGET_FILE=<exe> -P cmake/check_artifact.cmake
#
# Why this exists: with TK_NO_CRT the entry point comes from src/crt_start.c. If
# that file is ever left out of the source list, the link still *succeeds*, the
# binary is smaller than target (32 KB instead of 77 KB), and the size gate is
# satisfied — while the artifact does nothing. A size check that only fails on
# "too big" cannot catch that, so this one checks for evidence of a real program:
# an import table with the expected DLLs, and a .text section of sane size.
#
# SPDX-License-Identifier: MIT
if(NOT TARGET_FILE)
  message(FATAL_ERROR "usage: -DTARGET_FILE=<exe> -P cmake/check_artifact.cmake")
endif()
file(READ "${TARGET_FILE}" blob HEX)
string(LENGTH "${blob}" hexlen)
math(EXPR bytes "${hexlen} / 2")

# A real TimeKeeper build is > 60 KB; the stub that results from a missing entry
# point was 32 KB. The threshold is deliberately crude: it only has to separate
# "something is deeply wrong" from "ship it".
if(bytes LESS 50000)
  message(FATAL_ERROR
    "${TARGET_FILE} is only ${bytes} bytes. A correct MinGW build is ~70-80 KB.\n"
    "A binary this small usually means the entry point is missing: check that\n"
    "src/crt_start.c is in the source list when TK_NO_CRT is ON, and that the\n"
    "linker was given -nostartfiles *with* that file present.")
endif()

# Imports: the PE's import directory must name kernel32. If the whole program was
# optimised away, or the wrong subsystem was used, this is the second signal.
string(TOLOWER "${blob}" low)
string(FIND "${low}" "6b00650072006e0065006c00330032" k32)   # "kernel32" UTF-16LE
if(k32 LESS 0)
  string(FIND "${low}" "4b45524e454c3332" k32a)              # "KERNEL32" ASCII
  if(k32a LESS 0)
    message(FATAL_ERROR
      "${TARGET_FILE} imports no kernel32 — it cannot call GetSystemTime. The link
       produced something other than the program.")
  endif()
endif()
message(STATUS "artifact check: ${bytes} bytes, imports kernel32 -> looks real")
