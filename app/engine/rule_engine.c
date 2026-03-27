#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <syslog.h>
#include <glib.h>
#include <fcntl.h>
#include <unistd.h>

#include "rule_engine.h"
#include "triggers.h"
#include "conditions.h"
#include "actions.h"
#include "event_log.h"
#include "variables.h"
#include "../ACAP.h"

#define LOG(fmt, args...)      syslog(LOG_INFO,    "rule_engine: " fmt, ## args)
#define LOG_WARN(fmt, args...) syslog(LOG_WARNING, "rule_engine: " fmt, ## args)

#define MAX_RULES 256

typedef struct {
    char    id[37];
    char    name[128];
    int     enabled;
    cJSON*  triggers_json;    /* retained ref */
    cJSON*  conditions_json;  /* retained ref */
    cJSON*  actions_json;     /* retained ref */
    int     trigger_logic;    /* 0=OR, 1=AND */
    int     condition_logic;  /* 0=AND, 1=OR */
    int     cooldown;
    int     max_executions;
    char    max_exec_period[8]; /* "minute", "hour", "day", or "" = lifetime */
    /* runtime state */
    time_t  last_fired;
    int     execution_count;
    int     period_exec_count;
    time_t  period_start;
    /* -1=unknown, 0=conditions were closed, 1=conditions were open */
    int     cond_window_state;
    /* set when a real trigger fired but was blocked by a time_window condition;
     * cleared when the rule actually fires or when a new rule load resets state */
    int     trigger_pending;
} Rule;

static Rule            rules[MAX_RULES];
static int             rule_count = 0;
static pthread_mutex_t store_lock = PTHREAD_MUTEX_INITIALIZER;

/*-----------------------------------------------------
 * UUID generation (v4 using /dev/urandom)
 *-----------------------------------------------------*/
static void gen_uuid(char* out37) {
    unsigned char buf[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        if (read(fd, buf, sizeof(buf)) != sizeof(buf))
            memset(buf, 0, sizeof(buf)); /* fallback zeros — extremely unlikely */
        close(fd);
    } else {
        /* Fallback to rand() if /dev/urandom unavailable */
        for (int i = 0; i < 16; i++) buf[i] = (unsigned char)(rand() & 0xFF);
    }
    buf[6] = (buf[6] & 0x0F) | 0x40; /* version 4 */
    buf[8] = (buf[8] & 0x3F) | 0x80; /* variant 1 */
    snprintf(out37, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        buf[0],buf[1],buf[2],buf[3], buf[4],buf[5], buf[6],buf[7],
        buf[8],buf[9], buf[10],buf[11],buf[12],buf[13],buf[14],buf[15]);
}

/*-----------------------------------------------------
 * Persistence
 *-----------------------------------------------------*/
static void rules_save_locked(void) {
    cJSON* arr = cJSON_CreateArray();
    for (int i = 0; i < rule_count; i++) {
        Rule* r = &rules[i];
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "id",      r->id);
        cJSON_AddStringToObject(obj, "name",    r->name);
        cJSON_AddBoolToObject(obj,   "enabled", r->enabled);
        if (r->triggers_json)   cJSON_AddItemToObject(obj, "triggers",   cJSON_Duplicate(r->triggers_json,   1));
        if (r->conditions_json) cJSON_AddItemToObject(obj, "conditions", cJSON_Duplicate(r->conditions_json, 1));
        if (r->actions_json)    cJSON_AddItemToObject(obj, "actions",    cJSON_Duplicate(r->actions_json,    1));
        cJSON_AddStringToObject(obj, "trigger_logic",   r->trigger_logic   == 0 ? "OR"  : "AND");
        cJSON_AddStringToObject(obj, "condition_logic", r->condition_logic == 0 ? "AND" : "OR");
        cJSON_AddNumberToObject(obj, "cooldown",        r->cooldown);
        cJSON_AddNumberToObject(obj, "max_executions",  r->max_executions);
        if (r->max_exec_period[0]) cJSON_AddStringToObject(obj, "max_exec_period", r->max_exec_period);
        cJSON_AddItemToArray(arr, obj);
    }
    ACAP_FILE_Write("localdata/rules.json", arr);
    cJSON_Delete(arr);
}

static void rule_from_json(Rule* r, cJSON* obj) {
    const char* id   = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "id"));
    const char* name = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "name"));
    snprintf(r->id,   sizeof(r->id),   "%s", id   ? id   : "");
    snprintf(r->name, sizeof(r->name), "%s", name ? name : "Unnamed Rule");

    cJSON* en = cJSON_GetObjectItem(obj, "enabled");
    r->enabled = (en && cJSON_IsFalse(en)) ? 0 : 1;

    r->triggers_json   = cJSON_Duplicate(cJSON_GetObjectItem(obj, "triggers"),   1);
    r->conditions_json = cJSON_Duplicate(cJSON_GetObjectItem(obj, "conditions"), 1);
    r->actions_json    = cJSON_Duplicate(cJSON_GetObjectItem(obj, "actions"),    1);

    if (!r->triggers_json)   r->triggers_json   = cJSON_CreateArray();
    if (!r->conditions_json) r->conditions_json = cJSON_CreateArray();
    if (!r->actions_json)    r->actions_json    = cJSON_CreateArray();

    const char* tl = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "trigger_logic"));
    r->trigger_logic = (tl && strcmp(tl, "AND") == 0) ? 1 : 0;

    const char* cl = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "condition_logic"));
    r->condition_logic = (cl && strcmp(cl, "OR") == 0) ? 1 : 0;

    cJSON* cd = cJSON_GetObjectItem(obj, "cooldown");
    r->cooldown = cd ? (int)cd->valuedouble : 0;
    cJSON* mx = cJSON_GetObjectItem(obj, "max_executions");
    r->max_executions = mx ? (int)mx->valuedouble : 0;
    const char* mxp = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "max_exec_period"));
    snprintf(r->max_exec_period, sizeof(r->max_exec_period), "%s", mxp ? mxp : "");

    r->last_fired        = 0;
    r->execution_count   = 0;
    r->period_exec_count = 0;
    r->period_start      = 0;
    r->cond_window_state = -1;
    r->trigger_pending   = 0;
}

static void rule_free_json(Rule* r) {
    cJSON_Delete(r->triggers_json);
    cJSON_Delete(r->conditions_json);
    cJSON_Delete(r->actions_json);
    r->triggers_json   = NULL;
    r->conditions_json = NULL;
    r->actions_json    = NULL;
}

static int rule_has_time_window(Rule* r) {
    if (!r->conditions_json) return 0;
    cJSON* c;
    cJSON_ArrayForEach(c, r->conditions_json) {
        const char* type = cJSON_GetStringValue(cJSON_GetObjectItem(c, "type"));
        if (type && strcmp(type, "time_window") == 0) return 1;
    }
    return 0;
}

/*-----------------------------------------------------
 * Trigger fire callback (from triggers.c)
 * Called on GMainLoop thread.
 *-----------------------------------------------------*/
static void on_trigger_fired(const char* rule_id, int trigger_index, cJSON* trigger_data) {
    (void)trigger_index;

    pthread_mutex_lock(&store_lock);
    Rule* r = NULL;
    for (int i = 0; i < rule_count; i++) {
        if (strcmp(rules[i].id, rule_id) == 0) { r = &rules[i]; break; }
    }
    if (!r || !r->enabled) {
        pthread_mutex_unlock(&store_lock);
        return;
    }

    /* Cooldown check */
    if (r->cooldown > 0 && r->last_fired > 0) {
        time_t now = time(NULL);
        if ((now - r->last_fired) < r->cooldown) {
            EventLog_Append(r->id, r->name, 0, "cooldown", trigger_data, 0, 0);
            pthread_mutex_unlock(&store_lock);
            return;
        }
    }

    /* Max executions check */
    if (r->max_executions > 0) {
        if (r->max_exec_period[0]) {
            /* Rate limit: reset counter when period rolls over */
            time_t now = time(NULL);
            time_t period_secs = 60;
            if (strcmp(r->max_exec_period, "hour") == 0) period_secs = 3600;
            else if (strcmp(r->max_exec_period, "day") == 0) period_secs = 86400;
            if (r->period_start == 0 || (now - r->period_start) >= period_secs) {
                r->period_start = now;
                r->period_exec_count = 0;
            }
            if (r->period_exec_count >= r->max_executions) {
                EventLog_Append(r->id, r->name, 0, "max_executions", trigger_data, 0, 0);
                pthread_mutex_unlock(&store_lock);
                return;
            }
        } else {
            /* Lifetime limit */
            if (r->execution_count >= r->max_executions) {
                EventLog_Append(r->id, r->name, 0, "max_executions", trigger_data, 0, 0);
                pthread_mutex_unlock(&store_lock);
                return;
            }
        }
    }

    /* Condition evaluation */
    const char* ttype = trigger_data ? cJSON_GetStringValue(cJSON_GetObjectItem(trigger_data, "type")) : NULL;
    int is_refire = ttype && strcmp(ttype, "time_window_open") == 0;
    int cond_pass = Conditions_Evaluate(r->conditions_json, r->condition_logic, trigger_data);
    if (!cond_pass) {
        /* If a real trigger fired while the time window is closed, remember it so
         * we can re-attempt when the window reopens. */
        if (!is_refire && rule_has_time_window(r) && r->cond_window_state == 0)
            r->trigger_pending = 1;
        EventLog_Append(r->id, r->name, 0, "condition", trigger_data, 0, 0);
        pthread_mutex_unlock(&store_lock);
        return;
    }

    /* Execute */
    r->trigger_pending = 0;
    r->last_fired = time(NULL);
    r->execution_count++;
    if (r->max_exec_period[0]) r->period_exec_count++;
    char rid_copy[37]; snprintf(rid_copy, sizeof(rid_copy), "%s", r->id);
    char rname_copy[128]; snprintf(rname_copy, sizeof(rname_copy), "%s", r->name);
    cJSON* actions_dup = cJSON_Duplicate(r->actions_json, 1);
    cJSON* trigger_dup = trigger_data ? cJSON_Duplicate(trigger_data, 1) : cJSON_CreateObject();
    pthread_mutex_unlock(&store_lock);

    EventLog_Append(rid_copy, rname_copy, 1, NULL, trigger_data, 0, 0);
    cJSON* fired_data = cJSON_CreateObject();
    cJSON_AddStringToObject(fired_data, "rule_id",   rid_copy);
    cJSON_AddStringToObject(fired_data, "rule_name", rname_copy);
    ACAP_EVENTS_Fire_JSON("RuleFired", fired_data);
    cJSON_Delete(fired_data);

    Actions_Execute(rid_copy, actions_dup, trigger_dup);
    Triggers_On_Rule_Fired(rid_copy);

    cJSON_Delete(actions_dup);
    cJSON_Delete(trigger_dup);
}

/*-----------------------------------------------------
 * Idle callback to subscribe a new rule on the main loop
 *-----------------------------------------------------*/
typedef struct { char rule_id[37]; cJSON* triggers; } SubscribeWork;

static gboolean do_subscribe(gpointer data) {
    SubscribeWork* w = (SubscribeWork*)data;
    pthread_mutex_lock(&store_lock);
    for (int i = 0; i < rule_count; i++) {
        if (strcmp(rules[i].id, w->rule_id) == 0) {
            Triggers_Subscribe_Rule(rules[i].id, rules[i].triggers_json);

            /* Register passive subscriptions for any vapix_query actions so
             * their event data is cached and available when the action runs. */
            int aidx = 0;
            cJSON* action;
            cJSON_ArrayForEach(action, rules[i].actions_json) {
                const char* atype = cJSON_GetStringValue(cJSON_GetObjectItem(action, "type"));
                if (atype && strcmp(atype, "vapix_query") == 0)
                    Triggers_Subscribe_Passive(rules[i].id, aidx, action);
                aidx++;
            }
            break;
        }
    }
    pthread_mutex_unlock(&store_lock);
    cJSON_Delete(w->triggers);
    free(w);
    return G_SOURCE_REMOVE;
}

static void schedule_subscribe(const char* rule_id, cJSON* triggers) {
    SubscribeWork* w = malloc(sizeof(SubscribeWork));
    snprintf(w->rule_id, sizeof(w->rule_id), "%s", rule_id);
    w->triggers = cJSON_Duplicate(triggers, 1);
    g_idle_add(do_subscribe, w);
}

/*-----------------------------------------------------
 * Public API
 *-----------------------------------------------------*/

int RuleEngine_Init(void) {
    pthread_mutex_lock(&store_lock);
    memset(rules, 0, sizeof(rules));
    rule_count = 0;

    Actions_Init();
    Triggers_Init(on_trigger_fired);

    /* Load saved rules */
    cJSON* saved = NULL;
    if (ACAP_FILE_Exists("localdata/rules.json")) {
        saved = ACAP_FILE_Read("localdata/rules.json");
        if (!saved) {
            LOG_WARN("rules.json exists but could not be parsed — file may be corrupt, loading defaults");
            ACAP_STATUS_SetString("engine", "rules_load_error", "rules.json corrupt — defaults loaded");
        }
    }
    if (!saved) {
        saved = ACAP_FILE_Read("settings/default_rules.json");
        if (saved) LOG("first run or recovery — loading default rules");
    }

    if (saved && cJSON_IsArray(saved)) {
        cJSON* obj;
        cJSON_ArrayForEach(obj, saved) {
            if (rule_count >= MAX_RULES) break;
            rule_from_json(&rules[rule_count], obj);
            rule_count++;
        }
        cJSON_Delete(saved);
    }

    pthread_mutex_unlock(&store_lock);

    /* Subscribe all enabled rules */
    for (int i = 0; i < rule_count; i++) {
        if (rules[i].enabled)
            schedule_subscribe(rules[i].id, rules[i].triggers_json);
    }

    LOG("engine initialized with %d rules", rule_count);
    return 1;
}

void RuleEngine_Cleanup(void) {
    pthread_mutex_lock(&store_lock);
    Triggers_Cleanup();
    for (int i = 0; i < rule_count; i++) rule_free_json(&rules[i]);
    rule_count = 0;
    pthread_mutex_unlock(&store_lock);
}

cJSON* RuleEngine_List(void) {
    pthread_mutex_lock(&store_lock);
    cJSON* arr = cJSON_CreateArray();
    for (int i = 0; i < rule_count; i++) {
        Rule* r = &rules[i];
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "id",      r->id);
        cJSON_AddStringToObject(obj, "name",    r->name);
        cJSON_AddBoolToObject(obj,   "enabled", r->enabled);
        cJSON_AddNumberToObject(obj, "cooldown", r->cooldown);
        cJSON_AddNumberToObject(obj, "max_executions", r->max_executions);
        if (r->max_exec_period[0]) cJSON_AddStringToObject(obj, "max_exec_period", r->max_exec_period);
        cJSON_AddNumberToObject(obj, "execution_count", r->execution_count);
        cJSON_AddNumberToObject(obj, "last_fired", (double)r->last_fired);
        cJSON_AddNumberToObject(obj, "trigger_count",
                                r->triggers_json ? cJSON_GetArraySize(r->triggers_json) : 0);
        cJSON_AddNumberToObject(obj, "condition_count",
                                r->conditions_json ? cJSON_GetArraySize(r->conditions_json) : 0);
        cJSON_AddNumberToObject(obj, "action_count",
                                r->actions_json ? cJSON_GetArraySize(r->actions_json) : 0);
        /* Include type lists so the UI can build human-readable summaries */
        cJSON* ttypes = cJSON_CreateArray();
        if (r->triggers_json) {
            cJSON* t; cJSON_ArrayForEach(t, r->triggers_json) {
                cJSON* ty = cJSON_GetObjectItem(t, "type");
                if (ty && ty->valuestring) cJSON_AddItemToArray(ttypes, cJSON_CreateString(ty->valuestring));
            }
        }
        cJSON_AddItemToObject(obj, "trigger_types", ttypes);
        cJSON* ctypes = cJSON_CreateArray();
        if (r->conditions_json) {
            cJSON* c; cJSON_ArrayForEach(c, r->conditions_json) {
                cJSON* ty = cJSON_GetObjectItem(c, "type");
                if (ty && ty->valuestring) cJSON_AddItemToArray(ctypes, cJSON_CreateString(ty->valuestring));
            }
        }
        cJSON_AddItemToObject(obj, "condition_types", ctypes);
        cJSON* atypes = cJSON_CreateArray();
        if (r->actions_json) {
            cJSON* a; cJSON_ArrayForEach(a, r->actions_json) {
                cJSON* ty = cJSON_GetObjectItem(a, "type");
                if (ty && ty->valuestring) cJSON_AddItemToArray(atypes, cJSON_CreateString(ty->valuestring));
            }
        }
        cJSON_AddItemToObject(obj, "action_types", atypes);
        cJSON_AddItemToArray(arr, obj);
    }
    pthread_mutex_unlock(&store_lock);
    return arr;
}

cJSON* RuleEngine_Get(const char* id) {
    if (!id) return NULL;
    pthread_mutex_lock(&store_lock);
    cJSON* result = NULL;
    for (int i = 0; i < rule_count; i++) {
        if (strcmp(rules[i].id, id) == 0) {
            Rule* r = &rules[i];
            result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "id",      r->id);
            cJSON_AddStringToObject(result, "name",    r->name);
            cJSON_AddBoolToObject(result,   "enabled", r->enabled);
            if (r->triggers_json)   cJSON_AddItemToObject(result, "triggers",   cJSON_Duplicate(r->triggers_json,   1));
            if (r->conditions_json) cJSON_AddItemToObject(result, "conditions", cJSON_Duplicate(r->conditions_json, 1));
            if (r->actions_json)    cJSON_AddItemToObject(result, "actions",    cJSON_Duplicate(r->actions_json,    1));
            cJSON_AddStringToObject(result, "trigger_logic",   r->trigger_logic   == 0 ? "OR"  : "AND");
            cJSON_AddStringToObject(result, "condition_logic", r->condition_logic == 0 ? "AND" : "OR");
            cJSON_AddNumberToObject(result, "cooldown",        r->cooldown);
            cJSON_AddNumberToObject(result, "max_executions",  r->max_executions);
            if (r->max_exec_period[0]) cJSON_AddStringToObject(result, "max_exec_period", r->max_exec_period);
            cJSON_AddNumberToObject(result, "execution_count", r->execution_count);
            cJSON_AddNumberToObject(result, "last_fired",      (double)r->last_fired);
            break;
        }
    }
    pthread_mutex_unlock(&store_lock);
    return result;
}

int RuleEngine_Add(cJSON* rule_json, char* id_out_37) {
    if (!rule_json) return 0;
    pthread_mutex_lock(&store_lock);
    if (rule_count >= MAX_RULES) {
        LOG_WARN("rule limit reached (%d) — cannot add more rules", MAX_RULES);
        pthread_mutex_unlock(&store_lock);
        return 0;
    }
    Rule* r = &rules[rule_count];
    rule_from_json(r, rule_json);
    /* Generate or preserve ID */
    const char* given_id = cJSON_GetStringValue(cJSON_GetObjectItem(rule_json, "id"));
    if (!given_id || strlen(given_id) < 8) gen_uuid(r->id);
    if (id_out_37) snprintf(id_out_37, 37, "%s", r->id);
    rule_count++;
    rules_save_locked();
    cJSON* triggers_dup = cJSON_Duplicate(r->triggers_json, 1);
    char rid[37]; snprintf(rid, sizeof(rid), "%s", r->id);
    int enabled = r->enabled;
    pthread_mutex_unlock(&store_lock);

    if (enabled) schedule_subscribe(rid, triggers_dup);
    cJSON_Delete(triggers_dup);
    return 1;
}

int RuleEngine_Update(const char* id, cJSON* rule_json) {
    if (!id || !rule_json) return 0;
    pthread_mutex_lock(&store_lock);
    for (int i = 0; i < rule_count; i++) {
        if (strcmp(rules[i].id, id) == 0) {
            Rule* r = &rules[i];
            rule_free_json(r);
            rule_from_json(r, rule_json);
            snprintf(r->id, sizeof(r->id), "%s", id); /* preserve original ID */
            rules_save_locked();

            cJSON* triggers_dup = cJSON_Duplicate(r->triggers_json, 1);
            int enabled = r->enabled;
            pthread_mutex_unlock(&store_lock);

            Triggers_Unsubscribe_Rule(id);
            if (enabled) schedule_subscribe(id, triggers_dup);
            cJSON_Delete(triggers_dup);
            return 1;
        }
    }
    pthread_mutex_unlock(&store_lock);
    return 0;
}

int RuleEngine_Delete(const char* id) {
    if (!id) return 0;
    pthread_mutex_lock(&store_lock);
    for (int i = 0; i < rule_count; i++) {
        if (strcmp(rules[i].id, id) == 0) {
            rule_free_json(&rules[i]);
            if (i < rule_count - 1)
                rules[i] = rules[rule_count - 1];
            rule_count--;
            rules_save_locked();
            pthread_mutex_unlock(&store_lock);
            Triggers_Unsubscribe_Rule(id);
            return 1;
        }
    }
    pthread_mutex_unlock(&store_lock);
    return 0;
}

int RuleEngine_SetEnabled(const char* id, int enabled) {
    if (!id) return 0;
    pthread_mutex_lock(&store_lock);
    for (int i = 0; i < rule_count; i++) {
        if (strcmp(rules[i].id, id) == 0) {
            rules[i].enabled = enabled;
            cJSON* triggers_dup = cJSON_Duplicate(rules[i].triggers_json, 1);
            rules_save_locked();
            pthread_mutex_unlock(&store_lock);

            Triggers_Unsubscribe_Rule(id);
            if (enabled) schedule_subscribe(id, triggers_dup);
            cJSON_Delete(triggers_dup);
            return 1;
        }
    }
    pthread_mutex_unlock(&store_lock);
    return 0;
}

void RuleEngine_Dispatch_Event(cJSON* event) {
    Triggers_On_VAPIX_Event(event);
}

int RuleEngine_Dispatch_Webhook(const char* token, cJSON* payload) {
    return Triggers_On_Webhook(token, payload);
}

void RuleEngine_Dispatch_RuleFired(const char* rule_id) {
    Triggers_On_Rule_Fired(rule_id);
}

static int siren_condition_check(const char* rule_id, void* userdata) {
    (void)userdata;
    pthread_mutex_lock(&store_lock);
    for (int i = 0; i < rule_count; i++) {
        if (strcmp(rules[i].id, rule_id) == 0) {
            Rule* r = &rules[i];
            if (!r->enabled || !r->conditions_json || cJSON_GetArraySize(r->conditions_json) == 0) {
                pthread_mutex_unlock(&store_lock);
                return 1; /* no conditions — keep running */
            }
            int pass = Conditions_Evaluate_Lightweight(r->conditions_json, r->condition_logic);
            pthread_mutex_unlock(&store_lock);
            return pass;
        }
    }
    pthread_mutex_unlock(&store_lock);
    return 0; /* rule gone — stop siren */
}

void RuleEngine_Tick(void) {
    Triggers_Tick();
    Variables_Flush();
    Actions_ForEach_Active_Siren(siren_condition_check, NULL);

    /* Re-fire rules whose time_window condition just transitioned closed→open.
     * This handles the case where a stateful trigger (e.g. CO2 threshold) fired
     * before the window and is still active when the window reopens — no new
     * trigger event arrives because the underlying state never changed. */
    char (*refire_ids)[37] = NULL;
    int refire_count = 0;

    pthread_mutex_lock(&store_lock);
    if (rule_count > 0)
        refire_ids = calloc((size_t)rule_count, sizeof(*refire_ids));

    for (int i = 0; i < rule_count; i++) {
        Rule* r = &rules[i];
        if (!r->enabled || !rule_has_time_window(r)) continue;

        int now_open = Conditions_Evaluate_Lightweight(r->conditions_json, r->condition_logic);
        int prev = r->cond_window_state;
        r->cond_window_state = now_open;

        /* Window just closed — remember if a trigger is still active so we can
         * re-fire when the window reopens (even if no new trigger event arrives
         * while the window is closed). */
        if (prev == 1 && now_open == 0 && Triggers_Any_Active(r->id))
            r->trigger_pending = 1;

        if (refire_ids && prev == 0 && now_open == 1 && r->trigger_pending &&
            Triggers_Any_Active(r->id))
            snprintf(refire_ids[refire_count++], 37, "%s", r->id);
    }
    pthread_mutex_unlock(&store_lock);

    for (int i = 0; i < refire_count; i++) {
        cJSON* tdata = cJSON_CreateObject();
        cJSON_AddStringToObject(tdata, "type", "time_window_open");
        on_trigger_fired(refire_ids[i], -1, tdata);
        cJSON_Delete(tdata);
    }
    free(refire_ids);
}

int RuleEngine_Fire(const char* id, cJSON* trigger_data) {
    pthread_mutex_lock(&store_lock);
    Rule* r = NULL;
    for (int i = 0; i < rule_count; i++) {
        if (strcmp(rules[i].id, id) == 0) { r = &rules[i]; break; }
    }
    if (!r) { pthread_mutex_unlock(&store_lock); return 0; }

    cJSON* actions_dup = cJSON_Duplicate(r->actions_json, 1);
    char rid[37]; snprintf(rid, sizeof(rid), "%s", r->id);
    char rname[128]; snprintf(rname, sizeof(rname), "%s", r->name);
    pthread_mutex_unlock(&store_lock);

    cJSON* tdata = trigger_data ? trigger_data : cJSON_CreateObject();
    EventLog_Append(rid, rname, 1, "manual", tdata, 0, 0);
    Actions_Execute(rid, actions_dup, tdata);
    if (!trigger_data) cJSON_Delete(tdata);
    cJSON_Delete(actions_dup);
    return 1;
}

int RuleEngine_Count(void) {
    pthread_mutex_lock(&store_lock);
    int c = rule_count;
    pthread_mutex_unlock(&store_lock);
    return c;
}

int RuleEngine_Count_Enabled(void) {
    pthread_mutex_lock(&store_lock);
    int c = 0;
    for (int i = 0; i < rule_count; i++) if (rules[i].enabled) c++;
    pthread_mutex_unlock(&store_lock);
    return c;
}
