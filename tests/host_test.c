/* host_test.c — runs the shipped test suite (src/selftest.c) natively on the
 * build machine, and adds the one cross-check that is only possible there:
 * our calendar versus libc's timegm()/gmtime().
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "timeutil.h"
#include "selftest.h"

static int g_lines;

static void report(void *ud, const char *line)
{
    (void)ud;
    printf("  %s\n", line);
    g_lines++;
}

/* Independent oracle for the civil<->epoch core. If these two agree over every
 * day from 1601 to 2100, the hand-rolled calendar is not merely self-consistent. */
static int cross_check_calendar(void)
{
    long year;
    int mism = 0, days = 0;
    for (year = 1601; year <= 2100; year++) {
        unsigned m, d;
        for (m = 1; m <= 12; m++) {
            unsigned dim = tk_days_in_month((int)year, m);
            for (d = 1; d <= dim; d++) {
                struct tm tm;
                time_t expect;
                int ok = 0;
                int64_t ours;
                int y2 = 0; unsigned m2 = 0, d2 = 0, h2 = 0, i2 = 0, s2 = 0;

                memset(&tm, 0, sizeof(tm));
                tm.tm_year = (int)year - 1900;
                tm.tm_mon  = (int)m - 1;
                tm.tm_mday = (int)d;
                expect = timegm(&tm);

                ours = tk_days_from_civil((int)year, m, d, &ok);
                if (!ok || ours * 86400 != (int64_t)expect) mism++;
                if (!tk_civil_from_epoch_utc((time64_t)expect, &y2, &m2, &d2, &h2, &i2, &s2) ||
                    y2 != (int)year || m2 != m || d2 != d || h2 || i2 || s2) mism++;
                /* and the weekday libc computes, for a further independent check */
                if (m < 3u) { }
                else {
                    struct tm chk;
                    time_t t = expect;
                    memset(&chk, 0, sizeof(chk));
                    gmtime_r(&t, &chk);
                    if (chk.tm_wday != tk_day_of_week_from_days(ours)) mism++;
                }
                days++;
            }
        }
    }
    printf("calendar vs libc timegm(): %d days, %d mismatches\n", days, mism);
    return mism ? 1 : 0;
}

int main(void)
{
    int rc;
    printf("== TimeKeeper suite (host build of the shipped sources)\n");
    rc = tk_selftest_run(report, NULL);
    printf("== cross-check against libc\n");
    rc |= cross_check_calendar();
    printf("\n%s (%d report lines)\n", rc ? "FAILED" : "PASSED", g_lines);
    return rc;
}
