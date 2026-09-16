# TimeKeeper

**A 73 KB, single-file, zero-dependency Windows utility that fixes the clock on a PC with a dead CMOS battery — at boot, once, then it exits.**

For the machine that comes up showing `2009-01-01` every morning: without network it sets a sane recent date from a companion file; with network it gets the real time from SNTP (or an HTTP `Date:` header when UDP/123 is blocked), and then *ages its own fallback forward* so the next offline boot is also correct. No service, no tray icon, no .NET, no scheduled reboot, no telemetry.

Target: **Windows 7 SP1 x86/x64**, and it also runs on anything newer. Two executables, both under 80 KB.

## Downloads

The verified Windows 7 binaries are published as assets of the public
[TimeKeeper v1.0.0 release](https://github.com/anacondy/CMOS-dead-7/releases/tag/v1.0.0):

- [TimeKeeper32.exe (x86)](https://github.com/anacondy/CMOS-dead-7/releases/download/v1.0.0/TimeKeeper32.exe)
- [TimeKeeper64.exe (x64)](https://github.com/anacondy/CMOS-dead-7/releases/download/v1.0.0/TimeKeeper64.exe)

The release assets are the audited, final executables; they are intentionally not
tracked in the source tree. SHA-256 checksums are recorded in the release notes.

---

## 1. The problem this solves

A CR2032 on a desktop motherboard dies after 5–10 years. When it does, the BIOS forgets the time and the system clock returns to the firmware default — commonly `2009-01-01` or `2015-01-01`. On Windows that means:

* **every HTTPS/TLS site fails** — "the certificate is not yet valid" — so you cannot even load the page that explains how to fix the clock;
* Windows Update, certificate revocation, and S/MIME/PGP all fail;
* file timestamps, backups, `make`, antivirus signature freshness and any log correlation are wrong;
* `w32time` cannot help, because its initial sync needs working DNS *and* outbound UDP/123 *and* it gives up after a few attempts, and on a machine with a wrong clock it can also refuse to step a > 5-minute offset without manual intervention.

The fix is a 50-rupee battery. Until someone opens the case, this program makes the machine usable.

I verified this failure mode the hard way while building TimeKeeper: a sandbox whose clock had been set to 2009 could not complete a single HTTPS request (`SSL certificate problem: certificate is not yet valid`), and the only way out was to ask an NTP server for the time and set it — which is precisely what TimeKeeper does at every boot.

## 2. How it works

```
                        ┌──────────────────────────────────────────────┐
   boot / logon ───────►│ TimeKeeper.exe  (SYSTEM, one-shot, no threads)│
                        └───────────────┬──────────────────────────────┘
                                        │
                   ┌────────────────────▼─────────────────────┐
                   │ Is the clock already within 5 min of the  │──yes──► log, exit 0
                   │ best offline reference? (1–2 ms, no I/O)  │         ("don't thrash")
                   └────────────────────┬─────────────────────┘
                                        │ no
        TIER 1  ────────────────────────▼──────────────────────────────────
        SNTP     0.pool.ntp.org → 1.pool.ntp.org → time.google.com →
                 time.cloudflare.com → time.windows.com
                 48-byte RFC 4330 packet, UDP/123, 800 ms per server,
                 3 s for the whole phase, first valid answer wins
                                        │ ok ─────────────────────────────┐
                                        │ all failed                      │
        TIER 2  ────────────────────────▼───────────────────┐            │
        HTTP    WinINet HEAD www.google.com (proxy-aware)    │            │
                → www.cloudflare.com/cdn-cgi/trace           │            │
                → raw TCP GET to 142.250.74.4 / 104.16.132.229│           │
                  (no DNS, no proxy: captive-portal safe)    │            │
                parse the Date: header, 1.5 s bound          │            │
                                        │ ok                              │
                                        ▼                                 ▼
                   ┌────────────────────────────────────────────────────────┐
                   │ SetSystemTime(UTC)   (never SetLocalTime)              │
                   └────────────────────┬───────────────────────────────────┘
                                        │ both tiers failed
        TIER 3  ────────────────────────▼────────────────────────────────────
        OFFLINE  read TimeKeeper.dat  ("2026-09-01", plaintext, editable)
                 apply ONLY if the clock is implausibly old; never move a
                 working clock backwards
                                        │
                                        ▼
                   ┌────────────────────────────────────────────────────────┐
                   │ SELF-UPDATE: after a *network* sync, if the month has  │
                   │ advanced, atomically rewrite TimeKeeper.dat to          │
                   │ YYYY-MM-01.  ← this is what keeps offline boots fresh   │
                   └────────────────────────────────────────────────────────┘
```

Three tiers, one exit. Total budget: **< 1.5 s when a time source answers, < 3.5 s when nothing does.** Measured in-process (from the program's own log line): 1–2 ms on the skip path, ~80 ms for a successful NTP sync, ~800 ms for a fully-offline run.

## 3. Fallback logic, precisely

Two things are called "fallback", and they are different:

| | what it is | when it is used | who updates it |
|---|---|---|---|
| **`TimeKeeper.dat`** | plaintext `YYYY-MM-DD` next to the .exe | offline boot, when the clock is implausibly old | **the program itself**, after every successful network sync |
| `FALLBACK_DATE` in `src/config.h` | compile-time constant | only when `TimeKeeper.dat` is missing or unparseable | the GitHub Action, monthly |

The date is applied as **`YYYY-MM-DD 00:00:00 UTC`**, which is **05:30 IST** — deliberate, so the calendar day the file names is the day the user sees, and a ±12 h error can never shift it. IST is UTC+05:30 with no DST, so there is no rule to get wrong, and the program never touches the timezone setting.

The gate that protects a working clock (`src/verdict.c`, pure and unit-tested):

```c
REF = max(build date, contents of TimeKeeper.dat, mtime of TimeKeeper.dat)
                                          /* a lower bound on "today", never a guess at it */

clock within 5 min of REF and REF is fresh            ->  do nothing, exit 0
clock older than REF                                   ->  sync (the clock is provably wrong)
clock newer than REF by > 45 days                     ->  sync; if the network fails,
                                                            DO NOT move it back
network failed and TimeKeeper.dat is newer than clock ->  apply the fallback
TimeKeeper.dat is older than the clock                 ->  refuse, log why, exit 0
```

The rule that matters most: **TimeKeeper never sets the clock backwards** unless you pass `/force`. A stale `.dat` can therefore never undo a clock that has since been corrected — the failure mode that would make a tool like this worse than no tool.

## 4. Build

### MinGW-w64 (recommended; this is what produces the < 80 KB binaries)

```sh
# Debian/Ubuntu
sudo apt-get install gcc-mingw-w64-i686 gcc-mingw-w64-x86-64 \
                     binutils-mingw-w64-i686 binutils-mingw-w64-x86-64
make -f Makefile.mingw            # both arches -> bin/TimeKeeper32.exe, TimeKeeper64.exe
make -f Makefile.mingw test       # host unit tests (needs only a native cc)
make -f Makefile.mingw check      # tests + build + PE audit + size budget
```

On Windows from an MSYS2 shell: `pacman -S mingw-w64-i686-toolchain mingw-w64-x86_64-toolchain` then the same `make` lines. `build.bat` does it without make.

The size recipe is `-Os -fno-unwind-tables -ffunction-sections -fdata-sections -Wl,--gc-sections -static -nostartfiles` plus `-DTK_SELFTEST_COMPACT`, i.e. the CRT's start-up objects and its locale/stdio machinery are dropped (see `src/crt_start.c`) and the seven C primitives actually used are implemented in `src/tklibc.c`. Result for the audited v1.0.0 release assets:

```
bin/TimeKeeper32.exe   77 312 bytes      bin/TimeKeeper64.exe   71 680 bytes
```

Import table of both files, read back out of the linked PE by `tests/pe_audit.py`:

```
KERNEL32.dll  advapi32.dll  ws2_32.dll  WININET.dll  USER32.dll
```

No `msvcrt.dll`, no `vcruntime`, no `ucrt`, no `shell32`, no GDI, no comctl32. `user32.dll` is a deliberate, documented addition: two `MessageBox*` calls on the interactive failure path, which the specification asks for and which cannot be reached without it.

### CMake (MSVC or MinGW)

```sh
cmake -B build/x86 -A Win32 && cmake --build build/x86 --config Release
cmake -B build/x64 -A Win64 && cmake --build build/x64 --config Release
ctest --test-dir build/x64 -V            # unit tests
```

### MSVC by hand (`build.bat`, VS 2017/2019/2022 Build Tools)

```bat
build.bat            &REM auto-detects: MSVC first, else MinGW
build.bat x86        &REM one architecture
build.bat mingw      &REM force MinGW
build.bat test       &REM unit tests only
```

`/O1 /Os /GL /MT /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF` — with MSVC the CRT can't be dropped the way MinGW can, so an MSVC build lands nearer 120 KB. Still inside the 150 KB hard limit; use MinGW if the 80 KB target matters.

### Verify what you built

```sh
python3 tests/pe_audit.py bin/TimeKeeper32.exe --require-manifest --require-versioninfo --max-size 153600
wine bin/TimeKeeper32.exe /test          # the shipped binary runs its own suite
```

## 5. Install on Windows 7

Copy the folder to the machine. Then, from an **elevated** command prompt:

```bat
cd C:\timekeeper
install.cmd
```

`install.cmd` does exactly four things: picks the binary matching the OS bitness, copies it plus `TimeKeeper.dat` to `%ProgramFiles%\TimeKeeper`, registers two scheduled tasks, and runs the task once so the clock is fixed without a reboot.

```bat
schtasks /create /f /tn "TimeKeeper"      /tr "\"%ProgramFiles%\TimeKeeper\TimeKeeper.exe\"" /sc onstart /ru SYSTEM /rl HIGHEST /delay 0000:10
schtasks /create /f /tn "TimeKeeperLogon" /tr "\"%ProgramFiles%\TimeKeeper\TimeKeeper.exe\"" /sc onlogon /ru SYSTEM /rl HIGHEST
```

Why a task and not a service: a service needs a control protocol, a start-up dependency, and a resident process — roughly 30× the code and a permanent 2 MB of RAM for something that runs for 100 ms. `onstart` covers a cold boot; `onlogon` covers the machine that is never actually shut down and the case where the network is only up after the user logs in. `/RL HIGHEST` on a `/RU SYSTEM` task is belt-and-braces so the task never gets filtered to a limited token; it also means **no UAC prompt ever appears**, for any user.

`/delay 0000:10` is passed for Task Scheduler 2.x; 1.0 ignores it, which is harmless because a run with no network exits in under a second anyway.

A monthly `schtasks /sc monthly` refresh task is **not** needed and is deliberately not installed: TimeKeeper rewrites `TimeKeeper.dat` itself after every successful sync, so the fallback stays current from the machine's own traffic. The scheduled task's only job is to run the program at boot.

Uninstall:

```bat
uninstall.cmd            &REM removes the two tasks, keeps files and log
uninstall.cmd /purge     &REM also deletes the install directory and the log
```

Equivalent without any script: `TimeKeeper.exe /install` and `/uninstall`.

## 6. Command line

```
TimeKeeper.exe [/debug] [/dry-run] [/force] [/test] [/install] [/uninstall] [/quiet] [/version] [/help]
```

| flag | effect |
|---|---|
| `/debug` | attach the parent console (or allocate one) and mirror every log line to it. Without it the program shows no window at all |
| `/dry-run` | do everything — resolve, send, parse, decide — and log what *would* be written. **No** `SetSystemTime`, **no** `.dat` write, **no** file changes at all. Implies `/debug` |
| `/force` | ignore the "clock is already right" check; also the only mode allowed to move the clock backwards |
| `/test` | run the built-in self-test (parsers, calendar, NTP codec, policy) and exit. No network, no changes. Same vectors as the build's unit tests |
| `/install` | copy to `%ProgramFiles%\TimeKeeper` and register both tasks |
| `/uninstall` | delete both tasks |
| `/quiet` | log to the file only |
| `/version`, `/help` | print and exit 0 |

Exit codes: **0** = synced, or nothing to do, or a sane offline fallback; **1** = no usable time source, or `SetSystemTime` refused for a non-privilege reason; **2** = could not obtain `SeSystemtimePrivilege` (run it as the SYSTEM task, or elevated).

Options are accepted with `/`, `-` or `--` (`/dry-run`, `-dry-run`, `--dry-run`) because whoever types them at 2 a.m. shouldn't have to remember which convention this tool picked.

## 7. Log

`%ProgramData%\TimeKeeper\timekeeper.log`, falling back to beside the `.exe`, then `%TEMP%` — whichever is first *actually openable for append*, so a root-owned leftover from another account's run degrades instead of going silent. UTF-16LE with BOM (Notepad on Win7 reads it; ANSI would depend on the codepage), capped at 16 KiB, rotated by keeping the newest half, one line per event:

```
[2026-09-01 00:00:00.425Z] [INFO] start v1.0.0 at tick 19003922 verdict=SYNC clock=2009-01-01 12:00:00 ref=2026-09-01 00:00:00 dat=2026-09-01 00:00:00 tz=330min(IST)
[2026-09-01 00:00:00.500Z] [SRC:NTP(time.google.com)] reply in 78 ms
[2026-09-01 00:00:00.501Z] [SRC:NTP(time.google.com)] Set UTC 2026-08-31 18:30:00 -> 00:00:00 GMT+330 OK
[2026-09-01 00:00:00.501Z] [FILE] fallback .dat update: REWRITE(stale)
[2026-09-01 00:00:00.501Z] [INFO] done in 77 ms, exit 0
```

`done in N ms` is printed by the program itself, so the latency budget is checkable on a customer's machine from the log alone — no profiler, no stopwatch.

## 8. Security notes

**Why it needs SeSystemtimePrivilege / administrator.** `SetSystemTime` is a machine-wide, security-relevant operation: the OS checks the token for `SeSystemtimePrivilege` (Windows grants it to Administrators and SYSTEM by default). TimeKeeper enables it in its own token with `OpenProcessToken` + `LookupPrivilegeValueW` + `AdjustTokenPrivileges`, and — importantly — it *reads the result* rather than assuming: `ERROR_NOT_ALL_ASSIGNED` means the account doesn't have the right at all, which is the standard-user case, and it exits 2 with that explanation instead of silently doing nothing. Group policy can also strip the privilege after logon; that surfaces as `SetSystemTime` failing with 1314, which TimeKeeper maps to exit 2 as well, with a message saying to run it from the SYSTEM task.

**Why the manifest says `requireAdministrator`** even though the task runs as SYSTEM: with `asInvoker`, a manual double-click would run unelevated, fail to set the clock, and exit quietly — the worst possible UX for a troubleshooting tool. Elevation makes the manual path honest.

**What an attacker who can write `TimeKeeper.dat` gains.** Only a bounded boot-clock value: the year must be inside `[2005, 2040]` (config.h), it is applied only when the real clock is already implausibly old, and it never beats a network answer. That is far less than what writing to `%ProgramFiles%` would imply anyway, which is why the install directory is admin-writable only.

**SNTP is unauthenticated.** A man on the LAN can offset you by seconds, exactly as with stock `w32time`. TimeKeeper reduces the blast radius rather than pretending the threat is absent: mode/stratum/leap validation, reject on `Kiss-o'-Death`, reject if the reply is not answering *our* request (Originate echo check), reject a verbatim reflection, reject a local server that merely agrees with the clock we already have, and reject anything outside the year window. The HTTP tier is treated as a coarse second-precision hint and is sanity-clamped the same way; a 3xx is refused so a captive portal's own clock can't win.

**No telemetry, no phone-home, no inbound surface, no persistence beyond two scheduled tasks.** Every byte of network traffic is one 48-byte UDP datagram per NTP server and one `HEAD` per HTTP host, all to hosts listed in `src/config.h`.

## 9. The GitHub Action

Two freshness mechanisms, deliberately not the same one:

1. **Local self-update (primary).** Any successful SNTP/HTTP sync bumps `TimeKeeper.dat` to the first of the current month, atomically (`TimeKeeper.dat.tmp` → `FlushFileBuffers` → `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)`). No GitHub, no push, works on an air-gapped LAN that can reach one NTP server.
2. **`.github/workflows/bump-fallback.yml` (secondary, for distribution).** Runs `0 0 1 * *` and on demand. It rewrites `TK_FALLBACK_DATE` (and the numeric `TK_FALLBACK_YEAR/MONTH/DAY`) in `src/config.h` plus `TimeKeeper.dat` to the current month, then **refuses to move the date backwards**, re-runs the unit tests and a real cross-compile, and either pushes to the default branch or — if that branch is protected — pushes a branch and opens a pull request. So a clone from 2028 doesn't boot into a 2026 fallback.

`ci.yml` runs the same suite on every push: host unit tests with ASan/UBSan, both cross- builds, the PE audit (subsystem, OS version, import allow-list, embedded manifest), the size budget, and `/test` inside the Windows binaries under Wine.

## 10. Troubleshooting

| symptom | cause and fix |
|---|---|
| Log says `PRIV … not elevated`, exit 2 | Run from the scheduled task, or from an elevated prompt. Check `secpol.msc` → Local Policies → User Rights Assignment → "Change the system time" |
| `SNTP unavailable (ZERO-TIMESTAMP …)` and then HTTP works | Normal on Indian ISP/hostel networks: outbound UDP/123 is silently dropped. Tier 2 exists for exactly this. If you want NTP to work, ask the network admin to allow UDP/123 to `time.google.com`, or use `/test` to confirm nothing else is wrong |
| Everything fails offline and the clock is still 2009 | `TimeKeeper.dat` is missing or unparseable and the embedded date is older than the machine's own clock bound. Write the date yourself: `echo 2026-09-01> "%ProgramFiles%\TimeKeeper\TimeKeeper.dat"` |
| Runs slowly at boot, 5–15 s | `getaddrinfo` has no timeout in Winsock: a *configured but dead* DNS server stalls the resolve, then the deadline check takes the offline branch. Mitigations: fix the DNS, or add the server names to `%SystemRoot%\System32\drivers\etc\hosts` (Winsock consults that file first, so resolution becomes instant). The raw-socket HTTP path uses no DNS at all, so the time is still recovered |
| Clock is right but 1–2 seconds slow | By design: SNTP gives second precision, one-shot, with nearest-second rounding. There is no PLL/frequency discipline — for a dead-CMOS box, sub-second accuracy is not the problem. For continuous accuracy, fix the battery and let `w32time` run |
| The date is right, the time is 11 hours off | Someone ran with the wrong timezone, or `/force` back-dated it. Set the zone once in Control Panel; TimeKeeper never changes the timezone, only the UTC instant |
| Nothing in the log | You're looking in the wrong place: it tries `%ProgramData%\TimeKeeper\` first, then next to the `.exe`, then `%TEMP%` — and says which one it used in the run's own line if you add `/debug` |
| Antivirus flags the .exe | Unsigned, `requireAdministrator`, and it calls `SetSystemTime` — a heuristic combo AVs dislike. The build is 14 small C files and the CI publishes deterministic artifacts; sign it with your own cert if policy requires. Self-updating *the .dat* is not self-updating the .exe, deliberately: patching one's own image trips SmartScreen and is unrecoverable if it goes wrong |
| `w32time` and TimeKeeper fighting | They don't: `w32time` refuses large offsets without `/once`, and TimeKeeper exits early whenever the clock is already sane. If you want only one of them, disable the other service or the task — not both |

## 11. Repository layout

```
src/
  main.c            CLI, state machine, self-update, install/uninstall
  config.h          every tunable; the one file the Action edits
  timeutil.{c,h}    calendar, FILETIME/NTP/Unix, date+HTTP-date parsers   pure
  verdict.{c,h}     every branch decision, as a pure function              pure
  sntp.c            UDP/123 client, deadline-bounded
  sntp_proto.{c,h}  RFC 4330 packet build/validate                         pure
  http_date.c       WinINet HEAD + raw TCP fallback
  fallback.c        TimeKeeper.dat I/O, atomic writes
  fallback_proto.c  .dat format + the update rule                          pure
  privilege.c       SeSystemtimePrivilege
  log.c             UTF-16 log, 16 KiB cap, console mirror
  winutil.c         the Win32 primitives, in one auditable place
  install.c         copy + schtasks
  tklibc.c          the 7 libc primitives, so no CRT DLL is imported
  crt_start.c       CRT-free entry point
  selftest.c        the test suite (also backs `TimeKeeper.exe /test`)
  tklibc.c          the 7 libc primitives, so no CRT DLL is imported
  crt_start.c       CRT-free entry point
  version.rc app.manifest
tests/
  host_test.c        native runner: the shared suite + the libc cross-check
  vectors.h          the one test corpus, shared with `TimeKeeper.exe /test`
  pe_audit.py        reads subsystem/imports/manifest/size straight out of the PE
  sntp_responder.py  deterministic SNTP server (kod / reflect / drop / fixed epoch)
  http_date_server.py deterministic HTTP Date fixture (ok/redirect/nodoc/bogus/hang)
  acceptance.sh      runs the real .exe under Wine against those fixtures
  win7-vm-tests.md   the checklist only a real Windows 7 box can answer
ARCHITECTURE.md · TESTING.md · install.cmd · uninstall.cmd · build.bat · Makefile.mingw · CMakeLists.txt
```

The three `*_proto.c` files and `verdict.c`/`timeutil.c` contain all the arithmetic and all of the policy, and they compile and run on Linux with no stubs — which is how 8,800 assertions can be checked on every commit while the shipped `.exe` runs the same code.

## 12. Licence

MIT — see `LICENSE`. Version 1.0.0.
