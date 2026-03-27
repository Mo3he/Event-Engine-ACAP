#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <syslog.h>
#include <math.h>

#include "variables.h"
#include "../ACAP.h"

#define LOG(fmt, args...)      syslog(LOG_INFO,    "variables: " fmt, ## args)
#define LOG_WARN(fmt, args...) syslog(LOG_WARNING, "variables: " fmt, ## args)

#define MAX_VARS 256
#define VAR_NAME_LEN  64
#define VAR_VALUE_LEN 512

typedef struct {
    char   name[VAR_NAME_LEN];
    char   value[VAR_VALUE_LEN];
    double numeric;
    int    is_counter;
} Variable;

static Variable    store[MAX_VARS];
static int         store_count = 0;
static int         dirty = 0;
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
        if (var->is_counter)
            var->numeric = atof(var->value);
    }
    cJSON_Delete(json);
    LOG("loaded %d variables", store_count);
}

static void save_locked(void) {
    cJSON* arr = cJSON_CreateArray();
    for (int i = 0; i < store_count; i++) {
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name",  store[i].name);
        cJSON_AddStringToObject(obj, "value", store[i].value);
        cJSON_AddBoolToObject(obj,   "is_counter", store[i].is_counter);
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
