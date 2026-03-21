#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <glib.h>
#include <curl/curl.h>
#include <time.h>

#include "actions.h"
#include "triggers.h"
#include "variables.h"
#include "mqtt_client.h"
#include "../ACAP.h"

#define LOG(fmt, args...)      syslog(LOG_INFO,    "actions: " fmt, ## args)
#define LOG_WARN(fmt, args...) syslog(LOG_WARNING, "actions: " fmt, ## args)

/* Forward declaration — rule_engine.c provides this */
extern void RuleEngine_Dispatch_RuleFired(const char* rule_id);

/* Forward declarations needed by while_active_undo (defined later in this file) */
#define MAX_OVERLAY_CHANNELS 8
static int overlay_identity[MAX_OVERLAY_CHANNELS];
static int overlay_identity_init = 0;
static void overlay_remove(int identity);

/* Generic "while_active" tracking — one entry per active rule/action pair */
#define MAX_WHILE_ACTIVE 64
typedef struct {
    char rule_id[64];
    char atype[24];        /* "siren_light", "recording", "overlay_text", "io_output" */
    char siren_profile[128];
    int  overlay_channel;
    int  io_port;
    char io_restore[16];   /* opposite state to restore: "active" or "inactive" */
} WhileActiveEntry;
static WhileActiveEntry while_active_entries[MAX_WHILE_ACTIVE];
static int while_active_count = 0;

/* Register a new while_active entry, replacing any existing one for the same rule */
static void while_active_register(WhileActiveEntry* e) {
    for (int i = 0; i < while_active_count; i++) {
        if (strcmp(while_active_entries[i].rule_id, e->rule_id) == 0 &&
            strcmp(while_active_entries[i].atype,   e->atype)   == 0) {
            while_active_entries[i] = *e;
            return;
        }
    }
    if (while_active_count < MAX_WHILE_ACTIVE)
        while_active_entries[while_active_count++] = *e;
}

static void while_active_undo(WhileActiveEntry* e) {
    if (strcmp(e->atype, "siren_light") == 0) {
        cJSON* req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "apiVersion", "1.0");
        cJSON_AddStringToObject(req, "method", "stop");
        cJSON* p = cJSON_AddObjectToObject(req, "params");
        cJSON_AddStringToObject(p, "profile", e->siren_profile);
        char* body = cJSON_PrintUnformatted(req); cJSON_Delete(req);
        char* resp = ACAP_VAPIX_Post("siren_and_light.cgi", body);
        free(body); if (resp) free(resp);
        LOG("while_active: stopped siren '%s' for rule %s", e->siren_profile, e->rule_id);
    } else if (strcmp(e->atype, "recording") == 0) {
        cJSON* req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "apiVersion", "1.0");
        cJSON_AddStringToObject(req, "method", "stop");
        cJSON* p = cJSON_AddObjectToObject(req, "params");
        cJSON_AddStringToObject(p, "diskId", "SD_DISK");
        char* body = cJSON_PrintUnformatted(req); cJSON_Delete(req);
        char* resp = ACAP_VAPIX_Post("record.cgi", body);
        free(body); if (resp) free(resp);
        LOG("while_active: stopped recording for rule %s", e->rule_id);
    } else if (strcmp(e->atype, "overlay_text") == 0) {
        int ch = e->overlay_channel;
        if (ch >= 1 && ch <= MAX_OVERLAY_CHANNELS && overlay_identity[ch - 1] >= 0) {
            overlay_remove(overlay_identity[ch - 1]);
            overlay_identity[ch - 1] = -1;
            LOG("while_active: cleared overlay ch%d for rule %s", ch, e->rule_id);
        }
    } else if (strcmp(e->atype, "io_output") == 0) {
        char req[128];
        snprintf(req, sizeof(req), "io/port.cgi?action=%d:/%s", e->io_port, e->io_restore);
        char* resp = ACAP_VAPIX_Get(req); if (resp) free(resp);
        LOG("while_active: restored I/O port %d to %s for rule %s", e->io_port, e->io_restore, e->rule_id);
    }
}

void Actions_Stop_Active_Siren(const char* rule_id) {
    if (!rule_id) return;
    for (int i = 0; i < while_active_count; i++) {
        if (strcmp(while_active_entries[i].rule_id, rule_id) == 0 &&
            strcmp(while_active_entries[i].atype, "siren_light") == 0) {
            while_active_undo(&while_active_entries[i]);
            while_active_entries[i] = while_active_entries[--while_active_count];
            return;
        }
    }
}

void Actions_ForEach_Active_Siren(int (*cb)(const char* rule_id, void* userdata), void* userdata) {
    for (int i = 0; i < while_active_count; ) {
        if (!cb(while_active_entries[i].rule_id, userdata)) {
            while_active_undo(&while_active_entries[i]);
            while_active_entries[i] = while_active_entries[--while_active_count];
        } else {
            i++;
        }
    }
}

void Actions_Init(void) {
    while_active_count = 0;
}

/*-----------------------------------------------------
 * Template engine
 *
 * Replaces {{key}} tokens in tmpl.
 * Supported tokens:
 *   {{timestamp}}        ISO-8601 timestamp
 *   {{date}}             YYYY-MM-DD
 *   {{time}}             HH:MM:SS
 *   {{camera.name}}      device model
 *   {{camera.serial}}    device serial
 *   {{camera.ip}}        device IPv4
 *   {{trigger.KEY}}      individual key from trigger_data (e.g. {{trigger.AverageTemp}})
 *   {{trigger_json}}     all trigger data as a compact JSON object
 *   {{var.NAME}}         variable value
 *   {{counter.NAME}}     counter value
 *-----------------------------------------------------*/
char* Actions_Expand_Template(const char* tmpl, cJSON* trigger_data) {
    if (!tmpl) return strdup("");

    size_t out_cap = strlen(tmpl) * 2 + 256;
    char* out = malloc(out_cap);
    if (!out) return strdup(tmpl);
    out[0] = '\0';
    size_t out_len = 0;

    const char* p = tmpl;
    while (*p) {
        if (p[0] == '{' && p[1] == '{') {
            const char* end = strstr(p + 2, "}}");
            if (!end) { /* no closing — copy rest literally */
                size_t rem = strlen(p);
                if (out_len + rem + 1 > out_cap) {
                    out_cap = out_len + rem + 256;
                    out = realloc(out, out_cap);
                }
                memcpy(out + out_len, p, rem);
                out_len += rem;
                out[out_len] = '\0';
                break;
            }
            size_t key_len = end - (p + 2);
            char key[128] = "";
            if (key_len < sizeof(key)) {
                memcpy(key, p + 2, key_len);
                key[key_len] = '\0';
            }

            const char* replacement = NULL;
            char  tmp[256];
            char* dyn_replacement = NULL; /* malloc'd; freed after copying */

            if (strcmp(key, "timestamp") == 0) {
                replacement = ACAP_DEVICE_ISOTime();
            } else if (strcmp(key, "date") == 0) {
                replacement = ACAP_DEVICE_Date();
            } else if (strcmp(key, "time") == 0) {
                replacement = ACAP_DEVICE_Time();
            } else if (strcmp(key, "camera.name") == 0) {
                replacement = ACAP_DEVICE_Prop("model");
            } else if (strcmp(key, "camera.serial") == 0) {
                replacement = ACAP_DEVICE_Prop("serial");
            } else if (strcmp(key, "camera.ip") == 0) {
                replacement = ACAP_DEVICE_Prop("IPv4");
            } else if (strncmp(key, "trigger.", 8) == 0 && trigger_data) {
                cJSON* v = cJSON_GetObjectItem(trigger_data, key + 8);
                if (v) {
                    if (cJSON_IsString(v)) replacement = v->valuestring;
                    else { cJSON_PrintPreallocated(v, tmp, sizeof(tmp), 0); replacement = tmp; }
                }
            } else if (strcmp(key, "trigger_json") == 0 && trigger_data) {
                /* Serialize the full trigger data object, dropping the internal "type" key */
                cJSON* copy = cJSON_Duplicate(trigger_data, 1);
                if (copy) {
                    cJSON_DeleteItemFromObject(copy, "type");
                    dyn_replacement = cJSON_PrintUnformatted(copy);
                    cJSON_Delete(copy);
                    if (dyn_replacement) replacement = dyn_replacement;
                }
            } else if (strncmp(key, "var.", 4) == 0) {
                replacement = Variables_Get(key + 4);
            } else if (strncmp(key, "counter.", 8) == 0) {
                snprintf(tmp, sizeof(tmp), "%g", Counter_Get(key + 8));
                replacement = tmp;
            }

            if (!replacement) replacement = "";
            size_t rlen = strlen(replacement);
            if (out_len + rlen + 1 > out_cap) {
                out_cap = out_len + rlen + 256;
                out = realloc(out, out_cap);
            }
            memcpy(out + out_len, replacement, rlen);
            out_len += rlen;
            out[out_len] = '\0';
            free(dyn_replacement);
            p = end + 2;
        } else {
            if (out_len + 2 > out_cap) {
                out_cap = out_len + 256;
                out = realloc(out, out_cap);
            }
            out[out_len++] = *p++;
            out[out_len]   = '\0';
        }
    }
    return out;
}

/*-----------------------------------------------------
 * Individual action implementations
 *-----------------------------------------------------*/

/* Forward declaration — execute_from is defined later in this file, but called by action_http_request */
static void execute_from(const char* rule_id, cJSON* actions_array,
                         int start_index, cJSON* trigger_data);

/* Base64 encoding for snapshot attachment */
static const char b64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char* base64_encode(const unsigned char* data, size_t len) {
    size_t out_len = 4 * ((len + 2) / 3);
    char* out = malloc(out_len + 1);
    if (!out) return NULL;
    size_t i = 0, j = 0;
    while (i < len) {
        unsigned int a = (i < len) ? (unsigned char)data[i++] : 0;
        unsigned int b = (i < len) ? (unsigned char)data[i++] : 0;
        unsigned int c = (i < len) ? (unsigned char)data[i++] : 0;
        unsigned int triple = (a << 16) | (b << 8) | c;
        out[j++] = b64_chars[(triple >> 18) & 0x3F];
        out[j++] = b64_chars[(triple >> 12) & 0x3F];
        out[j++] = b64_chars[(triple >>  6) & 0x3F];
        out[j++] = b64_chars[(triple      ) & 0x3F];
    }
    if (len % 3 >= 1) out[out_len - 1] = '=';
    if (len % 3 == 1) out[out_len - 2] = '=';
    out[out_len] = '\0';
    return out;
}

/* http_request */
static size_t curl_discard(void* p, size_t sz, size_t n, void* ud) {
    (void)p; (void)ud; return sz * n;
}

static void action_http_request(const char* rule_id, cJSON* cfg, cJSON* trigger_data) {
    const char* url_tmpl = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "url"));
    if (!url_tmpl) return;

    /* Optional snapshot attachment: fetch JPEG and inject as {{snapshot_base64}} */
    cJSON* snap_j = cJSON_GetObjectItem(cfg, "attach_snapshot");
    char* snap_b64 = NULL;
    cJSON* td_with_snap = NULL; /* extended trigger_data that includes snapshot_base64 */
    if (snap_j && cJSON_IsTrue(snap_j)) {
        size_t snap_len = 0;
        char* snap_raw = ACAP_VAPIX_GetBinary("jpg/image.cgi", &snap_len);
        if (snap_raw && snap_len > 0) {
            snap_b64 = base64_encode((unsigned char*)snap_raw, snap_len);
            free(snap_raw);
        }
        if (snap_b64) {
            /* Add snapshot_base64 to a copy of trigger_data so {{trigger.snapshot_base64}} expands */
            td_with_snap = trigger_data ? cJSON_Duplicate(trigger_data, 1) : cJSON_CreateObject();
            if (td_with_snap)
                cJSON_AddStringToObject(td_with_snap, "snapshot_base64", snap_b64);
        }
    }
    cJSON* effective_td = td_with_snap ? td_with_snap : trigger_data;

    char* url    = Actions_Expand_Template(url_tmpl, effective_td);
    char* body   = NULL;
    const char* method = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "method"));
    if (!method) method = "GET";

    const char* body_tmpl = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "body"));
    if (body_tmpl) body = Actions_Expand_Template(body_tmpl, effective_td);

    if (td_with_snap) cJSON_Delete(td_with_snap);
    free(snap_b64);

    CURL* curl = curl_easy_init();
    if (!curl) { free(url); free(body); return; }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_discard);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    struct curl_slist* hdrs = NULL;
    const char* hdrs_str = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "headers"));
    if (hdrs_str) {
        /* Each header on its own line */
        char* hcopy = strdup(hdrs_str);
        char* line;
        char* saveptr;
        line = strtok_r(hcopy, "\n", &saveptr);
        while (line) {
            /* strip leading/trailing whitespace */
            while (*line == ' ' || *line == '\r') line++;
            char* end = line + strlen(line) - 1;
            while (end > line && (*end == ' ' || *end == '\r' || *end == '\n')) *end-- = '\0';
            if (*line) hdrs = curl_slist_append(hdrs, line);
            line = strtok_r(NULL, "\n", &saveptr);
        }
        free(hcopy);
    }

    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (body) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
        } else {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
        }
    } else if (strcmp(method, "PUT") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        if (body) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    } else if (strcmp(method, "DELETE") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    }

    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    else
        LOG_WARN("http_request to %s failed: %s", url, curl_easy_strerror(res));

    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    free(url);
    free(body);

    /* On failure (curl error or non-2xx), run on_failure actions if defined */
    int failed = (res != CURLE_OK) || (http_code < 200) || (http_code >= 300);
    if (failed) {
        cJSON* on_failure = cJSON_GetObjectItem(cfg, "on_failure");
        if (on_failure && cJSON_IsArray(on_failure) && cJSON_GetArraySize(on_failure) > 0) {
            LOG("http_request failed (curl=%d, http=%ld) — running on_failure actions", (int)res, http_code);
            execute_from(rule_id, on_failure, 0, trigger_data);
        }
    }
}

/* recording */
static void action_recording(const char* rule_id, cJSON* cfg) {
    const char* op = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "operation"));
    if (!op) op = "start";
    int starting = strcmp(op, "stop") != 0;

    cJSON* req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "apiVersion", "1.0");
    cJSON_AddStringToObject(req, "method", starting ? "start" : "stop");
    cJSON* params = cJSON_AddObjectToObject(req, "params");
    cJSON_AddStringToObject(params, "diskId", "SD_DISK");
    if (starting) {
        const char* profile = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "profile"));
        cJSON_AddStringToObject(params, "streamProfile", profile ? profile : "Quality");
        cJSON* dur = cJSON_GetObjectItem(cfg, "duration");
        if (dur) cJSON_AddNumberToObject(params, "maxRecordingTime", dur->valuedouble);
    }

    char* body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    char* resp = ACAP_VAPIX_Post("record.cgi", body);
    free(body);
    if (resp) free(resp);

    if (starting && rule_id) {
        cJSON* wa = cJSON_GetObjectItem(cfg, "while_active");
        if (wa && cJSON_IsTrue(wa)) {
            WhileActiveEntry e = {0};
            snprintf(e.rule_id, sizeof(e.rule_id), "%s", rule_id);
            snprintf(e.atype,   sizeof(e.atype),   "recording");
            while_active_register(&e);
        }
    }
}

/* overlay_text
 * Uses the VAPIX Dynamic Overlay API: POST dynamicoverlay/dynamicoverlay.cgi
 * We track one identity per channel (1-8) in memory so we reuse the same
 * overlay slot on repeated firings.  After reboot the identity is -1 and
 * a new overlay is created automatically.
 */
static void overlay_remove(int identity) {
    char body[64];
    snprintf(body, sizeof(body), "{\"apiVersion\":\"1.0\",\"method\":\"remove\",\"params\":{\"identity\":%d}}", identity);
    char* resp = ACAP_VAPIX_Post("dynamicoverlay/dynamicoverlay.cgi", body);
    if (resp) free(resp);
}

static gboolean overlay_remove_cb(gpointer user_data) {
    int identity = GPOINTER_TO_INT(user_data);
    overlay_remove(identity);
    /* Also clear from our tracking array if it matches */
    for (int i = 0; i < MAX_OVERLAY_CHANNELS; i++) {
        if (overlay_identity[i] == identity) overlay_identity[i] = -1;
    }
    return G_SOURCE_REMOVE;
}

static void action_overlay_text(const char* rule_id, cJSON* cfg, cJSON* trigger_data) {
    if (!overlay_identity_init) {
        for (int i = 0; i < MAX_OVERLAY_CHANNELS; i++) overlay_identity[i] = -1;
        overlay_identity_init = 1;
    }

    int channel = 1;
    cJSON* ch = cJSON_GetObjectItem(cfg, "channel");
    if (ch) channel = (int)ch->valuedouble;
    if (channel < 1 || channel > MAX_OVERLAY_CHANNELS) channel = 1;

    const char* text_tmpl = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "text"));
    if (!text_tmpl) return;
    char* text = Actions_Expand_Template(text_tmpl, trigger_data);

    const char* position   = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "position"));
    const char* text_color = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "text_color"));
    if (!position)   position   = "topLeft";
    if (!text_color) text_color = "white";

    cJSON* dur_j = cJSON_GetObjectItem(cfg, "duration");
    int duration = dur_j ? (int)dur_j->valuedouble : 0;

    int identity = overlay_identity[channel - 1];
    const char* method = (identity >= 0) ? "setText" : "addText";

    cJSON* req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "apiVersion", "1.0");
    cJSON_AddStringToObject(req, "method", method);
    cJSON* params = cJSON_AddObjectToObject(req, "params");
    cJSON_AddNumberToObject(params, "camera", channel);
    cJSON_AddStringToObject(params, "text", text);
    free(text);
    if (identity >= 0) {
        cJSON_AddNumberToObject(params, "identity", identity);
    } else {
        cJSON_AddStringToObject(params, "position",   position);
        cJSON_AddStringToObject(params, "textColor",  text_color);
    }

    char* body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    char* resp_str = ACAP_VAPIX_Post("dynamicoverlay/dynamicoverlay.cgi", body);
    free(body);

    if (resp_str) {
        cJSON* resp_j = cJSON_Parse(resp_str);
        free(resp_str);
        if (resp_j) {
            /* On setText failure (e.g. after reboot), reset and retry as addText */
            if (cJSON_GetObjectItem(resp_j, "error") && identity >= 0) {
                overlay_identity[channel - 1] = -1;
                cJSON_Delete(resp_j);
                action_overlay_text(rule_id, cfg, trigger_data); /* one retry */
                return;
            }
            /* On addText success, store the returned identity */
            cJSON* data = cJSON_GetObjectItem(resp_j, "data");
            if (data) {
                cJSON* id_j = cJSON_GetObjectItem(data, "identity");
                if (id_j) overlay_identity[channel - 1] = (int)id_j->valuedouble;
            }
            cJSON_Delete(resp_j);
        }
    }

    /* If duration > 0, schedule removal after that many seconds */
    if (duration > 0 && overlay_identity[channel - 1] >= 0) {
        g_timeout_add_seconds((guint)duration,
                              overlay_remove_cb,
                              GINT_TO_POINTER(overlay_identity[channel - 1]));
    } else if (duration == 0 && rule_id) {
        cJSON* wa = cJSON_GetObjectItem(cfg, "while_active");
        if (wa && cJSON_IsTrue(wa)) {
            WhileActiveEntry e = {0};
            snprintf(e.rule_id, sizeof(e.rule_id), "%s", rule_id);
            snprintf(e.atype,   sizeof(e.atype),   "overlay_text");
            e.overlay_channel = channel;
            while_active_register(&e);
        }
    }
}

/* ptz_preset */
static void action_ptz_preset(cJSON* cfg) {
    int channel = 1;
    cJSON* ch = cJSON_GetObjectItem(cfg, "channel");
    if (ch) channel = (int)ch->valuedouble;

    const char* preset = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "preset"));
    if (!preset) {
        cJSON* pid = cJSON_GetObjectItem(cfg, "preset_id");
        if (!pid) return;
        char req[128];
        snprintf(req, sizeof(req), "com/ptz.cgi?camera=%d&gotoserverpresetno=%d",
                 channel, (int)pid->valuedouble);
        char* resp = ACAP_VAPIX_Get(req);
        if (resp) free(resp);
    } else {
        char req[256];
        snprintf(req, sizeof(req), "com/ptz.cgi?camera=%d&gotoserverpresetname=%s",
                 channel, preset);
        char* resp = ACAP_VAPIX_Get(req);
        if (resp) free(resp);
    }
}

/* io_output */
static void action_io_output(const char* rule_id, cJSON* cfg) {
    cJSON* port_j = cJSON_GetObjectItem(cfg, "port");
    if (!port_j) return;
    int port = (int)port_j->valuedouble;
    const char* state = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "state"));
    if (!state) state = "active";

    cJSON* dur = cJSON_GetObjectItem(cfg, "duration");
    char req[128];
    if (dur && dur->valuedouble > 0) {
        snprintf(req, sizeof(req), "io/port.cgi?action=%d:/%s/%d",
                 port, state, (int)dur->valuedouble);
    } else {
        snprintf(req, sizeof(req), "io/port.cgi?action=%d:/%s", port, state);
    }
    char* resp = ACAP_VAPIX_Get(req);
    if (resp) free(resp);

    /* Register while_active only when no duration (duration handles its own reset) */
    if ((!dur || dur->valuedouble == 0) && rule_id) {
        cJSON* wa = cJSON_GetObjectItem(cfg, "while_active");
        if (wa && cJSON_IsTrue(wa)) {
            WhileActiveEntry e = {0};
            snprintf(e.rule_id, sizeof(e.rule_id), "%s", rule_id);
            snprintf(e.atype,   sizeof(e.atype),   "io_output");
            e.io_port = port;
            snprintf(e.io_restore, sizeof(e.io_restore), "%s",
                     strcmp(state, "active") == 0 ? "inactive" : "active");
            while_active_register(&e);
        }
    }
}

/* audio_clip */
static void action_audio_clip(cJSON* cfg) {
    const char* clip = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "clip_name"));
    if (!clip) return;
    char req[256];
    snprintf(req, sizeof(req), "mediaclip.cgi?action=play&clip=%s", clip);
    char* resp = ACAP_VAPIX_Get(req);
    if (resp) free(resp);
}

/* send_syslog */
static void action_send_syslog(cJSON* cfg, cJSON* trigger_data) {
    const char* msg_tmpl = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "message"));
    if (!msg_tmpl) return;
    char* msg = Actions_Expand_Template(msg_tmpl, trigger_data);
    int priority = LOG_INFO;
    const char* lvl = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "level"));
    if (lvl) {
        if (strcmp(lvl, "warning") == 0) priority = LOG_WARNING;
        else if (strcmp(lvl, "error") == 0) priority = LOG_ERR;
    }
    syslog(priority, "EventEngine: %s", msg);
    free(msg);
}

/* fire_vapix_event */
static void action_fire_vapix_event(cJSON* cfg) {
    const char* id = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "event_id"));
    if (!id) return;
    cJSON* state = cJSON_GetObjectItem(cfg, "state");
    if (state) {
        ACAP_EVENTS_Fire_State(id, cJSON_IsTrue(state) ? 1 : 0);
    } else {
        ACAP_EVENTS_Fire(id);
    }
}

/* set_variable */
static void action_set_variable(cJSON* cfg, cJSON* trigger_data) {
    const char* name     = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "name"));
    const char* val_tmpl = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "value"));
    if (!name || !val_tmpl) return;
    char* val = Actions_Expand_Template(val_tmpl, trigger_data);
    Variables_Set(name, val);
    free(val);
}

/* increment_counter */
static void action_increment_counter(cJSON* cfg) {
    const char* name = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "name"));
    if (!name) return;
    cJSON* delta_j = cJSON_GetObjectItem(cfg, "delta");
    double delta = delta_j ? delta_j->valuedouble : 1.0;
    const char* op = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "operation"));
    if (op && strcmp(op, "reset") == 0)
        Counter_Reset(name);
    else if (op && strcmp(op, "set") == 0)
        Counter_Set(name, delta);
    else
        Counter_Increment(name, delta);
}

/* mqtt_publish */
static void action_mqtt_publish(cJSON* cfg, cJSON* trigger_data) {
    const char* topic_tmpl = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "topic"));
    if (!topic_tmpl) return;
    char* topic = Actions_Expand_Template(topic_tmpl, trigger_data);

    char* payload = NULL;
    const char* payload_tmpl = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "payload"));
    if (payload_tmpl) payload = Actions_Expand_Template(payload_tmpl, trigger_data);

    cJSON* retain_j = cJSON_GetObjectItem(cfg, "retain");
    int retain = retain_j && cJSON_IsTrue(retain_j) ? 1 : 0;

    cJSON* qos_j = cJSON_GetObjectItem(cfg, "qos");
    int qos = qos_j ? (int)qos_j->valuedouble : 0;
    if (qos < 0 || qos > 1) qos = 0;

    if (!MQTT_Publish(topic, payload, retain, qos))
        LOG_WARN("mqtt_publish failed (not connected?)");

    free(topic);
    free(payload);
}

/* siren_light — control Axis Siren and Light via VAPIX */
static void action_siren_light(const char* rule_id, cJSON* cfg) {
    const char* signal_action = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "signal_action"));
    const char* profile        = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "profile"));
    if (!profile || profile[0] == '\0') { LOG_WARN("siren_light: no profile specified"); return; }
    int stopping = (signal_action && strcmp(signal_action, "stop") == 0);
    const char* method = stopping ? "stop" : "start";

    cJSON* req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "apiVersion", "1.0");
    cJSON_AddStringToObject(req, "method", method);
    cJSON* params = cJSON_AddObjectToObject(req, "params");
    cJSON_AddStringToObject(params, "profile", profile);
    char* body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    char* resp = ACAP_VAPIX_Post("siren_and_light.cgi", body);
    free(body);
    if (resp) free(resp);

    if (!stopping && rule_id) {
        cJSON* wa = cJSON_GetObjectItem(cfg, "while_active");
        if (wa && cJSON_IsTrue(wa)) {
            WhileActiveEntry e = {0};
            snprintf(e.rule_id, sizeof(e.rule_id), "%s", rule_id);
            snprintf(e.atype,   sizeof(e.atype),   "siren_light");
            snprintf(e.siren_profile, sizeof(e.siren_profile), "%s", profile);
            while_active_register(&e);
        }
    }
}

/* run_rule — forward to rule engine */
static void action_run_rule(cJSON* cfg) {
    const char* rid = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "rule_id"));
    if (rid) RuleEngine_Dispatch_RuleFired(rid);
}

/* vapix_query — fetch cached VAPIX event data and inject into trigger_data
 * so subsequent actions in the same rule can use {{trigger.FIELD}} tokens.
 * Requires a passive subscription to be active (registered at rule load time). */
static void action_vapix_query(cJSON* cfg, cJSON* trigger_data) {
    cJSON* cached = Triggers_Get_Cached(cfg);
    if (!cached) {
        LOG_WARN("vapix_query: no cached data yet (event may not have fired since startup)");
        return;
    }
    cJSON* item;
    cJSON_ArrayForEach(item, cached) {
        if (!item->string) continue;
        /* Overwrite with latest data so subsequent actions get fresh values */
        cJSON_DeleteItemFromObject(trigger_data, item->string);
        cJSON_AddItemToObject(trigger_data, item->string, cJSON_Duplicate(item, 1));
    }
}

/*-----------------------------------------------------
 * Async delay support
 *-----------------------------------------------------*/
typedef struct {
    char    rule_id[37];
    cJSON*  remaining_actions; /* cJSON array slice (we own this) */
    cJSON*  trigger_data;      /* duplicated */
} DelayCtx;

static gboolean delay_resume(gpointer user_data);

static gboolean delay_resume(gpointer user_data) {
    DelayCtx* ctx = (DelayCtx*)user_data;
    execute_from(ctx->rule_id, ctx->remaining_actions, 0, ctx->trigger_data);
    cJSON_Delete(ctx->remaining_actions);
    cJSON_Delete(ctx->trigger_data);
    free(ctx);
    return G_SOURCE_REMOVE;
}

static void execute_from(const char* rule_id, cJSON* actions_array,
                         int start_index, cJSON* trigger_data) {
    int total = cJSON_GetArraySize(actions_array);
    for (int i = start_index; i < total; i++) {
        cJSON* action = cJSON_GetArrayItem(actions_array, i);
        if (!action) continue;
        const char* type = cJSON_GetStringValue(cJSON_GetObjectItem(action, "type"));
        if (!type) continue;

        if (strcmp(type, "delay") == 0) {
            cJSON* sec_j = cJSON_GetObjectItem(action, "seconds");
            int seconds = sec_j ? (int)sec_j->valuedouble : 1;
            if (seconds < 1) seconds = 1;

            /* Build remaining slice */
            cJSON* remaining = cJSON_CreateArray();
            for (int j = i + 1; j < total; j++)
                cJSON_AddItemToArray(remaining, cJSON_Duplicate(cJSON_GetArrayItem(actions_array, j), 1));

            DelayCtx* ctx = malloc(sizeof(DelayCtx));
            snprintf(ctx->rule_id, sizeof(ctx->rule_id), "%s", rule_id ? rule_id : "");
            ctx->remaining_actions = remaining;
            ctx->trigger_data = trigger_data ? cJSON_Duplicate(trigger_data, 1) : cJSON_CreateNull();
            g_timeout_add_seconds((guint)seconds, delay_resume, ctx);
            return; /* stop synchronous execution */
        }

        /* Synchronous actions */
        if      (strcmp(type, "http_request")      == 0) action_http_request(rule_id, action, trigger_data);
        else if (strcmp(type, "recording")         == 0) action_recording(rule_id, action);
        else if (strcmp(type, "overlay_text")      == 0) action_overlay_text(rule_id, action, trigger_data);
        else if (strcmp(type, "ptz_preset")        == 0) action_ptz_preset(action);
        else if (strcmp(type, "io_output")         == 0) action_io_output(rule_id, action);
        else if (strcmp(type, "audio_clip")        == 0) action_audio_clip(action);
        else if (strcmp(type, "siren_light")       == 0) action_siren_light(rule_id, action);
        else if (strcmp(type, "send_syslog")       == 0) action_send_syslog(action, trigger_data);
        else if (strcmp(type, "fire_vapix_event")  == 0) action_fire_vapix_event(action);
        else if (strcmp(type, "set_variable")      == 0) action_set_variable(action, trigger_data);
        else if (strcmp(type, "increment_counter") == 0) action_increment_counter(action);
        else if (strcmp(type, "mqtt_publish")      == 0) action_mqtt_publish(action, trigger_data);
        else if (strcmp(type, "run_rule")          == 0) action_run_rule(action);
        else if (strcmp(type, "vapix_query")       == 0) action_vapix_query(action, trigger_data);
        else LOG_WARN("unknown action type '%s'", type);
    }
}

void Actions_Execute(const char* rule_id, cJSON* actions_array, cJSON* trigger_data) {
    if (!actions_array) return;
    execute_from(rule_id, actions_array, 0, trigger_data);
}
