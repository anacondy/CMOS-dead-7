#!/usr/bin/env python3
"""inflate_log.py — push a TimeKeeper log past its size cap, with a witness.

    python3 tests/inflate_log.py <log> [--marker TEXT]

`--marker` is written `--tail` filler lines before the end of the padded content
(36 lines ~= 4 KiB by default), i.e. inside the newest 8 KiB that a correct
rotation must keep. Grepping for that marker after the next run is what separates
"rotated" from "truncated": a truncate-based fallback also keeps the file small,
and a test that only checks the size cannot tell the two apart and will happily
pass on a log-pipeline bug.

Placement matters and used to be the thing that was wrong here. Rotation keeps
the newest TK_LOG_KEEP_BYTES (8 KiB) *as whole lines*: a window that ends mid-line
loses the unfinished line, and content already past the 8 KiB window is gone by
design. So the witness must sit inside that window but not so close to the end
that one more appended line pushes the oldest kept line out. ~4 KiB from the end
is the middle of the window and is stable under either rule; a marker at 8.5 KiB
from the end "fails" against a correct implementation. The size assertion alone
does not catch a rotation that chews the tail down, which is why both checks are
kept and why the same arithmetic is unit-tested in src/selftest.c (t_rotate).

The file is UTF-16LE with a BOM, matching what src/log.c writes; the filler uses
the same "[timestamp] [TAG] text" shape so the reader sees a realistic file.

SPDX-License-Identifier: MIT
"""
import argparse
import os
import sys

UNIT = "[2026-09-15 00:00:00.000Z] [INFO] filler line used to force rotation\r\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--marker", default="")
    ap.add_argument("--over", type=int, default=400, help="filler lines beyond the cap")
    ap.add_argument("--tail", type=int, default=36,
                    help="filler lines after the marker (~112 B each; 36 = ~4 KiB, "
                         "the middle of the 8 KiB window rotation preserves)")
    a = ap.parse_args()

    data = b""
    if os.path.exists(a.log):
        with open(a.log, "rb") as f:
            data = f.read()
    if not data.startswith(b"\xff\xfe"):
        data = b"\xff\xfe" + data

    unit = UNIT.encode("utf-16-le")
    blob = data + unit * a.over
    if a.marker:
        blob += ("[2026-09-15 00:00:00.000Z] [INFO] MARKER %s\r\n" % a.marker).encode("utf-16-le")
    blob += unit * a.tail

    with open(a.log, "wb") as f:
        f.write(blob)
    print("inflated %s to %d bytes%s" % (
        a.log, len(blob), " (marker %d bytes from the end)" % (len(unit) * a.tail + 2)
        if a.marker else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
