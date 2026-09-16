/* winutil.c — the OS primitives. Everything Win32-specific that more than one
 * module needs lives here, so the "what does this touch" audit is one file.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TIMEKEEPER_NO_WINDOWS
#include "config.h"
#include "winutil.h"

#include <windows.h>
#include "tklibc.h"

/* ------------------------------------------------------------------ time ---- */

int tk_win_now_epoch(time64_t *out_epoch, uint32_t *out_ms)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return tk_epoch_from_filetime(ft.dwLowDateTime, ft.dwHighDateTime,
                                  out_epoch, out_ms);
}

/* Deadline arithmetic on GetTickCount (ms since boot). Unsigned subtraction is
 * exact across the 49.7-day wrap, which is why we do not need a 64-bit tick:
 * elapsed = now - start is correct for any wrap as long as the true elapsed
 * time is under 2^31 ms (~24 days) — it is measured in milliseconds here. */
uint32_t tk_tick(void) { return (uint32_t)GetTickCount(); }

uint32_t tk_tick_left(uint32_t now_tick, uint32_t deadline_tick)
{
    int32_t left = (int32_t)(deadline_tick - now_tick);
    return left > 0 ? (uint32_t)left : 0u;
}

int tk_win_apply_utc(time64_t epoch, uint32_t ms, int dry_run)
{
    SYSTEMTIME st;
    uint32_t lo, hi;

    if (!tk_filetime_from_epoch_ms(epoch, ms, &lo, &hi)) return -1;  /* unrepresentable */
    st.wYear = 0; /* filled by FileTimeToSystemTime; also validates range */
    {
        FILETIME ft;
        SYSTEMTIME out;
        ft.dwLowDateTime = lo; ft.dwHighDateTime = hi;
        if (!FileTimeToSystemTime(&ft, &out)) return -2;
        st = out;
    }
    if (dry_run) return 0;
    SetLastError(0);
    if (!SetSystemTime(&st)) {
        /* -3 is returned as -GetLastError() by the caller convention below. */
        return -(int)GetLastError();
    }
    return 0;
}

/* --------------------------------------------------------------- timezone ---- */

int tk_win_tz(tk_tz_t *tz)
{
    TIME_ZONE_INFORMATION tzi;
    DWORD r;

    if (!tz) return 0;
    tk_memset(tz, 0, sizeof(*tz));
    r = GetTimeZoneInformation(&tzi);
    if (r == TIME_ZONE_ID_UNKNOWN && tzi.Bias == 0 && tzi.StandardBias == 0) {
        /* A machine with no zone configured at all: Bias is then genuinely 0
         * (UTC) and we report that rather than "unknown". */
        tz->bias_min = 0;
        tz->dst_on   = 0;
    } else if (r == TIME_ZONE_ID_INVALID) {
        return 0;
    } else {
        tz->bias_min = -(int)tzi.Bias -
                       (int)((r == TIME_ZONE_ID_DAYLIGHT) ? tzi.DaylightBias
                                                          : tzi.StandardBias);
        tz->dst_on   = (r == TIME_ZONE_ID_DAYLIGHT);
    }
    tz->have   = 1;
    tz->is_ist = (tz->bias_min == TK_IST_BIAS_MIN);
    return 1;
}

/* "11:59:59 IST+330" style suffix, for the log only. Never used for a decision. */
int tk_win_epoch_to_local_text(time64_t epoch, char *dst, size_t cap, int *is_utc)
{
    SYSTEMTIME utc, loc;
    tk_tz_t tz;
    int bias = 0;
    size_t n = 0;
    char sign;

    if (cap < 24) return 0;
    utc.wYear = 0;
    {
        FILETIME ft;
        uint32_t lo, hi;
        if (!tk_filetime_from_epoch_ms(epoch, 0, &lo, &hi)) return 0;
        ft.dwLowDateTime = lo; ft.dwHighDateTime = hi;
        if (!FileTimeToSystemTime(&ft, &utc)) return 0;
    }
    if (tk_win_tz(&tz) && tz.have) bias = tz.bias_min;
    if (is_utc) *is_utc = (bias == 0);

    tk_memset(&loc, 0, sizeof(loc));
    if (bias == 0) {
        loc = utc;
    } else if (!SystemTimeToTzSpecificLocalTime(NULL, &utc, &loc)) {
        /* No zone data: fall back to plain arithmetic on the bias, which is all
         * a fixed-offset zone (IST) needs and matches what the OS would do. */
        time64_t l = epoch + (int64_t)bias * 60;
        unsigned hh, mm, ss, mo, d;
        int y;
        if (!tk_civil_from_epoch_utc(l, &y, &mo, &d, &hh, &mm, &ss)) return 0;
        loc.wYear = (WORD)y; loc.wMonth = (WORD)mo; loc.wDay = (WORD)d;
        loc.wHour = (WORD)hh; loc.wMinute = (WORD)mm; loc.wSecond = (WORD)ss;
    }
    n += tk_fmt_time(dst + n, cap - n, loc.wHour, loc.wMinute, loc.wSecond);
    sign = (bias < 0) ? '-' : '+';
    if (n + 8 < cap) {
        dst[n++] = ' ';
        if (bias == 0) { dst[n++] = 'U'; dst[n++] = 'T'; dst[n++] = 'C'; }
        else {
            dst[n++] = 'G'; dst[n++] = 'M'; dst[n++] = 'T';
            dst[n++] = sign;
            n += tk_fmt_u32(dst + n, cap - n, (uint32_t)(bias < 0 ? -bias : bias), 3);
        }
    }
    dst[n] = 0;
    return 1;
}

/* ------------------------------------------------------------------- files --- */

static int filetime_to_epoch(const FILETIME *ft, time64_t *out)
{
    uint64_t total = ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    int64_t secs;
    if (total == 0) return 0;
    secs = (int64_t)(total / TK_FT_TICKS_PER_SEC) + TK_EPOCH_FT_MIN;
    *out = secs;
    return 1;
}

int tk_file_read_head(const wchar_t *path, void *buf, size_t cap, size_t *out_len)
{
    HANDLE h;
    DWORD  got = 0;

    if (out_len) *out_len = 0;
    if (!path || !path[0] || !buf || cap == 0) return 0;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                    NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(h, buf, (DWORD)cap, &got, NULL)) got = 0;
    CloseHandle(h);
    if (out_len) *out_len = got;
    return got > 0;
}

int tk_file_write_atomic(const wchar_t *path, const void *data, size_t len)
{
    wchar_t tmp[MAX_PATH];
    size_t  n = 0;
    HANDLE  h;
    DWORD   wr = 0;
    BOOL    flushed;

    if (!path || !path[0] || !data || len == 0) return 0;
    while (n + 1 < MAX_PATH && path[n]) { tmp[n] = path[n]; n++; }
    if (n + 5 >= MAX_PATH) return 0;
    tmp[n++] = L'.'; tmp[n++] = L't'; tmp[n++] = L'm'; tmp[n++] = L'p'; tmp[n] = 0;

    /* 1. write + flush the temp file  2. move it over the target with
     * REPLACE_EXISTING|WRITE_THROUGH.  A crash at any point leaves either the
     * old .dat or the new one — never a truncated half file. */
    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!WriteFile(h, data, (DWORD)len, &wr, NULL) || wr != (DWORD)len) {
        CloseHandle(h);
        DeleteFileW(tmp);
        return 0;
    }
    flushed = FlushFileBuffers(h);
    CloseHandle(h);
    if (!flushed) { DeleteFileW(tmp); return 0; }

    if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION) {
            /* Someone holds the target open (editor, AV scan). MoveFileEx cannot
             * replace it; deleting first has the same effect and is still atomic
             * enough here because the file is advisory, not authoritative. */
            if (DeleteFileW(path)) {
                if (MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    return 1;
            }
        }
        DeleteFileW(tmp);
        SetLastError(e);
        return 0;
    }
    return 1;
}

int tk_file_mtime_epoch(const wchar_t *path, time64_t *out)
{
    HANDLE h;
    FILETIME ft;
    int r;

    if (out) *out = TK_EPOCH_INVALID;
    if (!path || !path[0]) return 0;
    h = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    r = GetFileTime(h, NULL, NULL, &ft);
    CloseHandle(h);
    if (!r) return 0;
    return filetime_to_epoch(&ft, out);
}

/* ------------------------------------------------------- string utilities --- */

size_t tk_utf8_wcs(const char *src, wchar_t *dst, size_t cap)
{
    int n;
    if (cap == 0) return 0;
    dst[0] = 0;
    if (!src) return 0;
    n = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, (int)cap);
    if (n <= 0) {                                   /* invalid UTF-8: ASCII copy */
        size_t i = 0;
        while (i + 1 < cap && src[i]) { dst[i] = (wchar_t)(unsigned char)src[i]; i++; }
        dst[i] = 0;
        return i;
    }
    return (size_t)(n - 1);
}

size_t tk_wcs_utf8(const wchar_t *src, char *dst, size_t cap)
{
    int n;
    if (cap == 0) return 0;
    dst[0] = 0;
    if (!src) return 0;
    n = WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, (int)cap, NULL, NULL);
    if (n <= 0) {
        size_t i = 0;
        while (i + 1 < cap && src[i]) {
            dst[i] = (src[i] < 0x20 || src[i] > 0x7E) ? '?' : (char)src[i];
            i++;
        }
        dst[i] = 0;
        return i;
    }
    return (size_t)(n - 1);
}

size_t tk_strcat_n(char *dst, size_t cap, size_t used, const char *s)
{
    size_t n = used;
    if (cap == 0) return 0;
    while (s && *s && n + 1 < cap) dst[n++] = *s++;
    dst[n] = 0;
    return n;
}

/* ------------------------------------------------------ system directory ---- */

int tk_schtasks_path(wchar_t *buf, size_t cap)
{
    wchar_t root[MAX_PATH];
    BOOL    wow = FALSE;
    const wchar_t *tail;
    size_t  n = 0;
    DWORD   a;

    if (!buf || cap < 24) return 0;
    if (GetEnvironmentVariableW(L"SystemRoot", root, MAX_PATH) == 0 || !root[0])
        return 0;
    IsWow64Process(GetCurrentProcess(), &wow);
    /* %windir%\System32 seen from a 32-bit process is silently redirected to
     * SysWOW64; Sysnative is the documented escape hatch to the 64-bit copy. */
    tail = wow ? L"\\Sysnative\\schtasks.exe" : L"\\System32\\schtasks.exe";
    while (n + 1 < cap && root[n]) { buf[n] = root[n]; n++; }
    while (*tail && n + 1 < cap) buf[n++] = *tail++;
    buf[n] = 0;
    a = GetFileAttributesW(buf);
    if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) return 1;

    /* 32-bit Windows (and any odd install without Sysnative): plain System32. */
    if (GetSystemDirectoryW(root, MAX_PATH) > 0) {
        n = 0;
        while (n + 1 < cap && root[n]) { buf[n] = root[n]; n++; }
        if (n > 0 && buf[n-1] != L'\\') { if (n + 1 < cap) buf[n++] = L'\\'; }
        tail = L"schtasks.exe";
        while (*tail && n + 1 < cap) buf[n++] = *tail++;
        buf[n] = 0;
        if (GetFileAttributesW(buf) != INVALID_FILE_ATTRIBUTES) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------- misc --- */

void tk_win_lasterr_str(char *dst, size_t cap, DWORD err)
{
    wchar_t *msg = NULL;
    DWORD n;

    if (cap == 0) return;
    dst[0] = 0;
    n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0, (LPWSTR)&msg, 0, NULL);
    if (n && msg) {
        char utf8[256];
        size_t m = tk_wcs_utf8(msg, utf8, sizeof(utf8));
        while (m > 0 && (utf8[m-1] == '\r' || utf8[m-1] == '\n')) utf8[--m] = 0;
        { size_t i = 0;
          while (i < m && i + 1 < cap) { dst[i] = utf8[i]; i++; }
          dst[i] = 0; }
    }
    if (msg) LocalFree(msg);
}

#endif /* TIMEKEEPER_NO_WINDOWS */
