/* winutil.h — the handful of OS primitives TimeKeeper needs, in one place so
 * an auditor has exactly one file to read for "what does this program touch".
 *
 * Header order: files that use winsock2.h include it before windows.h. That is the
 * order mingw-w64 asks for; the conflict it avoids (windows.h pulling in the
 * 16-bit-era winsock.h) is also why every build recipe here passes
 * -DWIN32_LEAN_AND_MEAN. Without that define the header graph still reaches
 * windows.h first through this file, so treat -DWIN32_LEAN_AND_MEAN as part of the
 * required flag set, not an optimisation — CMakeLists.txt and Makefile.mingw both
 * define it, and build.bat does too.
 *
 * API surface is deliberately Win7-era only (checked against _WIN32_WINNT=0x0601):
 * kernel32, advapi32, ws2_32, wininet. Nothing from 8+; nothing dynamic-loaded.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TIMEKEEPER_WINUTIL_H
#define TIMEKEEPER_WINUTIL_H

#include "timeutil.h"

#ifndef TIMEKEEPER_NO_WINDOWS
#include <windows.h>

typedef struct {
    int      have;      /* 1 if the value could be read                     */
    int      bias_min;  /* currentbias, minutes AHEAD of UTC (IST: +330)  */
    int      is_ist;    /* bias == 330, i.e. the assumed deployment zone   */
    int      dst_on;    /* TIME_ZONE_ID_DAYLIGHT was active                */
} tk_tz_t;

void     tk_win_lasterr_str(char *dst, size_t cap, DWORD err);
int      tk_win_now_epoch(time64_t *out_epoch, uint32_t *out_ms);
int      tk_win_apply_utc(time64_t epoch, uint32_t ms, int dry_run);
int      tk_win_tz(tk_tz_t *tz);
int      tk_win_epoch_to_local_text(time64_t epoch, char *dst, size_t cap,
                                          int *is_utc);
uint32_t tk_tick(void);                       /* ms since boot, wrap-safe */
uint32_t tk_tick_left(uint32_t now_tick, uint32_t deadline_tick);
int      tk_file_write_atomic(const wchar_t *path, const void *data, size_t len);
int      tk_file_read_head(const wchar_t *path, void *buf, size_t cap, size_t *out_len);
int      tk_file_mtime_epoch(const wchar_t *path, time64_t *out);
size_t   tk_wcs_utf8(const wchar_t *src, char *dst, size_t cap);
size_t   tk_utf8_wcs(const char *src, wchar_t *dst, size_t cap);
size_t   tk_strcat_n(char *dst, size_t cap, size_t used, const char *s);
/* Locate schtasks.exe for the *native* bitness: a 32-bit process must go
 * through Sysnative, since System32 is redirected to SysWOW64. */
int      tk_schtasks_path(wchar_t *buf, size_t cap);
#endif /* !TIMEKEEPER_NO_WINDOWS */

#endif /* TIMEKEEPER_WINUTIL_H */
