# Manual verification on a real Windows 7 SP1 machine

Wine proves the logic and the wire formats; it cannot prove UAC token filtering, group
policy, an antivirus product's opinions, a real CMOS battery, or a real
`SetSystemTime`. Those need the actual OS. This is the checklist for one afternoon with a
VM or the machine in question.

Take a snapshot first. Several tests change the system clock, and one changes
`HKLM\...\User Rights` — the snapshot is the undo button.

## 0. Setup

1. VM: Windows 7 SP1, either SKU. Install nothing else. Take the snapshot.
2. Copy the release folder to `C:\timekeeper`.
3. Note the baseline: `winver`, `wmic os get Caption,OSArchitecture`, `date /t`, `time /t`.
4. For each test, capture the log afterwards:
   `copy "%ALLUSERSPROFILE%\TimeKeeper\timekeeper.log" C:\tk.log`

Test both the x86 and x64 binary on the matching OS; also run the x86 binary on the x64
OS once (WOW64 path, which is where `Sysnative` and the `Program Files (x86)` choice get
exercised).

## 1. Dead CMOS, no network — the offline fallback

*Discharges the assumption that a user has internet when the clock is wrong.*

1. `w32tm /stop /service` (so nothing else competes), then unplug the virtual NIC / set
   the NIC to "not connected".
2. Set the clock backwards deliberately:
   ```bat
   date 01-01-2009
   time 12:00:00
   ```
3. `C:\timekeeper\TimeKeeper.exe /debug`
4. **Expect:** exit 0, and `date /t` + `time /t` show `2026-09-01 05:30:00` (the
   fallback date at 00:00 UTC = 05:30 IST). Log line reads
   `Set UTC 2026-09-01 00:00:00 -> 05:30:00 GMT+330 OK`.
5. Check the timezone was **not** modified: `控制`/`timedate.cpl` still shows
   (UTC+05:30) Chennai. TimeKeeper only sets the UTC instant.
6. Now with a *stale* `.dat`: `echo 2020-01-01> C:\timekeeper\TimeKeeper.dat`, set the
   clock to 2009 again, re-run. **Expect:** the 2020 date is used (older than the
   45-day trust window but still newer than the clock), not the embedded 2026 default.
7. `.dat` absent entirely (rename it) → **Expect:** embedded default used, log says
   `SRC:OFFLINE(embedded)`.

## 2. Dead CMOS, working network — SNTP path

1. NIC connected. `date 01-01-2009`, `time 12:00:00`.
2. `TimeKeeper.exe /debug`
3. **Expect:** exit 0 within ~1 s, clock within a few seconds of reality, log shows
   `SRC:NTP(<server>)` and `TimeKeeper.dat update: REWRITE(stale)`.
4. `type C:\timekeeper\TimeKeeper.dat` → **Expect** the first of the current month.
5. `w32tm /query /status` → confirm `w32time` is not the thing that fixed it (it is
   stopped in step 1; that is the point).

## 3. Idempotence and "don't thrash a good clock"

1. Immediately repeat test 2's run. **Expect:** log says `NOOP(idempotent)`, the `.dat`
   mtime is unchanged (`dir C:\timekeeper\TimeKeeper.dat`), exit 0.
2. Third run: now the clock is correct, so **Expect** `SRC:SKIP` and
   `done in 1 ms` / `done in 2 ms`. This is the number the "<500 ms" claim is about —
   read it from the log, not from a stopwatch.
3. Reboot the VM, confirm `timekeeper.log` contains a fresh boot-time run that skipped in
   a couple of milliseconds, and that the boot was not visibly slowed.

## 4. UDP/123 blocked — the HTTP tier

This is the Indian ISP / hostel / corporate case.

1. Block outbound UDP 123 on the VM (host firewall), or point the VM at a network that
   drops it. Confirm with `w32tm /stripchart /computer:time.google.com /samples:1`, which
   should time out.
2. Set the clock to 2009 again, run `TimeKeeper.exe /debug`.
3. **Expect:** log shows `SNTP unavailable (…)` then `SRC:HTTP(www.google.com)` and a
   correct second-precision clock. Total elapsed under 3.5 s.
4. Now also block TCP/80 egress but leave DNS alive → **Expect** both tiers to fail and
   the offline tier to apply, exit 0. (Blocking 80 while allowing 53 is how you prove the
   `.dat` path really is independent of the network.)
5. With DNS poisoned to a captive portal (a real hostel network does this), confirm the
   portal's own `Date:` header is refused if it is out of the year window, and that a 3xx
   is refused outright.

## 5. Firewall that returns ICMP unreachable instead of dropping

Different code path (fast failure rather than a timeout).

1. Reject (not drop) UDP 123 on the host: `iptables -A FORWARD -p udp --dport 123 -j REJECT`
   or the Windows firewall's "Block" with the equivalent behaviour.
2. **Expect:** all five servers fail in well under a second total, HTTP is tried
   immediately. This is the case where a "3 s timeout per server" implementation would
   hang for 15 s; verify the log timestamps prove it did not.

## 6. No privilege — the non-admin and filtered-token cases

1. Standard user, UAC on, double-click `TimeKeeper.exe`.
   **Expect:** the UAC prompt appears (the manifest requires elevation). Decline it →
   nothing happens, no crash. Accept it → it works. Then delete the task and run from a
   *non-elevated* cmd: **Expect** exit code 2 and a log line
   `[PRIV] cannot set the system clock: not elevated: run as SYSTEM task or elevate`.
2. Group-policy-filtered token (the interesting one — an *administrator* account whose
   "Change the system time" right has been removed):
   - `secpol.msc` → Local Policies → User Rights Assignment → **Change the system time**
     → remove Administrators, keep only SYSTEM. Apply, log off/on.
   - In an *elevated* admin prompt run `TimeKeeper.exe /debug`.
     **Expect:** exit 2, because `AdjustTokenPrivileges` returns
     `ERROR_NOT_ALL_ASSIGNED`, and the log says `SeSystemtimePrivilege not assigned to this
     account`. This is the case a naive tool reports as success.
   - The scheduled task (running as SYSTEM) must still work → confirm with
     `schtasks /run /tn TimeKeeper` and re-read the log.
   - Restore the policy afterwards.
3. Kill the privilege *after* logon (some corporate environments do this with a logoff
   script): the token claims the right and the LSA refuses. **Expect** the log line
   `Set UTC … FAILED Win32 1314 (privilege revoked by policy: run via the SYSTEM task)`
   and exit 2.
4. Corrupt/locked log directory: `icacls "%ALLUSERSPROFILE%\TimeKeeper" /deny
   Everyone:(W)` → run → **Expect** a log file beside the .exe (or %TEMP%) and a normal
   exit code; the program must not fail just because logging failed.

## 7. Size, memory, and startup cost

1. Both binaries under 150 KB, target under 80 KB:
   `dir C:\timekeeper\*.exe`. Record the numbers.
2. Memory: run `TimeKeeper.exe /test` (it holds long enough to observe) and check
   `tasklist /fi "IMAGENAME eq TimeKeeper.exe" /v`, or Process Explorer's "Private Bytes".
   **Expect:** under 3 MB, and no `msvcr*.dll`/`vcruntime*.dll`/`ucrtbase.dll` in the
   module list — the module list is the honest check of "zero external dependencies".
3. Boot impact: `schtasks /run /tn TimeKeeper` from a cold boot with `uptime`/Task
   Manager's "BIOS time" comparison. Also confirm the process is *gone* afterwards:
   `tasklist | findstr TimeKeeper` must print nothing. A one-shot tool that leaves a
   process behind has failed at being one-shot.

## 8. Long-run / robustness checks

1. **100 cold boots** with the battery still dead and no network: the clock must land on
   the `.dat` date every time, the log must stay ≤ 16 KiB, and `TimeKeeper.dat` must not
   be rewritten (offline runs never bump it). Check the log line count grows by ~5 per boot.
2. Fill the disk (`fsutil file createnew C:\big 1048576` until near full) → run:
   **Expect** a logged `cannot write TimeKeeper.dat (clock is still corrected)`, an
   unchanged `.dat`, and exit 0 if the clock was set.
3. Put a 4 KB random blob in `TimeKeeper.dat` (mimics a corrupted flash read) → **Expect**
   `dat unparseable`, embedded default used, no crash.
4. Set the BIOS clock to a *future* date (e.g. 2035) → run with no network → **Expect**
   "refusing to set the clock backwards", exit 1, clock untouched. A clock that looks too
   new must never be pulled back to a stale fallback.
5. `date 02-30-2026` is impossible; instead set the clock to 2028 with the network alive,
   then run: **Expect** SNTP corrects it *backwards* (network sources are trusted over the
   clock, unlike the `.dat`), and the log records it.
6. Timezone edge: set the zone to `(UTC-08:00) Pacific` and run offline → the fallback
   still lands on `00:00:00 UTC`; confirm the log's local-time annotation changed and the
   program did not attempt to "fix" the zone.
7. DST-boundary host (for anyone reusing this outside India): set the zone to
   `(UTC-05:00) Eastern`, clock to 2009, run offline near a DST transition, and check the
   applied instant is still exactly midnight UTC.
8. Update safety: install, let it bump `.dat` to the current month, then copy a *new*
   build over it. **Expect** the newer on-disk `.dat` wins (it is not reset to the
   compile-time default), and `NOOP(newer-on-disk)` in the log.

## 9. Uninstall

```bat
uninstall.cmd
schtasks /query /tn TimeKeeper      :: must say "ERROR: The system cannot find the file."
wmic ntpclient list full            :: untouched, for the record
```
Then confirm the clock survives a reboot (it will keep whatever it was given; the tool
never persists anything into the time subsystem itself), and that
`uninstall.cmd /purge` removes the folder and the log.

## Recording results

Put the numbers you actually measured into `TESTING.md` §"recorded run" — sizes, private
bytes, boot-to-exit time, and the log lines for tests 1–6. If anything there disagrees
with what this file says to expect, that is a bug: open an issue with the log attached
(the log is ≤ 16 KiB by construction, so it pastes cleanly).
