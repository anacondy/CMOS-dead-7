# Testing TimeKeeper

Three layers, because the interesting risks are of three different kinds.

| layer | what it proves | where it runs | cost |
|---|---|---|---|
| **unit** | the calendar, both parsers, the SNTP codec, and every policy decision | native Linux/macOS, no OS stubs | ~2 s |
| **artifact** | that the linked PE *is* what we claim: subsystem, OS version, import list, embedded manifest, size | `tests/pe_audit.py` on the built .exe | ~1 s |
| **behavioural** | that the real binary resolves, sends, parses, decides, writes the log, and moves the clock | Wine + local SNTP/HTTP fixtures | ~4 min |

## 1. Unit layer

```sh
make -f Makefile.mingw test        # ASan + UBSan, -Wall -Wextra -Wshadow
```

`tests/host_test.c` compiles `src/timeutil.c`, `src/verdict.c`, `src/selftest.c`,
`src/sntp_proto.c`, `src/fallback_proto.c`, `src/tklibc.c` **unmodified** — the same
objects that go into the .exe — and runs the suite in `src/selftest.c`. Then it adds
the one check only a host build can do: it compares our calendar against libc.

```
== TimeKeeper suite (host build of the shipped sources)
== cross-check against libc
calendar vs libc timegm(): 182621 days, 0 mismatches

PASSED (0 report lines)
```

*182,621 days* = every day from 1601-01-01 to 2100-12-31, checked in **both**
directions against `timegm()`/`gmtime()`, plus `tm_wday` for the weekday. That is what
caught the one genuinely subtle bug in this project: an `era = (y-1)/400` shortcut in
`days_from_civil` is wrong exactly for years divisible by 400 (2000-03-01 came out 366
days early), and only a round-trip against an independent implementation over
182 k days finds that reliably.

The suite is 8,839 assertions covering:

- **parsers** — 22 `TimeKeeper.dat` bodies (BOM, CRLF, `#` comments, `2026-9-1`,
  binary noise with embedded NULs, Feb 29 on leap/non-leap years, month 13, out-of-window
  years, every truncation of a valid date) and 18 HTTP `Date` values (RFC 1123, RFC 850,
  2-digit year, missing seconds, `+0530` folded to UTC, weekday deliberately lying,
  `Feb 31`, hour 25, every truncation of a real response).
- **round-trip fuzzing** — every prefix length of a valid response must either fail or
  return the one true instant. That is what proves the parser cannot be talked into a
  plausible-but-wrong time by a truncated header.
- **FILETIME bounds** — 1970 must be `0x019DB1DED53E8000`; below/above the expressible
  range, and `ms == 1000`, must all be rejected rather than saturating.
- **NTP timestamp math** — era-rollover pivot (below 2000-01-01 in era 0 → assume era 1),
  sub-second milliseconds computed exactly (`frac*1000>>32`; dividing by `2^32/1000`
  rounded `0x7FFFFFFF` up to 500 ms and flipped the rounding decision), zero-timestamp
  rejection.
- **SNTP reply validation** — a synthetic valid reply, then each rejection path
  individually: short packet, wrong version, mode ≠ 4/5, stratum 0 (Kiss-o'-Death),
  stratum 16, LI = 3, zero transmit timestamp, missing Originate echo, out-of-range year.
- **policy** — the skip/sync verdict table, the offline-fallback gate, and the `.dat`
  self-update gate, including its four "do nothing" branches.
- **formatters** — `tk_fmt_*` at `cap = 0..23` with a canary byte just past `cap`,
  asserting nothing is ever written out of bounds and every string is terminated.

### Sharing the suite with the .exe

`src/selftest.c` is compiled twice: into the test binary, and into `TimeKeeper.exe`,
where it backs `/test`. Same file, same vectors header, so a build cannot pass CI and
then fail on a customer's machine with the identical bug. The product build adds
`-DTK_SELFTEST_COMPACT`, which swaps the report formatter for one that emits
`FAIL@<line>` instead of building prose — the vectors stay identical, the ~5 KB of
human-readable strings stay out of the shipped binary.

`TimeKeeper.exe /test` exit status is the verdict: 0 = pass, 1 = fail, and each failure
is also appended to the log file, so `/test` works over a reboot with no console.

## 2. Artifact layer

```sh
python3 tests/pe_audit.py bin/TimeKeeper32.exe bin/TimeKeeper64.exe \
        --require-manifest --require-versioninfo --max-size 153600 --show-imports
```

A 200-line stdlib-only PE parser (no `pefile`, no `objdump` output scraping) that reads
the facts we promised out of the file: `SizeOfImage` = 98,304 bytes.

```
machine            : 0x014C x86                     machine            : 0x8664 x64
subsystem          : 2  WINDOWS (no console flash)  OS version field   : 6.01
DllCharacteristics : NX_COMPAT DYNAMICBASE/ASLR     resources          : VERSION, MANIFEST
imports: ADVAPI32(4) KERNEL32(51) USER32(2) WININET(7) WS2_32(18)
AUDIT: OK
```

It also fails the build on any symbol that does not exist on Windows 7 SP1
(`GetSystemTimePreciseAsFileTime`, `CreateFile2`, `GetTickCount64`, …) — the
"works on my machine, dies at load on theirs" class of error, which is otherwise only
discovered on the machine you are trying to rescue. That list of banned names is the
reason `winutil.c` uses `GetTickCount` + wrap-safe unsigned subtraction rather than
`GetTickCount64`, despite the latter being available from Vista on.

**Why `user32.dll` is in the allow-list.** The specification's list is
kernel32/advapi32/ws2_32/wininet, and `MessageBoxW` (the interactive failure path the
specification also asks for) lives in user32. `pe_audit.py` documents both additions and
rejects anything else; there is no way to have the required dialog without the DLL, so
the deviation is recorded here rather than hidden. `msvcrt.dll` was also originally
imported, for exactly seven pure functions — that one was *fixable*, so it was fixed:
`src/tklibc.c` implements them, and the audit now rejects `msvcrt`/`ucrt`/`vcruntime` if
they ever reappear.

## 3. Behavioural layer

```sh
tests/acceptance.sh                     # user-level, no clock changes
sudo tests/acceptance.sh                 # adds "the clock actually moved" with real SetSystemTime
```

Design decisions in the harness, all of them learned the hard way:

- **Never depend on the public pool.** A second binary is compiled with
  `-DTK_TEST_NTP_ONLY='"127.0.0.1"' -DTK_TEST_NTP_PORT=12300`, and
  `tests/sntp_responder.py` answers on a high port (a test rig that needs root to bind
  :123 silently degrades into "the responder never started", which is how three negative
  cases once passed for exactly the wrong reason). Same for HTTP:
  `-DTK_TEST_HTTP_HOST`/`_PORT` point at `tests/http_date_server.py`.
- **Serve an absolute epoch.** When the test has already set the clock to 2009, a
  responder that serves `now + 31 days` computes `now` from the doctored clock and the
  test agrees with itself. `--epoch` is decided *before* the clock is touched.
- **A negative case must prove the program ran.** Every "refused" assertion is preceded
  by a check that the binary produced a log line at all; otherwise a missing executable
  reads as a passing security test.
- **The clock is restored on `EXIT` and re-verified against NTP**, and the privileged
  prefix lives on disk, not in `/tmp` (a Wine prefix is ~1 GB and `/tmp` is a tmpfs).

### Recorded run (Debian 13, Wine 10.0, MinGW-w64 gcc 14)

```
49 passed, 0 failed, 1 skipped
  PASS  host unit tests (ASan+UBSan) and the libc calendar cross-check
  PASS  PE audit: subsystem WINDOWS, OS 6.01, manifest+VERSIONINFO present, imports within allow-list
  PASS  TimeKeeper32.exe = 77 312 bytes (< 81 920 target)      TimeKeeper64.exe = 71 680 bytes
  PASS  /test (embedded suite) passes in the shipped binary
  PASS  test 5: app-reported duration 2 ms (< 500 ms budget)
  PASS  SNTP reply from the loopback server accepted, parsed and applied
  PASS  --kod: rejected (KISS-OF-DEATH)   --reflect: rejected (LOOPED-REQUEST)
  --drop: rejected (no reply)             year ceiling: rejected (OUT-OF-RANGE)
  PASS  test 4 (wininet): Date header accepted, target instant correct
  PASS  test 4 (raw): no-DNS transport used and accepted
  PASS  redirect / nodoc / bogus fixture responses all refused
  PASS  hanging server bounded by the app's own timeout
  PASS  malformed .dat handled cleanly (7 payloads incl. a 5 KB base64 blob)
  PASS  read-only install directory: exits cleanly, no crash, .dat intact
  PASS  log at %ProgramData%\TimeKeeper, UTF-16 BOM, <= 16 384 bytes
  PASS  rotation preserves the newest 8 KiB: a marker planted 4 KiB before the end of an
        oversized log survives the trim. Size alone cannot tell rotation from truncation,
        so the marker is the check that matters.
  PASS  test 1: offline fallback moved the clock to 2026-09-01 00:00:00 with no network (exit 0, 850 ms)
  PASS  test 2: SNTP sync MOVED the clock 2009-01-01 -> 2026-10-16 (exit 0, 495 ms)
  PASS  test 2: TimeKeeper.dat self-updated to 2026-10-01
  PASS  test 3: repeat sync in the same month left TimeKeeper.dat untouched
```

Against the specification's acceptance list:

| # | required | result | how it is established |
|---|---|---|---|
| 1 | 2009 clock, no network → 2026-09-01 | **PASS** | Wine, real `SetSystemTime`, clock read back |
| 2 | 2009 clock, network → real time < 2 s; `.dat` bumped | **PASS** | loopback responder at a known epoch; 495 ms wall, `.dat` → 2026-10-01 |
| 3 | run twice in a month → `.dat` not rewritten | **PASS** | mtime + size identical, log says `NOOP(idempotent)` |
| 4 | UDP blocked → HTTP fallback still syncs | **PASS** | both transports, against a local fixture |
| 5 | correct clock → exit < 500 ms, no change | **PASS** | `done in 2 ms` from the app's own log line |
| 6 | non-admin → graceful failure, no crash, log written | **PASS\*** | Wine can't drop to a real limited token, so this is proven by code path + the read-only/unwritable-directory and `1314` cases; the true non-admin check is in `tests/win7-vm-tests.md` §6 |
| 7 | both < 150 KB, RAM < 3 MB | **PASS** | 77 312 / 71 680 bytes; `SizeOfImage` 98 304 with **zero** heap allocations (see below) |

### What Wine cannot prove, and what covers it instead

Wine is a reimplementation, not Windows, so two things are out of its reach and are
handled differently:

- **`SetSystemTime` semantics.** Wine denies `SeSystemtimePrivilege` to non-root
  tokens, which produced `FAILED Win32 1314` on one run. That turned into a *better*
  test: the program detected the denial, logged the exact API error, exited 2 with
  "run via the SYSTEM task", and did not corrupt anything. The successful-set path runs
  as root under Wine, and on a real Win7 VM per `tests/win7-vm-tests.md`.
- **Group policy, UAC filtering, AV interference, and a genuinely dead CMOS.** Only a
  real machine has them. `tests/win7-vm-tests.md` is the checklist for that pass,
  including the two registry-backed privilege scenarios.

## 4. Memory: how the < 3 MB budget is met

Not by measurement alone — by construction, which is what makes it hold on a
256 MB machine:

- **zero heap.** No `malloc`, no `new`, no `_strdup`, no `LocalAlloc` on any
  success path. `LocalAlloc` appears twice, both on paths that only run for a human
  reading an error (`FormatMessageW` buffer, token-user query), and both are freed.
- **all buffers static or stack**, sized at compile time: the largest are an 8 KB log
  rotation tail and a 2 KB argv ring, both `.bss`/stack.
- **one thread, no CRT start-up machinery**, no locale tables, no stdio streams.
- `SizeOfImage` is **98,304 bytes** for x86 (read from the PE by the audit, so the
  claim is checked, not asserted); add a 64 KB stack reservation and you are at ~160 KB
  of private bytes against a 3 MB budget.

Verification on a real box: Task Manager (add the "Memory (private working set)"
column) or `VMMap`/`pslist` during a run, and `TimeKeeper.exe /dry-run` from a console.
The program exits in under a second, so for a manual reading use `/test`, which runs the
same code paths without changing anything.

## 5. Continuous integration

`.github/workflows/ci.yml` runs on every push and PR: unit tests with sanitizers, both
cross-builds, the PE audit as a hard gate, the size budget as a hard gate, and a Wine
job that executes `TimeKeeper32.exe /test` and `/dry-run` and prints the log the binary
wrote. `.github/workflows/bump-fallback.yml` re-runs the same tests on the *bumped*
`config.h` before committing, so the monthly automation cannot land a build that fails
its own parsers.

## 6. Running the tests by hand on Windows

```bat
build.bat test                          :: unit layer, needs gcc on PATH
bin\TimeKeeper32.exe /test              :: the suite inside the shipped binary
bin\TimeKeeper32.exe /dry-run /debug    :: full pipeline, nothing changed, output on screen
python tests\sntp_responder.py --port 12300 --epoch 1789600000
python tests\http_date_server.py --port 8080 --mode ok
```

Then, to see the decision for a specific clock without waiting for a reboot:

```bat
bin\TimeKeeper32.exe /dry-run /force /debug
type "%ALLUSERSPROFILE%\TimeKeeper\timekeeper.log"
```
