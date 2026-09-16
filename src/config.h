/* config.h — single source of truth for every tunable in TimeKeeper.
 *
 * TimeKeeper 1.0.0 — one-shot clock correction for Windows 7 boxes with a dead CMOS battery.
 * SPDX-License-Identifier: MIT
 *
 * Nothing else in the tree defines versions, server lists, timeouts or the fallback date.
 * The GitHub Action (.github/workflows/bump-fallback.yml) rewrites exactly one line here:
 * TK_STR(TK_FALLBACK_YEAR)-...-FALLBACK_DATE. Keep the format stable.
 */
#ifndef TIMEKEEPER_CONFIG_H
#define TIMEKEEPER_CONFIG_H

/* ---------------------------------------------------------------- version ---- */
/* Also consumed by version.rc (windres/rc run the preprocessor on .rc files),
 * so the binary's VERSIONINFO and the code can never disagree.               */
#define TK_VERSION_MAJOR 1
#define TK_VERSION_MINOR 0
#define TK_VERSION_PATCH 0
#define TK_VERSION_STR   "1.0.0"

/* ------------------------------------------------------------------ build ---- */
#define TK_BUILD_YEAR  2026   /* __DATE__ is locale-dependent; this is not. */
#define TK_BUILD_MONTH 9
#define TK_BUILD_DAY   15

/* ------------------------------------------------- offline fallback (the ---- */
/* ONE LINE THE GITHUB ACTION BUMPS: the compile-time default used only when  */
/* TimeKeeper.dat is missing or unparseable.                                  */
/* The bump target. The GitHub Action rewrites this one line (and TimeKeeper.dat)
 * on the 1st of each month, so a fresh clone carries a recent default.
 * Keep both forms in sync; the action does, and /test validates the string. */
#define TK_FALLBACK_DATE "2026-09-01"
#define TK_FALLBACK_YEAR  2026
#define TK_FALLBACK_MONTH 9
#define TK_FALLBACK_DAY   1

/* ----------------------------------------------------------------- layout ---- */
#define TK_APP_NAME      "TimeKeeper"
#define TK_APP_TITLE     "TimeKeeper " TK_VERSION_STR
#define TK_DAT_NAME      "TimeKeeper.dat"
#define TK_DAT_TMP_NAME  "TimeKeeper.dat.tmp"
#define TK_LOG_DIR_NAME  "TimeKeeper"
#define TK_LOG_NAME      "timekeeper.log"

/* Wide literals: every path we hand to the OS is UTF-16 (see log.c). */
#define TK_APP_NAME_W      L"TimeKeeper"
#define TK_DAT_NAME_W      L"TimeKeeper.dat"
#define TK_LOG_DIR_NAME_W  L"TimeKeeper"
#define TK_LOG_NAME_W      L"timekeeper.log"
#define TK_SUBDIR_W        L"TimeKeeper"
#define TK_TASK_ONSTART_W  L"TimeKeeper"
#define TK_TASK_ONLOGON_W  L"TimeKeeperLogon"

/* ---------------------------------------------------------------- network ---- */
/* Probed strictly in this order, sequentially, fast-fail. All port 123.      */
/* Overridable so a test build can point at a loopback SNTP responder
 * (-DTK_TEST_NTP_ONLY="127.0.0.1"); production uses the list below. */
#ifdef TK_TEST_NTP_ONLY
#define TK_NTP_SERVERS  { TK_TEST_NTP_ONLY }
#define TK_NTP_SERVER_COUNT 1
#else
#define TK_NTP_SERVERS  {"0.pool.ntp.org", "1.pool.ntp.org", "time.google.com", \
                         "time.cloudflare.com", "time.windows.com"}
#define TK_NTP_SERVER_COUNT 5
#endif

/* Port + server list are overridable so the loopback test rig can run
 * unprivileged on a high port; production is always 123. Both the numeric port
 * and its string form for getaddrinfo derive from ONE definition, so they can
 * never disagree (that mismatch would silently send to 123 while listening on
 * 12300, which reads as "the server is broken"). */
#ifndef TK_NTP_PORT
#  ifdef TK_TEST_NTP_PORT
#    define TK_NTP_PORT TK_TEST_NTP_PORT
#  else
#    define TK_NTP_PORT 123
#  endif
#endif
#ifndef TK_NTP_PORT_STR
#  ifdef TK_TEST_NTP_PORT
#    define TK_STRX(x) #x
#    define TK_STR(x)  TK_STRX(x)
#    define TK_NTP_PORT_STR TK_STR(TK_TEST_NTP_PORT)
#  else
#    define TK_NTP_PORT_STR "123"
#  endif
#endif
#define TK_NTP_TIMEOUT_MS       800    /* per-server recv budget                */
#define TK_NET_DEADLINE_MS      3000   /* hard cap for the whole network phase  */
#define TK_NET_MIN_LEFT_MS      120    /* skip a server if less remains         */
#define TK_NTP_TX_TOLERANCE_SEC 2      /* last-ditch guard vs. looped-back pkt  */

/* Secondary tier: HTTP Date header (for networks that black-hole UDP/123).   *
 * TK_TEST_HTTP_HOST / _PORT / TK_TEST_HTTP_IP are overridable so the acceptance
 * suite can point each transport at a local fixture instead of the internet.
 * They are taken as *already quoted* strings - passing -DTK_TEST_HTTP_IP=127.0.0.1
 * bare is a preprocessor error ("too many decimal points"), which is the right
 * failure mode for a test hook: loud, at compile time. */
#ifdef TK_TEST_NO_HTTP
#  define TK_HTTP_HOSTS      { "" }
#  define TK_HTTP_HOST_COUNT 0
#elif defined(TK_TEST_HTTP_HOST)
#  define TK_HTTP_HOSTS      { TK_TEST_HTTP_HOST }
#  define TK_HTTP_HOST_COUNT 1
#else
#  define TK_HTTP_HOSTS      {"www.google.com", "www.cloudflare.com/cdn-cgi/trace"}
#  define TK_HTTP_HOST_COUNT 2
#endif
/* One port for BOTH transports, so a test build can never ask one on :80 and
 * the other on the fixture port. */
#ifdef TK_TEST_HTTP_HOST_PORT
#  define TK_HTTP_PORT TK_TEST_HTTP_HOST_PORT
#else
#  define TK_HTTP_PORT 80
#endif
/* No-DNS rescue: raw TCP GET to anycast IPs, no resolver and no proxy: the
 * path that still works when DNS is poisoned and the proxy is down. */
#ifdef TK_TEST_HTTP_IP
#  define TK_HTTP_IP_ENDPOINTS { TK_TEST_HTTP_IP }
#  define TK_HTTP_IP_ENDPOINT_COUNT 1
#else
#  define TK_HTTP_IP_ENDPOINTS {"142.250.74.4", "104.16.132.229", "216.58.203.3"}
#  define TK_HTTP_IP_ENDPOINT_COUNT 3
#endif

#define TK_HTTP_TIMEOUT_MS      1500   /* resolve+connect+send+receive total    */
#define TK_HTTP_IP_TIMEOUT_MS   1000
#define TK_HTTP_MAX_RESP_BYTES  1536   /* we only need the first 2 KB or so     */

/* ------------------------------------------------------------ time policy ---- */
#define TK_MIN_EPOCH_YEAR   2005   /* reject NTP/HTTP/.dat results older than  */
#define TK_MAX_EPOCH_YEAR   2040   /* ... or newer than                          */
#define TK_SKIP_MATCH_MS    300000 /* 5 min: "clock already right" threshold     */
#define TK_REF_FRESH_DAYS   45     /* reference date younger than this = trust */
#define TK_FALLBACK_MAX_AGE_DAYS 3650 /* beyond this, ignore stale .dat        */
#define TK_SET_THRESHOLD_MS 2000 /* SetSystemTime only if delta exceeds this   */
#define TK_IST_BIAS_MIN     330   /* deployment zone: +05:30, no DST */

/* ------------------------------------------------------------ log / files ---- */
#define TK_LOG_MAX_BYTES     16384
#define TK_LOG_KEEP_BYTES    8192
#define TK_LOG_LINE_MAX      320   /* bytes, UTF-8 before conversion           */
#define TK_DAT_READ_MAX      512   /* never read more of the .dat than this    */

/* ----------------------------------------------------------------- exits ---- */
#define TK_EXIT_OK          0   /* clock correct/synced, or sane offline fallback */
#define TK_EXIT_FAIL        1   /* no time source and nothing safe to apply       */
#define TK_EXIT_NO_PRIV     2   /* could not obtain SeSystemtimePrivilege         */

#endif /* TIMEKEEPER_CONFIG_H */
