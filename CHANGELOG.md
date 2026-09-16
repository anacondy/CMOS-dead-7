# Changelog

## 1.0.0 — 2026-09-15

First release. Everything below is in this repository and is what `build.bat` /
`make -f Makefile.mingw` produces.

### Added
- `TimeKeeper.exe`, a one-shot clock corrector for Windows 7 SP1 x86/x64 with a dead
  CMOS battery: three tiers (SNTP → HTTP `Date:` → offline companion file), then exit.
  77,312 / 71,680 bytes, no CRT, no heap, no service, no tray icon, no telemetry.
- Minimal SNTP client (`src/sntp.c`, `src/sntp_proto.c`): 48-byte RFC 4330 packet,
  stratum/leap/mode validation, Kiss-o'-Death refused, Originate-echo check, era-2036
  rollover handled, nearest-second rounding, 800 ms per server with a 3 s global
  deadline enforced by unsigned `GetTickCount` deltas.
- HTTP tier (`src/http_date.c`): WinINet `HEAD` honouring the system proxy (a corporate
  network is unreachable without it), plus a raw TCP/80 fallback to anycast IPs with no
  DNS and no proxy for captive-portal and dead-resolver cases. Redirects and non-2xx
  refused, so a login page's own clock can never win.
- `TimeKeeper.dat` companion file, read tolerantly (BOM, CRLF, comments, leading
  garbage, single-digit month/day) and validated strictly (real calendar, year window
  `[2005, 2040]`, FILETIME range). Atomic update
  (`tmp` → `FlushFileBuffers` → `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)`).
- Self-updating fallback: after a *successful network* sync the `.dat` moves to the
  first of the current month, so offline boots stay fresh with no repo push. Refuses to
  write on a failed sync, to the same month, or over a file that is ahead of today.
- Policy extracted into pure functions (`src/verdict.c`, `src/*_proto.c`) so that the
  calendar, both parsers and every decision are unit-testable on Linux with zero stubs;
  `src/selftest.c` is compiled both into the tests and into the .exe behind `/test`.
- `tests/pe_audit.py`: PE parser that checks subsystem, OS-version fields, NX/ASLR, the
  import list against an allow-list, banned post-Win7 symbols, embedded manifest and
  VERSIONINFO, and size — so the constraints are properties of the artifact.
- `tests/acceptance.sh` + `tests/sntp_responder.py` + `tests/http_date_server.py`: the
  acceptance matrix run against the real binaries under Wine, with deterministic local
  fixtures (fixed epoch, Kiss-o'-Death, reflection, black hole, redirect, no-Date, hang).
- `tests/win7-vm-tests.md`: the 9-section manual checklist for things Wine cannot
  prove — group policy, UAC filtering, real CMOS, AV interference, 100-boot soak.
- Install/uninstall: `install.cmd`, `uninstall.cmd`, and the same logic built in as
  `/install` `/uninstall`. Two `schtasks` tasks (`onstart` + `onlogon`, both
  `/ru SYSTEM /rl HIGHEST`), no service, and `Sysnative` handling so a 32-bit binary
  registers the task in the 64-bit task store rather than the WOW64 one.
- `UTF-16LE` log with a 16 KiB cap and atomic rotation, to `%ProgramData%\TimeKeeper\`,
  falling back beside the .exe and then `%TEMP%` based on an actual append probe.
- `requireAdministrator` manifest, `VERSIONINFO` from the same `config.h`,
  DPI awareness and the Win7+ `supportedOS` GUIDs.
- GitHub Actions: `ci.yml` (unit tests, both cross-builds, PE audit, size budget, Wine
  smoke) and `bump-fallback.yml` (monthly: bump `TK_FALLBACK_DATE` + `TimeKeeper.dat`,
  refuse backwards moves, re-run tests, push or open a PR).

### Decisions taken where the brief was ambiguous
- Always `SetSystemTime` in UTC, never `SetLocalTime`; the timezone is never modified.
  The fallback instant is `00:00:00 UTC` (= 05:30 IST) so the calendar day the file
  names is the day the user sees, immune to ±12 h error.
- Offline fallback is exit code **0**, not an error: a dead battery with no network at
  boot is the normal case, and a non-zero code would read as a failing startup.
- The "already correct" fast path is date-granular when the best offline reference is
  only date-granular (see `ARCHITECTURE.md` V1), instant-granular when it is not.
- No self-modifying binary, by design: the `.dat` sidecar achieves the same freshness
  without tripping SmartScreen, and a failed self-patch is unrecoverable.
- No monthly scheduled task for the fallback refresh: the program ages its own `.dat`
  after any successful sync, so the task would be redundant boot cost.
- `user32.dll` imported for two `MessageBox*` calls (the spec requires the dialog for
  the interactive failure path); `msvcrt.dll` removed entirely by implementing the
  seven C primitives in `src/tklibc.c`.
