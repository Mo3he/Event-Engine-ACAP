#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "event_log.h"
#include "../ACAP.h"

typedef struct {
    char   rule_id[37];
    char   rule_name[128];
    time_t timestamp;
    int    fired;
    char   block_reason[64];
    cJSON* trigger_snapshot;   /* cJSON_Duplicate at append time */
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
                     cJSON* trigger_data) {
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
    e->timestamp = time(NULL);
    e->fired     = fired;
    e->trigger_snapshot = trigger_data ? cJSON_Duplicate(trigger_data, 1) : NULL;

    head = (head + 1) % EVENT_LOG_SIZE;
    if (count < EVENT_LOG_SIZE) count++;

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
