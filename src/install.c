/* install.c — see install.h.
 *
 * Deliberately small: copy two files, register two tasks, done. Everything
 * else (ACL review, log inspection) belongs to the human running install.cmd.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TIMEKEEPER_NO_WINDOWS
#include "config.h"
#include "tklibc.h"
#include "install.h"
#include "winutil.h"
#include "fallback_proto.h"
#include "log.h"

#include <windows.h>

/* --- helpers --------------------------------------------------------------- */

static void sj(wchar_t *dst, size_t cap, const wchar_t *a, const wchar_t *b)
{
    size_t n = 0;
    if (cap == 0) return;
    while (n + 1 < cap && a && a[n]) { dst[n] = a[n]; n++; }
    if (n > 0 && dst[n-1] != L'\\') { if (n + 1 < cap) dst[n++] = L'\\'; }
    while (n + 1 < cap && *b) dst[n++] = *b++;
    dst[n] = 0;
}

static int env_dir(const wchar_t *name, wchar_t *dst, size_t cap)
{
    DWORD n = GetEnvironmentVariableW(name, dst, (DWORD)cap);
    return (n > 0 && n < cap);
}

/* First failure wins: later steps must not overwrite the reason. */
static void note(tk_install_result_t *r, const char *why)
{
    if (r && !r->ok) { tk_log_src("INSTALL", why); return; }
    if (r) {
        size_t i = 0;
        while (why[i] && i + 1 < sizeof(r->detail)) { r->detail[i] = why[i]; i++; }
        r->detail[i] = 0;
    }
    tk_log_src("INSTALL", why);
}

/* Run schtasks.exe to completion. The exit code is the only signal we consume:
 * the child writes to a console this GUI process does not have, and a 15 s
 * bound keeps a wedged Task Scheduler from hanging the boot. */
#define TK_SCHTASKS_WAIT_MS 15000u

static int run_schtasks(const wchar_t *args)
{
    wchar_t exe[MAX_PATH];
    wchar_t line[1200];
    size_t  n = 0;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD rc = (DWORD)-1;
    const wchar_t *p;

    if (!tk_schtasks_path(exe, MAX_PATH)) { tk_log_src("INSTALL", "schtasks.exe not found"); return 0; }

    line[n++] = L'"';
    for (p = exe; *p && n + 2 < sizeof(line)/sizeof(line[0]); p++) line[n++] = *p;
    line[n++] = L'"';
    line[n++] = L' ';
    for (p = args; *p && n + 1 < sizeof(line)/sizeof(line[0]); p++) line[n++] = *p;
    line[n] = 0;

    tk_memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    tk_memset(&pi, 0, sizeof(pi));

    if (!CreateProcessW(exe, line, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, NULL, NULL,
                        &si, &pi)) {
        tk_log_err("cannot start schtasks.exe", (int)GetLastError());
        return 0;
    }
    if (WaitForSingleObject(pi.hProcess, TK_SCHTASKS_WAIT_MS) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        tk_log_err("schtasks.exe did not finish in 15 s", 0);
        return 0;
    }
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return rc == 0;
}

/* /tr must carry the action path in its own quotes so a space in
 * "Program Files" survives both our command line and schtasks' parser. */
static int create_task(const wchar_t *task, const wchar_t *schedule,
                       const wchar_t *exe_path, const wchar_t *extra)
{
    wchar_t a[1000];
    size_t n = 0;
    const wchar_t *p;

#define PUT(s) do { p = (s); while (*p && n + 1 < sizeof(a)/sizeof(a[0])) a[n++] = *p++; } while (0)
    PUT(L"/create /f /tn \"");
    PUT(task);
    PUT(L"\" /tr \"\\\"");
    PUT(exe_path);
    PUT(L"\\\"\" /sc ");
    PUT(schedule);
    PUT(L" /ru SYSTEM /rl HIGHEST");
    if (extra && *extra) { PUT(L" "); PUT(extra); }
#undef PUT
    a[n] = 0;
    return run_schtasks(a);
}

static int delete_task(const wchar_t *task)
{
    wchar_t a[256];
    size_t n = 0;
    const wchar_t *p;
#define PUT(s) do { p = (s); while (*p && n + 1 < sizeof(a)/sizeof(a[0])) a[n++] = *p++; } while (0)
    PUT(L"/delete /f /tn \"");
    PUT(task);
    PUT(L"\"");
#undef PUT
    a[n] = 0;
    /* A missing task is not a failure for an uninstall. */
    return run_schtasks(a);
}

/* Directory that holds our own image, without a trailing separator. */
static const wchar_t *exe_dir_of_self(const wchar_t *image)
{
    static wchar_t dir[MAX_PATH];
    size_t n = 0;
    while (n + 1 < MAX_PATH && image[n]) { dir[n] = image[n]; n++; }
    while (n > 0 && dir[n-1] != L'\\' && dir[n-1] != L'/') n--;
    if (n > 0 && (dir[n-1] == L'\\' || dir[n-1] == L'/')) n--;
    dir[n] = 0;
    return dir;
}

/* --- install --------------------------------------------------------------- */

int tk_install(tk_install_result_t *res)
{
    wchar_t pf[MAX_PATH], src[MAX_PATH], dst[MAX_PATH], dat_src[MAX_PATH], dat_dst[MAX_PATH];
    DWORD a;
    BOOL same = FALSE;

    tk_memset(res, 0, sizeof(*res));
    res->ok = 1;

    if (GetModuleFileNameW(NULL, src, MAX_PATH) == 0) {
        note(res, "cannot locate own image");
        return 0;
    }

    if (!env_dir(L"ProgramFiles", pf, MAX_PATH)) {
        if (!env_dir(L"SystemDrive", pf, MAX_PATH)) { note(res, "no ProgramFiles/SystemDrive"); return 0; }
        sj(pf, MAX_PATH, pf, L"\\Program Files");
    }
    sj(res->target_dir, MAX_PATH, pf, TK_SUBDIR_W);
    sj(dst, MAX_PATH, res->target_dir, L"TimeKeeper.exe");
    sj(dat_dst, MAX_PATH, res->target_dir, TK_DAT_NAME_W);

    /* Installing *from* the target dir is a no-op for the copy step: a running
     * image cannot overwrite itself, and it is already the right file. */
    same = (lstrcmpiW(src, dst) == 0);

    a = GetFileAttributesW(res->target_dir);
    if (a == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryW(res->target_dir, NULL)) { note(res, "cannot create install directory"); return 0; }
    } else if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        note(res, "install path exists and is not a directory"); return 0;
    }

    if (same) {
        res->exe_copied = 1;
        tk_log_src("INSTALL", "executable already in place, copy skipped");
    } else {
        if (!CopyFileW(src, dst, FALSE)) { note(res, "CopyFile of TimeKeeper.exe failed"); return 0; }
        res->exe_copied = 1;
    }

    /* TimeKeeper.dat: seed the install directory from the compile-time default
     * when there is no file there yet, so the installed fallback is a visible,
     * editable artefact rather than an invisible constant. If the source
     * directory has one (portable layout), that copy wins instead. */
    sj(dat_src, MAX_PATH, exe_dir_of_self(src), TK_DAT_NAME_W);
    if (GetFileAttributesW(dat_dst) == INVALID_FILE_ATTRIBUTES) {
        if (lstrcmpiW(dat_src, dat_dst) != 0 && GetFileAttributesW(dat_src) != INVALID_FILE_ATTRIBUTES)
            (void)CopyFileW(dat_src, dat_dst, TRUE);
    }
    if (GetFileAttributesW(dat_dst) == INVALID_FILE_ATTRIBUTES) {
        char body[128];
        size_t len;
        {
            int yy = 0; unsigned mo = 0, dd = 0;
            char txt[32];
            size_t k = 0;
            while (k + 1 < sizeof(txt) && TK_FALLBACK_DATE[k]) { txt[k] = TK_FALLBACK_DATE[k]; k++; }
            txt[k] = 0;
            (void)tk_parse_date_text(txt, k, &yy, &mo, &dd, 0, 0, 0);
            len = tk_dat_render(body, sizeof(body), yy, mo);
        }
        if (len == 0) {
            note(res, "embedded fallback date is unparsable");
            return 0;
        }
        if (!tk_file_write_atomic(dat_dst, body, len)) { note(res, "cannot write TimeKeeper.dat"); return 0; }
        res->dat_copied = 1;
    } else {
        res->dat_copied = 1;
    }

    if (create_task(TK_TASK_ONSTART_W, L"onstart", dst, NULL)) res->task_start_ok = 1;
    else note(res, "schtasks /sc onstart failed");

    /* /delay 0000:10 is ignored by Task Scheduler 1.0 (Vista-era schtasks), so we do not
     * depend on it: the task itself fast-exits when the clock is already right. */
    if (create_task(TK_TASK_ONLOGON_W, L"onlogon", dst, NULL)) res->task_logon_ok = 1;
    else if (res->ok) note(res, "schtasks /sc onlogon failed (optional)");

    /* Correct the clock now, once, so installing does not require a reboot. */
    if (res->ok) {
        wchar_t a2[600];
        size_t n = 0;
        const wchar_t *p;
#define PUT(s) do { p = (s); while (*p && n + 1 < sizeof(a2)/sizeof(a2[0])) a2[n++] = *p++; } while (0)
        PUT(L"/run /tn \""); PUT(TK_TASK_ONSTART_W); PUT(L"\"");
#undef PUT
        a2[n] = 0;
        (void)run_schtasks(a2);          /* best effort; failure here is not fatal */
    }
    return res->ok;
}

int tk_uninstall(tk_install_result_t *res)
{
    tk_memset(res, 0, sizeof(*res));
    res->ok = 1;

    if (delete_task(TK_TASK_ONSTART_W)) res->task_start_ok = 1;
    else note(res, "could not delete the boot task");
    if (delete_task(TK_TASK_ONLOGON_W)) res->task_logon_ok = 1;

    tk_log_src("INSTALL", "tasks deleted; files under the install directory and the log "
                          "are left in place (delete by hand if desired)");
    return res->ok;
}

#endif /* TIMEKEEPER_NO_WINDOWS */
