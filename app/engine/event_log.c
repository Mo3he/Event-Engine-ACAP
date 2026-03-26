#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "event_log.h"
#include "../ACAP.h"

#define EVENT_LOG_PERSIST_FILE "localdata/event_log.json"
static int persist_dirty = 0;  /* pending writes since last flush */

typedef struct {
    char   rule_id[37];
    char   rule_name[128];
    time_t timestamp;
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
    e->timestamp      = time(NULL);
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
    cJSON_AddStringToObject(obj, "rule_id",      e->rule_id);
    cJSON_AddStringToObject(obj, "rule_name",    e->rule_name);
    cJSON_AddNumberToObject(obj, "timestamp",    (double)e->timestamp);
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
        struct tm* tm_e = localtime(&entries[idx].timestamp);
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
}

void EventLog_Persist(void) {
    pthread_mutex_lock(&lock);
    if (!persist_dirty) { pthread_mutex_unlock(&lock); return; }
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
}

void EventLog_Load(void) {
    cJSON* arr = ACAP_FILE_Read(EVENT_LOG_PERSIST_FILE);
    if (!arr || !cJSON_IsArray(arr)) { if (arr) cJSON_Delete(arr); return; }

    pthread_mutex_lock(&lock);
    /* Load entries in order (oldest first) */
    int n = cJSON_GetArraySize(arr);
    /* Load at most EVENT_LOG_SIZE entries, oldest first (arr is newest-first from save) */
    int start = n > EVENT_LOG_SIZE ? n - EVENT_LOG_SIZE : 0;
    for (int i = n - 1; i >= start; i--) {
        cJSON* obj = cJSON_GetArrayItem(arr, i);
        if (!obj) continue;
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
        e->timestamp    = ts_j ? (time_t)ts_j->valuedouble : 0;
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
    cJSON_Delete(arr);
}
