#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <pthread.h>
#include <syslog.h>
#include <time.h>

#include "event_log.h"
#include "../ACAP.h"

#define EVENT_LOG_PERSIST_FILE  "localdata/event_log.json"
#define SYSTEM_LOG_PERSIST_FILE "localdata/system_log.json"
static int persist_dirty = 0;  /* pending writes since last flush */

typedef struct {
    char   rule_id[37];
    char   rule_name[128];
    double timestamp;          /* epoch seconds, millisecond resolution */
    int    fired;
    char   block_reason[64];
    cJSON* trigger_snapshot;   /* cJSON_Duplicate at append time */
    int    actions_run;
    int    actions_failed;
    char   action_error[256];
} LogEntry;

static LogEntry         entries[EVENT_LOG_SIZE];
static int              head  = 0;   /* next write index */
static int              count = 0;   /* valid entries */
static pthread_mutex_t  lock  = PTHREAD_MUTEX_INITIALIZER;

double EventLog_Now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)(ts.tv_nsec / 1000000) / 1000.0;
}

/*-----------------------------------------------------
 * System entries: Event Engine's own status and warnings (MQTT connection,
 * startup, warnings from the app log). Kept in their own ring with their own
 * lock so they never push out rule entries, and so code holding the rule lock
 * (or writing files) can still log a warning without deadlocking.
 *-----------------------------------------------------*/
typedef struct {
    double timestamp;
    double last_timestamp;     /* latest repeat */
    char   source[24];
    char   level[8];           /* info, warning, error */
    char   message[512];
    char   key[160];           /* source|level|message with digits folded, for de-duplication */
    int    repeats;
} SysEntry;

#define SYS_REPEAT_WINDOW  600.0   /* identical messages within 10 min become repeats */
#define SYS_RATE_PER_MIN   20      /* new entries per minute; the rest are counted */

static SysEntry         sys_entries[SYSTEM_LOG_SIZE];
static int              sys_head = 0, sys_count = 0, sys_dirty = 0;
static double           sys_window_start = 0;
static int              sys_window_count = 0, sys_suppressed = 0;
static pthread_mutex_t  sys_lock = PTHREAD_MUTEX_INITIALIZER;

static void sys_key(char* out, size_t n, const char* source, const char* level, const char* msg) {
    size_t o = (size_t)snprintf(out, n, "%s|%s|", source, level);
    if (o >= n) o = n - 1;
    int in_digits = 0;
    for (const char* p = msg; *p && o + 1 < n; p++) {
        if (isdigit((unsigned char)*p)) {
            if (!in_digits) out[o++] = '#';
            in_digits = 1;
        } else {
            out[o++] = *p;
            in_digits = 0;
        }
    }
    out[o] = '\0';
}

static void sys_append_locked(double now, const char* source, const char* level, const char* msg, const char* key) {
    SysEntry* e = &sys_entries[sys_head];
    memset(e, 0, sizeof(*e));
    e->timestamp = e->last_timestamp = now;
    e->repeats = 1;
    snprintf(e->source,  sizeof(e->source),  "%s", source);
    snprintf(e->level,   sizeof(e->level),   "%s", level);
    snprintf(e->message, sizeof(e->message), "%s", msg);
    snprintf(e->key,     sizeof(e->key),     "%s", key);
    sys_head = (sys_head + 1) % SYSTEM_LOG_SIZE;
    if (sys_count < SYSTEM_LOG_SIZE) sys_count++;
    sys_dirty = 1;
}

void EventLog_System(const char* source, const char* level, const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    size_t len = strlen(msg);
    while (len && (msg[len - 1] == '\n' || msg[len - 1] == '\r' || msg[len - 1] == ' ')) msg[--len] = '\0';
    if (!len) return;
    if (!source) source = "";
    if (!level) level = "info";

    char key[160];
    sys_key(key, sizeof(key), source, level, msg);
    double now = EventLog_Now();

    pthread_mutex_lock(&sys_lock);
    /* A repeat of a recent message: count it instead of adding an entry. */
    for (int i = 0; i < sys_count && i < 20; i++) {
        SysEntry* e = &sys_entries[(sys_head - 1 - i + SYSTEM_LOG_SIZE) % SYSTEM_LOG_SIZE];
        if (strcmp(e->key, key) == 0 && now - e->last_timestamp < SYS_REPEAT_WINDOW) {
            e->repeats++;
            e->last_timestamp = now;
            snprintf(e->message, sizeof(e->message), "%s", msg);
            sys_dirty = 1;
            pthread_mutex_unlock(&sys_lock);
            return;
        }
    }
    if (now - sys_window_start >= 60.0) {
        if (sys_suppressed > 0) {
            char note[128], nkey[160];
            snprintf(note, sizeof(note), "%d more message(s) not shown in the minute before; see the app log",
                     sys_suppressed);
            sys_key(nkey, sizeof(nkey), "system", "warning", note);
            sys_append_locked(now, "system", "warning", note, nkey);
        }
        sys_window_start = now;
        sys_window_count = 0;
        sys_suppressed = 0;
    }
    if (sys_window_count >= SYS_RATE_PER_MIN) {
        sys_suppressed++;
        pthread_mutex_unlock(&sys_lock);
        return;
    }
    sys_window_count++;
    sys_append_locked(now, source, level, msg, key);
    pthread_mutex_unlock(&sys_lock);
}

void EventLog_Syslog(int priority, const char* source, const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (source && *source) syslog(priority, "%s: %s", source, msg);
    else syslog(priority, "%s", msg);
    if (priority <= LOG_WARNING)
        EventLog_System(source && *source ? source : "engine", priority <= LOG_ERR ? "error" : "warning", "%s", msg);
}

static cJSON* sys_to_json(const SysEntry* e) {
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "kind",      "system");
    cJSON_AddNumberToObject(obj, "timestamp", e->timestamp);
    cJSON_AddStringToObject(obj, "source",    e->source);
    cJSON_AddStringToObject(obj, "level",     e->level);
    cJSON_AddStringToObject(obj, "message",   e->message);
    if (e->repeats > 1) {
        cJSON_AddNumberToObject(obj, "repeats",        e->repeats);
        cJSON_AddNumberToObject(obj, "last_timestamp", e->last_timestamp);
    }
    return obj;
}

cJSON* EventLog_Get_System(int limit) {
    pthread_mutex_lock(&sys_lock);
    if (limit <= 0 || limit > sys_count) limit = sys_count;
    cJSON* arr = cJSON_CreateArray();
    for (int i = 0; i < limit; i++)
        cJSON_AddItemToArray(arr, sys_to_json(&sys_entries[(sys_head - 1 - i + SYSTEM_LOG_SIZE) % SYSTEM_LOG_SIZE]));
    pthread_mutex_unlock(&sys_lock);
    return arr;
}

void EventLog_System_Clear(void) {
    pthread_mutex_lock(&sys_lock);
    memset(sys_entries, 0, sizeof(sys_entries));
    sys_head = sys_count = 0;
    sys_dirty = 1;
    pthread_mutex_unlock(&sys_lock);
}

static void system_persist(void) {
    pthread_mutex_lock(&sys_lock);
    if (!sys_dirty) { pthread_mutex_unlock(&sys_lock); return; }
    cJSON* arr = cJSON_CreateArray();
    for (int i = sys_count - 1; i >= 0; i--)
        cJSON_AddItemToArray(arr, sys_to_json(&sys_entries[(sys_head - 1 - i + SYSTEM_LOG_SIZE) % SYSTEM_LOG_SIZE]));
    sys_dirty = 0;
    pthread_mutex_unlock(&sys_lock);
    /* Written outside the lock: a failed write may itself log a warning. */
    ACAP_FILE_Write(SYSTEM_LOG_PERSIST_FILE, arr);
    cJSON_Delete(arr);
}

static void system_load(void) {
    cJSON* arr = ACAP_FILE_Read(SYSTEM_LOG_PERSIST_FILE);
    if (!cJSON_IsArray(arr)) { cJSON_Delete(arr); return; }
    pthread_mutex_lock(&sys_lock);
    cJSON* it;
    cJSON_ArrayForEach(it, arr) {
        const char* src = cJSON_GetStringValue(cJSON_GetObjectItem(it, "source"));
        const char* lvl = cJSON_GetStringValue(cJSON_GetObjectItem(it, "level"));
        const char* msg = cJSON_GetStringValue(cJSON_GetObjectItem(it, "message"));
        cJSON* ts = cJSON_GetObjectItem(it, "timestamp");
        if (!src || !lvl || !msg || !cJSON_IsNumber(ts)) continue;
        char key[160];
        sys_key(key, sizeof(key), src, lvl, msg);
        sys_append_locked(ts->valuedouble, src, lvl, msg, key);
        SysEntry* e = &sys_entries[(sys_head - 1 + SYSTEM_LOG_SIZE) % SYSTEM_LOG_SIZE];
        cJSON* rep = cJSON_GetObjectItem(it, "repeats");
        cJSON* last = cJSON_GetObjectItem(it, "last_timestamp");
        if (cJSON_IsNumber(rep) && rep->valueint > 1) e->repeats = rep->valueint;
        if (cJSON_IsNumber(last)) e->last_timestamp = last->valuedouble;
    }
    sys_dirty = 0;
    pthread_mutex_unlock(&sys_lock);
    cJSON_Delete(arr);
}

int EventLog_Init(void) {
    pthread_mutex_lock(&lock);
    memset(entries, 0, sizeof(entries));
    head = 0; count = 0;
    pthread_mutex_unlock(&lock);
    return 1;
}

void EventLog_Cleanup(void) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < count; i++) {
        int idx = (head - count + i + EVENT_LOG_SIZE) % EVENT_LOG_SIZE;
        if (entries[idx].trigger_snapshot)
            cJSON_Delete(entries[idx].trigger_snapshot);
    }
    memset(entries, 0, sizeof(entries));
    head = 0; count = 0;
    pthread_mutex_unlock(&lock);
}

void EventLog_Append(const char* rule_id, const char* rule_name,
                     int fired, const char* block_reason,
                     cJSON* trigger_data,
                     int actions_run, int actions_failed) {
    pthread_mutex_lock(&lock);

    LogEntry* e = &entries[head];
    /* Free previous occupant if overwriting */
    if (e->trigger_snapshot) {
        cJSON_Delete(e->trigger_snapshot);
        e->trigger_snapshot = NULL;
    }

    snprintf(e->rule_id,      sizeof(e->rule_id),      "%s", rule_id   ? rule_id   : "");
    snprintf(e->rule_name,    sizeof(e->rule_name),    "%s", rule_name ? rule_name : "");
    snprintf(e->block_reason, sizeof(e->block_reason), "%s", block_reason ? block_reason : "");
    e->timestamp      = EventLog_Now();
    e->fired          = fired;
    e->trigger_snapshot = trigger_data ? cJSON_Duplicate(trigger_data, 1) : NULL;
    e->actions_run    = actions_run;
    e->actions_failed = actions_failed;
    e->action_error[0] = '\0';

    head = (head + 1) % EVENT_LOG_SIZE;
    if (count < EVENT_LOG_SIZE) count++;

    persist_dirty++;

    pthread_mutex_unlock(&lock);
}

void EventLog_Set_Action_Error(const char* rule_id, const char* error_msg) {
    if (!rule_id || !error_msg) return;
    pthread_mutex_lock(&lock);
    /* Find the most recent entry for this rule and set the error */
    for (int i = 0; i < count; i++) {
        int idx = (head - 1 - i + EVENT_LOG_SIZE) % EVENT_LOG_SIZE;
        if (strcmp(entries[idx].rule_id, rule_id) == 0) {
            snprintf(entries[idx].action_error, sizeof(entries[idx].action_error), "%s", error_msg);
            if (entries[idx].actions_failed == 0) entries[idx].actions_failed = 1;
            break;
        }
    }
    pthread_mutex_unlock(&lock);
}

static cJSON* entry_to_json(LogEntry* e) {
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "kind",         "rule");
    cJSON_AddStringToObject(obj, "rule_id",      e->rule_id);
    cJSON_AddStringToObject(obj, "rule_name",    e->rule_name);
    cJSON_AddNumberToObject(obj, "timestamp",    e->timestamp);
    cJSON_AddBoolToObject(obj,   "fired",        e->fired);
    if (!e->fired && e->block_reason[0])
        cJSON_AddStringToObject(obj, "block_reason", e->block_reason);
    if (e->trigger_snapshot)
        cJSON_AddItemToObject(obj, "trigger_data", cJSON_Duplicate(e->trigger_snapshot, 1));
    if (e->actions_run > 0)
        cJSON_AddNumberToObject(obj, "actions_run", e->actions_run);
    if (e->actions_failed > 0) {
        cJSON_AddNumberToObject(obj, "actions_failed", e->actions_failed);
        if (e->action_error[0])
            cJSON_AddStringToObject(obj, "action_error", e->action_error);
    }
    return obj;
}

cJSON* EventLog_Get_Recent(int limit) {
    pthread_mutex_lock(&lock);
    if (limit <= 0 || limit > count) limit = count;
    cJSON* arr = cJSON_CreateArray();
    /* Most recent first */
    for (int i = 0; i < limit; i++) {
        int idx = (head - 1 - i + EVENT_LOG_SIZE) % EVENT_LOG_SIZE;
        if (i >= count) break;
        cJSON_AddItemToArray(arr, entry_to_json(&entries[idx]));
    }
    pthread_mutex_unlock(&lock);
    return arr;
}

cJSON* EventLog_Get_For_Rule(const char* rule_id, int limit) {
    if (!rule_id) return cJSON_CreateArray();
    pthread_mutex_lock(&lock);
    cJSON* arr = cJSON_CreateArray();
    int found = 0;
    for (int i = 0; i < count && (limit <= 0 || found < limit); i++) {
        int idx = (head - 1 - i + EVENT_LOG_SIZE) % EVENT_LOG_SIZE;
        if (strcmp(entries[idx].rule_id, rule_id) == 0) {
            cJSON_AddItemToArray(arr, entry_to_json(&entries[idx]));
            found++;
        }
    }
    pthread_mutex_unlock(&lock);
    return arr;
}

int EventLog_Count_Today(void) {
    time_t now = time(NULL);
    struct tm* tm_now = localtime(&now);
    int today_yday = tm_now->tm_yday;
    int today_year = tm_now->tm_year;

    pthread_mutex_lock(&lock);
    int cnt = 0;
    for (int i = 0; i < count; i++) {
        int idx = (head - 1 - i + EVENT_LOG_SIZE) % EVENT_LOG_SIZE;
        if (!entries[idx].fired) continue;
        time_t ts = (time_t)entries[idx].timestamp;
        struct tm* tm_e = localtime(&ts);
        if (tm_e->tm_yday == today_yday && tm_e->tm_year == today_year)
            cnt++;
    }
    pthread_mutex_unlock(&lock);
    return cnt;
}

void EventLog_Clear(void) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < EVENT_LOG_SIZE; i++) {
        if (entries[i].trigger_snapshot) {
            cJSON_Delete(entries[i].trigger_snapshot);
            entries[i].trigger_snapshot = NULL;
        }
    }
    memset(entries, 0, sizeof(entries));
    head = 0; count = 0;
    persist_dirty++;
    pthread_mutex_unlock(&lock);
    EventLog_System_Clear();
}

void EventLog_Persist(void) {
    pthread_mutex_lock(&lock);
    if (!persist_dirty) { pthread_mutex_unlock(&lock); system_persist(); return; }
    cJSON* arr = cJSON_CreateArray();
    /* Save up to 200 most recent entries */
    int save_count = count < 200 ? count : 200;
    for (int i = save_count - 1; i >= 0; i--) {
        int idx = (head - 1 - i + EVENT_LOG_SIZE) % EVENT_LOG_SIZE;
        cJSON_AddItemToArray(arr, entry_to_json(&entries[idx]));
    }
    ACAP_FILE_Write(EVENT_LOG_PERSIST_FILE, arr);
    cJSON_Delete(arr);
    persist_dirty = 0;
    pthread_mutex_unlock(&lock);
    system_persist();
}

typedef struct { cJSON* obj; double ts; int pos; } LoadItem;

static int load_item_cmp(const void* a, const void* b) {
    const LoadItem* x = a;
    const LoadItem* y = b;
    if (x->ts != y->ts) return x->ts < y->ts ? -1 : 1;
    return x->pos - y->pos;
}

void EventLog_Load(void) {
    system_load();
    cJSON* arr = ACAP_FILE_Read(EVENT_LOG_PERSIST_FILE);
    if (!arr || !cJSON_IsArray(arr)) { if (arr) cJSON_Delete(arr); return; }

    /* Sorted by time: files written by versions up to 1.9.15 may be newest-first. */
    int n = cJSON_GetArraySize(arr);
    LoadItem* items = n > 0 ? calloc((size_t)n, sizeof(LoadItem)) : NULL;
    int m = 0;
    cJSON* it;
    cJSON_ArrayForEach(it, arr) {
        if (!items) break;
        cJSON* ts_j = cJSON_GetObjectItem(it, "timestamp");
        items[m].obj = it;
        items[m].ts  = ts_j ? ts_j->valuedouble : 0;
        items[m].pos = m;
        m++;
    }
    if (m > 1) qsort(items, (size_t)m, sizeof(LoadItem), load_item_cmp);

    pthread_mutex_lock(&lock);
    int start = m > EVENT_LOG_SIZE ? m - EVENT_LOG_SIZE : 0;
    for (int i = start; i < m; i++) {
        cJSON* obj = items[i].obj;
        LogEntry* e = &entries[head];
        if (e->trigger_snapshot) { cJSON_Delete(e->trigger_snapshot); e->trigger_snapshot = NULL; }

        const char* rid = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "rule_id"));
        const char* rname = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "rule_name"));
        cJSON* ts_j = cJSON_GetObjectItem(obj, "timestamp");
        cJSON* fired_j = cJSON_GetObjectItem(obj, "fired");
        const char* br = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "block_reason"));

        snprintf(e->rule_id,      sizeof(e->rule_id),      "%s", rid   ? rid   : "");
        snprintf(e->rule_name,    sizeof(e->rule_name),    "%s", rname ? rname : "");
        snprintf(e->block_reason, sizeof(e->block_reason), "%s", br    ? br    : "");
        e->timestamp    = ts_j ? ts_j->valuedouble : 0;
        e->fired        = fired_j && cJSON_IsTrue(fired_j) ? 1 : 0;
        e->actions_run  = 0;
        e->actions_failed = 0;
        e->action_error[0] = '\0';

        cJSON* td = cJSON_GetObjectItem(obj, "trigger_data");
        e->trigger_snapshot = td ? cJSON_Duplicate(td, 1) : NULL;

        head = (head + 1) % EVENT_LOG_SIZE;
        if (count < EVENT_LOG_SIZE) count++;
    }
    persist_dirty = 0;
    pthread_mutex_unlock(&lock);
    free(items);
    cJSON_Delete(arr);
}
