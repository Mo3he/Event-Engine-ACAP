#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <pthread.h>
#include <syslog.h>
#include <time.h>
#include <glib.h>

#include "sparkplug.h"
#include "sparkplug_pb.h"
#include "mqtt_client.h"
#include "../ACAP.h"

#define LOG(fmt, args...)      syslog(LOG_INFO,    "sparkplug: " fmt, ## args)
#define LOG_WARN(fmt, args...) syslog(LOG_WARNING, "sparkplug: " fmt, ## args)

#define SPB_NAMESPACE       "spBv1.0"
#define SPB_MAX_METRICS     128
#define SPB_HISTORY_MAX     256
#define SPB_VALUE_LEN       256
#define SPB_BDSEQ_FILE      "localdata/sparkplug_bdseq.json"

/* Reserved by the spec — never publish these as user metrics */
#define SPB_METRIC_BDSEQ    "bdSeq"
#define SPB_METRIC_REBIRTH  "Node Control/Rebirth"

/* =====================================================
 * State
 * ===================================================== */
typedef struct {
    char     name[128];
    int      datatype;
    uint64_t alias;

    /* Optional binding to a device event (auto-mapped, report by exception) */
    cJSON*   source;             /* topic0..topic3 declaration, NULL = rule-driven */
    char     source_path[200];   /* expected ACAP event path, derived from source */
    char     source_key[64];     /* which field of the event payload to read */
    int      sub_id;             /* ACAP event subscription id, 0 = none */

    char     value[SPB_VALUE_LEN];
    int      has_value;
    uint64_t value_ts;
} MetricDef;

typedef struct {
    int      def_idx;
    char     value[SPB_VALUE_LEN];
    uint64_t ts;
} Sample;

static pthread_mutex_t spb_lock = PTHREAD_MUTEX_INITIALIZER;

static int  enabled           = 0;
static int  spec_version      = SPB_SPEC_3_0;
static char group_id[128]     = "";
static char edge_node_id[128] = "";
static char device_id[128]    = "";
static char primary_host[128] = "";

static MetricDef defs[SPB_MAX_METRICS];
static int       def_count = 0;

/* Ring buffer of samples collected while the broker was unreachable */
static Sample history[SPB_HISTORY_MAX];
static int    hist_head  = 0;
static int    hist_count = 0;

static uint64_t bdseq      = 0;
static uint8_t  seq        = 0;
static int      birth_sent = 0;
static int      initialized = 0;
static uint64_t last_birth_ms = 0;
static uint64_t data_published = 0;
static uint64_t config_hash = 0;
static int      config_seen = 0;

/* spBv1.0/<group>/DCMD/<node>/<device> with three 128-byte ids */
#define SPB_TOPIC_LEN 512

static char cmd_filter_node[SPB_TOPIC_LEN] = "";
static char cmd_filter_dev[SPB_TOPIC_LEN]  = "";
static char state_filter[SPB_TOPIC_LEN]    = "";

/* ACAP event subscriptions may only be torn down on the GMainLoop thread, but
 * settings arrive on the FastCGI thread, so retired ids queue up here. */
static int pending_unsub[SPB_MAX_METRICS];
static int pending_unsub_count = 0;

static Sparkplug_Command_Fn command_fn = NULL;

/* =====================================================
 * Helpers
 * ===================================================== */
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

/* Sparkplug ids must not contain the MQTT topic separators or wildcards */
static int valid_id(const char* s) {
    if (!s || !s[0]) return 0;
    for (; *s; s++)
        if (*s == '/' || *s == '+' || *s == '#') return 0;
    return 1;
}

static void build_topic(char* out, size_t n, const char* msgtype) {
    if (device_id[0] && msgtype[0] == 'D')
        snprintf(out, n, SPB_NAMESPACE "/%s/%s/%s/%s", group_id, msgtype, edge_node_id, device_id);
    else
        snprintf(out, n, SPB_NAMESPACE "/%s/%s/%s", group_id, msgtype, edge_node_id);
}

static MetricDef* find_def(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < def_count; i++)
        if (strcmp(defs[i].name, name) == 0) return &defs[i];
    return NULL;
}

/* Render an event field the same way the rule engine renders template values */
static void json_to_text(cJSON* item, char* out, size_t n) {
    out[0] = '\0';
    if (!item) return;
    if (cJSON_IsBool(item))        snprintf(out, n, "%s", cJSON_IsTrue(item) ? "true" : "false");
    else if (cJSON_IsNumber(item)) {
        if (item->valuedouble == (double)(long long)item->valuedouble)
            snprintf(out, n, "%lld", (long long)item->valuedouble);
        else
            snprintf(out, n, "%g", item->valuedouble);
    }
    else if (cJSON_IsString(item)) snprintf(out, n, "%s", item->valuestring ? item->valuestring : "");
}

/* Mirror ACAP_EVENTS_Parse: topic levels joined by '/', topic0
 * "CameraApplicationPlatform" encoded as "acap". */
static void source_to_path(cJSON* source, char* out, size_t n) {
    out[0] = '\0';
    if (!source) return;
    const char* keys[] = { "topic0", "topic1", "topic2", "topic3", NULL };
    size_t pos = 0;
    for (int i = 0; keys[i] && pos + 1 < n; i++) {
        cJSON* t = cJSON_GetObjectItem(source, keys[i]);
        if (!t || !t->child || !t->child->valuestring || !t->child->valuestring[0]) continue;
        const char* val = (i == 0 && strcmp(t->child->valuestring, "CameraApplicationPlatform") == 0)
                          ? "acap" : t->child->valuestring;
        if (pos > 0) out[pos++] = '/';
        for (const char* p = val; *p && pos + 1 < n; p++) out[pos++] = *p;
    }
    out[pos < n ? pos : n - 1] = '\0';
}

/* FNV-1a over a field, with a separator so "ab"+"c" != "a"+"bc" */
static void hash_field(uint64_t* h, const char* s) {
    for (const char* p = s ? s : ""; *p; p++) {
        *h ^= (unsigned char)*p;
        *h *= 1099511628211ULL;
    }
    *h ^= 0xFFu;
    *h *= 1099511628211ULL;
}

/* =====================================================
 * Publishing
 * ===================================================== */
static int publish_payload(const char* msgtype, SPB_Metric* metrics, int count,
                           int has_seq, uint64_t use_seq, int qos) {
    char topic[512];
    build_topic(topic, sizeof(topic), msgtype);

    SPB_Buf buf;
    if (!SPB_Encode(&buf, now_ms(), has_seq, use_seq, metrics, count)) {
        LOG_WARN("failed to encode %s payload", msgtype);
        return 0;
    }
    int ok = MQTT_Publish_Binary(topic, buf.data, buf.len, 0, qos);
    SPB_Buf_Free(&buf);
    if (!ok) LOG_WARN("failed to publish %s", msgtype);
    return ok;
}

static uint64_t take_seq(void) {
    uint64_t s = seq;
    seq = (uint8_t)((seq + 1) & 0xFF);
    return s;
}

/* bdSeq must differ between MQTT sessions, including across app restarts, or a
 * late NDEATH from a previous session would look like it kills the current one. */
static void load_bdseq(void) {
    cJSON* obj = ACAP_FILE_Read(SPB_BDSEQ_FILE);
    if (!obj) return;
    cJSON* v = cJSON_GetObjectItem(obj, "bdseq");
    if (cJSON_IsNumber(v) && v->valuedouble >= 0) bdseq = (uint64_t)v->valuedouble;
    cJSON_Delete(obj);
}

static void save_bdseq(void) {
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "bdseq", (double)bdseq);
    ACAP_FILE_Write(SPB_BDSEQ_FILE, obj);
    cJSON_Delete(obj);
}

/* NDEATH carries only the bdSeq that identifies the session it kills. */
static int register_will(void) {
    SPB_Metric m;
    memset(&m, 0, sizeof(m));
    m.name         = SPB_METRIC_BDSEQ;
    m.datatype     = SPB_DT_UINT64;
    m.i64          = (int64_t)bdseq;
    m.timestamp_ms = now_ms();

    SPB_Buf buf;
    if (!SPB_Encode(&buf, m.timestamp_ms, 0, 0, &m, 1)) return 0;

    char topic[512];
    build_topic(topic, sizeof(topic), "NDEATH");
    int ok = MQTT_Set_Will(topic, buf.data, buf.len, 1, 0);
    SPB_Buf_Free(&buf);
    return ok;
}

/* Caller must hold spb_lock. */
static void publish_birth(void) {
    if (!enabled || !valid_id(group_id) || !valid_id(edge_node_id)) return;

    SPB_Metric m[SPB_MAX_METRICS + 2];
    memset(m, 0, sizeof(m));
    uint64_t ts = now_ms();
    int n = 0;

    m[n].name = SPB_METRIC_BDSEQ;   m[n].datatype = SPB_DT_UINT64;
    m[n].i64  = (int64_t)bdseq;     m[n].timestamp_ms = ts; n++;

    m[n].name = SPB_METRIC_REBIRTH; m[n].datatype = SPB_DT_BOOLEAN;
    m[n].i64  = 0;                  m[n].timestamp_ms = ts; n++;

    /* When a device id is set the metrics live under the device, so NBIRTH
     * declares only the node controls and DBIRTH carries the metric list. */
    int node_carries_metrics = device_id[0] ? 0 : 1;
    if (node_carries_metrics) {
        for (int i = 0; i < def_count; i++) {
            m[n].name         = defs[i].name;
            m[n].alias        = defs[i].alias;
            m[n].has_alias    = 1;
            m[n].datatype     = defs[i].datatype;
            m[n].timestamp_ms = defs[i].has_value ? defs[i].value_ts : ts;
            if (defs[i].has_value) SPB_Metric_Set_Value(&m[n], defs[i].value);
            else                   m[n].is_null = 1;
            n++;
        }
    }

    seq = 0;
    if (!publish_payload("NBIRTH", m, n, 1, take_seq(), 0)) return;

    if (!node_carries_metrics) {
        SPB_Metric d[SPB_MAX_METRICS];
        memset(d, 0, sizeof(d));
        for (int i = 0; i < def_count; i++) {
            d[i].name         = defs[i].name;
            d[i].alias        = defs[i].alias;
            d[i].has_alias    = 1;
            d[i].datatype     = defs[i].datatype;
            d[i].timestamp_ms = defs[i].has_value ? defs[i].value_ts : ts;
            if (defs[i].has_value) SPB_Metric_Set_Value(&d[i], defs[i].value);
            else                   d[i].is_null = 1;
        }
        if (!publish_payload("DBIRTH", d, def_count, 1, take_seq(), 0)) return;
    }

    birth_sent   = 1;
    last_birth_ms = ts;
    LOG("birth published (bdSeq=%llu, %d metrics)", (unsigned long long)bdseq, def_count);
}

/* Caller must hold spb_lock. Replays samples captured while offline. */
static void flush_history(void) {
    if (!hist_count) return;

    SPB_Metric m[SPB_HISTORY_MAX];
    memset(m, 0, sizeof(m));
    int n = 0;

    for (int k = 0; k < hist_count && n < SPB_HISTORY_MAX; k++) {
        Sample* s = &history[(hist_head + k) % SPB_HISTORY_MAX];
        if (s->def_idx < 0 || s->def_idx >= def_count) continue;
        m[n].name          = defs[s->def_idx].name;
        m[n].alias         = defs[s->def_idx].alias;
        m[n].has_alias     = 1;
        m[n].datatype      = defs[s->def_idx].datatype;
        m[n].timestamp_ms  = s->ts;
        m[n].is_historical = 1;
        SPB_Metric_Set_Value(&m[n], s->value);
        n++;
    }

    hist_head = hist_count = 0;
    if (n) {
        publish_payload(device_id[0] ? "DDATA" : "NDATA", m, n, 1, take_seq(), 0);
        LOG("replayed %d buffered metric samples", n);
    }
}

/* Caller must hold spb_lock. */
static void buffer_sample(int def_idx, const char* value, uint64_t ts) {
    int slot;
    if (hist_count < SPB_HISTORY_MAX) {
        slot = (hist_head + hist_count) % SPB_HISTORY_MAX;
        hist_count++;
    } else {
        slot = hist_head;                      /* full — drop the oldest */
        hist_head = (hist_head + 1) % SPB_HISTORY_MAX;
    }
    history[slot].def_idx = def_idx;
    history[slot].ts      = ts;
    snprintf(history[slot].value, sizeof(history[slot].value), "%s", value);
}

/* Caller must hold spb_lock. */
static int publish_data(SPB_Metric* metrics, int count) {
    if (!count) return 1;
    if (!birth_sent || !MQTT_Is_Connected()) return 0;
    int ok = publish_payload(device_id[0] ? "DDATA" : "NDATA", metrics, count, 1, take_seq(), 0);
    if (ok) data_published++;
    return ok;
}

/* =====================================================
 * Subscriptions
 * ===================================================== */
static void update_command_subscriptions(void) {
    char node[SPB_TOPIC_LEN], dev[SPB_TOPIC_LEN], state[SPB_TOPIC_LEN];
    node[0] = dev[0] = state[0] = '\0';

    if (enabled && valid_id(group_id) && valid_id(edge_node_id)) {
        snprintf(node, sizeof(node), SPB_NAMESPACE "/%s/NCMD/%s/#", group_id, edge_node_id);
        if (device_id[0])
            snprintf(dev, sizeof(dev), SPB_NAMESPACE "/%s/DCMD/%s/%s", group_id, edge_node_id, device_id);
        if (primary_host[0]) {
            /* Sparkplug 3.0 moved STATE under the spBv1.0 namespace */
            if (spec_version == SPB_SPEC_3_0)
                snprintf(state, sizeof(state), SPB_NAMESPACE "/STATE/%s", primary_host);
            else
                snprintf(state, sizeof(state), "STATE/%s", primary_host);
        }
    }

    struct { char* cur; size_t cur_sz; const char* next; } subs[] = {
        { cmd_filter_node, sizeof(cmd_filter_node), node  },
        { cmd_filter_dev,  sizeof(cmd_filter_dev),  dev   },
        { state_filter,    sizeof(state_filter),    state },
    };

    for (size_t i = 0; i < sizeof(subs) / sizeof(subs[0]); i++) {
        if (strcmp(subs[i].cur, subs[i].next) == 0) continue;
        if (subs[i].cur[0]) MQTT_Unsubscribe(subs[i].cur);
        snprintf(subs[i].cur, subs[i].cur_sz, "%s", subs[i].next);
        if (subs[i].cur[0]) MQTT_Subscribe(subs[i].cur);
    }
}

/* Auto-mapped metrics need their own ACAP event subscriptions, and those must
 * be created on the GMainLoop thread. */
static gboolean resubscribe_events_idle(gpointer data) {
    (void)data;
    pthread_mutex_lock(&spb_lock);

    for (int i = 0; i < pending_unsub_count; i++)
        ACAP_EVENTS_Unsubscribe(pending_unsub[i]);
    pending_unsub_count = 0;

    for (int i = 0; i < def_count; i++) {
        if (defs[i].sub_id || !defs[i].source) continue;

        cJSON* decl = cJSON_CreateObject();
        char name[160];
        snprintf(name, sizeof(name), "sparkplug_metric_%d", i);
        cJSON_AddStringToObject(decl, "name", name);
        const char* keys[] = { "topic0", "topic1", "topic2", "topic3", NULL };
        for (int k = 0; keys[k]; k++) {
            cJSON* t = cJSON_GetObjectItem(defs[i].source, keys[k]);
            if (t) cJSON_AddItemToObject(decl, keys[k], cJSON_Duplicate(t, 1));
        }
        defs[i].sub_id = ACAP_EVENTS_Subscribe(decl, NULL);
        cJSON_Delete(decl);
        if (!defs[i].sub_id)
            LOG_WARN("event subscribe failed for metric '%s'", defs[i].name);
    }
    pthread_mutex_unlock(&spb_lock);
    return G_SOURCE_REMOVE;
}

/* Caller must hold spb_lock. When defer is set the ACAP event subscriptions are
 * released later on the GMainLoop thread instead of here. */
static void clear_defs(int defer) {
    for (int i = 0; i < def_count; i++) {
        if (defs[i].sub_id) {
            if (defer && pending_unsub_count < SPB_MAX_METRICS)
                pending_unsub[pending_unsub_count++] = defs[i].sub_id;
            else
                ACAP_EVENTS_Unsubscribe(defs[i].sub_id);
        }
        if (defs[i].source) cJSON_Delete(defs[i].source);
    }
    memset(defs, 0, sizeof(defs));
    def_count = 0;
}

/* =====================================================
 * MQTT lifecycle
 * ===================================================== */
static void mqtt_state_cb(int state, void* user_data) {
    (void)user_data;

    if (state == MQTT_STATE_PRECONNECT) {
        pthread_mutex_lock(&spb_lock);
        if (enabled && valid_id(group_id) && valid_id(edge_node_id)) {
            bdseq++;
            save_bdseq();
            birth_sent = 0;
            register_will();
        } else {
            MQTT_Set_Will(NULL, NULL, 0, 0, 0);
        }
        pthread_mutex_unlock(&spb_lock);
        return;
    }

    if (state == MQTT_STATE_CONNECTED) {
        pthread_mutex_lock(&spb_lock);
        if (enabled && valid_id(group_id) && valid_id(edge_node_id)) {
            publish_birth();
            if (birth_sent) flush_history();
        }
        pthread_mutex_unlock(&spb_lock);
        return;
    }

    if (state == MQTT_STATE_DISCONNECTED) {
        pthread_mutex_lock(&spb_lock);
        birth_sent = 0;
        pthread_mutex_unlock(&spb_lock);
    }
}

/* =====================================================
 * Inbound commands
 * ===================================================== */
typedef struct {
    int  rebirth;
    char cmds[SPB_MAX_METRICS][128];
    char vals[SPB_MAX_METRICS][SPB_VALUE_LEN];
    int  cmd_count;
} CommandBatch;

static void command_metric_handler(const char* name, uint64_t alias, int has_alias,
                                   int datatype, const char* value_str, void* user) {
    (void)datatype;
    CommandBatch* batch = (CommandBatch*)user;

    /* Hosts may address a metric by alias instead of name */
    const char* resolved = name;
    char alias_name[128] = "";
    if ((!resolved || !resolved[0]) && has_alias) {
        for (int i = 0; i < def_count; i++) {
            if (defs[i].alias == alias) {
                snprintf(alias_name, sizeof(alias_name), "%s", defs[i].name);
                resolved = alias_name;
                break;
            }
        }
    }
    if (!resolved || !resolved[0]) return;

    if (strcmp(resolved, SPB_METRIC_REBIRTH) == 0) {
        if (value_str && strcasecmp(value_str, "true") == 0) batch->rebirth = 1;
        return;
    }
    if (batch->cmd_count >= SPB_MAX_METRICS) return;
    snprintf(batch->cmds[batch->cmd_count], 128, "%s", resolved);
    snprintf(batch->vals[batch->cmd_count], SPB_VALUE_LEN, "%s", value_str ? value_str : "");
    batch->cmd_count++;
}

void Sparkplug_On_MQTT_Message(const char* topic, const char* payload, int payload_len) {
    if (!topic || !enabled) return;

    pthread_mutex_lock(&spb_lock);
    int is_cmd   = (cmd_filter_node[0] && strncmp(topic, SPB_NAMESPACE "/", 8) == 0 &&
                    strstr(topic, "/NCMD/") != NULL) ||
                   (cmd_filter_dev[0] && strstr(topic, "/DCMD/") != NULL);
    int is_state = state_filter[0] && strstr(topic, "STATE/") != NULL &&
                   strstr(topic, primary_host) != NULL;
    pthread_mutex_unlock(&spb_lock);

    if (is_state) {
        /* A primary host coming back online invalidates our session state.
         * 2.2 sends the bare word ONLINE, 3.0 sends {"online":true,...}. */
        int host_online = payload && payload_len > 0 &&
                          (strstr(payload, "ONLINE") || strstr(payload, "\"online\":true") ||
                           strstr(payload, "\"online\": true"));
        if (host_online) {
            LOG("primary host '%s' online — rebirthing", primary_host);
            pthread_mutex_lock(&spb_lock);
            publish_birth();
            pthread_mutex_unlock(&spb_lock);
        }
        return;
    }
    if (!is_cmd || !payload || payload_len <= 0) return;

    CommandBatch batch;
    memset(&batch, 0, sizeof(batch));

    pthread_mutex_lock(&spb_lock);
    int parsed = SPB_Decode((const uint8_t*)payload, payload_len, command_metric_handler, &batch);
    if (parsed < 0) {
        LOG_WARN("malformed command payload on %s", topic);
        pthread_mutex_unlock(&spb_lock);
        return;
    }
    if (batch.rebirth) {
        LOG("rebirth requested by host");
        publish_birth();
    }
    pthread_mutex_unlock(&spb_lock);

    /* Run user callbacks outside the lock so rules can publish in response. */
    if (command_fn)
        for (int i = 0; i < batch.cmd_count; i++)
            command_fn(batch.cmds[i], batch.vals[i]);
}

/* =====================================================
 * Auto-mapped device events
 * ===================================================== */
void Sparkplug_On_Device_Event(cJSON* event) {
    if (!enabled || !event) return;
    const char* path = cJSON_GetStringValue(cJSON_GetObjectItem(event, "event"));
    if (!path) return;

    SPB_Metric m[SPB_MAX_METRICS];
    char       values[SPB_MAX_METRICS][SPB_VALUE_LEN];
    int        n = 0;
    uint64_t   ts = now_ms();

    memset(m, 0, sizeof(m));

    pthread_mutex_lock(&spb_lock);
    for (int i = 0; i < def_count && n < SPB_MAX_METRICS; i++) {
        if (!defs[i].source || !defs[i].source_path[0]) continue;

        size_t plen = strlen(defs[i].source_path);
        if (strncmp(path, defs[i].source_path, plen) != 0) continue;
        char next = path[plen];
        if (next != '\0' && next != '/') continue;

        cJSON* item = defs[i].source_key[0] ? cJSON_GetObjectItem(event, defs[i].source_key) : NULL;
        if (!item) continue;

        char text[SPB_VALUE_LEN];
        json_to_text(item, text, sizeof(text));

        /* Report by exception: only publish when the value actually changed */
        if (defs[i].has_value && strcmp(defs[i].value, text) == 0) continue;

        snprintf(defs[i].value, sizeof(defs[i].value), "%s", text);
        defs[i].has_value = 1;
        defs[i].value_ts  = ts;

        snprintf(values[n], SPB_VALUE_LEN, "%s", text);
        m[n].alias        = defs[i].alias;
        m[n].has_alias    = 1;
        m[n].datatype     = defs[i].datatype;
        m[n].timestamp_ms = ts;
        SPB_Metric_Set_Value(&m[n], values[n]);
        n++;

        if (!birth_sent || !MQTT_Is_Connected()) buffer_sample(i, text, ts);
    }

    if (n) publish_data(m, n);
    pthread_mutex_unlock(&spb_lock);
}

/* =====================================================
 * Rule-driven publish
 * ===================================================== */
int Sparkplug_Publish(cJSON* metrics) {
    if (!enabled) { LOG_WARN("publish ignored — Sparkplug is disabled"); return 0; }
    if (!metrics || !cJSON_IsArray(metrics)) return 0;

    SPB_Metric m[SPB_MAX_METRICS];
    char       values[SPB_MAX_METRICS][SPB_VALUE_LEN];
    int        n = 0;
    uint64_t   ts = now_ms();

    memset(m, 0, sizeof(m));

    pthread_mutex_lock(&spb_lock);
    cJSON* item;
    cJSON_ArrayForEach(item, metrics) {
        if (n >= SPB_MAX_METRICS) break;
        const char* name = cJSON_GetStringValue(cJSON_GetObjectItem(item, "name"));
        if (!name || !name[0]) continue;

        MetricDef* d = find_def(name);
        if (!d) {
            LOG_WARN("metric '%s' is not declared in Settings — not in the birth certificate, skipped", name);
            continue;
        }

        cJSON* val = cJSON_GetObjectItem(item, "value");
        char text[SPB_VALUE_LEN];
        if (cJSON_IsString(val)) snprintf(text, sizeof(text), "%s", val->valuestring ? val->valuestring : "");
        else                     json_to_text(val, text, sizeof(text));

        snprintf(d->value, sizeof(d->value), "%s", text);
        d->has_value = 1;
        d->value_ts  = ts;

        snprintf(values[n], SPB_VALUE_LEN, "%s", text);
        m[n].alias        = d->alias;
        m[n].has_alias    = 1;
        m[n].datatype     = d->datatype;
        m[n].timestamp_ms = ts;
        SPB_Metric_Set_Value(&m[n], values[n]);
        n++;

        if (!birth_sent || !MQTT_Is_Connected())
            buffer_sample((int)(d - defs), text, ts);
    }

    int ok = n ? publish_data(m, n) : 0;
    /* Buffered for replay counts as accepted */
    if (!ok && n && (!birth_sent || !MQTT_Is_Connected())) ok = 1;
    pthread_mutex_unlock(&spb_lock);
    return ok;
}

/* =====================================================
 * Configuration
 * ===================================================== */
int Sparkplug_Configure(cJSON* cfg) {
    if (!cfg) return 0;

    char new_group[128] = "", new_node[128] = "", new_dev[128] = "", new_host[128] = "";
    const char* s;
    if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "group_id"))))        snprintf(new_group, sizeof(new_group), "%s", s);
    if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "edge_node_id"))))    snprintf(new_node,  sizeof(new_node),  "%s", s);
    if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "device_id"))))       snprintf(new_dev,   sizeof(new_dev),   "%s", s);
    if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "primary_host_id")))) snprintf(new_host,  sizeof(new_host),  "%s", s);

    /* Default the node id to the device serial so two cameras never collide */
    if (!new_node[0]) {
        const char* serial = ACAP_DEVICE_Prop("serial");
        if (serial && serial[0]) snprintf(new_node, sizeof(new_node), "%s", serial);
    }

    int new_enabled = cJSON_IsTrue(cJSON_GetObjectItem(cfg, "enabled")) ? 1 : 0;
    int new_spec    = SPB_SPEC_3_0;
    if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "spec_version"))) && strcmp(s, "2.2") == 0)
        new_spec = SPB_SPEC_2_2;

    if (new_enabled && !valid_id(new_group)) {
        LOG_WARN("group_id is required and must not contain / + #");
        new_enabled = 0;
    }
    if (new_enabled && !valid_id(new_node)) {
        LOG_WARN("edge_node_id is invalid");
        new_enabled = 0;
    }
    if (new_dev[0] && !valid_id(new_dev)) {
        LOG_WARN("device_id is invalid — ignoring");
        new_dev[0] = '\0';
    }

    /* ACAP re-delivers the settings block on events other than a real edit.
     * Re-running the configuration would cycle the ACAP event subscriptions and
     * emit a spurious second NBIRTH, so hash it and ignore no-op updates. */
    uint64_t new_hash = 1469598103934665603ULL;
    char hbuf[32];
    snprintf(hbuf, sizeof(hbuf), "%d|%d", new_enabled, new_spec);
    hash_field(&new_hash, hbuf);
    hash_field(&new_hash, new_group);
    hash_field(&new_hash, new_node);
    hash_field(&new_hash, new_dev);
    hash_field(&new_hash, new_host);

    cJSON* sig_item;
    cJSON_ArrayForEach(sig_item, cJSON_GetObjectItem(cfg, "metrics")) {
        hash_field(&new_hash, cJSON_GetStringValue(cJSON_GetObjectItem(sig_item, "name")));
        hash_field(&new_hash, cJSON_GetStringValue(cJSON_GetObjectItem(sig_item, "datatype")));
        hash_field(&new_hash, cJSON_GetStringValue(cJSON_GetObjectItem(sig_item, "source_key")));
        char sig_path[200];
        source_to_path(cJSON_GetObjectItem(sig_item, "source"), sig_path, sizeof(sig_path));
        hash_field(&new_hash, sig_path);
    }

    pthread_mutex_lock(&spb_lock);

    if (config_seen && new_hash == config_hash) {
        pthread_mutex_unlock(&spb_lock);
        return 1;
    }
    config_hash = new_hash;
    config_seen = 1;

    int identity_changed = new_enabled != enabled ||
                           strcmp(new_group, group_id) != 0 ||
                           strcmp(new_node,  edge_node_id) != 0 ||
                           strcmp(new_dev,   device_id) != 0 ||
                           strcmp(new_host,  primary_host) != 0 ||
                           new_spec != spec_version;

    enabled      = new_enabled;
    spec_version = new_spec;
    snprintf(group_id,     sizeof(group_id),     "%s", new_group);
    snprintf(edge_node_id, sizeof(edge_node_id), "%s", new_node);
    snprintf(device_id,    sizeof(device_id),    "%s", new_dev);
    snprintf(primary_host, sizeof(primary_host), "%s", new_host);

    /* Rebuild the metric registry. Aliases are positional and stable for the
     * life of a session; any change to the list forces a new birth. */
    char prev_names[SPB_MAX_METRICS][128];
    char prev_values[SPB_MAX_METRICS][SPB_VALUE_LEN];
    int  prev_has[SPB_MAX_METRICS];
    int  prev_count = def_count;
    for (int i = 0; i < prev_count; i++) {
        snprintf(prev_names[i],  128,           "%.127s", defs[i].name);
        snprintf(prev_values[i], SPB_VALUE_LEN, "%.*s", SPB_VALUE_LEN - 1, defs[i].value);
        prev_has[i] = defs[i].has_value;
    }

    clear_defs(1);

    cJSON* list = cJSON_GetObjectItem(cfg, "metrics");
    cJSON* item;
    cJSON_ArrayForEach(item, list) {
        if (def_count >= SPB_MAX_METRICS) {
            LOG_WARN("metric limit (%d) reached — extra metrics ignored", SPB_MAX_METRICS);
            break;
        }
        const char* name = cJSON_GetStringValue(cJSON_GetObjectItem(item, "name"));
        if (!name || !name[0]) continue;
        if (strcmp(name, SPB_METRIC_BDSEQ) == 0 || strcmp(name, SPB_METRIC_REBIRTH) == 0) {
            LOG_WARN("'%s' is reserved by the Sparkplug spec — ignored", name);
            continue;
        }

        MetricDef* d = &defs[def_count];
        snprintf(d->name, sizeof(d->name), "%s", name);
        d->datatype = SPB_DataType_From_Name(cJSON_GetStringValue(cJSON_GetObjectItem(item, "datatype")));
        if (d->datatype == SPB_DT_UNKNOWN) d->datatype = SPB_DT_STRING;
        d->alias = (uint64_t)def_count + 1;   /* alias 0 is reserved */

        cJSON* source = cJSON_GetObjectItem(item, "source");
        if (source && cJSON_IsObject(source) && source->child) {
            d->source = cJSON_Duplicate(source, 1);
            source_to_path(d->source, d->source_path, sizeof(d->source_path));
            const char* key = cJSON_GetStringValue(cJSON_GetObjectItem(item, "source_key"));
            snprintf(d->source_key, sizeof(d->source_key), "%s", key && key[0] ? key : "state");
        }

        /* Carry forward the last known value so a rebirth is not all-null */
        for (int p = 0; p < prev_count; p++) {
            if (strcmp(prev_names[p], d->name) == 0 && prev_has[p]) {
                snprintf(d->value, sizeof(d->value), "%s", prev_values[p]);
                d->has_value = 1;
                d->value_ts  = now_ms();
                break;
            }
        }
        def_count++;
    }

    update_command_subscriptions();
    pthread_mutex_unlock(&spb_lock);

    g_idle_add(resubscribe_events_idle, NULL);

    if (!enabled) {
        MQTT_Set_Will(NULL, NULL, 0, 0, 0);
        pthread_mutex_lock(&spb_lock);
        birth_sent = 0;
        pthread_mutex_unlock(&spb_lock);
        return 1;
    }

    /* A new metric list or identity means the old birth certificate is stale.
     * Reconnecting is the only way to also refresh the registered will. */
    if (identity_changed && MQTT_Is_Connected()) {
        LOG("configuration changed — reconnecting to rebirth");
        MQTT_Force_Reconnect();
    } else if (MQTT_Is_Connected()) {
        pthread_mutex_lock(&spb_lock);
        publish_birth();
        pthread_mutex_unlock(&spb_lock);
    }
    return 1;
}

/* =====================================================
 * Public API
 * ===================================================== */
int Sparkplug_Init(void) {
    if (initialized) return 1;
    /* ACAP_Init() delivers the saved settings block before this runs, so the
     * metric registry may already be populated. Do not reset it here. */
    load_bdseq();
    MQTT_Set_State_Callback(mqtt_state_cb, NULL);
    initialized = 1;
    return 1;
}

void Sparkplug_Cleanup(void) {
    if (!initialized) return;
    MQTT_Set_State_Callback(NULL, NULL);

    pthread_mutex_lock(&spb_lock);
    if (enabled && birth_sent && MQTT_Is_Connected()) {
        SPB_Metric m;
        memset(&m, 0, sizeof(m));
        m.name         = SPB_METRIC_BDSEQ;
        m.datatype     = SPB_DT_UINT64;
        m.i64          = (int64_t)bdseq;
        m.timestamp_ms = now_ms();
        if (publish_payload("NDEATH", &m, 1, 0, 0, 1))
            LOG("NDEATH published (bdSeq=%llu)", (unsigned long long)bdseq);
    }
    birth_sent = 0;
    clear_defs(0);
    enabled = 0;
    pthread_mutex_unlock(&spb_lock);
    initialized = 0;
}

void Sparkplug_Set_Command_Callback(Sparkplug_Command_Fn fn) {
    command_fn = fn;
}

int Sparkplug_Is_Enabled(void) {
    return enabled;
}

cJSON* Sparkplug_Status(void) {
    cJSON* s = cJSON_CreateObject();
    pthread_mutex_lock(&spb_lock);
    cJSON_AddBoolToObject(s,   "enabled",        enabled);
    cJSON_AddStringToObject(s, "spec_version",   spec_version == SPB_SPEC_2_2 ? "2.2" : "3.0");
    cJSON_AddStringToObject(s, "group_id",       group_id);
    cJSON_AddStringToObject(s, "edge_node_id",   edge_node_id);
    cJSON_AddStringToObject(s, "device_id",      device_id);
    cJSON_AddStringToObject(s, "primary_host_id", primary_host);
    cJSON_AddBoolToObject(s,   "online",         enabled && birth_sent && MQTT_Is_Connected());
    cJSON_AddBoolToObject(s,   "birth_sent",     birth_sent);
    cJSON_AddNumberToObject(s, "bdseq",          (double)bdseq);
    cJSON_AddNumberToObject(s, "seq",            (double)seq);
    cJSON_AddNumberToObject(s, "metric_count",   def_count);
    cJSON_AddNumberToObject(s, "buffered",       hist_count);
    cJSON_AddNumberToObject(s, "data_published", (double)data_published);
    cJSON_AddNumberToObject(s, "last_birth_ms",  (double)last_birth_ms);

    char topic[512] = "";
    if (enabled && valid_id(group_id) && valid_id(edge_node_id))
        build_topic(topic, sizeof(topic), device_id[0] ? "DDATA" : "NDATA");
    cJSON_AddStringToObject(s, "data_topic", topic);
    pthread_mutex_unlock(&spb_lock);
    return s;
}

cJSON* Sparkplug_Metrics(void) {
    cJSON* arr = cJSON_CreateArray();
    pthread_mutex_lock(&spb_lock);
    for (int i = 0; i < def_count; i++) {
        cJSON* m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "name",     defs[i].name);
        cJSON_AddStringToObject(m, "datatype", SPB_DataType_Name(defs[i].datatype));
        cJSON_AddNumberToObject(m, "alias",    (double)defs[i].alias);
        cJSON_AddBoolToObject(m,   "auto",     defs[i].source ? 1 : 0);
        if (defs[i].source) {
            cJSON_AddStringToObject(m, "source_path", defs[i].source_path);
            cJSON_AddStringToObject(m, "source_key",  defs[i].source_key);
        }
        if (defs[i].has_value) cJSON_AddStringToObject(m, "value", defs[i].value);
        else                   cJSON_AddNullToObject(m,   "value");
        cJSON_AddItemToArray(arr, m);
    }
    pthread_mutex_unlock(&spb_lock);
    return arr;
}
