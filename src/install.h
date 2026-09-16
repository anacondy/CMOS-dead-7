/* install.h / install.c — self-installation of the scheduled task.
 *
 * The primary deployment is install.cmd (plain batch, auditable). This mode
 * exists so the .exe can be dropped on a machine and registered by someone who
 * cannot copy a script in — and so that install.cmd stays a 20-line wrapper
 * rather than duplicating the logic.
 *
 * Task design (Windows 7 Task Scheduler 1.0/2.x compatible, hence schtasks
 * rather than the COM API or PowerShell):
 *   "TimeKeeper"        /sc onstart  /ru SYSTEM /rl HIGHEST  — fixes the clock
 *                                                                  at boot
 *   "TimeKeeperLogon"   /sc onlogon  /ru SYSTEM /rl HIGHEST  — fixes it again
 *                                                                  when nobody
 *                                                                  was logged
 *                                                                  in at boot
 * Both are needed: on a machine that is never shut down, onstart alone can sit
 * for weeks; on a machine booted unattended, onlogon alone is too late for
 * whatever runs first. The task is a plain, deletable, non-service artefact.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TIMEKEEPER_INSTALL_H
#define TIMEKEEPER_INSTALL_H

#ifndef TIMEKEEPER_NO_WINDOWS
#include <windows.h>

typedef struct {
    int   ok;               /* 1 = every step succeeded                  */
    char  detail[200];      /* first failure reason, for the log         */
    wchar_t target_dir[MAX_PATH];
    int   exe_copied, dat_copied, task_start_ok, task_logon_ok;
} tk_install_result_t;

int tk_install(tk_install_result_t *res);
int tk_uninstall(tk_install_result_t *res);

#endif /* !TIMEKEEPER_NO_WINDOWS */
#endif /* TIMEKEEPER_INSTALL_H */
