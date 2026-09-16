/* log.c — append-only UTF-16 log with a hard size cap, plus console mirroring.
 *
 * Why UTF-16: this file is written by a SYSTEM process at boot, and the ANSI
 * codepage of a Win7 box is anybody's guess (cp1252, cp437, 65001...). A
 * WriteFileW of a UTF-16 buffer with a BOM is the one thing Notepad and
 * `more` both get right on every install.
 *
 * Why no printf: the CRT's formatting drags in locale data (tens of KB) and
 * pre-2015 MSVC has no %zu at all. tk_fmt_* in timeutil.c is deterministic,
 * bounded, and auditable in one screen.
 *
 * Failure policy: a log that cannot be written must never change the exit code
 * or abort the run, so every Win32 result here is checked and then ignored.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TIMEKEEPER_NO_WINDOWS
#include "config.h"
#include "tklibc.h"
#include "winutil.h"
#include "log.h"
#include "log_rotate.h"

#include <windows.h>

typedef struct {
    wchar_t dir[MAX_PATH];
    wchar_t file[MAX_PATH];
    int     dir_kind;   /* 0 none, 1 ProgramData, 2 exe dir, 3 temp  */
    int     console;    /* mirror to stdout                           */
    int     path_warned;
} tk_log_ctx;

static tk_log_ctx g_L;

/* --- tiny path helpers (no shlwapi dependency) --------------------------- */

static size_t wlen(const wchar_t *s, size_t cap)
{
    size_t n = 0;
    while (n < cap && s[n]) n++;
    return n;
}

static void path_join(wchar_t *dst, size_t cap, const wchar_t *a, const wchar_t *b)
{
    size_t la, n = 0;

    if (cap == 0) return;
    la = wlen(a, cap);
    if (la + 1 >= cap) { dst[0] = 0; return; }
    while (n < la) { dst[n] = a[n]; n++; }
    if (n > 0 && dst[n-1] != L'\\' && dst[n-1] != L'/') dst[n++] = L'\\';
    while (*b && n + 1 < cap) dst[n++] = *b++;
    dst[n] = 0;
}

/* Open-for-append test, then close. Costs two syscalls once per run. */
static int log_writable(const wchar_t *p)
{
    HANDLE h = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(h);
    return 1;
}

static int dir_exists(const wchar_t *p)
{
    DWORD a;
    if (!p || !p[0]) return 0;
    a = GetFileAttributesW(p);
    return (a != INVALID_FILE_ATTRIBUTES) && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* --- open ----------------------------------------------------------------- */

int tk_log_open(const wchar_t *exe_dir, int mirror_console)
{
    wchar_t base[MAX_PATH];

    g_L.console   = mirror_console;
    g_L.dir_kind  = 0;
    g_L.file[0]   = 0;
    g_L.dir[0]    = 0;

    /* 1. %ProgramData%\TimeKeeper — documented, SYSTEM-writable, user-read-only. */
    if (GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH) > 0 && base[0]) {
        path_join(g_L.dir, MAX_PATH, base, TK_LOG_DIR_NAME_W);
        if (!dir_exists(g_L.dir)) (void)CreateDirectoryW(g_L.dir, NULL);
        if (dir_exists(g_L.dir)) {
            path_join(g_L.file, MAX_PATH, g_L.dir, TK_LOG_NAME_W);
            g_L.dir_kind = 1;
        }
    }
    /* 2. beside the .exe — portable install, or ProgramData on a read-only volume. */
    if (!g_L.dir_kind && exe_dir && exe_dir[0]) {
        path_join(g_L.file, MAX_PATH, exe_dir, TK_LOG_NAME_W);
        if (log_writable(g_L.file)) {
            lstrcpynW(g_L.dir, exe_dir, MAX_PATH);
            g_L.dir_kind = 2;
        }
    }
    /* 3. %TEMP% — last resort so a run never ends with no trace at all. */
    if (!g_L.dir_kind && GetTempPathW(MAX_PATH, base) > 0) {
        path_join(g_L.file, MAX_PATH, base, TK_LOG_NAME_W);
        if (log_writable(g_L.file)) g_L.dir_kind = 3;
    }
    if (!g_L.dir_kind) g_L.file[0] = 0;
    return g_L.dir_kind != 0;
}

const wchar_t *tk_log_path(void) { return g_L.file; }

/* --- rotation: keep the newest half, atomically --------------------------- */

/* tmp = path + ".tmp". Note the trap this code originally fell into: joining a
 * *file* path with a separator ("...timekeeper.log\") and then appending ".tmp"
 * yields a path whose parent is a nonexistent directory, the create fails, and
 * the caller silently drops to the lossy truncate-instead-of-rotate fallback. */
static void sibling_tmp(const wchar_t *path, wchar_t *dst, size_t cap)
{
    size_t n = 0;
    static const wchar_t kTmp[] = L".tmp";
    while (n + 1 < cap && path[n]) { dst[n] = path[n]; n++; }
    if (n + lstrlenW(kTmp) + 1 >= cap) { dst[0] = 0; return; }
    { size_t i = 0; while (kTmp[i] && n + 1 < cap) dst[n++] = kTmp[i++]; }
    dst[n] = 0;
}

static void rotate_if_full(void)
{
    static char tail[TK_LOG_KEEP_BYTES + 8];
    HANDLE h, w;
    DWORD  size, got = 0, wr = 0, skip;
    wchar_t tmp[MAX_PATH];

    h = CreateFileW(g_L.file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size <= TK_LOG_MAX_BYTES) { CloseHandle(h); return; }

    skip = size - TK_LOG_KEEP_BYTES;
    skip &= ~1u;                        /* stay on a UTF-16 code-unit boundary */
    SetFilePointer(h, (LONG)skip, NULL, FILE_BEGIN);
    if (!ReadFile(h, tail, TK_LOG_KEEP_BYTES, &got, NULL) || got < 64) {
        /* The tail is unreadable (locked, or shorter than the size claimed a
         * moment ago). Truncating is better than unbounded growth on a small
         * system drive, and it is lossy, so it happens in place rather than via
         * the tmp+move path below. */
        CloseHandle(h);
        h = CreateFileW(g_L.file, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) { SetEndOfFile(h); CloseHandle(h); }
        return;
    }
    CloseHandle(h);
    h = INVALID_HANDLE_VALUE;

    /* Keep only the whole lines at the end. The log is UTF-16LE, so a line
     * boundary is the four bytes 0D 00 0A 00 at an even offset; cutting anywhere
     * else yields a file whose first line is half a character, which Notepad
     * shows as mojibake and which makes the file look corrupt even though the
     * data is fine. The scan is in tk_log_trim_tail() (src/log_rotate.c) so it
     * can be unit-tested on any box, including this one. */
    got = (DWORD)tk_log_trim_tail((unsigned char *)tail, (size_t)got, NULL);
    if (got == 0) {
        h = CreateFileW(g_L.file, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) { SetEndOfFile(h); CloseHandle(h); }
        return;
    }

    sibling_tmp(g_L.file, tmp, MAX_PATH);
    if (!tmp[0]) return;
    w = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (w == INVALID_HANDLE_VALUE) {
        DeleteFileW(tmp);
        w = CreateFileW(g_L.file, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (w != INVALID_HANDLE_VALUE) { SetEndOfFile(w); CloseHandle(w); }
        return;
    }
    WriteFile(w, "\xFF\xFE", 2, &wr, NULL);
    WriteFile(w, tail, got, &wr, NULL);
    FlushFileBuffers(w);
    CloseHandle(w);
    /* Same move-only-overwrite discipline as TimeKeeper.dat: the file is either
     * the old one or the new one, never half of either. */
    if (!MoveFileExW(tmp, g_L.file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        /* Someone else holds it open (an editor, an AV scan). Leaving the
         * oversized file alone is the right call: it is bounded by the next run,
         * and losing the log to a failed replace would be worse than a
         * temporarily large one. */
        DeleteFileW(tmp);
    }
}

/* --- line assembly -------------------------------------------------------- */

void tk_log_write(const char *src, const char *msg)
{
    char     u8[TK_LOG_LINE_MAX];
    wchar_t  wide[TK_LOG_LINE_MAX];
    size_t   n = 0, cap = sizeof(u8) - 2;
    time64_t e = TK_EPOCH_INVALID;
    uint32_t ms = 0;
    int      y = 1970;
    unsigned mo = 1, d = 1, hh = 0, mi = 0, ss = 0;
    HANDLE   f;
    DWORD    wr = 0;

    /* Timestamped *after* a SetSystemTime on purpose: the line then shows the
     * corrected clock, which is the one number an operator checks. */
    if (tk_win_now_epoch(&e, &ms))
        tk_civil_from_epoch_utc(e, &y, &mo, &d, &hh, &mi, &ss);

    /* "[<UTC date> <time>.<ms>Z] [<SRC>] <message>": the bracketed timestamp is
     * the format the specification prescribes, and it makes `grep '^\['` and
     * "sort" work on the file. Local time is in the message where it matters. */
    u8[n++] = '[';
    n += tk_fmt_date(u8 + n, cap - n, y, mo, d);
    if (n + 24 < cap) {
        u8[n++] = ' ';
        n += tk_fmt_time(u8 + n, cap - n, hh, mi, ss);
        u8[n++] = '.';
        n += tk_fmt_u32(u8 + n, cap - n, ms, 3);
        u8[n++] = 'Z';
        u8[n++] = ']';
        u8[n++] = ' ';
        u8[n++] = '[';
        { const char *p = src ? src : "INFO";
          while (*p && n + 6 < cap) u8[n++] = *p++; }
        u8[n++] = ']';
        u8[n++] = ' ';
        { const char *p = msg ? msg : "";
          while (*p && n + 3 < cap) u8[n++] = *p++; }
    }
    u8[n++] = '\r'; u8[n++] = '\n'; u8[n] = 0;

    if (!g_L.file[0]) {
        if (!g_L.path_warned) { g_L.path_warned = 1; }
        return;
    }
    tk_utf8_wcs(u8, wide, sizeof(wide) / sizeof(wide[0]));

    f = CreateFileW(g_L.file, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                    NULL);
    if (f != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz;
        if (GetFileSizeEx(f, &sz) && sz.QuadPart == 0)
            WriteFile(f, "\xFF\xFE", 2, &wr, NULL);   /* BOM only on creation */
        WriteFile(f, wide, (DWORD)(lstrlenW(wide) * sizeof(wchar_t)), &wr, NULL);
        CloseHandle(f);
        rotate_if_full();
    }

    if (g_L.console) {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out && out != INVALID_HANDLE_VALUE)
            WriteFile(out, wide, (DWORD)(lstrlenW(wide) * sizeof(wchar_t)), &wr, NULL);
    }
}

void tk_log_info(const char *msg) { tk_log_write("INFO", msg); }
void tk_log_src (const char *src, const char *msg) { tk_log_write(src, msg); }

void tk_log_err(const char *msg, int win32_err)
{
    char buf[TK_LOG_LINE_MAX];
    size_t n = 0;
    const char *p = msg ? msg : "error";
    while (*p && n + 40 < sizeof(buf) - 1) buf[n++] = *p++;
    if (win32_err) {
        static const char kErr[] = " (Win32 error ";
        p = kErr;
        while (*p && n + 12 < sizeof(buf) - 1) buf[n++] = *p++;
        n += tk_fmt_u32(buf + n, sizeof(buf) - 1 - n, (uint32_t)(DWORD)win32_err, 1);
        if (n + 2 < sizeof(buf) - 1) { buf[n++] = ')'; buf[n] = 0; }
    }
    buf[n] = 0;
    tk_log_write("ERROR", buf);
}

void tk_log_close(void) { }

#endif /* TIMEKEEPER_NO_WINDOWS */
