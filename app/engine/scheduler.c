#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <syslog.h>

#include "scheduler.h"

#define LOG(fmt, args...)      syslog(LOG_INFO,    "scheduler: " fmt, ## args)
#define LOG_WARN(fmt, args...) syslog(LOG_WARNING, "scheduler: " fmt, ## args)

#define MAX_SCHEDULES 256
#define SCHED_CRON        0
#define SCHED_INTERVAL    1
#define SCHED_DAILY_TIME  2

typedef struct {
    char    rule_id[37];
    int     trigger_index;
    int     type;

    /* cron fields (bitmasks) */
    long long minute_mask;  /* bits 0-59 */
    int       hour_mask;    /* bits 0-23 */
    int       dom_mask;     /* bits 1-31 */
    int       month_mask;   /* bits 1-12 */
    int       dow_mask;     /* bits 0-6  (0=Sun) */
    int       cron_last_min; /* last minute we fired to debounce */

    /* interval fields */
    int       interval_seconds;
    time_t    last_run;

    /* daily_time fields */
    int       seconds_of_day; /* target seconds since midnight */
    int       days_mask;      /* bitmask: bit0=Sun ... bit6=Sat */
    int       fired_today;    /* reset at midnight crossover */
    int       prev_sod;       /* previous seconds_of_day reading */
} ScheduleEntry;

static ScheduleEntry    entries[MAX_SCHEDULES];
static int              entry_count = 0;
static Scheduler_Callback fire_cb  = NULL;

/*-----------------------------------------------------
 * Cron field parser
 *-----------------------------------------------------*/

static int parse_cron_field(const char* field, int min_val, int max_val,
                             long long* mask_out) {
    *mask_out = 0;
    if (!field) return 0;

    /* Handle "*" */
    if (strcmp(field, "*") == 0) {
        for (int i = min_val; i <= max_val; i++)
            *mask_out |= (1LL << i);
        return 1;
    }

    /* Handle comma-separated list */
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", field);
    char* tok;
    char* saveptr;
    tok = strtok_r(buf, ",", &saveptr);
    while (tok) {
        char* dash = strchr(tok, '-');
        char* slash = strchr(tok, '/');
        if (dash) {
            int a = atoi(tok);
            int b = atoi(dash + 1);
            int step = 1;
            if (slash) { *slash = '\0'; step = atoi(slash + 1); }
            for (int i = a; i <= b; i += step)
                if (i >= min_val && i <= max_val)
                    *mask_out |= (1LL << i);
        } else if (slash) {
            int step = atoi(slash + 1);
            if (step < 1) step = 1;
            for (int i = min_val; i <= max_val; i += step)
                *mask_out |= (1LL << i);
        } else {
            int v = atoi(tok);
            if (v >= min_val && v <= max_val)
                *mask_out |= (1LL << v);
        }
        tok = strtok_r(NULL, ",", &saveptr);
    }
    return 1;
}

static int parse_cron(ScheduleEntry* e, const char* expr) {
    if (!expr) return 0;
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", expr);

    char* fields[5];
    int n = 0;
    char* tok = strtok(buf, " ");
    while (tok && n < 5) { fields[n++] = tok; tok = strtok(NULL, " "); }
    if (n != 5) return 0;

    long long tmp;
    if (!parse_cron_field(fields[0], 0, 59, &tmp)) return 0;
    e->minute_mask = tmp;
    if (!parse_cron_field(fields[1], 0, 23, &tmp)) return 0;
    e->hour_mask = (int)tmp;
    if (!parse_cron_field(fields[2], 1, 31, &tmp)) return 0;
    e->dom_mask = (int)tmp;
    if (!parse_cron_field(fields[3], 1, 12, &tmp)) return 0;
    e->month_mask = (int)tmp;
    if (!parse_cron_field(fields[4], 0, 6,  &tmp)) return 0;
    e->dow_mask = (int)tmp;
    e->cron_last_min = -1;
    return 1;
}

static int days_array_to_mask(cJSON* days) {
    /* days: JSON array of ints where 0=Sun, 1=Mon ... 6=Sat */
    int mask = 0;
    if (!days || !cJSON_IsArray(days)) {
        /* default: all days */
        return 0x7F;
    }
    cJSON* d;
    cJSON_ArrayForEach(d, days) {
        int v = (int)d->valuedouble;
        if (v >= 0 && v <= 6) mask |= (1 << v);
    }
    return mask ? mask : 0x7F;
}

/*-----------------------------------------------------
 * Public API
 *-----------------------------------------------------*/

int Scheduler_Init(Scheduler_Callback cb) {
    memset(entries, 0, sizeof(entries));
    entry_count = 0;
    fire_cb = cb;
    return 1;
}

void Scheduler_Cleanup(void) {
    entry_count = 0;
    fire_cb = NULL;
}

int Scheduler_Register(const char* rule_id, int trigger_index, cJSON* config) {
    if (!rule_id || !config) return 0;
    if (entry_count >= MAX_SCHEDULES) {
        LOG_WARN("schedule table full");
        return 0;
    }

    const char* stype = cJSON_GetStringValue(cJSON_GetObjectItem(config, "schedule_type"));
    if (!stype) { LOG_WARN("missing schedule_type for rule %s", rule_id); return 0; }

    ScheduleEntry* e = &entries[entry_count];
    memset(e, 0, sizeof(ScheduleEntry));
    snprintf(e->rule_id, sizeof(e->rule_id), "%s", rule_id);
    e->trigger_index = trigger_index;

    if (strcmp(stype, "cron") == 0) {
        const char* expr = cJSON_GetStringValue(cJSON_GetObjectItem(config, "cron"));
        if (!parse_cron(e, expr)) {
            LOG_WARN("invalid cron expression '%s' for rule %s", expr ? expr : "(null)", rule_id);
            return 0;
        }
        e->type = SCHED_CRON;

    } else if (strcmp(stype, "interval") == 0) {
        cJSON* iv = cJSON_GetObjectItem(config, "interval_seconds");
        e->interval_seconds = iv ? (int)iv->valuedouble : 60;
        if (e->interval_seconds < 1) e->interval_seconds = 1;
        e->last_run = 0;
        e->type = SCHED_INTERVAL;

    } else if (strcmp(stype, "daily_time") == 0) {
        const char* t = cJSON_GetStringValue(cJSON_GetObjectItem(config, "time"));
        if (!t) { LOG_WARN("missing time for daily_time rule %s", rule_id); return 0; }
        int hh = 0, mm = 0;
        sscanf(t, "%d:%d", &hh, &mm);
        e->seconds_of_day = hh * 3600 + mm * 60;
        e->days_mask  = days_array_to_mask(cJSON_GetObjectItem(config, "days"));
        e->fired_today = 0;
        e->prev_sod    = -1;
        e->type = SCHED_DAILY_TIME;

    } else {
        LOG_WARN("unknown schedule_type '%s' for rule %s", stype, rule_id);
        return 0;
    }

    entry_count++;
    LOG("registered %s schedule for rule %s", stype, rule_id);
    return 1;
}

void Scheduler_Unregister_Rule(const char* rule_id) {
    if (!rule_id) return;
    int i = 0;
    while (i < entry_count) {
        if (strcmp(entries[i].rule_id, rule_id) == 0) {
            if (i < entry_count - 1)
                entries[i] = entries[entry_count - 1];
            entry_count--;
        } else {
            i++;
        }
    }
}

void Scheduler_Tick(void) {
    if (!fire_cb) return;

    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    int sod = tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
    int dow = tm->tm_wday; /* 0=Sun */

    for (int i = 0; i < entry_count; i++) {
        ScheduleEntry* e = &entries[i];

        switch (e->type) {
        case SCHED_CRON: {
            /* Fire once per matching minute */
            int cur_min = tm->tm_hour * 60 + tm->tm_min;
            if (tm->tm_sec != 0) break;
            if (cur_min == e->cron_last_min) break;
            int month = tm->tm_mon + 1; /* 1-12 */
            int dom   = tm->tm_mday;    /* 1-31 */
            if ((e->minute_mask & (1LL << tm->tm_min)) &&
                (e->hour_mask  & (1   << tm->tm_hour)) &&
                (e->dom_mask   & (1   << dom)) &&
                (e->month_mask & (1   << month)) &&
                (e->dow_mask   & (1   << dow))) {
                e->cron_last_min = cur_min;
                fire_cb(e->rule_id, e->trigger_index);
            }
            break;
        }
        case SCHED_INTERVAL: {
            if (e->last_run == 0) { e->last_run = now; break; }
            if ((now - e->last_run) >= e->interval_seconds) {
                e->last_run = now;
                fire_cb(e->rule_id, e->trigger_index);
            }
            break;
        }
        case SCHED_DAILY_TIME: {
            /* Detect midnight crossover — reset fired_today */
            if (e->prev_sod > 0 && sod < e->prev_sod)
                e->fired_today = 0;
            e->prev_sod = sod;

            if (!e->fired_today &&
                sod >= e->seconds_of_day &&
                sod <  e->seconds_of_day + 60 &&  /* 1-minute window */
                (e->days_mask & (1 << dow))) {
                e->fired_today = 1;
                fire_cb(e->rule_id, e->trigger_index);
            }
            break;
        }
        }
    }
}
