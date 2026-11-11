#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <syslog.h>
#include <math.h>

#include "event_log.h"
#include "variables.h"
#include "../ACAP.h"

#define LOG(fmt, args...)      syslog(LOG_INFO,    "variables: " fmt, ## args)
#define LOG_WARN(fmt, args...) EventLog_Syslog(LOG_WARNING, "variables", fmt, ## args)

#define MAX_VARS 256
#define VAR_NAME_LEN  64
#define VAR_VALUE_LEN 512

typedef struct {
    char   name[VAR_NAME_LEN];
    char   value[VAR_VALUE_LEN];
    double numeric;
    int    is_counter;
    int    persistent;              /* 0 = volatile: reset to initial at startup */
    char   initial[VAR_VALUE_LEN];
} Variable;

static Variable    store[MAX_VARS];
static int         store_count = 0;
static int         dirty = 0;
static char        startup_summary[1024] = "";
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static Variable* find_var(const char* name) {
    for (int i = 0; i < store_count; i++) {
        if (strcmp(store[i].name, name) == 0)
            return &store[i];
    }
    return NULL;
}

static Variable* find_or_create(const char* name) {
    Variable* v = find_var(name);
    if (v) return v;
    if (store_count >= MAX_VARS) {
        LOG_WARN("variable store full (%d) — cannot create '%s'", MAX_VARS, name);
        return NULL;
    }
    v = &store[store_count++];
    memset(v, 0, sizeof(Variable));
    snprintf(v->name, VAR_NAME_LEN, "%s", name);
    v->persistent = 1;
    return v;
}

static void load(void) {
    cJSON* json = ACAP_FILE_Read("localdata/variables.json");
    if (!json) return;

    cJSON* item;
    cJSON_ArrayForEach(item, json) {
        if (!cJSON_IsObject(item)) continue;
        cJSON* n = cJSON_GetObjectItem(item, "name");
        cJSON* v = cJSON_GetObjectItem(item, "value");
        cJSON* c = cJSON_GetObjectItem(item, "is_counter");
        if (!n || !n->valuestring) continue;

        Variable* var = find_or_create(n->valuestring);
        if (!var) continue;
        if (v && v->valuestring)
            snprintf(var->value, VAR_VALUE_LEN, "%s", v->valuestring);
        var->is_counter = (c && cJSON_IsTrue(c)) ? 1 : 0;
        cJSON* p = cJSON_GetObjectItem(item, "persistent");
        var->persistent = p ? cJSON_IsTrue(p) : 1;
        const char* init = cJSON_GetStringValue(cJSON_GetObjectItem(item, "initial"));
        snprintf(var->initial, VAR_VALUE_LEN, "%s", init ? init : "");
        if (!var->persistent) {
            if (var->is_counter && !var->initial[0]) snprintf(var->value, VAR_VALUE_LEN, "0");
            else snprintf(var->value, VAR_VALUE_LEN, "%s", var->initial);
        }
        if (var->is_counter)
            var->numeric = atof(var->value);
    }
    cJSON_Delete(json);

    /* One line for the event log: what was loaded, and what was reset. */
    int kept = 0, reset = 0;
    for (int i = 0; i < store_count; i++) {
        if (store[i].persistent) kept++;
        else reset++;
    }
    char* o = startup_summary;
    size_t left = sizeof(startup_summary);
    int w = snprintf(o, left, "Loaded %d variable(s)", store_count);
    for (int pass = 0; pass < 2 && w > 0 && (size_t)w < left; pass++) {
        int want_persistent = pass == 0;
        if (!(want_persistent ? kept : reset)) continue;
        o += w; left -= (size_t)w;
        w = snprintf(o, left, want_persistent ? ": " : "%sreset volatile: ", kept ? "; " : ": ");
        int first = 1;
        for (int i = 0; i < store_count && w > 0 && (size_t)w < left; i++) {
            if (store[i].persistent != want_persistent) continue;
            o += w; left -= (size_t)w;
            w = snprintf(o, left, "%s%s=%.40s", first ? "" : ", ", store[i].name, store[i].value);
            first = 0;
        }
    }
    if (w > 0 && (size_t)w >= left && sizeof(startup_summary) > 4)
        snprintf(startup_summary + sizeof(startup_summary) - 4, 4, "...");
    LOG("%s", startup_summary);
    if (reset) dirty = 1;
}

static void save_locked(void) {
    cJSON* arr = cJSON_CreateArray();
    for (int i = 0; i < store_count; i++) {
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name",  store[i].name);
        cJSON_AddStringToObject(obj, "value", store[i].value);
        cJSON_AddBoolToObject(obj,   "is_counter", store[i].is_counter);
        cJSON_AddBoolToObject(obj,   "persistent", store[i].persistent);
        if (!store[i].persistent || store[i].initial[0])
            cJSON_AddStringToObject(obj, "initial", store[i].initial);
        cJSON_AddItemToArray(arr, obj);
    }
    ACAP_FILE_Write("localdata/variables.json", arr);
    cJSON_Delete(arr);
    dirty = 0;
}

/*-----------------------------------------------------
 * Public API
 *-----------------------------------------------------*/

int Variables_Init(void) {
    pthread_mutex_lock(&lock);
    load();
    pthread_mutex_unlock(&lock);
    return 1;
}

void Variables_Startup_Summary(char* buf, size_t size) {
    if (!buf || !size) return;
    pthread_mutex_lock(&lock);
    snprintf(buf, size, "%s", startup_summary[0] ? startup_summary : "Loaded 0 variable(s)");
    pthread_mutex_unlock(&lock);
}

int Variables_Set_Options(const char* name, int persistent, const char* initial) {
    if (!name || !*name) return 0;
    pthread_mutex_lock(&lock);
    Variable* existing = find_var(name);
    Variable* v = existing ? existing : find_or_create(name);
    if (!v) { pthread_mutex_unlock(&lock); return 0; }
    if (initial) snprintf(v->initial, VAR_VALUE_LEN, "%s", initial);
    if (persistent >= 0) v->persistent = persistent ? 1 : 0;
    if (!existing) {
        /* A new variable starts at its initial value. */
        snprintf(v->value, VAR_VALUE_LEN, "%s", v->initial);
    }
    dirty = 1;
    pthread_mutex_unlock(&lock);
    return 1;
}

cJSON* Variables_List_Detail(void) {
    pthread_mutex_lock(&lock);
    cJSON* arr = cJSON_CreateArray();
    for (int i = 0; i < store_count; i++) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", store[i].name);
        if (store[i].is_counter) cJSON_AddNumberToObject(o, "value", store[i].numeric);
        else cJSON_AddStringToObject(o, "value", store[i].value);
        cJSON_AddBoolToObject(o, "is_counter", store[i].is_counter);
        cJSON_AddBoolToObject(o, "persistent", store[i].persistent);
        cJSON_AddStringToObject(o, "initial", store[i].initial);
        cJSON_AddItemToArray(arr, o);
    }
    pthread_mutex_unlock(&lock);
    return arr;
}

void Variables_Cleanup(void) {
    pthread_mutex_lock(&lock);
    if (dirty) save_locked();
    pthread_mutex_unlock(&lock);
}

void Variables_Flush(void) {
    pthread_mutex_lock(&lock);
    if (dirty) save_locked();
    pthread_mutex_unlock(&lock);
}

int Variables_Set(const char* name, const char* value) {
    if (!name || !value) return 0;
    pthread_mutex_lock(&lock);
    Variable* v = find_or_create(name);
    if (!v) { pthread_mutex_unlock(&lock); return 0; }
    snprintf(v->value, VAR_VALUE_LEN, "%s", value);
    v->is_counter = 0;
    v->numeric = 0;
    dirty = 1;
    pthread_mutex_unlock(&lock);
    return 1;
}

char* Variables_Get(const char* name) {
    if (!name) return NULL;
    pthread_mutex_lock(&lock);
    Variable* v = find_var(name);
    char* ret = v ? strdup(v->value) : NULL;
    pthread_mutex_unlock(&lock);
    return ret;
}

int Variables_Delete(const char* name) {
    if (!name) return 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < store_count; i++) {
        if (strcmp(store[i].name, name) == 0) {
            if (i < store_count - 1)
                store[i] = store[store_count - 1];
            store_count--;
            dirty = 1;
            pthread_mutex_unlock(&lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&lock);
    return 0;
}

int Variables_Compare(const char* name, const char* op, const char* value) {
    if (!name || !op || !value) return 0;
    pthread_mutex_lock(&lock);
    Variable* v = find_var(name);
    if (!v) { pthread_mutex_unlock(&lock); return 0; }
    int cmp = strcmp(v->value, value);
    pthread_mutex_unlock(&lock);
    if (strcmp(op, "eq") == 0) return cmp == 0;
    if (strcmp(op, "ne") == 0) return cmp != 0;
    if (strcmp(op, "lt") == 0) return cmp < 0;
    if (strcmp(op, "lte") == 0) return cmp <= 0;
    if (strcmp(op, "gt") == 0) return cmp > 0;
    if (strcmp(op, "gte") == 0) return cmp >= 0;
    return 0;
}

cJSON* Variables_List(void) {
    pthread_mutex_lock(&lock);
    cJSON* obj = cJSON_CreateObject();
    for (int i = 0; i < store_count; i++) {
        if (store[i].is_counter)
            cJSON_AddNumberToObject(obj, store[i].name, store[i].numeric);
        else
            cJSON_AddStringToObject(obj, store[i].name, store[i].value);
    }
    pthread_mutex_unlock(&lock);
    return obj;
}

int Counter_Set(const char* name, double value) {
    if (!name) return 0;
    pthread_mutex_lock(&lock);
    Variable* v = find_or_create(name);
    if (!v) { pthread_mutex_unlock(&lock); return 0; }
    v->is_counter = 1;
    v->numeric = value;
    snprintf(v->value, VAR_VALUE_LEN, "%g", value);
    dirty = 1;
    pthread_mutex_unlock(&lock);
    return 1;
}

double Counter_Get(const char* name) {
    if (!name) return 0.0;
    pthread_mutex_lock(&lock);
    Variable* v = find_var(name);
    double ret = v ? v->numeric : 0.0;
    pthread_mutex_unlock(&lock);
    return ret;
}

int Counter_Increment(const char* name, double delta) {
    if (!name) return 0;
    pthread_mutex_lock(&lock);
    Variable* v = find_or_create(name);
    if (!v) { pthread_mutex_unlock(&lock); return 0; }
    v->is_counter = 1;
    v->numeric += delta;
    snprintf(v->value, VAR_VALUE_LEN, "%g", v->numeric);
    dirty = 1;
    pthread_mutex_unlock(&lock);
    return 1;
}

int Counter_Reset(const char* name) {
    return Counter_Set(name, 0.0);
}

int Counter_Compare(const char* name, const char* op, double threshold) {
    double val = Counter_Get(name);
    if (strcmp(op, "eq") == 0)  return fabs(val - threshold) < 1e-9;
    if (strcmp(op, "ne") == 0)  return fabs(val - threshold) >= 1e-9;
    if (strcmp(op, "lt") == 0)  return val < threshold;
    if (strcmp(op, "lte") == 0) return val <= threshold;
    if (strcmp(op, "gt") == 0)  return val > threshold;
    if (strcmp(op, "gte") == 0) return val >= threshold;
    return 0;
}
