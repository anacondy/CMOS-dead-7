/* log.h — the only output channel TimeKeeper has.
 *
 * A run happens at boot, as SYSTEM, with no console, so the file must carry the
 * whole story. One call site per event; no levels, no categories, no buffering.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TIMEKEEPER_LOG_H
#define TIMEKEEPER_LOG_H

#include <stddef.h>

#ifndef TIMEKEEPER_NO_WINDOWS
#include <windows.h>

/* Picks %ProgramData%\TimeKeeper\timekeeper.log, else next to the .exe, else
 * %TEMP%. Returns 1 if some path was chosen (writing may still fail, which is
 * never fatal). `mirror_console` echoes every line to stdout as well. */
int  tk_log_open(const wchar_t *exe_dir, int mirror_console);
void tk_log_close(void);

/* src: short tag for the log bracket, e.g. "SRC:NTP(time.google.com)",
 * "PRIV", "FILE". msg: one sentence. Both ASCII/UTF-8, no format string. */
void tk_log_write(const char *src, const char *msg);

/* Convenience wrappers that keep the call sites one line long. */
void tk_log_info(const char *msg);
void tk_log_src (const char *src, const char *msg);
void tk_log_err (const char *msg, int win32_err);

const wchar_t *tk_log_path(void);
#endif /* !TIMEKEEPER_NO_WINDOWS */

#endif /* TIMEKEEPER_LOG_H */
