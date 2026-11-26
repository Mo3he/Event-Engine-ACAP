/* Tests for the Day/Night window: offsets, windows that cross midnight,
 * time zones far from their longitude, and polar day/night. Builds
 * app/engine/scheduler.c with a fixed clock and without the ACAP runtime. */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../app/cJSON.h"

/* Keep the real ACAP.h (GLib, FastCGI) out; scheduler.c needs only these. */
#define _ACAP_H_
cJSON* ACAP_Get_Config(const char* service);
int ACAP_DEVICE_Seconds_Since_Midnight(void);

static time_t fake_now;
static time_t fake_time(time_t* t) { if (t) *t = fake_now; return fake_now; }
#define time(x) fake_time(x)
#include "../app/engine/scheduler.c"
#undef time

cJSON* ACAP_Get_Config(const char* service) { (void)service; return NULL; }
void EventLog_Syslog(int priority, const char* source, const char* fmt, ...) {
    (void)priority; (void)source; (void)fmt;
}
int ACAP_DEVICE_Seconds_Since_Midnight(void) {
    struct tm* t = localtime(&fake_now);
    return t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
}

static int failures;

static void set_clock(const char* tz, int y, int mo, int d, int sod) {
    setenv("TZ", tz, 1);
    tzset();
    struct tm tm = {0};
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    tm.tm_hour = sod / 3600; tm.tm_min = sod / 60 % 60; tm.tm_sec = sod % 60;
    tm.tm_isdst = -1;
    fake_now = mktime(&tm);
}

static void check(const char* name, int got, int want) {
    if (got != want) {
        printf("FAIL %s: got %d, want %d\n", name, got, want);
        failures++;
    }
}

/* Is it day at sod (local seconds) on the given date? */
static int day_at(const char* tz, double lat, double lon, int y, int mo, int d, int sod,
                  int rise_off_min, int set_off_min) {
    set_clock(tz, y, mo, d, sod);
    return Scheduler_Is_Daytime(lat, lon, rise_off_min * 60, set_off_min * 60);
}

int main(void) {
    const char* sthlm = "Europe/Stockholm";
    const double sla = 59.3293, slo = 18.0686;

    /* Offsets move the boundaries to the second */
    set_clock(sthlm, 2026, 11, 26, 12 * 3600);
    int rise = compute_solar_event(sla, slo, ASTRO_SUNRISE, fake_now);
    int set  = compute_solar_event(sla, slo, ASTRO_SUNSET, fake_now);
    check("Stockholm sunrise is around 08:05", rise / 60, 8 * 60 + 5);
    check("Stockholm sunset is around 15:04", set / 60, 15 * 60 + 4);
    check("1 s before sunrise+60 is night", day_at(sthlm, sla, slo, 2026, 11, 26, rise + 3600 - 1, 60, -30), 0);
    check("sunrise+60 is day",              day_at(sthlm, sla, slo, 2026, 11, 26, rise + 3600,     60, -30), 1);
    check("1 s before sunset-30 is day",    day_at(sthlm, sla, slo, 2026, 11, 26, set - 1800 - 1,  60, -30), 1);
    check("sunset-30 is night",             day_at(sthlm, sla, slo, 2026, 11, 26, set - 1800,      60, -30), 0);
    check("offsets that empty the day give night", day_at(sthlm, sla, slo, 2026, 11, 26, 12 * 3600, 720, -720), 0);
    check("window of 24 h or more is day",  day_at(sthlm, sla, slo, 2026, 6, 21, 3 * 3600, -720, 720), 1);

    /* A sunset offset past midnight keeps it day after 00:00 */
    check("sunset+120 in midsummer: 23:59 is day", day_at(sthlm, sla, slo, 2026, 6, 21, 23 * 3600 + 59 * 60, 0, 120), 1);
    check("sunset+120 in midsummer: 00:05 is day", day_at(sthlm, sla, slo, 2026, 6, 22, 5 * 60, 0, 120), 1);
    check("sunset+120 in midsummer: 01:00 is night", day_at(sthlm, sla, slo, 2026, 6, 22, 3600, 0, 120), 0);

    /* Local sunset after midnight without offsets (about 00:03) */
    const char* rvk = "Atlantic/Reykjavik";
    check("Reykjavik midsummer noon is day",  day_at(rvk, 64.1466, -21.9426, 2026, 6, 21, 12 * 3600, 0, 0), 1);
    check("Reykjavik midsummer 00:02 is day", day_at(rvk, 64.1466, -21.9426, 2026, 6, 22, 2 * 60, 0, 0), 1);
    check("Reykjavik midsummer 01:00 is night", day_at(rvk, 64.1466, -21.9426, 2026, 6, 22, 3600, 0, 0), 0);

    /* UTC+13 at longitude -175: local noon is far from the UTC day */
    const char* tonga = "Pacific/Tongatapu";
    check("Tonga noon is day",    day_at(tonga, -21.1394, -175.2049, 2026, 11, 26, 12 * 3600, 0, 0), 1);
    check("Tonga 02:00 is night", day_at(tonga, -21.1394, -175.2049, 2026, 11, 26, 2 * 3600, 0, 0), 0);
    check("Tonga 23:00 is night", day_at(tonga, -21.1394, -175.2049, 2026, 11, 26, 23 * 3600, 0, 0), 0);

    /* Polar night is night and midnight sun is day, whatever the offsets */
    const char* oslo = "Europe/Oslo";
    check("Tromso polar night noon is night", day_at(oslo, 69.6492, 18.9553, 2026, 12, 15, 12 * 3600, -720, 720), 0);
    check("Tromso midnight sun 00:00 is day", day_at(oslo, 69.6492, 18.9553, 2026, 6, 21, 0, 720, -720), 1);

    /* Astronomical triggers still get wrapped times, -1 in polar night */
    set_clock(rvk, 2026, 6, 21, 12 * 3600);
    check("Reykjavik sunset wraps to just after 00:00",
          compute_solar_event(64.1466, -21.9426, ASTRO_SUNSET, fake_now) < 3600, 1);
    set_clock(oslo, 2026, 12, 15, 12 * 3600);
    check("no sunrise in polar night", compute_solar_event(69.6492, 18.9553, ASTRO_SUNRISE, fake_now), -1);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all scheduler tests passed\n");
    return 0;
}
