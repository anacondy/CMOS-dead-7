/* main.c — TimeKeeper entry point: CLI, orchestration, application.
 *
 * Design rule for this file: gather inputs, ask the pure modules (verdict.c,
 * *_proto.c) what to do, then do it and log it. No date arithmetic and no
 * policy live here — that is what makes the dangerous parts testable without a
 * Windows box, and what keeps this file readable top to bottom in one sitting.
 *
 * Subsystem is WINDOWS (no console flash at boot). A console is created only
 * for /debug, /dry-run and /test, i.e. when a human is watching.
 *
 * SPDX-License-Identifier: MIT
 */
#include "config.h"

#include <windows.h>
#include "tklibc.h"

#include "winutil.h"
#include "log.h"
#include "privilege.h"
#include "verdict.h"
#include "sntp.h"
#include "sntp_proto.h"
#include "http_date.h"
#include "fallback.h"
#include "install.h"
#include "selftest.h"

/* ---------------------------------------------------------------- options */

typedef struct {
    int debug, dry_run, force, test_only, do_install, do_uninstall, quiet, help, show_version;
    char unknown[128];                /* deferred: parse_args runs before the log opens */
    int console;                       /* stdout is usable                     */
    wchar_t exe_dir[MAX_PATH];
    tk_tz_t tz;
} tk_ctx;

/* console ownership: FreeConsole would yank the shell's console away. */
static int g_allocated_console;

/* Accepted with '/', '-' or '--': this program gets run by hand at odd hours,
 * and the Windows convention genuinely varies between tools. */
static int opt_is(const wchar_t *a, const char *name)
{
    size_t i = 0;
    while (*a == L'/' || *a == L'-') a++;
    while (name[i] && a[i]) {
        char c = (char)a[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != name[i]) return 0;
        i++;
    }
    return name[i] == 0 && a[i] == 0;
}

static void parse_args(int argc, wchar_t **argv, tk_ctx *c)
{
    int i;
    for (i = 1; i < argc; i++) {
        const wchar_t *a = argv[i];
        if      (opt_is(a, "debug"))       c->debug = 1;
        else if (opt_is(a, "dry-run") || opt_is(a, "dryrun")) c->dry_run = 1;
        else if (opt_is(a, "force"))       c->force = 1;
        else if (opt_is(a, "test"))        c->test_only = 1;
        else if (opt_is(a, "install"))     c->do_install = 1;
        else if (opt_is(a, "uninstall"))   c->do_uninstall = 1;
        else if (opt_is(a, "quiet"))       c->quiet = 1;
        else if (opt_is(a, "help") || opt_is(a, "?")) c->help = 1;
        else if (opt_is(a, "version"))     c->show_version = 1;
        else {
            /* Unknown switch: say so, then carry on with the default job - a typo
             * at a console must not silently do nothing, and must not fail either.
             * Recorded rather than logged: the log file is not open yet here. */
            size_t n = 0;
            const char *p = "unknown option ignored: ";
            while (*p && n + 1 < sizeof(c->unknown)) c->unknown[n++] = *p++;
            while (n + 2 < sizeof(c->unknown) && *a) {
                c->unknown[n++] = (char)((*a < 0x20 || *a > 0x7e) ? '?' : *a); a++;
            }
            c->unknown[n] = 0;
        }
    }
    /* A bare double-click has no console and nobody watching: do not open one,
     * and do not pop a message box. Everything goes to the log file. */
}

/* ------------------------------------------------------ argument splitting
 * CommandLineToArgvW lives in shell32.dll, which is outside this program's
 * allowed DLL set (kernel32/advapi32/ws2_32/wininet, plus user32 for the one
 * MessageBox). Splitting GetCommandLineW by hand is 40 lines, follows the
 * documented MS rules for backslash/quote interaction, and removes a DLL. */
static int split_cmdline(const wchar_t *line, wchar_t **argv, int max)
{
    int   argc = 0;
    size_t i = 0;

    if (!line) return 0;
    while (argc < max) {
        int    in_quotes = 0;
        while (line[i] == L' ' || line[i] == L'\t') i++;
        if (!line[i]) break;

        /* Tokens are written from offset 0 of their own buffer, while `i` walks
         * the source. They are different cursors: using `i` for both produced a
         * NUL-prefixed token and silently ignored every flag (caught by running
         * the real .exe under Wine, where the log showed a full run for /test). */
        {
            size_t w = 0;
            while (line[i]) {
                int bs = 0;
                while (line[i] == L'\\') { bs++; i++; }
                if (line[i] == L'"') {
                    /* 2n backslashes before a quote -> n literals + a quote
                     * toggle; 2n+1 -> n literals + an escaped quote. */
                    int j;
                    for (j = 0; j < bs / 2; j++) argv[argc][w++] = L'\\';
                    if (bs & 1) argv[argc][w++] = L'"';
                    else in_quotes = !in_quotes;
                    i++;
                    continue;
                }
                if (!in_quotes && (line[i] == L' ' || line[i] == L'\t')) break;
                for (; bs > 0; bs--) argv[argc][w++] = L'\\';
                argv[argc][w++] = line[i++];
            }
            argv[argc][w] = 0;
        }
        argc++;
    }
    return argc;
}

/* ------------------------------------------------------------- console ---- */

/* Attach to the parent console if we were launched from one (the normal
 * interactive case); otherwise allocate one. Then wire the three std handles
 * to CONOUT$, because a WINDOWS-subsystem process starts with them invalid. */
static int console_enable(void)
{
    HANDLE h;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (!AllocConsole()) return 0;
        g_allocated_console = 1;                 /* we created it, we close it */
    }

    h = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        SetStdHandle(STD_OUTPUT_HANDLE, h);
        CloseHandle(h);
    }
    h = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        SetStdHandle(STD_ERROR_HANDLE, h);
        CloseHandle(h);
    }
    return 1;
}

static void console_puts(const char *s)
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    wchar_t buf[512];
    size_t n = 0;
    if (!out || out == INVALID_HANDLE_VALUE) return;
    for (; s[n] && n < sizeof(buf) / sizeof(buf[0]) - 2; n++) buf[n] = (wchar_t)(unsigned char)s[n];
    buf[n] = L'\n';
    {
        DWORD wr;
        WriteFile(out, buf, (DWORD)((n + 1) * sizeof(wchar_t)), &wr, NULL);
    }
}

/* ------------------------------------------------------------ tiny output */

typedef struct { char *b; size_t cap, n; } sb_t;
static void sb_init(sb_t *s, char *b, size_t cap) { s->b = b; s->cap = cap; s->n = 0; if (cap) b[0] = 0; }
static void sb_puts(sb_t *s, const char *t) { while (*t && s->n + 1 < s->cap) s->b[s->n++] = *t++; s->b[s->n] = 0; }
static void sb_num(sb_t *s, uint32_t v, unsigned pad)
{
    if (s->n + 12 < s->cap) s->n += tk_fmt_u32(s->b + s->n, s->cap - s->n, v, pad);
    s->b[s->n] = 0;
}
static void sb_wide(sb_t *s, const wchar_t *w)
{
    while (*w && s->n + 1 < s->cap) { s->b[s->n++] = (char)((*w < 0x20 || *w > 0x7e) ? '?' : *w); w++; }
    s->b[s->n] = 0;
}

/* ------------------------------------------------------- apply a new time */

typedef struct {
    tk_ctx *c;
    time64_t now;
    uint32_t now_ms;
} tk_apply_t;

/* Returns 1 = clock changed, 0 = nothing to do / dry run, -1 = SetSystemTime failed. */
static int apply_utc(const tk_apply_t *ap, time64_t target, uint32_t target_ms,
                     const char *src)
{
    char   msg[TK_LOG_LINE_MAX];
    sb_t   s;
    int    r;
    char   stamp[32], local[48];
    int    is_utc = 0;
    int64_t delta;

    sb_init(&s, msg, sizeof(msg));
    sb_puts(&s, "Set UTC ");
    tk_fmt_epoch_utc(stamp, sizeof(stamp), target);
    sb_puts(&s, stamp);
    if (tk_win_epoch_to_local_text(target, local, sizeof(local), &is_utc)) {
        sb_puts(&s, " -> ");
        sb_puts(&s, local);
    }
    if (ap->c->tz.have) {
        sb_puts(&s, " (bias ");
        sb_num(&s, (uint32_t)(ap->c->tz.bias_min < 0 ? -ap->c->tz.bias_min : ap->c->tz.bias_min), 1);
        sb_puts(&s, " min");
        if (ap->c->tz.dst_on) sb_puts(&s, ", DST active");
        sb_puts(&s, ")");
    }

    delta = ((int64_t)target - (int64_t)ap->now) * 1000 + (int64_t)target_ms - (int64_t)ap->now_ms;
    if (delta < 0) delta = -delta;
    if (delta <= TK_SET_THRESHOLD_MS) {
        sb_puts(&s, " skipped: delta ");
        sb_num(&s, (uint32_t)delta, 1);
        sb_puts(&s, " ms is below the set threshold");
        tk_log_src(src, msg);
        return 0;
    }
    if (ap->c->dry_run) {
        sb_puts(&s, " [DRY-RUN] not applied");
        tk_log_src(src, msg);
        if (ap->c->console) console_puts(msg);
        return 0;
    }
    r = tk_win_apply_utc(target, target_ms, 0);
    if (r == 0) {
        sb_puts(&s, " OK");
        tk_log_src(src, msg);
        if (ap->c->console) console_puts(msg);
        return 1;
    }
    sb_puts(&s, " FAILED Win32 ");
    sb_num(&s, (uint32_t)(-r), 1);
    tk_log_src(src, msg);
    if (ap->c->console) console_puts(msg);
    return -1;
}

/* Every exit reports its own wall time: the specification's performance budget
 * (<1.5 s online, <500 ms when the clock is already right) is then verifiable
 * from the log file alone, on the customer's machine, with no profiler. */
static void log_duration(uint32_t t0, int code)
{
    char     m[96];
    sb_t     s;
    uint32_t ms = (uint32_t)tk_tick() - t0;

    sb_init(&s, m, sizeof(m));
    sb_puts(&s, "done in ");
    sb_num(&s, ms, 1);
    sb_puts(&s, " ms, exit ");
    sb_num(&s, (uint32_t)(code < 0 ? 0 : code), 1);
    tk_log_info(m);
}

/* ------------------------------------------------- .dat self-update (D5) */

static void self_update_dat(const tk_fallback_t *fb, time64_t synced_utc, const tk_ctx *c)
{
    int      dec = 0, want;
    int      y = 0;
    unsigned mo = 0, d = 0, hh = 0, mi = 0, ss = 0;
    wchar_t  written[MAX_PATH];

    want = tk_dat_should_write(1, synced_utc, fb->stored, fb->embedded, &dec);
    if (c->dry_run && want) {
        char m[200]; sb_t t;
        sb_init(&t, m, sizeof(m));
        sb_puts(&t, "DRY-RUN: would write TimeKeeper.dat for month ");
        {
            int      dy = 0;
            unsigned dmo = 0, dd = 0, dh = 0, dmi = 0, ds = 0;
            if (tk_civil_from_epoch_utc(synced_utc, &dy, &dmo, &dd, &dh, &dmi, &ds)) {
                char dt[16]; tk_fmt_date(dt, sizeof(dt), dy, dmo, 1u); sb_puts(&t, dt);
            }
        }
        tk_log_src("FILE", m);
        if (c->console) console_puts(m);
        return;
    }
    {
        char msg[256]; sb_t s;
        sb_init(&s, msg, sizeof(msg));
        sb_puts(&s, "fallback .dat update: ");
        sb_puts(&s, tk_dat_name(dec));
        if (!want) sb_puts(&s, " (no write)");
        tk_log_src("FILE", msg);
        if (c->console && !c->quiet) console_puts(msg);
    }
    if (!want) return;

    if (!tk_civil_from_epoch_utc(synced_utc, &y, &mo, &d, &hh, &mi, &ss)) return;
    written[0] = 0;
    if (tk_fallback_update(fb, y, mo, c->exe_dir[0] ? c->exe_dir : NULL,
                           written, MAX_PATH)) {
        char msg[300]; sb_t s;
        sb_init(&s, msg, sizeof(msg));
        sb_puts(&s, "wrote ");
        sb_wide(&s, written[0] ? written : L"TimeKeeper.dat");
        sb_puts(&s, " = ");
        { char t[16]; tk_fmt_date(t, sizeof(t), y, mo, 1u); sb_puts(&s, t); }
        tk_log_src("FILE", msg);
        if (c->console) console_puts(msg);
    } else {
        tk_log_err("could not update TimeKeeper.dat (clock is still corrected)", (int)GetLastError());
    }
}

/* ------------------------------------------------------------------ help */

static const char kHelp[] =
"TimeKeeper " TK_VERSION_STR " - one-shot clock correction for a dead CMOS battery\n"
"\n"
"usage: TimeKeeper.exe [/debug] [/dry-run] [/force] [/test] [/install] [/uninstall] [/quiet]\n"
"\n"
"  /debug      attach or allocate a console, mirror every log line to it\n"
"  /dry-run    do everything except SetSystemTime; implies /debug\n"
"  /force      skip the 'clock already correct' check; also allows back-dating\n"
"  /test       run the built-in self test (no network, no changes) and exit\n"
"  /install    copy to %ProgramFiles%\\TimeKeeper and register the two scheduled tasks\n"
"  /uninstall  delete the scheduled tasks (files and log are left alone)\n"
"  /quiet      suppress console output, keep the log file\n"
"  /version    print version and exit\n"
"\n"
"exit codes: 0 synced / nothing to do / offline fallback applied\n"
"            1 no usable time source, or SetSystemTime failed\n"
"            2 SeSystemtimePrivilege unavailable (run elevated or via the task)\n";

static void show_usage(const tk_ctx *c)
{
    const char *p = kHelp;
    char  line[512];
    size_t n = 0;
    if (!c->console) return;
    for (; *p; p++) {
        if (*p == '\n') { line[n] = 0; console_puts(line); n = 0; continue; }
        if (n + 1 < sizeof(line)) line[n++] = *p;
    }
    if (n) { line[n] = 0; console_puts(line); }
}

/* --------------------------------------------------------------- the run */

static int do_run(tk_ctx *c)
{
    uint32_t t_run0 = (uint32_t)tk_tick();
    time64_t      now = TK_EPOCH_INVALID, build_ep, ref, cand;
    uint32_t      now_ms = 0;
    tk_fallback_t fb;
    tk_priv_t     priv;
    int           verdict, ref_fresh, force = c->force;
    uint32_t      t_net, deadline;
    tk_apply_t    ap;
    char          msg[TK_LOG_LINE_MAX];
    sb_t          s;

    tk_win_now_epoch(&now, &now_ms);
    tk_fallback_load(c->exe_dir[0] ? c->exe_dir : NULL, &fb);

    build_ep = tk_build_epoch(TK_BUILD_YEAR, TK_BUILD_MONTH, TK_BUILD_DAY);
    ref      = tk_choose_reference(build_ep, fb.stored, fb.mtime);
    if (ref == TK_EPOCH_INVALID) ref = build_ep;
    ref_fresh = tk_ref_is_fresh(now, ref, TK_REF_FRESH_DAYS);
    cand      = fb.epoch;
    if (!tk_candidate_ok(cand, TK_MIN_EPOCH_YEAR, TK_MAX_EPOCH_YEAR)) cand = TK_EPOCH_INVALID;

    /* Which source produced REF decides how precisely it can certify the clock:
     * a .dat mtime is an instant, a date or a build stamp is only a day. */
    verdict = tk_verdict_skip_or_sync(now, ref, ref_fresh, TK_SKIP_MATCH_MS, force,
                                     !(fb.source == 0 && ref == fb.mtime));

    sb_init(&s, msg, sizeof(msg));
    sb_puts(&s, "start v" TK_VERSION_STR " at tick ");
    sb_num(&s, t_run0, 1);
    sb_puts(&s, " verdict=");
    sb_puts(&s, tk_verdict_name(verdict));
    sb_puts(&s, " clock=");
    { char t[32]; tk_fmt_epoch_utc(t, sizeof(t), now); sb_puts(&s, t); }
    sb_puts(&s, " ref=");
    { char t[32]; tk_fmt_epoch_utc(t, sizeof(t), ref); sb_puts(&s, t); }
    sb_puts(&s, " dat=");
    { char t[32]; tk_fmt_epoch_utc(t, sizeof(t), fb.source == 0 ? fb.stored : fb.embedded); sb_puts(&s, t); }
    if (fb.source != 0) sb_puts(&s, "(embedded)");
    if (fb.file_bad && fb.source != 0) sb_puts(&s, "(dat unparseable)");
    sb_puts(&s, " tz=");
    if (c->tz.have) {
        sb_num(&s, (uint32_t)(c->tz.bias_min < 0 ? -c->tz.bias_min : c->tz.bias_min), 1);
        sb_puts(&s, "min");
        if (c->tz.is_ist) sb_puts(&s, "(IST)");
    } else sb_puts(&s, "unknown");
    tk_log_info(msg);
    if (c->console && !c->quiet) console_puts(msg);

    if (verdict == TKV_SKIP_ALREADY_OK) {
        tk_log_src("SRC:SKIP", "clock agrees with a fresh reference; nothing to do");
        log_duration(t_run0, TK_EXIT_OK);
        return TK_EXIT_OK;
    }

    /* Privilege first: without it no branch can apply anything, and finding
     * that out before spending 3 s on a dead network is worth it. */
    tk_priv_acquire(&priv);
    if (!priv.priv_held) {
        sb_init(&s, msg, sizeof(msg));
        sb_puts(&s, "cannot set the system clock: ");
        sb_wide(&s, priv.fail ? priv.fail : L"SeSystemtimePrivilege unavailable");
        if (priv.err) { sb_puts(&s, " (Win32 "); sb_num(&s, (uint32_t)priv.err, 1); sb_puts(&s, ")"); }
        if (priv.elevated && !priv.is_system) sb_puts(&s, " - administrator without this right");
        if (!priv.elevated) sb_puts(&s, " - run the TimeKeeper task as SYSTEM, or elevate");
        tk_log_src("PRIV", msg);
        if (c->console) console_puts(msg);
        else if (!c->quiet) {
            wchar_t w[512];
            tk_utf8_wcs(msg, w, sizeof(w) / sizeof(w[0]));
            MessageBoxW(NULL, w, L"TimeKeeper: needs SeSystemtimePrivilege",
                        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        }
        if (!c->dry_run) { log_duration(t_run0, TK_EXIT_NO_PRIV); return TK_EXIT_NO_PRIV; }
        tk_log_src("PRIV", "continuing: /dry-run cannot change the clock anyway");
    }

    ap.c = c; ap.now = now; ap.now_ms = now_ms;

    /* ---------------------------------------------------- tier 1: SNTP ---- */
    t_net   = (uint32_t)tk_tick();
    deadline = t_net + TK_NET_DEADLINE_MS;
    {
        tk_sntp_result_t nr;
        if (tk_sntp_sync(deadline, &nr)) {
            char src[64]; sb_t t;
            time64_t target = nr.utc_sec;
            uint32_t ms = nr.ms;
            int applied;
            sb_init(&t, src, sizeof(src));
            sb_puts(&t, "SRC:NTP("); sb_puts(&t, nr.server); sb_puts(&t, ")");
            sb_init(&t, msg, sizeof(msg));
            sb_puts(&t, "reply in "); sb_num(&t, (uint32_t)(nr.rtt_ms < 0 ? 0 : nr.rtt_ms), 1);
            sb_puts(&t, " ms");
            tk_log_src(src, msg);
            if (ms >= 500u) { target += 1; ms = 0; }        /* nearest second */
            else ms = 0;                                     /* SetSystemTime is second-granular */
            applied = apply_utc(&ap, target, ms, src);
            if (applied >= 0) {
                self_update_dat(&fb, target, c);
                log_duration(t_run0, TK_EXIT_OK);
                return TK_EXIT_OK;
            }
            log_duration(t_run0, applied == -2 ? TK_EXIT_NO_PRIV : TK_EXIT_FAIL);
            return applied == -2 ? TK_EXIT_NO_PRIV : TK_EXIT_FAIL;
        }
        {
            uint32_t used = (uint32_t)tk_tick() - t_net;
            sb_init(&s, msg, sizeof(msg));
            sb_puts(&s, "SNTP unavailable (");
            sb_puts(&s, tk_ntp_status_name(nr.status));
            sb_puts(&s, "; ");
            sb_puts(&s, nr.last_fail[0] ? nr.last_fail : "no detail");
            sb_puts(&s, "; ");
            sb_num(&s, used, 1);
            sb_puts(&s, " ms spent)");
            tk_log_info(msg);
            if (c->console && !c->quiet) console_puts(msg);
            /* If the UDP tier burned half its budget with no packet at all, the
             * firewall is silently dropping 123/udp: HTTP is the only thing
             * worth trying, and it should start now, not after two more probes. */
        }
    }

    /* ---------------------------------------------------- tier 2: HTTP ---- */
    deadline = (uint32_t)tk_tick() + TK_HTTP_TIMEOUT_MS * 2u;
    {
        tk_http_result_t hr;
        if (tk_http_date(deadline, &hr)) {
            char src[64]; sb_t t;
            int applied;
            sb_init(&t, src, sizeof(src));
            sb_puts(&t, "SRC:HTTP("); sb_puts(&t, hr.server);
            sb_puts(&t, hr.transport ? ",raw" : "");
            sb_puts(&t, ")");
            tk_log_src(src, "Date header accepted");
            applied = apply_utc(&ap, hr.utc_sec, 0, src);
            if (applied >= 0) {
                self_update_dat(&fb, hr.utc_sec, c);
                log_duration(t_run0, TK_EXIT_OK);
                return TK_EXIT_OK;
            }
            log_duration(t_run0, applied == -2 ? TK_EXIT_NO_PRIV : TK_EXIT_FAIL);
            return applied == -2 ? TK_EXIT_NO_PRIV : TK_EXIT_FAIL;
        }
        sb_init(&s, msg, sizeof(msg));
        sb_puts(&s, "HTTP date unavailable: ");
        sb_puts(&s, hr.last_fail[0] ? hr.last_fail : "no detail");
        tk_log_info(msg);
    }

    /* ------------------------------------------------ tier 3: offline ---- */
    {
        int  action = TKF_SKIP_OK;
        char src[48];
        tk_fallback_apply(now, ref, cand, TK_FALLBACK_MAX_AGE_DAYS, force, &action);

        sb_init(&s, src, sizeof(src));
        sb_puts(&s, "SRC:OFFLINE(");
        sb_puts(&s, fb.source == 0 ? "dat" : "embedded");
        sb_puts(&s, ")");

        sb_init(&s, msg, sizeof(msg));
        sb_puts(&s, "offline decision: ");
        sb_puts(&s, tk_fallback_name(action));
        if (cand != TK_EPOCH_INVALID) {
            sb_puts(&s, " candidate=");
            { char t[32]; tk_fmt_epoch_utc(t, sizeof(t), cand); sb_puts(&s, t); }
        }
        tk_log_src(src, msg);
        if (c->console && !c->quiet) console_puts(msg);

        switch (action) {
        case TKF_APPLY: {
            int applied = apply_utc(&ap, cand, 0, src);
            if (applied < 0) return TK_EXIT_FAIL;
            tk_log_src(src, c->dry_run ? "offline fallback would be applied (dry run)"
                                       : "offline fallback applied: network was unreachable; "
                                         "set the CMOS battery or run once online");
            return TK_EXIT_OK;
        }
        case TKF_SKIP_OK:
            tk_log_src(src, "offline but clock seems ok, skipping");
            return TK_EXIT_OK;
        case TKF_SKIP_WOULD_REGRESS:
        default:
            tk_log_src(src, "refusing to set the clock backwards to the stored fallback");
            return TK_EXIT_FAIL;
        }
    }
}

/* ------------------------------------------------------------------ entry */

/* Failures must be visible with no console: /test on a machine that is already
 * misbehaving is exactly when nobody has a terminal attached. */
static void selftest_report(void *ud, const char *line)
{
    (void)ud;
    if (line && line[0] == 'F') tk_log_src("SELFTEST", line);
    console_puts(line);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow)
{
    tk_ctx   c;
    int      argc = 0, rc = TK_EXIT_FAIL;
    /* Fixed, bounded argv: 16 arguments of 64 wchar_t each = 2 KB of stack.
     * No heap anywhere in this program, which is how the <3 MB RSS budget is met
     * without reasoning about allocator behaviour at all. */
    wchar_t argbuf[16][64];
    wchar_t *argv[16];
    int      i;

    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;
    tk_memset(&c, 0, sizeof(c));

    for (i = 0; i < 16; i++) argv[i] = argbuf[i];
    argc = split_cmdline(GetCommandLineW(), argv, 16);

    parse_args(argc, argv, &c);

    /* Open a console for the interactive modes. /install and /uninstall also
     * get one, because the operator wants to see the outcome. */
    if (c.dry_run) c.debug = 1;
    if (c.debug || c.test_only || c.do_install || c.do_uninstall || c.show_version || c.help)
        c.console = console_enable();

    {
        wchar_t p[MAX_PATH];
        if (GetModuleFileNameW(NULL, p, MAX_PATH) > 0) {
            size_t n = 0;
            while (n + 1 < MAX_PATH && p[n]) { c.exe_dir[n] = p[n]; n++; }
            while (n > 0 && c.exe_dir[n-1] != L'\\') n--;
            if (n > 0) { c.exe_dir[n-1] = 0; }
            else c.exe_dir[0] = 0;
        }
    }
    tk_log_open(c.exe_dir[0] ? c.exe_dir : NULL, c.console);
    tk_win_tz(&c.tz);
    if (c.unknown[0]) { tk_log_info(c.unknown); if (c.console) console_puts(c.unknown); }

    if (c.help)         { show_usage(&c); rc = TK_EXIT_OK; goto out; }
    if (c.show_version) { if (c.console) console_puts("TimeKeeper " TK_VERSION_STR);
                          rc = TK_EXIT_OK; goto out; }

    if (c.test_only) {
        rc = tk_selftest_run(selftest_report, NULL);
        if (c.console) console_puts(rc == 0 ? "self test: PASS" : "self test: FAIL");
        else if (rc) MessageBoxA(NULL, "TimeKeeper self test FAILED - do not deploy this build",
                                  TK_APP_TITLE, MB_OK | MB_ICONERROR);
        goto out;
    }

    if (c.do_install || c.do_uninstall) {
        tk_install_result_t ir;
        int ok = c.do_install ? tk_install(&ir) : tk_uninstall(&ir);
        rc = ok ? TK_EXIT_OK : TK_EXIT_FAIL;
        if (c.console) {
            char m[256]; sb_t s;
            sb_init(&s, m, sizeof(m));
            sb_puts(&s, c.do_install ? "install " : "uninstall ");
            sb_puts(&s, ok ? "succeeded" : "failed: ");
            if (!ok) sb_puts(&s, ir.detail);
            console_puts(m);
            if (ok && c.do_install) {
                char b[256]; sb_t t;
                sb_init(&t, b, sizeof(b));
                sb_wide(&t, ir.target_dir);
                sb_puts(&t, "  tasks: ");
                sb_puts(&t, ir.task_start_ok ? "onstart=ok " : "onstart=FAIL ");
                sb_puts(&t, ir.task_logon_ok ? "onlogon=ok" : "onlogon=FAIL");
                console_puts(b);
            }
        }
        goto out;
    }

    rc = do_run(&c);

out:
    tk_log_close();
    if (g_allocated_console) FreeConsole();
    return rc;
}

/* FreeConsole on exit is also what a console user needs to see the final lines
 * before the window closes if we allocated it ourselves; the parent-console case
 * leaves the shell exactly as it found it. */
