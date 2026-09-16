/* log_rotate.h — size-bounded tail trimming for the run log.
 *
 * Pure C: no windows.h, no allocation, no I/O. See log_rotate.c for the
 * contract, and note the documented line-fragment drop before writing a test
 * that asserts "content at offset X survives rotation".
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TIMEKEEPER_LOG_ROTATE_H
#define TIMEKEEPER_LOG_ROTATE_H

#include <stddef.h>

/* Scans `buf` (the newest `n` bytes of a UTF-16LE log, BOM already stripped)
 * backwards for the last CRLF at an even offset. `buf` is read, never written:
 * the caller decides whether the prefix it no longer wants is worth compacting
 * away, and in the shipped path it is not (the kept bytes are simply written out
 * to a fresh file).
 *
 * Returns the length of the longest prefix of `buf` that ends on a whole-line
 * boundary — that is, everything up to and including the last terminator. 0 means
 * "nothing recoverable" and the caller may truncate. `dropped_out`, if non-NULL,
 * receives how many bytes at the *end* are not covered (an incomplete trailing
 * line, or the whole window when nothing is recoverable).
 *
 * What "keeps the newest 8 KiB" means in practice: the caller hands over the last
 * 8 KiB of the file, and this returns everything up to the last complete line in
 * it. Because every line the program writes ends with CRLF, the window ends on a
 * terminator and the whole window is kept. A window whose only terminator sits at
 * offset 4 (nothing but a fragment ahead of it) is refused outright, and a window
 * with no terminator at all keeps nothing; both are the truncation path.
 *
 * `buf` is treated as raw bytes; the caller sizes it. */
size_t tk_log_trim_tail(unsigned char *buf, size_t n, size_t *dropped_out);

#endif /* TIMEKEEPER_LOG_ROTATE_H */
