/* fallback.c — offline tier file I/O. See fallback.h.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TIMEKEEPER_NO_WINDOWS
#include "config.h"
#include "tklibc.h"
#include "fallback.h"
#include "winutil.h"
#include "verdict.h"

#include <windows.h>

static void wjoin(wchar_t *dst, size_t cap, const wchar_t *dir, const wchar_t *name)
{
    size_t n = 0;
    if (cap == 0) return;
    while (n + 1 < cap && dir && dir[n]) { dst[n] = dir[n]; n++; }
    if (n > 0 && dst[n-1] != L'\\') { if (n + 1 < cap) dst[n++] = L'\\'; }
    while (n + 1 < cap && *name) dst[n++] = *name++;
    dst[n] = 0;
}

static int programdata_dir(wchar_t *dst, size_t cap)
{
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 0;
    wjoin(dst, cap, base, TK_LOG_DIR_NAME_W);
    return 1;
}

void tk_fallback_load(const wchar_t *exe_dir, tk_fallback_t *out)
{
    char     raw[TK_DAT_READ_MAX + 1];
    wchar_t  cand[3][MAX_PATH];
    size_t   len = 0;
    int      i, n = 0;

    tk_memset(out, 0, sizeof(*out));
    out->epoch    = TK_EPOCH_INVALID;
    out->stored   = TK_EPOCH_INVALID;
    out->mtime    = TK_EPOCH_INVALID;
    out->embedded = TK_EPOCH_INVALID;
    out->source   = 1;

    /* candidate order: exe dir (2 names), then ProgramData */
    wjoin(cand[n++], MAX_PATH, exe_dir, TK_DAT_NAME_W);
    wjoin(cand[n++], MAX_PATH, exe_dir, L"fallback.dat");
    {
        wchar_t pd[MAX_PATH];
        if (programdata_dir(pd, MAX_PATH))
            wjoin(cand[n++], MAX_PATH, pd, TK_DAT_NAME_W);
    }

    for (i = 0; i < n; i++) {
        raw[0] = 0;
        if (!tk_file_read_head(cand[i], raw, sizeof(raw) - 1, &len)) continue;
        raw[len] = 0;
        out->file_bad = 1;                       /* a file exists here */
        lstrcpynW(out->path, cand[i], MAX_PATH);
        if (tk_dat_parse(raw, len, TK_MIN_EPOCH_YEAR, TK_MAX_EPOCH_YEAR,
                         &out->epoch, &out->year, &out->month, &out->day)) {
            out->source = 0;
            out->stored = out->epoch;
            tk_file_mtime_epoch(cand[i], &out->mtime);   /* for REF in verdict */
            return;
        }
        /* Unparseable: remember that we saw it (worth a log line) but keep the
         * embedded default and keep looking — a good ProgramData copy should
         * still win over a garbage file beside the exe. */
        if (out->epoch == TK_EPOCH_INVALID) out->source = 1;
    }

    /* Embedded compile-time default. Parsed with the *same* function as a real
     * file: if a typo or a bad regex edit ever puts garbage in config.h, we
     * detect it exactly the way we detect a garbage TimeKeeper.dat. */
    if (!tk_dat_parse(TK_FALLBACK_DATE, sizeof(TK_FALLBACK_DATE) - 1,
                      TK_MIN_EPOCH_YEAR, TK_MAX_EPOCH_YEAR,
                      &out->epoch, &out->year, &out->month, &out->day)) {
        out->epoch = TK_EPOCH_INVALID;   /* refuse to touch the clock */
        return;
    }
    out->embedded = out->epoch;
}

int tk_fallback_update(const tk_fallback_t *cur, int y, unsigned mo,
                       const wchar_t *exe_dir, wchar_t *written_path,
                       size_t path_cap)
{
    char    body[128];
    size_t  len;
    wchar_t target[MAX_PATH];

    len = tk_dat_render(body, sizeof(body), y, mo);
    if (len == 0) return 0;

    /* Prefer the file we read from; that is the one the user maintains. */
    if (cur && cur->source == 0 && cur->path[0])
        lstrcpynW(target, cur->path, MAX_PATH);
    else if (exe_dir && exe_dir[0])
        wjoin(target, MAX_PATH, exe_dir, TK_DAT_NAME_W);
    else {
        wchar_t pd[MAX_PATH];
        if (programdata_dir(pd, MAX_PATH)) {
            DWORD a = GetFileAttributesW(pd);
            if (a == INVALID_FILE_ATTRIBUTES) CreateDirectoryW(pd, NULL);
            wjoin(target, MAX_PATH, pd, TK_DAT_NAME_W);
        } else {
            return 0;
        }
    }

    /* CreateFileW inside tk_file_write_atomic will fail on a read-only dir; in
     * that case retry once in ProgramData so the self-update still works. */
    if (tk_file_write_atomic(target, body, len)) {
        if (written_path && path_cap) lstrcpynW(written_path, target, (int)path_cap);
        return 1;
    }
    {
        wchar_t pd[MAX_PATH];
        if (programdata_dir(pd, MAX_PATH)) {
            DWORD a = GetFileAttributesW(pd);
            if (a == INVALID_FILE_ATTRIBUTES) CreateDirectoryW(pd, NULL);
            wjoin(pd, MAX_PATH, pd, TK_DAT_NAME_W);
            if (tk_file_write_atomic(pd, body, len)) {
                if (written_path && path_cap) lstrcpynW(written_path, pd, (int)path_cap);
                return 1;
            }
        }
    }
    return 0;
}

#endif /* TIMEKEEPER_NO_WINDOWS */
