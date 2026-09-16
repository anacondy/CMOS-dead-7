/* privilege.c — enable SeSystemtimePrivilege and report what we can do.
 *
 * Reading "can I set the clock" from the token *before* calling SetSystemTime
 * is what turns a silent no-op into an actionable log line. A standard user on
 * Vista+ has the privilege removed (not just disabled), so this is also how we
 * detect "run it from the scheduled task / elevated, not by double-click".
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TIMEKEEPER_NO_WINDOWS
#include "config.h"
#include "tklibc.h"
#include "privilege.h"

#include <windows.h>

#define TK_ADJUST_RETRIES 2       /* one retry covers the rare race where the
                                   * LSA is still applying a policy change     */

static int sid_is_local_system(SID *sid)
{
    /* Well-known LocalSystem SID S-1-5-18: authoritative, version 1,
     * subauthorities 18 and 21 (the 21 belongs to the "NT Authority" root). */
    if (!sid) return 0;
    if (sid->Revision != SID_REVISION) return 0;
    if (sid->SubAuthorityCount < 1) return 0;
    if (sid->IdentifierAuthority.Value[5] != 5) return 0;
    return sid->SubAuthority[0] == SECURITY_LOCAL_SYSTEM_RID;
}

void tk_priv_acquire(tk_priv_t *p)
{
    HANDLE  tok = NULL;
    TOKEN_PRIVILEGES tp;
    LUID    luid;
    DWORD   i, len = 0;
    PTOKEN_USER tu = NULL;
    BOOL  ok;

    if (!p) return;
    tk_memset(p, 0, sizeof(*p));
    p->fail = NULL;

    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &tok)) {
        p->fail = L"OpenProcessToken failed";
        p->err  = GetLastError();
        return;
    }
    p->token_ok = 1;

    /* --- who are we? (for the log, and to explain a denial) --------------- */
    if (GetTokenInformation(tok, TokenElevation, NULL, 0, &len) == 0 &&
        GetLastError() == ERROR_INSUFFICIENT_BUFFER && len >= sizeof(TOKEN_ELEVATION)) {
        TOKEN_ELEVATION el;
        if (len > sizeof(el)) len = sizeof(el);
        if (GetTokenInformation(tok, TokenElevation, &el, len, &len))
            p->elevated = el.TokenIsElevated != 0;
    }
    len = 0;
    if (!GetTokenInformation(tok, TokenUser, NULL, 0, &len) &&
        GetLastError() == ERROR_INSUFFICIENT_BUFFER && len > 0 &&
        len < 4096) {
        tu = (PTOKEN_USER)LocalAlloc(LPTR, len);
        if (tu && GetTokenInformation(tok, TokenUser, tu, len, &len))
            p->is_system = sid_is_local_system(tu->User.Sid);
        if (tu) { LocalFree(tu); tu = NULL; }
    }

    /* --- enable SeSystemtimePrivilege ------------------------------------- */
    if (!LookupPrivilegeValueW(NULL, SE_SYSTEMTIME_NAME, &luid)) {
        p->fail = L"LookupPrivilegeValue(SeSystemtimePrivilege) failed";
        p->err  = GetLastError();
        CloseHandle(tok);
        return;
    }

    for (i = 0; i < TK_ADJUST_RETRIES; i++) {
        tk_memset(&tp, 0, sizeof(tp));
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid           = luid;
        tp.Privileges[0].Attributes     = SE_PRIVILEGE_ENABLED;

        SetLastError(ERROR_SUCCESS);
        ok = AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
        if (ok && GetLastError() == ERROR_SUCCESS) { p->priv_held = 1; break; }
        if (!ok) {
            p->err = GetLastError();
            break;                        /* hard failure: retrying is pointless */
        }
        p->err = GetLastError();          /* normally ERROR_NOT_ALL_ASSIGNED    */
    }

    if (!p->priv_held) {
        if (p->err == ERROR_NOT_ALL_ASSIGNED)
            p->fail = p->elevated
                      ? L"SeSystemtimePrivilege not assigned to this account"
                      : L"not elevated: run as SYSTEM task or elevate";
        else
            p->fail = L"AdjustTokenPrivileges failed";
    }
    CloseHandle(tok);
}

#endif /* TIMEKEEPER_NO_WINDOWS */
