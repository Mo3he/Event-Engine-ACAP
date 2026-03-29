#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include "triggers.h"
#include "scheduler.h"
#include "variables.h"
#include "mqtt_client.h"
#include "actions.h"
#include "../ACAP.h"

/* Simple MQTT topic filter matching (supports + and #) */
static int mqtt_topic_matches(const char* filter, const char* topic) {
    const char* f = filter;
    const char* t = topic;
    while (*f && *t) {
        if (*f == '#') return 1;   /* matches everything remaining */
        if (*f == '+') {
            /* skip one level in topic */
            while (*t && *t != '/') t++;
            f++;
        } else {
            if (*f != *t) return 0;
            f++; t++;
        }
    }
    /* Both exhausted, or filter ends with # */
    return (*f == '\0' && *t == '\0') || (*f == '#');
}

#define LOG(fmt, args...)      syslog(LOG_INFO,    "triggers: " fmt, ## args)
#define LOG_WARN(fmt, args...) syslog(LOG_WARNING, "triggers: " fmt, ## args)

#define MAX_SUBS 512
#define TRIG_VAPIX_EVENT       0
#define TRIG_HTTP_WEBHOOK      1
#define TRIG_SCHEDULE          2
#define TRIG_IO_INPUT          3
#define TRIG_COUNTER_THRESHOLD 4
#define TRIG_RULE_FIRED        5
#define TRIG_MQTT_MESSAGE      6
#define TRIG_AOA_SCENARIO      7
#define TRIG_MANUAL            8

typedef struct {
    int    type;
    char   rule_id[37];
    int    trigger_index;
    int    acap_subscription_id; /* for VAPIX subscriptions */

    /* vapix_event fields — stored as JSON for matching */
    cJSON* topic_filter;   /* {topic0:{ns:val}, topic1:{ns:val}, ...} */
    char   filter_key[64]; /* e.g. "active" (boolean filter) */
    int    filter_value;   /* expected bool value (-1 = don't filter) */

    /* Numeric value threshold filter (vapix_event only) */
    char   value_key[64];      /* field name to compare, e.g. "MaxTemp" */
    char   value_op[8];        /* "gt", "lt", "gte", "lte", "eq", "between" */
    double value_threshold;    /* comparison value (lower bound for "between") */
    double value_threshold2;   /* upper bound for "between" */
    int    value_hold_secs;    /* condition must hold for N secs; 0 = fire immediately */
    time_t value_since;        /* when hold condition first became true; 0 = not active */
    int    value_hysteresis;   /* 1 = fired for this activation; reset when cond drops */

    /* String value filter (vapix_event only) */
    char   string_key[64];     /* field name to match, e.g. "state" */
    char   string_value[256];  /* expected substring */

    /* Passive subscription — caches data but never fires rules */
    int    passive;
    cJSON* cached_data;        /* last-seen event data; we own this */

    /* http_webhook */
    char   token[128];

    /* io_input */
    int    io_port;
    int    io_edge;       /* 0=rising, 1=falling, 2=both */
    int    io_hold_secs;  /* must hold in triggered state for N secs; 0=fire immediately */
    time_t io_since;      /* when edge first matched; 0=not pending */
    int    io_last_state; /* last known state: 1=active, 0=inactive, -1=unknown */

    /* counter_threshold */
    char   counter_name[64];
    char   counter_op[8];
    double counter_threshold;
    int    counter_hysteresis; /* 1 = waiting for reset before re-firing */

    /* rule_fired */
    char   watched_rule_id[37];

    /* mqtt_message */
    char   mqtt_topic_filter[256]; /* supports MQTT wildcards + / # */
    char   mqtt_payload_filter[256]; /* optional substring match; empty = any payload */

    /* aoa_scenario */
    int    aoa_scenario_id;          /* AOA scenario number (1-based) */
    char   aoa_object_class[32];     /* optional: "human", "car", "bike", "truck", "bus", "" = any */
} Subscription;

static Subscription   subs[MAX_SUBS];
static int            sub_count = 0;
static Trigger_Fire_Fn fire_fn  = NULL;

/*-----------------------------------------------------
 * Scheduler callback bridge
 *-----------------------------------------------------*/
static void sched_fired(const char* rule_id, int trigger_index) {
    if (!fire_fn) return;
    cJSON* data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "type", "schedule");
    fire_fn(rule_id, trigger_index, data);
    cJSON_Delete(data);
}

/*-----------------------------------------------------
 * Numeric value comparison helper
 *-----------------------------------------------------*/
static int value_passes(double actual, const char* op, double threshold, double threshold2) {
    if (strcmp(op, "gt")      == 0) return actual >  threshold;
    if (strcmp(op, "lt")      == 0) return actual <  threshold;
    if (strcmp(op, "gte")     == 0) return actual >= threshold;
    if (strcmp(op, "lte")     == 0) return actual <= threshold;
    if (strcmp(op, "eq")      == 0) return actual == threshold;
    if (strcmp(op, "between") == 0) return actual >= threshold && actual <= threshold2;
    return 0;
}

/*-----------------------------------------------------
 * Build a VAPIX subscription declaration from trigger config
 *
 * Config keys: topic0, topic1, topic2, topic3 as {"namespace": "value"}
 *-----------------------------------------------------*/
static cJSON* build_subscription_decl(const char* rule_id, int tidx, cJSON* cfg) {
    cJSON* decl = cJSON_CreateObject();
    char name[128];
    snprintf(name, sizeof(name), "rule_%s_trigger_%d", rule_id, tidx);
    cJSON_AddStringToObject(decl, "name", name);

    const char* keys[] = {"topic0", "topic1", "topic2", "topic3", NULL};
    for (int i = 0; keys[i]; i++) {
        cJSON* t = cJSON_GetObjectItem(cfg, keys[i]);
        if (t) cJSON_AddItemToObject(decl, keys[i], cJSON_Duplicate(t, 1));
    }
    return decl;
}

/*-----------------------------------------------------
 * Check if a VAPIX event matches a subscription's topic filter
 *-----------------------------------------------------*/
static int event_topic_matches(Subscription* s, cJSON* event) {
    if (!s->topic_filter) return 1; /* no filter = match all */

    /* ACAP_EVENTS_Parse encodes topic levels into a single "event" path string
     * (e.g. "VideoSource/Thermometry/TemperatureDetection") rather than as
     * separate topic0/topic1/topic2 keys.  Build the expected path from the
     * filter's topic values (local names only; namespace prefixes are ignored
     * since they are lost during ACAP event parsing) and compare. */
    cJSON* event_path = cJSON_GetObjectItem(event, "event");
    if (!event_path || !event_path->valuestring) return 0;

    char expected[200];
    int pos = 0;
    const char* keys[] = {"topic0", "topic1", "topic2", "topic3", NULL};
    for (int i = 0; keys[i]; i++) {
        cJSON* t = cJSON_GetObjectItem(s->topic_filter, keys[i]);
        if (!t || !t->child || !t->child->valuestring || !t->child->valuestring[0]) continue;
        if (pos > 0 && pos < (int)sizeof(expected) - 1) expected[pos++] = '/';
        const char* val = t->child->valuestring;
        while (*val && pos < (int)sizeof(expected) - 1) expected[pos++] = *val++;
    }
    expected[pos] = '\0';
    /* Prefix match: if filter has fewer topic levels than the event, still match.
     * e.g. filter "VideoSource/Thermometry" matches event path
     * "VideoSource/Thermometry/TemperatureDetection". */
    size_t elen = strlen(expected);
    if (strncmp(event_path->valuestring, expected, elen) != 0) return 0;
    char next = event_path->valuestring[elen];
    return next == '\0' || next == '/';
}

/*-----------------------------------------------------
 * Public API
 *-----------------------------------------------------*/

int Triggers_Init(Trigger_Fire_Fn fn) {
    memset(subs, 0, sizeof(subs));
    sub_count = 0;
    fire_fn = fn;
    Scheduler_Init(sched_fired);
    return 1;
}

void Triggers_Cleanup(void) {
    for (int i = 0; i < sub_count; i++) {
        if (subs[i].acap_subscription_id > 0)
            ACAP_EVENTS_Unsubscribe(subs[i].acap_subscription_id);
        if (subs[i].topic_filter) cJSON_Delete(subs[i].topic_filter);
        if (subs[i].cached_data)  cJSON_Delete(subs[i].cached_data);
    }
    sub_count = 0;
    Scheduler_Cleanup();
}

void Triggers_Unsubscribe_Rule(const char* rule_id) {
    if (!rule_id) return;
    Scheduler_Unregister_Rule(rule_id);

    int i = 0;
    while (i < sub_count) {
        if (strcmp(subs[i].rule_id, rule_id) == 0) {
            if (subs[i].acap_subscription_id > 0)
                ACAP_EVENTS_Unsubscribe(subs[i].acap_subscription_id);
            if (subs[i].topic_filter) cJSON_Delete(subs[i].topic_filter);
            if (subs[i].cached_data)  cJSON_Delete(subs[i].cached_data);
            if (i < sub_count - 1)
                subs[i] = subs[sub_count - 1];
            sub_count--;
        } else {
            i++;
        }
    }
}

int Triggers_Subscribe_Rule(const char* rule_id, cJSON* triggers_array) {
    if (!rule_id || !triggers_array) return 0;

    /* Remove existing subs for this rule first */
    Triggers_Unsubscribe_Rule(rule_id);

    int idx = 0;
    cJSON* trig;
    cJSON_ArrayForEach(trig, triggers_array) {
        if (sub_count >= MAX_SUBS) { LOG_WARN("subscription table full (%d) — cannot subscribe more triggers", MAX_SUBS); break; }

        const char* type = cJSON_GetStringValue(cJSON_GetObjectItem(trig, "type"));
        if (!type) { idx++; continue; }

        Subscription* s = &subs[sub_count];
        memset(s, 0, sizeof(Subscription));
        snprintf(s->rule_id, sizeof(s->rule_id), "%s", rule_id);
        s->trigger_index = idx;
        s->filter_value  = -1; /* not filtering by value */

        if (strcmp(type, "vapix_event") == 0 || strcmp(type, "io_input") == 0) {
            s->type = (strcmp(type, "io_input") == 0) ? TRIG_IO_INPUT : TRIG_VAPIX_EVENT;

            /* Store topic filter for demuxing */
            cJSON* tf = cJSON_CreateObject();
            const char* tkeys[] = {"topic0","topic1","topic2","topic3",NULL};
            for (int k = 0; tkeys[k]; k++) {
                cJSON* t = cJSON_GetObjectItem(trig, tkeys[k]);
                if (t) cJSON_AddItemToObject(tf, tkeys[k], cJSON_Duplicate(t, 1));
            }
            s->topic_filter = tf;

            /* Optional boolean value filter */
            const char* fkey = cJSON_GetStringValue(cJSON_GetObjectItem(trig, "filter_key"));
            if (fkey) snprintf(s->filter_key, sizeof(s->filter_key), "%s", fkey);
            cJSON* fval = cJSON_GetObjectItem(trig, "filter_value");
            if (fval) s->filter_value = cJSON_IsTrue(fval) ? 1 : 0;

            /* Numeric value threshold filter */
            const char* vkey = cJSON_GetStringValue(cJSON_GetObjectItem(trig, "value_key"));
            if (vkey && vkey[0]) snprintf(s->value_key, sizeof(s->value_key), "%s", vkey);
            const char* vop = cJSON_GetStringValue(cJSON_GetObjectItem(trig, "value_op"));
            if (vop && vop[0]) snprintf(s->value_op, sizeof(s->value_op), "%s", vop);
            cJSON* vthresh = cJSON_GetObjectItem(trig, "value_threshold");
            if (vthresh) s->value_threshold = vthresh->valuedouble;
            cJSON* vthresh2 = cJSON_GetObjectItem(trig, "value_threshold2");
            if (vthresh2) s->value_threshold2 = vthresh2->valuedouble;
            cJSON* vhold = cJSON_GetObjectItem(trig, "value_hold_secs");
            if (vhold) s->value_hold_secs = (int)vhold->valuedouble;

            /* String value filter */
            const char* skey = cJSON_GetStringValue(cJSON_GetObjectItem(trig, "string_key"));
            if (skey && skey[0]) snprintf(s->string_key, sizeof(s->string_key), "%s", skey);
            const char* sval = cJSON_GetStringValue(cJSON_GetObjectItem(trig, "string_value"));
            if (sval && sval[0]) snprintf(s->string_value, sizeof(s->string_value), "%s", sval);

            if (s->type == TRIG_IO_INPUT) {
                cJSON* port_j = cJSON_GetObjectItem(trig, "port");
                s->io_port = port_j ? (int)port_j->valuedouble : 1;
                const char* edge = cJSON_GetStringValue(cJSON_GetObjectItem(trig, "edge"));
                if (!edge || strcmp(edge, "rising") == 0) s->io_edge = 0;
                else if (strcmp(edge, "falling") == 0) s->io_edge = 1;
                else s->io_edge = 2;
                cJSON* hold_j = cJSON_GetObjectItem(trig, "hold_secs");
                if (hold_j) s->io_hold_secs = (int)hold_j->valuedouble;
                s->io_last_state = -1;
            }

            /* Subscribe to VAPIX events */
            cJSON* decl = build_subscription_decl(rule_id, idx, trig);
            int sub_id = ACAP_EVENTS_Subscribe(decl, NULL);
            cJSON_Delete(decl);
            s->acap_subscription_id = sub_id;
            if (!sub_id) LOG_WARN("VAPIX subscribe failed for rule %s trigger %d", rule_id, idx);

        } else if (strcmp(type, "http_webhook") == 0) {
            s->type = TRIG_HTTP_WEBHOOK;
            const char* tok = cJSON_GetStringValue(cJSON_GetObjectItem(trig, "token"));
            if (tok && strlen(tok) >= sizeof(s->token))
                LOG_WARN("webhook token for rule %s truncated to %d chars", rule_id, (int)sizeof(s->token) - 1);
            snprintf(s->token, sizeof(s->token), "%s", tok ? tok : "");

        } else if (strcmp(type, "schedule") == 0) {
            s->type = TRIG_SCHEDULE;
            Scheduler_Register(rule_id, idx, trig);

        } else if (strcmp(type, "counter_threshold") == 0) {
            s->type = TRIG_COUNTER_THRESHOLD;
            const char* cname = cJSON_GetStringValue(cJSON_GetObjectItem(trig, "counter_name"));
            const char* cop   = cJSON_GetStringValue(cJSON_GetObjectItem(trig, "op"));
            cJSON* cval       = cJSON_GetObjectItem(trig, "value");
            snprintf(s->counter_name, sizeof(s->counter_name), "%s", cname ? cname : "");
            snprintf(s->counter_op,   sizeof(s->counter_op),   "%s", cop   ? cop   : "gt");
            s->counter_threshold = cval ? cval->valuedouble : 0;
            s->counter_hysteresis = 0;

        } else if (strcmp(type, "rule_fired") == 0) {
            s->type = TRIG_RULE_FIRED;
            const char* rid = cJSON_GetStringValue(cJSON_GetObjectItem(trig, "rule_id"));
            snprintf(s->watched_rule_id, sizeof(s->watched_rule_id), "%s", rid ? rid : "");

        } else if (strcmp(type, "mqtt_message") == 0) {
            s->type = TRIG_MQTT_MESSAGE;
            const char* tf = cJSON_GetStringValue(cJSON_GetObjectItem(trig, "topic_filter"));
            snprintf(s->mqtt_topic_filter, sizeof(s->mqtt_topic_filter), "%s", tf ? tf : "#");
            const char* pf = cJSON_GetStringValue(cJSON_GetObjectItem(trig, "payload_filter"));
            snprintf(s->mqtt_payload_filter, sizeof(s->mqtt_payload_filter), "%s", pf ? pf : "");
            MQTT_Subscribe(s->mqtt_topic_filter);

        } else if (strcmp(type, "aoa_scenario") == 0) {
            s->type = TRIG_AOA_SCENARIO;
            cJSON* sid_j = cJSON_GetObjectItem(trig, "scenario_id");
            s->aoa_scenario_id = sid_j ? (int)sid_j->valuedouble : 1;
            const char* oc = cJSON_GetStringValue(cJSON_GetObjectItem(trig, "object_class"));
            snprintf(s->aoa_object_class, sizeof(s->aoa_object_class), "%s", oc ? oc : "");

            /* Build topic filter for Device1Scenario<N> */
            char scen_name[32];
            snprintf(scen_name, sizeof(scen_name), "Device1Scenario%d", s->aoa_scenario_id);
            cJSON* tf = cJSON_CreateObject();
            cJSON* t0 = cJSON_CreateObject(); cJSON_AddStringToObject(t0, "axis", "CameraApplicationPlatform");
            cJSON* t1 = cJSON_CreateObject(); cJSON_AddStringToObject(t1, "axis", "ObjectAnalytics");
            cJSON* t2 = cJSON_CreateObject(); cJSON_AddStringToObject(t2, "axis", scen_name);
            cJSON_AddItemToObject(tf, "topic0", t0);
            cJSON_AddItemToObject(tf, "topic1", t1);
            cJSON_AddItemToObject(tf, "topic2", t2);
            s->topic_filter = tf;

            cJSON* decl = cJSON_CreateObject();
            char dname[128];
            snprintf(dname, sizeof(dname), "rule_%s_trigger_%d_aoa", rule_id, idx);
            cJSON_AddStringToObject(decl, "name", dname);
            cJSON_AddItemToObject(decl, "topic0", cJSON_Duplicate(t0, 1));
            cJSON_AddItemToObject(decl, "topic1", cJSON_Duplicate(t1, 1));
            cJSON_AddItemToObject(decl, "topic2", cJSON_Duplicate(t2, 1));
            int sub_id = ACAP_EVENTS_Subscribe(decl, NULL);
            cJSON_Delete(decl);
            s->acap_subscription_id = sub_id;
            if (!sub_id) LOG_WARN("AOA subscribe failed for rule %s scenario %d", rule_id, s->aoa_scenario_id);

        } else if (strcmp(type, "manual") == 0) {
            /* Manual trigger — no subscription; fires only via the HTTP fire endpoint */
            s->type = TRIG_MANUAL;

        } else {
            LOG_WARN("unknown trigger type '%s' for rule %s", type, rule_id);
            idx++;
            continue;
        }

        sub_count++;
        idx++;
    }
    return 1;
}

void Triggers_On_VAPIX_Event(cJSON* event) {
    if (!event || !fire_fn) return;

    for (int i = 0; i < sub_count; i++) {
        Subscription* s = &subs[i];
        if (s->type != TRIG_VAPIX_EVENT && s->type != TRIG_IO_INPUT && s->type != TRIG_AOA_SCENARIO) continue;
        if (!event_topic_matches(s, event)) continue;

        /* Update cache for all matching subs (passive and active).
         * Flatten root-level event fields (excluding the "event" path key). */
        cJSON_Delete(s->cached_data);
        s->cached_data = cJSON_CreateObject();
        cJSON* item;
        cJSON_ArrayForEach(item, event) {
            if (!item->string || strcmp(item->string, "event") == 0) continue;
            cJSON_AddItemToObject(s->cached_data, item->string, cJSON_Duplicate(item, 1));
        }

        /* Passive subscriptions only cache — they never fire rules */
        if (s->passive) continue;

        /* AOA scenario: require active=true, apply optional object-class filter */
        if (s->type == TRIG_AOA_SCENARIO) {
            cJSON* av = cJSON_GetObjectItem(event, "active");
            if (!av || !cJSON_IsTrue(av)) {
                Actions_Stop_Active_Siren(s->rule_id);
                continue;
            }
            if (s->aoa_object_class[0]) {
                const char* reason = cJSON_GetStringValue(cJSON_GetObjectItem(event, "reason"));
                if (!reason || strcmp(reason, s->aoa_object_class) != 0) continue;
            }
            cJSON* tdata = cJSON_CreateObject();
            cJSON_AddStringToObject(tdata, "type", "aoa_scenario");
            cJSON_AddNumberToObject(tdata, "scenario_id", s->aoa_scenario_id);
            cJSON* ci;
            cJSON_ArrayForEach(ci, s->cached_data) {
                if (!ci->string) continue;
                if (!cJSON_GetObjectItem(tdata, ci->string))
                    cJSON_AddItemToObject(tdata, ci->string, cJSON_Duplicate(ci, 1));
            }
            fire_fn(s->rule_id, s->trigger_index, tdata);
            cJSON_Delete(tdata);
            continue;
        }

        /* Boolean value filter */
        if (s->filter_key[0] && s->filter_value >= 0) {
            cJSON* fv = cJSON_GetObjectItem(event, s->filter_key);
            if (!fv || (cJSON_IsTrue(fv) ? 1 : 0) != s->filter_value) {
                Actions_Stop_Active_Siren(s->rule_id);
                continue;
            }
        } else if (!s->value_key[0] && !s->string_key[0]) {
            /* No filter — stop siren if event carries active=false */
            cJSON* av = cJSON_GetObjectItem(event, "active");
            if (av && cJSON_IsBool(av) && !cJSON_IsTrue(av))
                Actions_Stop_Active_Siren(s->rule_id);
        }

        /* String value filter */
        if (s->string_key[0] && s->string_value[0]) {
            cJSON* sv = cJSON_GetObjectItem(event, s->string_key);
            const char* svstr = cJSON_GetStringValue(sv);
            if (!svstr || strstr(svstr, s->string_value) == NULL) continue;
        }

        /* IO edge filter + hold duration */
        if (s->type == TRIG_IO_INPUT) {
            /* Filter by port number (if specified) */
            if (s->io_port > 0) {
                cJSON* port_j = cJSON_GetObjectItem(event, "port");
                int event_port = port_j && cJSON_IsNumber(port_j) ? (int)port_j->valuedouble : -1;
                if (event_port != s->io_port) continue; /* port mismatch — skip this event */
            }

            cJSON* st = cJSON_GetObjectItem(event, "state");
            int active = st ? (cJSON_IsTrue(st) ? 1 : 0) : -1;
            /* NOTE: io_last_state is currently unused; commented out to avoid dead code */
            /* if (active >= 0) s->io_last_state = active; */

            /* Check edge match */
            int edge_matches = 1;
            if (s->io_edge == 0 && active == 0) edge_matches = 0; /* rising  = want active   */
            if (s->io_edge == 1 && active == 1) edge_matches = 0; /* falling = want inactive */

            if (!edge_matches) {
                s->io_since = 0; /* cancel any pending hold */
                continue;
            }

            if (s->io_hold_secs > 0) {
                /* Start hold timer — actual fire happens in Triggers_Tick */
                if (!s->io_since) s->io_since = time(NULL);
                continue;
            }
            /* No hold — fall through to fire immediately */
        }

        /* Numeric value threshold filter */
        if (s->value_key[0] && s->value_op[0]) {
            cJSON* vitem = cJSON_GetObjectItem(event, s->value_key);
            if (!vitem || !cJSON_IsNumber(vitem)) {
                /* Field absent or non-numeric — reset and skip */
                s->value_since = 0; s->value_hysteresis = 0;
                continue;
            }
            int passes = value_passes(vitem->valuedouble, s->value_op, s->value_threshold, s->value_threshold2);
            if (!passes) {
                /* Condition dropped — reset so next activation can fire */
                if (s->value_hysteresis) Actions_Stop_Active_Siren(s->rule_id);
                s->value_since = 0; s->value_hysteresis = 0;
                continue;
            }
            /* Condition passes — apply hysteresis always so we fire once per activation */
            if (s->value_hysteresis) continue; /* already fired; waiting for reset */
            if (s->value_hold_secs > 0) {
                /* Must hold for N seconds before firing */
                if (!s->value_since) { s->value_since = time(NULL); continue; }
                if ((time(NULL) - s->value_since) < (time_t)s->value_hold_secs) continue;
            }
            /* Fire once and latch until condition clears */
            s->value_hysteresis = 1;
        }

        /* Build trigger_data from the cache we just populated */
        cJSON* tdata = cJSON_CreateObject();
        cJSON_AddStringToObject(tdata, "type", s->type == TRIG_IO_INPUT ? "io_input" : "vapix_event");
        cJSON* ci;
        cJSON_ArrayForEach(ci, s->cached_data) {
            if (!ci->string) continue;
            if (!cJSON_GetObjectItem(tdata, ci->string))
                cJSON_AddItemToObject(tdata, ci->string, cJSON_Duplicate(ci, 1));
        }

        fire_fn(s->rule_id, s->trigger_index, tdata);
        cJSON_Delete(tdata);
    }
}

/* Constant-time string comparison to prevent timing attacks on webhook tokens */
static int token_equals(const char* a, const char* b) {
    size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = (la != lb);
    size_t len = la < lb ? la : lb;
    for (size_t i = 0; i < len; i++)
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return diff == 0;
}

int Triggers_On_Webhook(const char* token, cJSON* payload) {
    if (!fire_fn) return 0;
    int fired = 0;
    for (int i = 0; i < sub_count; i++) {
        Subscription* s = &subs[i];
        if (s->type != TRIG_HTTP_WEBHOOK) continue;
        if (s->token[0] && !token_equals(s->token, token ? token : "")) continue;

        cJSON* tdata = cJSON_CreateObject();
        cJSON_AddStringToObject(tdata, "type", "http_webhook");
        if (payload) cJSON_AddItemToObject(tdata, "payload", cJSON_Duplicate(payload, 1));
        fire_fn(s->rule_id, s->trigger_index, tdata);
        cJSON_Delete(tdata);
        fired++;
    }
    return fired;
}

void Triggers_On_Rule_Fired(const char* fired_rule_id) {
    if (!fire_fn || !fired_rule_id) return;
    for (int i = 0; i < sub_count; i++) {
        Subscription* s = &subs[i];
        if (s->type != TRIG_RULE_FIRED) continue;
        if (s->watched_rule_id[0] &&
            strcmp(s->watched_rule_id, fired_rule_id) != 0) continue;

        cJSON* tdata = cJSON_CreateObject();
        cJSON_AddStringToObject(tdata, "type", "rule_fired");
        cJSON_AddStringToObject(tdata, "fired_rule_id", fired_rule_id);
        fire_fn(s->rule_id, s->trigger_index, tdata);
        cJSON_Delete(tdata);
    }
}

void Triggers_On_MQTT_Message(const char* topic, const char* payload, int payload_len) {
    (void)payload_len;
    if (!fire_fn || !topic) return;
    for (int i = 0; i < sub_count; i++) {
        Subscription* s = &subs[i];
        if (s->type != TRIG_MQTT_MESSAGE) continue;
        if (!mqtt_topic_matches(s->mqtt_topic_filter, topic)) continue;
        if (s->mqtt_payload_filter[0] &&
            (!payload || !strstr(payload, s->mqtt_payload_filter))) continue;

        cJSON* tdata = cJSON_CreateObject();
        cJSON_AddStringToObject(tdata, "type",    "mqtt_message");
        cJSON_AddStringToObject(tdata, "topic",   topic);
        cJSON_AddStringToObject(tdata, "payload", payload ? payload : "");
        fire_fn(s->rule_id, s->trigger_index, tdata);
        cJSON_Delete(tdata);
    }
}

void Triggers_Tick(void) {
    Scheduler_Tick();

    /* IO input hold-duration triggers */
    for (int i = 0; i < sub_count; i++) {
        Subscription* s = &subs[i];
        if (s->type != TRIG_IO_INPUT || !s->io_since || !s->io_hold_secs) continue;
        if ((time(NULL) - s->io_since) < (time_t)s->io_hold_secs) continue;
        s->io_since = 0; /* reset so it can re-arm on next edge */
        cJSON* tdata = cJSON_CreateObject();
        cJSON_AddStringToObject(tdata, "type", "io_input");
        if (s->cached_data) {
            cJSON* ci;
            cJSON_ArrayForEach(ci, s->cached_data) {
                if (ci->string && !cJSON_GetObjectItem(tdata, ci->string))
                    cJSON_AddItemToObject(tdata, ci->string, cJSON_Duplicate(ci, 1));
            }
        }
        fire_fn(s->rule_id, s->trigger_index, tdata);
        cJSON_Delete(tdata);
    }

    /* Counter threshold triggers */
    for (int i = 0; i < sub_count; i++) {
        Subscription* s = &subs[i];
        if (s->type != TRIG_COUNTER_THRESHOLD || !s->counter_name[0]) continue;

        int passes = Counter_Compare(s->counter_name, s->counter_op, s->counter_threshold);
        if (passes && !s->counter_hysteresis) {
            s->counter_hysteresis = 1;
            cJSON* tdata = cJSON_CreateObject();
            cJSON_AddStringToObject(tdata, "type", "counter_threshold");
            cJSON_AddStringToObject(tdata, "counter_name", s->counter_name);
            cJSON_AddNumberToObject(tdata, "counter_value", Counter_Get(s->counter_name));
            fire_fn(s->rule_id, s->trigger_index, tdata);
            cJSON_Delete(tdata);
        } else if (!passes) {
            s->counter_hysteresis = 0; /* reset — allow re-firing */
        }
    }
}

/*-----------------------------------------------------
 * Passive subscription helpers (for vapix_query actions)
 *-----------------------------------------------------*/

void Triggers_Subscribe_Passive(const char* rule_id, int action_idx, cJSON* action_cfg) {
    if (!rule_id || !action_cfg || sub_count >= MAX_SUBS) return;

    Subscription* s = &subs[sub_count];
    memset(s, 0, sizeof(Subscription));
    snprintf(s->rule_id, sizeof(s->rule_id), "%s", rule_id);
    s->type         = TRIG_VAPIX_EVENT;
    s->trigger_index = -1;
    s->filter_value  = -1;
    s->passive       = 1;

    cJSON* tf = cJSON_CreateObject();
    const char* tkeys[] = {"topic0","topic1","topic2","topic3",NULL};
    for (int k = 0; tkeys[k]; k++) {
        cJSON* t = cJSON_GetObjectItem(action_cfg, tkeys[k]);
        if (t) cJSON_AddItemToObject(tf, tkeys[k], cJSON_Duplicate(t, 1));
    }
    s->topic_filter = tf;

    char name[128];
    snprintf(name, sizeof(name), "passive_%s_%d", rule_id, action_idx);
    cJSON* decl = cJSON_CreateObject();
    cJSON_AddStringToObject(decl, "name", name);
    for (int k = 0; tkeys[k]; k++) {
        cJSON* t = cJSON_GetObjectItem(action_cfg, tkeys[k]);
        if (t) cJSON_AddItemToObject(decl, tkeys[k], cJSON_Duplicate(t, 1));
    }
    int sub_id = ACAP_EVENTS_Subscribe(decl, NULL);
    cJSON_Delete(decl);
    s->acap_subscription_id = sub_id;
    if (!sub_id) LOG_WARN("passive subscribe failed for rule %s action %d", rule_id, action_idx);

    sub_count++;
}

cJSON* Triggers_Get_Cached(cJSON* topic_cfg) {
    if (!topic_cfg) return NULL;

    /* Build the expected path string from the requested config */
    char expected[200] = "";
    const char* keys[] = {"topic0","topic1","topic2","topic3",NULL};
    for (int i = 0; keys[i]; i++) {
        cJSON* t = cJSON_GetObjectItem(topic_cfg, keys[i]);
        if (!t || !t->child || !t->child->valuestring || !t->child->valuestring[0]) continue;
        if (expected[0]) strncat(expected, "/", sizeof(expected) - strlen(expected) - 1);
        strncat(expected, t->child->valuestring, sizeof(expected) - strlen(expected) - 1);
    }

    for (int i = 0; i < sub_count; i++) {
        Subscription* s = &subs[i];
        if (!s->passive || !s->cached_data || !s->topic_filter) continue;

        /* Build this passive sub's path and compare */
        char sub_path[200] = "";
        for (int k = 0; keys[k]; k++) {
            cJSON* t = cJSON_GetObjectItem(s->topic_filter, keys[k]);
            if (!t || !t->child || !t->child->valuestring || !t->child->valuestring[0]) continue;
            if (sub_path[0]) strncat(sub_path, "/", sizeof(sub_path) - strlen(sub_path) - 1);
            strncat(sub_path, t->child->valuestring, sizeof(sub_path) - strlen(sub_path) - 1);
        }
        if (strcmp(expected, sub_path) == 0) return s->cached_data; /* borrowed ref */
    }
    return NULL;
}

int Triggers_Any_Active(const char* rule_id) {
    for (int i = 0; i < sub_count; i++) {
        Subscription* s = &subs[i];
        if (s->passive || strcmp(s->rule_id, rule_id) != 0) continue;
        if (s->type == TRIG_VAPIX_EVENT) {
            /* Threshold trigger is active if it fired and hasn't been reset yet */
            if (s->value_op[0] && (s->value_hysteresis || s->value_since > 0)) return 1;
        } else if (s->type == TRIG_COUNTER_THRESHOLD) {
            if (s->counter_hysteresis) return 1;
        }
        /* Other trigger types (schedule, webhook, mqtt, io_input, rule_fired) are
         * momentary — no persistent active state to check. */
    }
    return 0;
}

int Triggers_All_Currently_Active(const char* rule_id) {
    int found = 0;
    for (int i = 0; i < sub_count; i++) {
        Subscription* s = &subs[i];
        if (s->passive || strcmp(s->rule_id, rule_id) != 0) continue;
        found = 1;

        int active = 0;

        if (s->type == TRIG_VAPIX_EVENT || s->type == TRIG_IO_INPUT) {
            if (!s->cached_data) return 0;
            if (s->filter_key[0] && s->filter_value >= 0) {
                cJSON* fv = cJSON_GetObjectItem(s->cached_data, s->filter_key);
                active = fv && (cJSON_IsTrue(fv) ? 1 : 0) == s->filter_value;
            } else if (s->value_key[0] && s->value_op[0]) {
                cJSON* vi = cJSON_GetObjectItem(s->cached_data, s->value_key);
                active = vi && cJSON_IsNumber(vi) &&
                         value_passes(vi->valuedouble, s->value_op,
                                      s->value_threshold, s->value_threshold2);
            } else {
                cJSON* av = cJSON_GetObjectItem(s->cached_data, "active");
                active = !av || cJSON_IsTrue(av);
            }
        } else if (s->type == TRIG_AOA_SCENARIO) {
            if (!s->cached_data) return 0;
            cJSON* av = cJSON_GetObjectItem(s->cached_data, "active");
            active = av && cJSON_IsTrue(av);
        } else if (s->type == TRIG_COUNTER_THRESHOLD) {
            active = Counter_Compare(s->counter_name, s->counter_op, s->counter_threshold);
        } else {
            /* Momentary triggers (schedule, webhook, mqtt, rule_fired, manual)
             * have no persistent state — assumed active for AND_ACTIVE. */
            active = 1;
        }

        if (!active) return 0;
    }
    return found;
}

cJSON* Triggers_Catalog(void) {
    cJSON* arr = cJSON_CreateArray();

    cJSON* t;
    t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "type", "vapix_event");
    cJSON_AddStringToObject(t, "label", "VAPIX Event");
    cJSON_AddStringToObject(t, "description", "Fires when a VAPIX event topic matches");
    cJSON_AddItemToArray(arr, t);

    t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "type", "http_webhook");
    cJSON_AddStringToObject(t, "label", "HTTP Webhook");
    cJSON_AddStringToObject(t, "description", "Fires on POST /local/acap_event_engine/fire?token=TOKEN");
    cJSON_AddItemToArray(arr, t);

    t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "type", "schedule");
    cJSON_AddStringToObject(t, "label", "Schedule");
    cJSON_AddStringToObject(t, "description", "Fires on a cron, interval, or daily-time schedule");
    cJSON_AddItemToArray(arr, t);

    t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "type", "io_input");
    cJSON_AddStringToObject(t, "label", "I/O Input");
    cJSON_AddStringToObject(t, "description", "Fires on digital input state change");
    cJSON_AddItemToArray(arr, t);

    t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "type", "counter_threshold");
    cJSON_AddStringToObject(t, "label", "Counter Threshold");
    cJSON_AddStringToObject(t, "description", "Fires when a counter crosses a threshold");
    cJSON_AddItemToArray(arr, t);

    t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "type", "rule_fired");
    cJSON_AddStringToObject(t, "label", "Rule Fired");
    cJSON_AddStringToObject(t, "description", "Fires when another rule executes (chaining)");
    cJSON_AddItemToArray(arr, t);

    t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "type", "mqtt_message");
    cJSON_AddStringToObject(t, "label", "MQTT Message");
    cJSON_AddStringToObject(t, "description", "Fires when a message arrives on a subscribed MQTT topic (supports + and # wildcards)");
    cJSON_AddItemToArray(arr, t);

    t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "type", "aoa_scenario");
    cJSON_AddStringToObject(t, "label", "Object Analytics");
    cJSON_AddStringToObject(t, "description", "Fires when an Axis Object Analytics scenario is triggered");
    cJSON_AddItemToArray(arr, t);

    t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "type", "manual");
    cJSON_AddStringToObject(t, "label", "Manual");
    cJSON_AddStringToObject(t, "description", "No automatic trigger — fires only via the Fire button or POST /fire API");
    cJSON_AddItemToArray(arr, t);

    return arr;
}
