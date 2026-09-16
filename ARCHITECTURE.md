# TimeKeeper — Architecture (v1.0.0)

One-shot, zero-dependency Windows time correction for machines with a dead CMOS battery.
Target: Windows 7 SP1 x86/x64. No service, no tray icon, no threads, no heap growth.

## 1. Components

```
                        ┌──────────────────────────────────────────────┐
  kernel32 ────────────►│ main.c      entry, CLI, orchestration,      │
  advapi32 ────────────►│             state machine, self-update      │
  ws2_32   ────────────►└──┬──────┬───────┬───────┬───────┬───────────┘
  wininet  ────────────────┘      │       │       │       │
                          ┌───────▼──┐ ┌──▼────┐ ┌▼─────┐ ┌▼─────────┐
                          │ sntp.c   │ │priv.c │ │log.c │ │fallback.c│
                          │ UDP/123  │ │token+ │ │UTF-16│ │ .dat read│
                          │ 48B RFC  │ │SeSys- │ │16KB  │ │ + atomic │
                          │ 4330     │ │temtime│ │rotate│ │  write   │
                          └────┬─────┘ └──┬────┘ └──┬───┘ └────┬─────┘
                               │          │         │          │
                          ┌────▼──────────▼─────────▼──────────▼─────┐
                          │ timeutil.c / verdict.c   (PURE, no OS)  │
                          │ FILETIME↔Unix↔civil date, NTP epoch,     │
                          │ RFC-1123 + .dat parsers, all decisions   │
                          └──────────────────────────────────────────┘
```

| File | Responsibility | Win32 APIs | Unit-testable |
|---|---|---|---|
| `src/config.h` | All tunables, version, embedded `FALLBACK_DATE` (single source of truth) | none | — |
| `src/timeutil.c` | Days↔civil date, FILETIME↔Unix↔NTP, `YYYY-MM-DD` and RFC-1123 parsing, formatting | none | **yes (host, no mocks)** |
| `src/verdict.c` | Every branch decision as a pure function | none | **yes (host, no mocks)** |
| `src/sntp.c` | Blocking SNTP client, per-server deadline | ws2_32 | parser/validator yes |
| `src/http_date.c` | `HEAD` via WinINet + raw-socket fallback, `Date:` parse | wininet | parser yes |
| `src/fallback.c` | `TimeKeeper.dat` read/validate/atomic write | kernel32 | read path yes |
| `src/privilege.c` | `SeSystemtimePrivilege` enable + capability report | advapi32 | — |
| `src/log.c` | UTF-16 append, 16 KiB cap, console mirror | kernel32 | — |
| `src/main.c` | CLI, state machine, `SetSystemTime`, self-update, install/uninstall | all | integration (Wine) |

**Testability rule:** OS calls never wrap a decision. `verdict.c`/`timeutil.c` return
*what to do*; `main.c` performs it. This is what lets the whole safety policy be tested on
Linux with no stubs and no `#ifdef` in shipping code.

## 2. Control flow

```
START (WinMain; no console unless /debug | /dry-run | /test)
  │
  ├─ parse CLI ──► /install ──► CopyFiles + schtasks ──► exit       /uninstall ──► exit
  ├─ read TZ bias, SystemTime, .dat, embedded default → reference REF
  ├─ ensure SeSystemtimePrivilege ──► if cannot set: log, exit 2 (dry-run: warn+continue)
  │
  ├─ DECIDE_SKIP ──► |now − REF| ≤ 5 min AND REF fresh (≤45 d) ─────► LOG + exit 0
  │                  (REF = max(build epoch, .dat epoch, file mtime epoch))
  ├─ NET: SNTP ──► pool0 ─► pool1 ─► google ─► cloudflare ─► windows       [budget 3000 ms,
  │                                                              800 ms/recv, global deadline]
  │        first valid reply ──► SET_SYSTEM_TIME(utc) ──► SELF_UPDATE ──► exit 0
  ├─ NET: HTTP Date ──► WinINet HEAD (2 hosts) ─► raw TCP :80 GET (2 IPs) [1500 ms]
  │        valid ──► SET_SYSTEM_TIME(utc) ──► SELF_UPDATE ──► exit 0
  └─ OFFLINE: fallback date 00:00:00 UTC
           apply only if verdict says clock implausibly old ──► exit 0
           else log "offline but clock seems ok, skipping" ──► exit 0
```

`SELF_UPDATE`: on a *successful network sync only*, if `YYYY-MM(now) >` stored month, atomically
write `YYYY-MM-01` to `TimeKeeper.dat`. Never on failure, never backwards.

## 3. File formats

**`TimeKeeper.dat`** (same directory as `.exe`; ASCII, tolerant of CRLF/BOM/comments/junk):
```
# TimeKeeper offline fallback. Parsed token = first YYYY-MM-DD.
2026-09-01
```
Only the first syntactically valid date is used. Read cap 512 B. Missing/corrupt/`EPOCH_INVALID`
→ embedded `FALLBACK_DATE`. Day-of-month validated against month length and leap years; year
must lie in `[TK_MIN_EPOCH_YEAR=2005, TK_MAX_EPOCH_YEAR=2040]` and inside FILETIME range.

**`timekeeper.log`** — UTF-16LE with BOM, append-only. Past 16 KiB the newest 8 KiB is
kept, trimmed to a whole-line boundary: the decision is the pure `tk_log_trim_tail()` in
`src/log_rotate.c`, the I/O stays in `log.c`. Keeping "the longest prefix that ends on
a line terminator" rather than "everything after the first" is what makes the trim idempotent.
Rotation runs after every append, so a trim that also discarded a whole line each time
would chew the retained tail down to nothing while still reporting a nicely bounded file
— a bug invisible to any test that only checks the size.
Path: `%ProgramData%\TimeKeeper\timekeeper.log`, else next to the `.exe`, else temp.
```
[2026-09-01 00:00:00.000 UTC] [SRC:NTP(time.google.com)] Set UTC 2026-09-01 00:00:00 -> 05:30:00 IST+330 OK
```
`SRC` ∈ `NTP(host) | HTTP(host) | OFFLINE(dat) | OFFLINE(embedded) | SKIP | PRIV | INSTALL`.
Log lines are also mirrored to stdout when a console is attached (console failure is ignored).

**Timebase.** Internally everything is a signed `long long` epoch-seconds (Unix) or 64-bit
100 ns `FILETIME`. NTP = 2 208 988 800 s ahead of Unix; FILETIME = `11644473600 s + 100 ns`.

## 4. Decisions (with reasons)

| # | Decision | Why |
|---|---|---|
| D1 | Always `SetSystemTime` with **UTC**; never `SetLocalTime` | One code path; IST is fixed +05:30 so no DST rule can be wrong. Log shows local via `SystemTimeToTzSpecificLocalTime`. |
| D2 | Fallback lands on `00:00:00 UTC` = `05:30 IST` | Keeps the documented date-of-month true in IST; ±12 h can never move the calendar day. |
| D3 | "Already correct" test compares against `REF = max(build, .dat, .dat mtime)` | A clock at 2031 must not be dragged back to a stale 2026 `.dat`; a clock at 2009 must be fixed. `max()` makes the tool monotone-forward. Precision of the comparison follows the precision of whichever source won — see V1. |
| D4 | Offline fallback is **not** an error → exit 0 | Cold boot with dead CMOS and no network is the normal case; non-zero codes would look like boot failures. |
| D5 | Exit codes `0` synced/ok, `1` could not determine or could not apply, `2` insufficient privilege | Three states are all the scheduler needs. |
| D6 | Never regress the clock: apply only if candidate > now, or now is implausibly old | Prevents a stale `.dat` from undoing a correct clock after the user's network came back. |
| D7 | `requireAdministrator` manifest, even though the task runs as SYSTEM | Manual double-click must elevate (settable); a `asInvoker` build silently fails instead. |
| D8 | Second-precision time is accepted (no PLL, no `adjtime`) | A dead-CMOS box drifts minutes/day; sub-second discipline is pointless. One-shot correction only. |
| D9 | No Event Log registration | `RegisterEventSource` needs a machine-wide registered source; file log is auditable and free. |
| D10 | No CRT-heavy helpers (`strftime`, `swprintf`, `malloc`) | Deterministic size, no locale dependence, zero heap, no leak surface. |

## 5. Budgets and limits

| Item | Limit | Enforced by |
|---|---|---|
| SNTP per-server `recv` | 800 ms | `select()` timeout + `SO_RCVTIMEO` |
| Total network phase | 3000 ms | deadline (`GetTickCount64`-free: `GetTickCount` + wrap-safe delta) before each server; server skipped if `<120 ms` left |
| HTTP phase | 1500 ms | `INTERNET_OPTION_*_TIMEOUT` + `recv` `select` |
| Wall clock, network OK | <1.5 s | measured — see `TESTING.md` |
| Peak RSS | <3 MiB | static/global buffers only; no heap, no `malloc` |
| `.exe` size | <80 KiB target, 150 KiB hard | `/O1 /Os` / `-Os -ffunction-sections --gc-sections`, `/MT` `-static`, stripped |

Residual known limit (accepted): `getaddrinfo` has no timeout in Winsock, so a machine with a
configured-but-dead DNS server can stall the DNS step until the OS gives up (1–15 s); the global
deadline then takes the offline branch immediately. The raw-TCP `:80` fallback in `http_date.c`
uses **no DNS at all**, so the HTTP path still works on such networks. See README §Troubleshooting.

## 6. Threat model / security notes

* Attacker-controlled `.dat` = attacker-controlled boot clock, bounded to `[2005, 2040]`; it is
  applied only when the real clock is already implausible. Directory ACLs are the control.
* SNTP replies are validated (mode, stratum, leap, epoch window) but the protocol is unauthenticated,
  so a man on the LAN can offset the clock by seconds. That is identical to stock `w32tm`.
* No telemetry, no inbound surface, no persistence beyond one scheduled task. HTTP `Date` is used
  only as a *coarse* (±2 s, second-granularity) source and is sanity-clamped.
* `install.cmd` copies into `%ProgramFiles%\TimeKeeper` (admin-writable only) and registers a
  SYSTEM task: an unprivileged user cannot swap the `.dat` or the `.exe` to gain a fake clock.

## 7. Deviations and refinements, with the evidence that forced them

Recorded here because a reader comparing this file to the source should find the
discrepancies explained rather than discovered.

| # | change from the sketch above | why |
|---|---|---|
| V1 | The "clock already correct" test is **date-granular when the reference is date-granular** (`ref_is_date_only`) | `TimeKeeper.dat` and a build date name a *day*, not an instant. Requiring ±5 min against `2026-09-01T00:00` would re-probe the network on every boot of a machine whose clock is fine, breaking the <500 ms requirement. A `.dat` **mtime** is an instant, so when that is what produced REF the strict 5-minute rule applies. Verified: `done in 2 ms` on a correct clock |
| V2 | The SNTP loop guard is the **Originate echo** (RFC 4330: the server must copy our Transmit Timestamp into its Originate field), not a Transmit-field comparison | Comparing the 8-byte Transmit field rejected *legitimate* replies: `time.google.com` answered with the same whole second and a zero fraction, i.e. byte-identical in that field. Found by running the real .exe, where every server reported `LOOPED-REQUEST`. A verbatim echo of our request is still refused separately |
| V3 | `SetSystemTime` failing with 5 / 1312 / 1314 maps to **exit 2**, not 1 | The token can claim `SeSystemtimePrivilege` while group policy has revoked it. Reporting that as a generic failure sends the operator to the wrong specialist. First seen under Wine (`Win32 1314`) |
| V4 | Log path selection **probes for append access**, then falls back | A root-owned `timekeeper.log` from an earlier elevated run made every subsequent unprivileged run write nothing at all. "ProgramData or nothing" would be asserting a bug; the fallback chain is the feature |
| V5 | `user32.dll` is imported (2 functions); `msvcrt.dll` is not imported at all | `MessageBox*` on the interactive failure path the spec requires cannot be reached without user32. msvcrt *was* imported for 7 pure functions (`memcpy`, `strlen`, …) — that one was fixable, so `src/tklibc.c` implements them and `tests/pe_audit.py` now rejects the CRT modules if they return |
| V6 | No-CRT entry point (`-nostartfiles` + `WinMainCRTStartup` in `src/crt_start.c`), `-Os` only, `--file-alignment 512`, self-test reporting compacted in the product build | With the conventional static CRT the binary was 107 KB. 76 800 / 71 168 bytes now. Documented consequences in `crt_start.c`: no `atexit`, no stdio flush, no locale — all unused |
| V7 | The HTTP tier asks WinINet for the **whole header block** (`HTTP_QUERY_RAW_HEADERS_CRLF`) and reuses the same extractor as the raw transport | `HTTP_QUERY_DATE` follows a two-call buffer contract; the single-call form returned 0 bytes for a perfectly good 200 response, which read as "no Date header". One parser for both transports also means one place to audit |
| V8 | `TK_NTP_PORT` and the HTTP host/port are compile-time overridable (`TK_TEST_NTP_PORT`, `TK_TEST_HTTP_HOST`, …) | A test rig that must bind UDP/123 only works as root, and when it silently fails to bind, *negative* tests pass for the wrong reason. Observed exactly that way. The overrides let the suite run unprivileged against a fixture, and the harness now refuses to call "the binary never ran" a pass |
