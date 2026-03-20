#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "triggers.h"
#include "scheduler.h"
#include "variables.h"
#include "mqtt_client.h"
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

typedef struct {
    int    type;
    char   rule_id[37];
    int    trigger_index;
    int    acap_subscription_id; /* for VAPIX subscriptions */

    /* vapix_event fields — stored as JSON for matching */
    cJSON* topic_filter;   /* {topic0:{ns:val}, topic1:{ns:val}, ...} */
    char   filter_key[64]; /* e.g. "active" */
    int    filter_value;   /* expected bool value (-1 = don't filter) */

    /* http_webhook */
    char   token[128];

    /* io_input */
    int    io_port;
    int    io_edge;    /* 0=rising, 1=falling, 2=both */

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

    char expected[200] = "";
    const char* keys[] = {"topic0", "topic1", "topic2", "topic3", NULL};
    for (int i = 0; keys[i]; i++) {
        cJSON* t = cJSON_GetObjectItem(s->topic_filter, keys[i]);
        if (!t || !t->child || !t->child->valuestring || !t->child->valuestring[0]) continue;
        if (expected[0]) strncat(expected, "/", sizeof(expected) - strlen(expected) - 1);
        strncat(expected, t->child->valuestring, sizeof(expected) - strlen(expected) - 1);
    }
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
        if (subs[i].topic_filter)
            cJSON_Delete(subs[i].topic_filter);
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
            if (subs[i].topic_filter)
                cJSON_Delete(subs[i].topic_filter);
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
        if (sub_count >= MAX_SUBS) { LOG_WARN("subscription table full"); break; }

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

            /* Optional value filter */
            const char* fkey = cJSON_GetStringValue(cJSON_GetObjectItem(trig, "filter_key"));
            if (fkey) snprintf(s->filter_key, sizeof(s->filter_key), "%s", fkey);
            cJSON* fval = cJSON_GetObjectItem(trig, "filter_value");
            if (fval) s->filter_value = cJSON_IsTrue(fval) ? 1 : 0;

            if (s->type == TRIG_IO_INPUT) {
                cJSON* port_j = cJSON_GetObjectItem(trig, "port");
                s->io_port = port_j ? (int)port_j->valuedouble : 1;
                const char* edge = cJSON_GetStringValue(cJSON_GetObjectItem(trig, "edge"));
                if (!edge || strcmp(edge, "rising") == 0) s->io_edge = 0;
                else if (strcmp(edge, "falling") == 0) s->io_edge = 1;
                else s->io_edge = 2;
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
        if (s->type != TRIG_VAPIX_EVENT && s->type != TRIG_IO_INPUT) continue;
        if (!event_topic_matches(s, event)) continue;

        /* Optional value filter — data fields are at root level of event */
        if (s->filter_key[0] && s->filter_value >= 0) {
            cJSON* fv = cJSON_GetObjectItem(event, s->filter_key);
            if (!fv) continue;
            int actual = cJSON_IsTrue(fv) ? 1 : 0;
            if (actual != s->filter_value) continue;
        }

        /* IO edge filter */
        if (s->type == TRIG_IO_INPUT && s->io_edge != 2) {
            cJSON* st = cJSON_GetObjectItem(event, "state");
            if (st) {
                int active = cJSON_IsTrue(st) ? 1 : 0;
                if (s->io_edge == 0 && !active) continue; /* rising = want active */
                if (s->io_edge == 1 &&  active) continue; /* falling = want inactive */
            }
        }

        /* Build trigger_data for template engine.
         * ACAP_EVENTS_Parse places all data fields at the root level of the event
         * JSON (no nested "data" key), so flatten everything from root directly. */
        cJSON* tdata = cJSON_CreateObject();
        cJSON_AddStringToObject(tdata, "type", s->type == TRIG_IO_INPUT ? "io_input" : "vapix_event");
        cJSON* item;
        cJSON_ArrayForEach(item, event) {
            if (!item->string) continue;
            if (strcmp(item->string, "event") == 0) continue; /* skip topic path meta key */
            if (!cJSON_GetObjectItem(tdata, item->string))
                cJSON_AddItemToObject(tdata, item->string, cJSON_Duplicate(item, 1));
        }

        fire_fn(s->rule_id, s->trigger_index, tdata);
        cJSON_Delete(tdata);
    }
}

int Triggers_On_Webhook(const char* token, cJSON* payload) {
    if (!fire_fn) return 0;
    int fired = 0;
    for (int i = 0; i < sub_count; i++) {
        Subscription* s = &subs[i];
        if (s->type != TRIG_HTTP_WEBHOOK) continue;
        if (s->token[0] && strcmp(s->token, token ? token : "") != 0) continue;

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

    return arr;
}
