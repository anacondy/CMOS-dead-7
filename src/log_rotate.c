/* log_rotate.c — pure half of the log rotation: which bytes survive a trim.
 *
 * The I/O lives in log.c (CreateFileW / ReadFile / MoveFileExW). The *decision*
 * lives here, because the decision is the only part that can be wrong in a way
 * nobody notices: get it wrong and the log stays small (looks healthy) while
 * quietly losing everything, or it grows without bound on a 30 GB system drive.
 *
 * Contract: given the newest `n` bytes of an over-cap UTF-16LE log, return the
 * LONGEST SUFFIX of that window that consists of whole lines. In this log a line
 * ends with the four bytes 0D 00 0A 00 (CRLF in UTF-16LE) at an even offset, so
 * "whole lines" means "everything after the last such terminator".
 *
 * Why the suffix and not "everything after the first terminator": scanning
 * forward and cutting there drops the leading line every single time, even when
 * the window happened to begin exactly on a line boundary (the common case,
 * since TK_LOG_KEEP_BYTES and the line length are both even). That is lossy on
 * every rotation, and it is not idempotent: trim the result again and it loses
 * another line. A rotation invoked after each append would then chew the
 * retained tail down to nothing. Scanning from the back fixes both, and costs
 * the same O(n).
 *
 * A returned length of 0 means "nothing recoverable" (no terminator at all, or
 * only a stub), and the caller in log.c falls back to truncating in place — still
 * bounded, and honest about being lossy.
 *
 * SPDX-License-Identifier: MIT
 */
#include "log_rotate.h"

size_t tk_log_trim_tail(unsigned char *buf, size_t n, size_t *dropped_out)
{
    size_t last = 0;          /* 0 = no complete line found          */
    size_t i;

    /* A trailing odd byte is half a UTF-16 unit; keeping it would write a stray
     * lead byte that Notepad renders as mojibake. Exclude it from the search. */
    if (n & 1u) n--;

    /* Even offsets only (a UTF-16 code unit is two bytes), and the search stops
     * at i = 4 so a terminator can never be the whole of a "kept" line. */
    for (i = n; i >= 8; i -= 2) {
        size_t j = i - 4;
        if (buf[j] == '\r' && buf[j + 1] == 0x00 &&
            buf[j + 2] == '\n' && buf[j + 3] == 0x00) {
            last = i;
            break;
        }
    }

    /* Floor: below one short line the "log" would be a fragment of a fragment.
     * Rewriting the file to store that is not worth a MoveFileExW. */
    if (last == 0 || last < 32) {
        if (dropped_out) *dropped_out = n;
        return 0;
    }
    if (dropped_out) *dropped_out = n - last;
    return last;
}
