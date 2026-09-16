/* privilege.h — SeSystemtimePrivilege, the one capability TimeKeeper needs.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TIMEKEEPER_PRIVILEGE_H
#define TIMEKEEPER_PRIVILEGE_H

#ifndef TIMEKEEPER_NO_WINDOWS
#include <windows.h>

typedef struct {
    int  token_ok;          /* OpenProcessToken succeeded                      */
    int  priv_held;         /* SeSystemtimePrivilege is enabled in this token  */
    int  elevated;          /* process runs with an elevated admin token       */
    int  is_system;         /* running as LocalSystem (the scheduled-task case)*/
    const wchar_t *fail;    /* short static reason for a failure, else NULL    */
    DWORD err;              /* Win32 error of the failing call, else 0         */
} tk_priv_t;

/* Enables SeSystemtimePrivilege if the token has it. Never fails the process:
 * the caller decides what to do with the report. Safe to call twice. */
void tk_priv_acquire(tk_priv_t *p);

#endif /* !TIMEKEEPER_NO_WINDOWS */
#endif /* TIMEKEEPER_PRIVILEGE_H */
