/* fallback.h — the offline tier: read TimeKeeper.dat, write it atomically.
 *
 * Parsing and the update decision are pure (fallback_proto.c). This file is only
 * about which path holds the file and how bytes reach/leave the disk.
 *
 * Search order for the .dat, and why:
 *   1. <exe dir>\TimeKeeper.dat        — the documented location, portable
 *   2. <exe dir>\fallback.dat          — the spec's alias, so a user can rename
 *   3. %ProgramData%\TimeKeeper\*.dat  — where the installer puts a managed copy
 * The *first readable* file wins. Writes always target the path we read from,
 * except when that path is read-only, in which case we create it in ProgramData
 * and note it in the log (a boot-time tool must not fail because of an ACL).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TIMEKEEPER_FALLBACK_H
#define TIMEKEEPER_FALLBACK_H

#include "timeutil.h"
#include "fallback_proto.h"

#ifndef TIMEKEEPER_NO_WINDOWS
#include <windows.h>

typedef struct {
    time64_t epoch;         /* 00:00:00 UTC of the stored date */
    int      year;
    unsigned month, day;
    int      source;        /* 0 = file, 1 = embedded default  */
    int      file_bad;      /* a file existed but did not parse */
    time64_t stored;        /* file value, or INVALID if there is none */
    time64_t embedded;      /* config.h default, or INVALID if it is nonsense */
    time64_t mtime;         /* .dat last-write time, or INVALID */
    wchar_t  path[MAX_PATH];
} tk_fallback_t;

/* Reads and validates. Never fails: on missing/corrupt input, epoch is the
 * embedded TK_FALLBACK_DATE and source==1. */
void tk_fallback_load(const wchar_t *exe_dir, tk_fallback_t *out);

/* Writes year-month-01 atomically to the file that was read (or creates one
 * beside the .exe). Returns 1 on success, 0 on failure (never fatal). */
int  tk_fallback_update(const tk_fallback_t *cur, int y, unsigned mo,
                        const wchar_t *exe_dir, wchar_t *written_path,
                        size_t path_cap);

#endif /* !TIMEKEEPER_NO_WINDOWS */
#endif /* TIMEKEEPER_FALLBACK_H */
