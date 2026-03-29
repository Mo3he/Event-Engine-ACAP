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
#include "event_log.h"
#include "../ACAP.h"

#define LOG(fmt, args...)      syslog(LOG_INFO,    "actions: " fmt, ## args)
#define LOG_WARN(fmt, args...) syslog(LOG_WARNING, "actions: " fmt, ## args)

/* LOG_ERR: logs to syslog, writes to event log for the current rule, and
 * captures into g_action_error so the test endpoint can return it to the UI. */
static char        g_action_error[512]   = "";
static const char* g_current_rule_id     = NULL;

static void action_error_setf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_action_error, sizeof(g_action_error), fmt, ap);
    va_end(ap);
    if (g_current_rule_id)
        EventLog_Set_Action_Error(g_current_rule_id, g_action_error);
}

#define LOG_ACTION_ERR(fmt, args...) do { \
    syslog(LOG_WARNING, "actions: " fmt, ## args); \
    action_error_setf(fmt, ## args); \
} while(0)

const char* Actions_Get_Last_Error(void) {
    return g_action_error[0] ? g_action_error : NULL;
}

static char g_socks5_proxy[256] = "";

void Actions_Set_Proxy(const char* proxy) {
    snprintf(g_socks5_proxy, sizeof(g_socks5_proxy), "%s", proxy ? proxy : "");
}

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
    else
        LOG_WARN("while_active table full (%d) — undo action will not be registered for rule %s", MAX_WHILE_ACTIVE, e->rule_id);
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
#define MAX_TEMPLATE_OUTPUT (256 * 1024) /* 256 KB cap on expanded template */

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
                dyn_replacement = Variables_Get(key + 4);
                if (dyn_replacement) replacement = dyn_replacement;
            } else if (strncmp(key, "counter.", 8) == 0) {
                snprintf(tmp, sizeof(tmp), "%g", Counter_Get(key + 8));
                replacement = tmp;
            }

            if (!replacement) replacement = "";
            size_t rlen = strlen(replacement);
            if (out_len + rlen + 1 > MAX_TEMPLATE_OUTPUT) {
                LOG_WARN("template expansion exceeded %d bytes, truncating", MAX_TEMPLATE_OUTPUT);
                free(dyn_replacement);
                break;
            }
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

static char* base64_wrap_lines(const char* b64, size_t line_len) {
    if (!b64 || !line_len) return NULL;
    size_t in_len = strlen(b64);
    size_t lines = (in_len + line_len - 1) / line_len;
    size_t out_len = in_len + (lines * 2) + 1; /* +CRLF per line +NUL */
    char* out = malloc(out_len);
    if (!out) return NULL;

    size_t i = 0, j = 0;
    while (i < in_len) {
        size_t chunk = in_len - i;
        if (chunk > line_len) chunk = line_len;
        memcpy(out + j, b64 + i, chunk);
        j += chunk;
        out[j++] = '\r';
        out[j++] = '\n';
        i += chunk;
    }
    out[j] = '\0';
    return out;
}

/* Some cameras can return a black first JPEG when the imaging pipeline is idle.
 * Warm up by grabbing one frame, then return the next frame for use in actions. */
static char* capture_snapshot_jpeg_warm_channel(int channel, size_t* out_len) {
    if (out_len) *out_len = 0;

    /* Some models (including door stations) may return low-detail/black frames
     * during warm-up. Capture a short burst and keep the most data-rich frame
     * (largest JPEG payload) instead of the last frame. */
    const int attempts = 6;
    const int delay_us = 200000; /* 200 ms */
    char* best = NULL;
    size_t best_len = 0;

    for (int i = 0; i < attempts; i++) {
        char endpoint[128];
        snprintf(endpoint, sizeof(endpoint), "jpg/image.cgi?camera=%d&_=%lld", channel,
                 (long long)(g_get_real_time() + i));
        size_t snap_len = 0;
        char* snap = ACAP_VAPIX_GetBinary(endpoint, &snap_len);
        if (snap && snap_len > 0) {
            if (!best || snap_len > best_len) {
                free(best);
                best = snap;
                best_len = snap_len;
            } else {
                free(snap);
            }
        } else {
            free(snap);
        }
        if (best_len > 60000 && i >= 1) break;
        if (i < attempts - 1) g_usleep(delay_us);
    }

    if (!best) {
        for (int i = 0; i < 2; i++) {
            char endpoint[96];
            snprintf(endpoint, sizeof(endpoint), "jpg/image.cgi?_=%lld",
                     (long long)(g_get_real_time() + i));
            size_t snap_len = 0;
            char* snap = ACAP_VAPIX_GetBinary(endpoint, &snap_len);
            if (snap && snap_len > 0) {
                if (!best || snap_len > best_len) {
                    free(best);
                    best = snap;
                    best_len = snap_len;
                } else {
                    free(snap);
                }
            } else {
                free(snap);
            }
            if (i < 1) g_usleep(delay_us);
        }
    }

    if (!best || best_len == 0) {
        free(best);
        return NULL;
    }
    if (out_len) *out_len = best_len;
    return best;
}

static char* capture_snapshot_jpeg_warm(size_t* out_len) {
    return capture_snapshot_jpeg_warm_channel(1, out_len);
}

static cJSON* build_trigger_data_with_snapshot(cJSON* cfg, cJSON* trigger_data) {
    cJSON* snap_j = cJSON_GetObjectItem(cfg, "attach_snapshot");
    if (!snap_j || !cJSON_IsTrue(snap_j)) return NULL;

    size_t snap_len = 0;
    char* snap_raw = capture_snapshot_jpeg_warm(&snap_len);
    if (!snap_raw || snap_len == 0) {
        free(snap_raw);
        return NULL;
    }

    char* snap_b64 = base64_encode((unsigned char*)snap_raw, snap_len);
    free(snap_raw);
    if (!snap_b64) return NULL;

    cJSON* td = trigger_data ? cJSON_Duplicate(trigger_data, 1) : cJSON_CreateObject();
    if (td)
        cJSON_AddStringToObject(td, "snapshot_base64", snap_b64);
    free(snap_b64);
    return td;
}

/* http_request */
static size_t curl_discard(void* p, size_t sz, size_t n, void* ud) {
    (void)p; (void)ud; return sz * n;
}

/* Common curl setup: timeout, discard-write, follow-redirects, proxy */
static void curl_apply_base(CURL* curl, long timeout) {
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_discard);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    if (g_socks5_proxy[0]) {
        curl_easy_setopt(curl, CURLOPT_PROXY, g_socks5_proxy);
        curl_easy_setopt(curl, CURLOPT_PROXYTYPE, (long)CURLPROXY_SOCKS5_HOSTNAME);
    }
}

static void action_http_request(const char* rule_id, cJSON* cfg, cJSON* trigger_data) {
    const char* url_tmpl = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "url"));
    if (!url_tmpl || !url_tmpl[0]) { LOG_ACTION_ERR("http_request: no url"); return; }

    /* Optional snapshot attachment: fetch JPEG and inject as {{snapshot_base64}} */
    cJSON* snap_j = cJSON_GetObjectItem(cfg, "attach_snapshot");
    char* snap_b64 = NULL;
    cJSON* td_with_snap = NULL; /* extended trigger_data that includes snapshot_base64 */
    if (snap_j && cJSON_IsTrue(snap_j)) {
        size_t snap_len = 0;
        char* snap_raw = capture_snapshot_jpeg_warm(&snap_len);
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
    curl_apply_base(curl, 10L);
    cJSON* allow_insecure_j = cJSON_GetObjectItem(cfg, "allow_insecure");
    if (allow_insecure_j && cJSON_IsTrue(allow_insecure_j)) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    /* HTTP Basic/Digest authentication */
    const char* username = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "username"));
    const char* password = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "password"));
    if (username && *username) {
        char userpwd[512];
        snprintf(userpwd, sizeof(userpwd), "%s:%s", username, password ? password : "");
        curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd);
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
    }

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
        LOG_ACTION_ERR("http_request to %s failed: %s", url, curl_easy_strerror(res));

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
        /* Send position and textColor on setText too — the overlay may be cleared and need repositioning */
        cJSON_AddStringToObject(params, "position",   position);
        cJSON_AddStringToObject(params, "textColor",  text_color);
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
        if (!pid) {
            LOG_ACTION_ERR("ptz_preset: neither preset name nor preset_id provided");
            return;
        }
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
    if (!id || !*id) { LOG_ACTION_ERR("fire_vapix_event: no event_id specified"); return; }
    cJSON* state = cJSON_GetObjectItem(cfg, "state");
    int ok;
    if (state) {
        ok = ACAP_EVENTS_Fire_State(id, cJSON_IsTrue(state) ? 1 : 0);
    } else {
        ok = ACAP_EVENTS_Fire(id);
    }
    if (!ok) LOG_ACTION_ERR("fire_vapix_event: event '%s' not registered or failed to send", id);
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
    else if (op && strcmp(op, "decrement") == 0)
        Counter_Increment(name, -delta);
    else
        Counter_Increment(name, delta);
}

/* mqtt_publish */
static void action_mqtt_publish(cJSON* cfg, cJSON* trigger_data) {
    const char* topic_tmpl = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "topic"));
    if (!topic_tmpl) return;

    cJSON* td_with_snap = build_trigger_data_with_snapshot(cfg, trigger_data);
    cJSON* effective_td = td_with_snap ? td_with_snap : trigger_data;

    char* topic = Actions_Expand_Template(topic_tmpl, effective_td);

    char* payload = NULL;
    const char* payload_tmpl = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "payload"));
    if (payload_tmpl) payload = Actions_Expand_Template(payload_tmpl, effective_td);

    cJSON* retain_j = cJSON_GetObjectItem(cfg, "retain");
    int retain = retain_j && cJSON_IsTrue(retain_j) ? 1 : 0;

    cJSON* qos_j = cJSON_GetObjectItem(cfg, "qos");
    int qos = qos_j ? (int)qos_j->valuedouble : 0;
    if (qos < 0 || qos > 1) qos = 0;

    if (!MQTT_Publish(topic, payload, retain, qos))
        LOG_ACTION_ERR("mqtt_publish failed (not connected?)");

    if (td_with_snap) cJSON_Delete(td_with_snap);
    free(topic);
    free(payload);
}

/* slack_webhook */
static void action_slack_webhook(cJSON* cfg, cJSON* trigger_data) {
    const char* url = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "webhook_url"));
    if (!url || !url[0]) { LOG_ACTION_ERR("slack_webhook: no webhook_url"); return; }

    const char* msg_tmpl = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "message"));
    if (!msg_tmpl) msg_tmpl = "";

    cJSON* td_with_snap = build_trigger_data_with_snapshot(cfg, trigger_data);
    cJSON* effective_td = td_with_snap ? td_with_snap : trigger_data;
    char* msg = Actions_Expand_Template(msg_tmpl, effective_td);

    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "text", msg);
    const char* channel = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "channel"));
    if (channel && channel[0]) cJSON_AddStringToObject(body, "channel", channel);
    const char* username = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "username"));
    if (username && username[0]) cJSON_AddStringToObject(body, "username", username);

    char* json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (td_with_snap) cJSON_Delete(td_with_snap);
    free(msg);

    CURL* curl = curl_easy_init();
    if (!curl) { free(json_str); return; }
    struct curl_slist* hdrs = curl_slist_append(NULL, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_apply_base(curl, 10L);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) LOG_ACTION_ERR("slack_webhook failed: %s", curl_easy_strerror(res));
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    free(json_str);
}

/* teams_webhook — Adaptive Card via Power Automate / Workflows connector */
static void action_teams_webhook(cJSON* cfg, cJSON* trigger_data) {
    const char* url = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "webhook_url"));
    if (!url || !url[0]) { LOG_ACTION_ERR("teams_webhook: no webhook_url"); return; }

    const char* title_tmpl = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "title"));
    const char* msg_tmpl   = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "message"));
    if (!msg_tmpl) msg_tmpl = "";

    cJSON* td_with_snap = build_trigger_data_with_snapshot(cfg, trigger_data);
    cJSON* effective_td = td_with_snap ? td_with_snap : trigger_data;

    char* title = title_tmpl ? Actions_Expand_Template(title_tmpl, effective_td) : NULL;
    char* msg   = Actions_Expand_Template(msg_tmpl, effective_td);

    /* Build Adaptive Card body array */
    cJSON* card_body = cJSON_CreateArray();
    if (title && title[0]) {
        cJSON* tb = cJSON_CreateObject();
        cJSON_AddStringToObject(tb, "type", "TextBlock");
        cJSON_AddStringToObject(tb, "text", title);
        cJSON_AddStringToObject(tb, "weight", "bolder");
        cJSON_AddStringToObject(tb, "size", "medium");
        cJSON_AddItemToArray(card_body, tb);
    }
    {
        cJSON* tb = cJSON_CreateObject();
        cJSON_AddStringToObject(tb, "type", "TextBlock");
        cJSON_AddStringToObject(tb, "text", msg);
        cJSON_AddBoolToObject(tb, "wrap", 1);
        cJSON_AddItemToArray(card_body, tb);
    }

    cJSON* card = cJSON_CreateObject();
    cJSON_AddStringToObject(card, "$schema", "http://adaptivecards.io/schemas/adaptive-card.json");
    cJSON_AddStringToObject(card, "type", "AdaptiveCard");
    cJSON_AddStringToObject(card, "version", "1.4");
    cJSON_AddItemToObject(card, "body", card_body);

    const char* theme_color = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "theme_color"));
    if (theme_color && theme_color[0])
        cJSON_AddStringToObject(card, "accentColor", theme_color);

    cJSON* attachment = cJSON_CreateObject();
    cJSON_AddStringToObject(attachment, "contentType", "application/vnd.microsoft.card.adaptive");
    cJSON_AddItemToObject(attachment, "content", card);

    cJSON* attachments = cJSON_CreateArray();
    cJSON_AddItemToArray(attachments, attachment);

    cJSON* envelope = cJSON_CreateObject();
    cJSON_AddStringToObject(envelope, "type", "message");
    cJSON_AddItemToObject(envelope, "attachments", attachments);

    char* json_str = cJSON_PrintUnformatted(envelope);
    cJSON_Delete(envelope);
    if (td_with_snap) cJSON_Delete(td_with_snap);
    free(title);
    free(msg);

    CURL* curl = curl_easy_init();
    if (!curl) { free(json_str); return; }
    struct curl_slist* hdrs = curl_slist_append(NULL, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_apply_base(curl, 10L);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) LOG_ACTION_ERR("teams_webhook failed: %s", curl_easy_strerror(res));
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    free(json_str);
}

/* influxdb_write — write a data point to InfluxDB v1 or v2 */
static void action_influxdb_write(cJSON* cfg, cJSON* trigger_data) {
    const char* base_url = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "url"));
    if (!base_url || !base_url[0]) { LOG_ACTION_ERR("influxdb_write: no url"); return; }

    const char* version = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "version"));
    if (!version) version = "v2";
    int is_v1 = strcmp(version, "v1") == 0;

    const char* meas_tmpl = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "measurement"));
    if (!meas_tmpl || !meas_tmpl[0]) { LOG_ACTION_ERR("influxdb_write: no measurement"); return; }
    const char* fields_tmpl = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "fields"));
    if (!fields_tmpl || !fields_tmpl[0]) { LOG_ACTION_ERR("influxdb_write: no fields"); return; }

    char* measurement = Actions_Expand_Template(meas_tmpl, trigger_data);
    char* fields      = Actions_Expand_Template(fields_tmpl, trigger_data);
    const char* tags_tmpl = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "tags"));
    char* tags = tags_tmpl ? Actions_Expand_Template(tags_tmpl, trigger_data) : NULL;

    /* Build line protocol: measurement[,tags] fields */
    char line[2048];
    if (tags && tags[0])
        snprintf(line, sizeof(line), "%s,%s %s", measurement, tags, fields);
    else
        snprintf(line, sizeof(line), "%s %s", measurement, fields);

    free(measurement);
    free(fields);
    free(tags);

    /* Build write URL */
    char write_url[1024];
    if (is_v1) {
        const char* db = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "database"));
        snprintf(write_url, sizeof(write_url), "%s/write?db=%s", base_url, db ? db : "");
    } else {
        const char* org    = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "org"));
        const char* bucket = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "bucket"));
        snprintf(write_url, sizeof(write_url), "%s/api/v2/write?org=%s&bucket=%s",
                 base_url, org ? org : "", bucket ? bucket : "");
    }

    CURL* curl = curl_easy_init();
    if (!curl) return;

    struct curl_slist* hdrs = curl_slist_append(NULL, "Content-Type: text/plain");
    /* v2 token auth */
    const char* token = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "token"));
    if (!is_v1 && token && token[0]) {
        char auth_hdr[512];
        snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Token %s", token);
        hdrs = curl_slist_append(hdrs, auth_hdr);
    }

    curl_easy_setopt(curl, CURLOPT_URL, write_url);
    curl_apply_base(curl, 10L);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, line);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    /* v1 basic auth */
    if (is_v1) {
        const char* user = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "username"));
        if (user && user[0]) {
            char userpwd[512];
            snprintf(userpwd, sizeof(userpwd), "%s:%s", user, token ? token : "");
            curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd);
        }
    }

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK)
        LOG_ACTION_ERR("influxdb_write failed: %s", curl_easy_strerror(res));
    else {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code != 204 && http_code != 200)
            LOG_ACTION_ERR("influxdb_write: unexpected HTTP %ld", http_code);
    }

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
}

/* Shared upload buffer for SMTP and FTP read callbacks */
typedef struct { const char* data; size_t pos; size_t len; } UploadBuf;

static size_t curl_read_buf(char* ptr, size_t sz, size_t n, void* ud) {
    UploadBuf* u = (UploadBuf*)ud;
    size_t avail = u->len - u->pos;
    size_t want = sz * n;
    size_t copy = avail < want ? avail : want;
    memcpy(ptr, u->data + u->pos, copy);
    u->pos += copy;
    return copy;
}

/* email — SMTP via libcurl.  Server/credentials come from global SMTP
 * settings (settings.json "smtp" section); per-action config only has
 * to, subject, and body. */
static void action_email(cJSON* cfg, cJSON* trigger_data) {
    /* Read SMTP config from global settings — smtp lives under app["settings"]["smtp"] */
    cJSON* _sett    = ACAP_Get_Config("settings");
    cJSON* smtp_cfg = _sett ? cJSON_GetObjectItem(_sett, "smtp") : NULL;
    const char* smtp_server = smtp_cfg ? cJSON_GetStringValue(cJSON_GetObjectItem(smtp_cfg, "server")) : NULL;
    const char* from     = smtp_cfg ? cJSON_GetStringValue(cJSON_GetObjectItem(smtp_cfg, "from"))   : NULL;
    if (!smtp_server || !smtp_server[0]) { LOG_ACTION_ERR("email: SMTP server not configured in settings"); return; }
    if (!from || !from[0]) { LOG_ACTION_ERR("email: 'from' address not configured in SMTP settings"); return; }

    cJSON* use_tls_j = smtp_cfg ? cJSON_GetObjectItem(smtp_cfg, "use_tls") : NULL;
    int use_tls = (!use_tls_j || cJSON_IsTrue(use_tls_j)) ? 1 : 0;

    char smtp_url_buf[768];
    const char* smtp_url = smtp_server;
    if (!strstr(smtp_server, "://")) {
        /* Accept plain host:port input from the UI and default to SMTP transport URL */
        snprintf(smtp_url_buf, sizeof(smtp_url_buf), "smtp://%s", smtp_server);
        smtp_url = smtp_url_buf;
    }

    const char* to = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "to"));
    if (!to || !to[0]) { LOG_ACTION_ERR("email: no 'to' address"); return; }

    /* Optional snapshot attachment: include JPEG as MIME attachment and expose {{trigger.snapshot_base64}} */
    cJSON* snap_j = cJSON_GetObjectItem(cfg, "attach_snapshot");
    int attach_snapshot = snap_j && cJSON_IsTrue(snap_j);
    char* snap_b64 = NULL;
    char* snap_b64_wrapped = NULL;
    cJSON* td_with_snap = NULL;
    if (attach_snapshot) {
        size_t snap_len = 0;
        char* snap_raw = capture_snapshot_jpeg_warm(&snap_len);
        if (snap_raw && snap_len > 0) {
            snap_b64 = base64_encode((unsigned char*)snap_raw, snap_len);
            free(snap_raw);
        }
        if (snap_b64) {
            td_with_snap = trigger_data ? cJSON_Duplicate(trigger_data, 1) : cJSON_CreateObject();
            if (td_with_snap)
                cJSON_AddStringToObject(td_with_snap, "snapshot_base64", snap_b64);
            snap_b64_wrapped = base64_wrap_lines(snap_b64, 76);
        } else {
            LOG_ACTION_ERR("email: attach_snapshot requested but snapshot capture failed");
        }
    }
    cJSON* effective_td = td_with_snap ? td_with_snap : trigger_data;

    const char* subj_tmpl = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "subject"));
    const char* body_tmpl = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "body"));
    char* subject = subj_tmpl ? Actions_Expand_Template(subj_tmpl, effective_td) : strdup("Event Engine Alert");
    char* body    = body_tmpl ? Actions_Expand_Template(body_tmpl, effective_td) : strdup("");
    if (td_with_snap) cJSON_Delete(td_with_snap);
    free(snap_b64);

    /* Build RFC 5322 message dynamically (plain text or multipart with snapshot attachment) */
    const char* boundary = "----ACAP_EVENT_ENGINE_BOUNDARY";
    int has_attachment = attach_snapshot && snap_b64_wrapped && snap_b64_wrapped[0];
    size_t msg_len;
    if (has_attachment) {
        msg_len = strlen(from) + strlen(to) + strlen(subject) + strlen(body) +
                  strlen(boundary) * 4 + strlen(snap_b64_wrapped) + 1024;
    } else {
        msg_len = strlen(from) + strlen(to) + strlen(subject) + strlen(body) + 128;
    }
    char* msg = malloc(msg_len);
    if (!msg) { free(subject); free(body); free(snap_b64_wrapped); return; }

    if (has_attachment) {
        snprintf(msg, msg_len,
            "From: %s\r\n"
            "To: %s\r\n"
            "Subject: %s\r\n"
            "MIME-Version: 1.0\r\n"
            "Content-Type: multipart/mixed; boundary=\"%s\"\r\n"
            "\r\n"
            "--%s\r\n"
            "Content-Type: text/plain; charset=UTF-8\r\n"
            "\r\n"
            "%s\r\n"
            "\r\n"
            "--%s\r\n"
            "Content-Type: image/jpeg; name=\"snapshot.jpg\"\r\n"
            "Content-Transfer-Encoding: base64\r\n"
            "Content-Disposition: attachment; filename=\"snapshot.jpg\"\r\n"
            "\r\n"
            "%s"
            "\r\n"
            "--%s--\r\n",
            from, to, subject, boundary,
            boundary,
            body,
            boundary,
            snap_b64_wrapped,
            boundary);
    } else {
        snprintf(msg, msg_len,
            "From: %s\r\n"
            "To: %s\r\n"
            "Subject: %s\r\n"
            "Content-Type: text/plain; charset=UTF-8\r\n"
            "\r\n"
            "%s\r\n", from, to, subject, body);
    }
    free(subject);
    free(body);
    free(snap_b64_wrapped);

    /* libcurl SMTP upload via CURLOPT_READFUNCTION on a buffer */
    UploadBuf upload = { msg, 0, strlen(msg) };

    CURL* curl = curl_easy_init();
    if (!curl) { free(msg); return; }

    curl_easy_setopt(curl, CURLOPT_URL, smtp_url);
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, from);

    /* Parse comma-separated "to" into recipients */
    struct curl_slist* recipients = NULL;
    char to_copy[1024];
    snprintf(to_copy, sizeof(to_copy), "%s", to);
    char* saveptr = NULL;
    char* addr = strtok_r(to_copy, ",;", &saveptr);
    while (addr) {
        while (*addr == ' ') addr++;
        if (*addr) recipients = curl_slist_append(recipients, addr);
        addr = strtok_r(NULL, ",;", &saveptr);
    }
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);

    /* Read callback for SMTP upload */
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, curl_read_buf);
    curl_easy_setopt(curl, CURLOPT_READDATA, &upload);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    const char* user = smtp_cfg ? cJSON_GetStringValue(cJSON_GetObjectItem(smtp_cfg, "username")) : NULL;
    /* Password is stored separately (stripped from settings on save) */
    char smtp_pw[256] = "";
    {
        cJSON* pw_file = ACAP_FILE_Read("localdata/smtp_password.txt");
        if (pw_file) {
            const char* pw = cJSON_GetStringValue(cJSON_GetObjectItem(pw_file, "pw"));
            if (pw) snprintf(smtp_pw, sizeof(smtp_pw), "%s", pw);
            cJSON_Delete(pw_file);
        }
    }
    if (user && user[0]) {
        curl_easy_setopt(curl, CURLOPT_USERNAME, user);
        curl_easy_setopt(curl, CURLOPT_PASSWORD, smtp_pw);
    }

    if (use_tls)
        curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);

    if (g_socks5_proxy[0]) {
        curl_easy_setopt(curl, CURLOPT_PROXY, g_socks5_proxy);
        curl_easy_setopt(curl, CURLOPT_PROXYTYPE, (long)CURLPROXY_SOCKS5_HOSTNAME);
    }

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) LOG_ACTION_ERR("email failed: %s", curl_easy_strerror(res));
    curl_slist_free_all(recipients);
    curl_easy_cleanup(curl);
    free(msg);
}

/* telegram — Telegram Bot API sendMessage */
static void action_telegram(cJSON* cfg, cJSON* trigger_data) {
    const char* bot_token = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "bot_token"));
    const char* chat_id   = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "chat_id"));
    if (!bot_token || !chat_id) { LOG_ACTION_ERR("telegram: missing bot_token/chat_id"); return; }

    const char* msg_tmpl = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "message"));
    if (!msg_tmpl) msg_tmpl = "";

    /* Optionally expose {{trigger.snapshot_base64}} in templates when attach_snapshot is enabled */
    cJSON* td_with_snap = build_trigger_data_with_snapshot(cfg, trigger_data);
    cJSON* effective_td = td_with_snap ? td_with_snap : trigger_data;
    char* msg = Actions_Expand_Template(msg_tmpl, effective_td);
    if (td_with_snap) cJSON_Delete(td_with_snap);

    cJSON* snap_j = cJSON_GetObjectItem(cfg, "attach_snapshot");
    int attach_snapshot = snap_j && cJSON_IsTrue(snap_j);

    CURL* curl = curl_easy_init();
    if (!curl) { free(msg); return; }
    curl_apply_base(curl, 20L);

    const char* parse_mode = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "parse_mode"));

    if (attach_snapshot) {
        size_t snap_len = 0;
        char* snap_raw = capture_snapshot_jpeg_warm(&snap_len);
        if (snap_raw && snap_len > 0) {
            char url[512];
            snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendPhoto", bot_token);
            curl_easy_setopt(curl, CURLOPT_URL, url);

            curl_mime* form = curl_mime_init(curl);
            curl_mimepart* part = curl_mime_addpart(form);
            curl_mime_name(part, "chat_id");
            curl_mime_data(part, chat_id, CURL_ZERO_TERMINATED);

            part = curl_mime_addpart(form);
            curl_mime_name(part, "caption");
            curl_mime_data(part, msg, CURL_ZERO_TERMINATED);

            if (parse_mode && parse_mode[0]) {
                part = curl_mime_addpart(form);
                curl_mime_name(part, "parse_mode");
                curl_mime_data(part, parse_mode, CURL_ZERO_TERMINATED);
            }

            part = curl_mime_addpart(form);
            curl_mime_name(part, "photo");
            curl_mime_filename(part, "snapshot.jpg");
            curl_mime_type(part, "image/jpeg");
            curl_mime_data(part, snap_raw, (curl_off_t)snap_len);

            curl_easy_setopt(curl, CURLOPT_MIMEPOST, form);
            CURLcode res = curl_easy_perform(curl);
            if (res != CURLE_OK) LOG_ACTION_ERR("telegram sendPhoto failed: %s", curl_easy_strerror(res));

            curl_mime_free(form);
            free(snap_raw);
            curl_easy_cleanup(curl);
            free(msg);
            return;
        }
        free(snap_raw);
        LOG_ACTION_ERR("telegram: attach_snapshot requested but snapshot capture failed; falling back to sendMessage");
    }

    {
        char url[512];
        snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", bot_token);
        curl_easy_setopt(curl, CURLOPT_URL, url);

        cJSON* body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "chat_id", chat_id);
        cJSON_AddStringToObject(body, "text", msg);
        if (parse_mode && parse_mode[0])
            cJSON_AddStringToObject(body, "parse_mode", parse_mode);
        cJSON* disable_preview = cJSON_GetObjectItem(cfg, "disable_preview");
        if (disable_preview && cJSON_IsTrue(disable_preview))
            cJSON_AddBoolToObject(body, "disable_web_page_preview", 1);

        char* json_str = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);

        struct curl_slist* hdrs = curl_slist_append(NULL, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) LOG_ACTION_ERR("telegram failed: %s", curl_easy_strerror(res));

        curl_slist_free_all(hdrs);
        free(json_str);
    }

    curl_easy_cleanup(curl);
    free(msg);
}

/* ftp_upload — Upload a snapshot to FTP/SFTP via libcurl */
static void action_ftp_upload(cJSON* cfg, cJSON* trigger_data) {
    const char* url_tmpl = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "url"));
    if (!url_tmpl) { LOG_WARN("ftp_upload: no url"); return; }

    char* url = Actions_Expand_Template(url_tmpl, trigger_data);

    /* Fetch JPEG snapshot */
    size_t snap_len = 0;
    char* snap = capture_snapshot_jpeg_warm(&snap_len);
    if (!snap || snap_len == 0) {
        LOG_WARN("ftp_upload: failed to capture snapshot");
        free(url); free(snap);
        return;
    }

    UploadBuf upload = { snap, 0, snap_len };

    CURL* curl = curl_easy_init();
    if (!curl) { free(url); free(snap); return; }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, curl_read_buf);
    curl_easy_setopt(curl, CURLOPT_READDATA, &upload);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)snap_len);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FTP_CREATE_MISSING_DIRS, 1L);

    const char* user = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "username"));
    const char* pass = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "password"));
    if (user && user[0]) {
        char userpwd[512];
        snprintf(userpwd, sizeof(userpwd), "%s:%s", user, pass ? pass : "");
        curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd);
    }

    if (g_socks5_proxy[0]) {
        curl_easy_setopt(curl, CURLOPT_PROXY, g_socks5_proxy);
        curl_easy_setopt(curl, CURLOPT_PROXYTYPE, (long)CURLPROXY_SOCKS5_HOSTNAME);
    }
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) LOG_WARN("ftp_upload to %s failed: %s", url, curl_easy_strerror(res));
    curl_easy_cleanup(curl);
    free(url);
    free(snap);
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

/* guard_tour — start or stop a configured PTZ guard tour via param.cgi
 * Tours are stored as root.GuardTour.G<n>.Name / .Running.
 * We scan up to 32 slots to find the tour by name, then set Running=yes/no. */
static void action_guard_tour(cJSON* cfg) {
    const char* op = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "operation"));
    int starting = (!op || strcmp(op, "stop") != 0);

    /* Parse channel field if present (unused; documented as VAPIX limitation) */
    cJSON* ch_j = cJSON_GetObjectItem(cfg, "channel");
    if (ch_j) LOG_WARN("guard_tour: 'channel' field is stored but not implemented (all tours are global)");

    const char* tour_name = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "tour_id"));
    if (starting && (!tour_name || !tour_name[0])) {
        LOG_WARN("guard_tour: no tour_id specified for start"); return;
    }

    /* If stopping with no name, stop all running tours */
    if (!starting && (!tour_name || !tour_name[0])) {
        for (int n = 0; n < 32; n++) {
            char running_req[128];
            snprintf(running_req, sizeof(running_req),
                     "param.cgi?action=update&root.GuardTour.G%d.Running=no", n);
            char* resp = ACAP_VAPIX_Get(running_req);
            if (!resp) break;  /* no more slots */
            free(resp);
        }
        return;
    }

    /* Find the tour index by name */
    for (int n = 0; n < 32; n++) {
        char name_req[128];
        snprintf(name_req, sizeof(name_req),
                 "param.cgi?action=list&group=root.GuardTour.G%d.Name", n);
        char* resp = ACAP_VAPIX_Get(name_req);
        if (!resp) break;  /* no more slots */

        /* Response is "root.GuardTour.G<n>.Name=<value>\n" */
        char* eq = strchr(resp, '=');
        if (eq) {
            char* nl = strchr(eq + 1, '\n');
            if (nl) *nl = '\0';
            char* name_val = eq + 1;
            /* strip trailing \r if present */
            size_t l = strlen(name_val);
            if (l > 0 && name_val[l-1] == '\r') name_val[l-1] = '\0';

            if (strcmp(name_val, tour_name) == 0) {
                free(resp);
                char run_req[128];
                snprintf(run_req, sizeof(run_req),
                         "param.cgi?action=update&root.GuardTour.G%d.Running=%s",
                         n, starting ? "yes" : "no");
                char* r2 = ACAP_VAPIX_Get(run_req);
                if (r2) free(r2);
                LOG("guard_tour: %s tour '%s' (G%d)", starting ? "started" : "stopped", tour_name, n);
                return;
            }
        }
        free(resp);
    }
    LOG_WARN("guard_tour: tour '%s' not found", tour_name ? tour_name : "");
}

/* set_device_param — update a camera parameter via param.cgi */
static void action_set_device_param(cJSON* cfg) {
    const char* param = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "parameter"));
    const char* value = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "value"));
    if (!param || !param[0] || !value) { LOG_WARN("set_device_param: missing parameter or value"); return; }

    /* Accept "root.X" or just "X" — normalise to always include "root." */
    char full_param[256];
    if (strncmp(param, "root.", 5) == 0)
        snprintf(full_param, sizeof(full_param), "%s", param);
    else
        snprintf(full_param, sizeof(full_param), "root.%s", param);

    char req[512];
    snprintf(req, sizeof(req), "param.cgi?action=update&%s=%s", full_param, value);
    char* resp = ACAP_VAPIX_Get(req);
    if (resp) free(resp);
}

/* snapshot_upload — capture a JPEG snapshot and POST/PUT it to a URL */
static void action_snapshot_upload(cJSON* cfg, cJSON* trigger_data) {
    const char* url_tmpl = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "url"));
    if (!url_tmpl) { LOG_WARN("snapshot_upload: no url specified"); return; }

    cJSON* ch_j = cJSON_GetObjectItem(cfg, "channel");
    int channel = ch_j ? (int)ch_j->valuedouble : 1;

    size_t snap_len = 0;
    char* snap_raw = capture_snapshot_jpeg_warm_channel(channel, &snap_len);
    if (!snap_raw || snap_len == 0) {
        LOG_WARN("snapshot_upload: failed to capture snapshot");
        if (snap_raw) free(snap_raw);
        return;
    }

    char* url = Actions_Expand_Template(url_tmpl, trigger_data);
    const char* method = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "method"));
    if (!method) method = "POST";

    CURL* curl = curl_easy_init();
    if (!curl) { free(url); free(snap_raw); return; }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_apply_base(curl, 30L);

    const char* username = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "username"));
    const char* password = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "password"));
    if (username && *username) {
        char userpwd[512];
        snprintf(userpwd, sizeof(userpwd), "%s:%s", username, password ? password : "");
        curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd);
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
        explicit_bzero(userpwd, sizeof(userpwd));
    }

    struct curl_slist* hdrs = curl_slist_append(NULL, "Content-Type: image/jpeg");
    const char* hdrs_str = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "headers"));
    if (hdrs_str) {
        char* hcopy = strdup(hdrs_str);
        char* line; char* saveptr;
        line = strtok_r(hcopy, "\n", &saveptr);
        while (line) {
            while (*line == ' ' || *line == '\r') line++;
            char* end = line + strlen(line) - 1;
            while (end > line && (*end == ' ' || *end == '\r' || *end == '\n')) *end-- = '\0';
            if (*line) hdrs = curl_slist_append(hdrs, line);
            line = strtok_r(NULL, "\n", &saveptr);
        }
        free(hcopy);
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    if (strcmp(method, "PUT") == 0)
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    else
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, snap_raw);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)snap_len);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK)
        LOG_WARN("snapshot_upload to %s failed: %s", url, curl_easy_strerror(res));
    else
        LOG("snapshot_upload: sent %zu bytes to %s", snap_len, url);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    free(url);
    free(snap_raw);
}

/* ir_cut_filter — force day/night mode or restore auto switching */
static void action_ir_cut_filter(cJSON* cfg) {
    const char* mode = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "mode"));
    if (!mode) mode = "auto";
    cJSON* ch_j = cJSON_GetObjectItem(cfg, "channel");
    int channel = ch_j ? (int)ch_j->valuedouble : 1;
    int idx = channel - 1;  /* param index: channel 1 → I0, channel 2 → I1, … */

    /* IrCutFilter values: yes = filter in (day), no = filter out (night), auto = automatic */
    const char* val;
    if (strcmp(mode, "day") == 0)       val = "yes";
    else if (strcmp(mode, "night") == 0) val = "no";
    else                                 val = "auto";

    char req[128];
    snprintf(req, sizeof(req),
             "param.cgi?action=update&root.ImageSource.I%d.DayNight.IrCutFilter=%s", idx, val);
    char* resp = ACAP_VAPIX_Get(req);
    if (resp) free(resp);
}

/* privacy_mask — enable or disable a named privacy mask.
 * Masks live under root.Image.I<ch-1>.Overlay.MaskWindows.M<n>.
 * We scan by name to find the right slot, then set Enabled=yes/no. */
static void action_privacy_mask(cJSON* cfg) {
    const char* mask_name = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "mask_id"));
    if (!mask_name || !mask_name[0]) { LOG_WARN("privacy_mask: no mask_id specified"); return; }
    cJSON* ch_j = cJSON_GetObjectItem(cfg, "channel");
    cJSON* en_j = cJSON_GetObjectItem(cfg, "enabled");
    int idx     = ch_j ? (int)ch_j->valuedouble - 1 : 0;
    int enabled = (!en_j || cJSON_IsTrue(en_j)) ? 1 : 0;

    for (int n = 0; n < 32; n++) {
        char name_req[128];
        snprintf(name_req, sizeof(name_req),
                 "param.cgi?action=list&group=root.Image.I%d.Overlay.MaskWindows.M%d.Name", idx, n);
        char* resp = ACAP_VAPIX_Get(name_req);
        if (!resp) break;

        char* eq = strchr(resp, '=');
        if (eq) {
            char* nl = strchr(eq + 1, '\n');
            if (nl) *nl = '\0';
            char* name_val = eq + 1;
            size_t l = strlen(name_val);
            if (l > 0 && name_val[l-1] == '\r') name_val[l-1] = '\0';

            if (strcmp(name_val, mask_name) == 0) {
                free(resp);
                char set_req[128];
                snprintf(set_req, sizeof(set_req),
                         "param.cgi?action=update&root.Image.I%d.Overlay.MaskWindows.M%d.Enabled=%s",
                         idx, n, enabled ? "yes" : "no");
                char* r2 = ACAP_VAPIX_Get(set_req);
                if (r2) free(r2);
                LOG("privacy_mask: %s mask '%s' (I%d M%d)", enabled ? "enabled" : "disabled", mask_name, idx, n);
                return;
            }
        }
        free(resp);
    }
    LOG_WARN("privacy_mask: mask '%s' not found on channel %d", mask_name, idx + 1);
}

/* wiper — trigger the camera clear view (wiper/speed dry) via clearviewcontrol.cgi */
static void action_wiper(cJSON* cfg) {
    const char* op = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "operation"));
    cJSON* id_j  = cJSON_GetObjectItem(cfg, "id");
    int id = id_j ? (int)id_j->valuedouble : 1;

    cJSON* req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "apiVersion", "1.0");
    cJSON_AddStringToObject(req, "method", (op && strcmp(op, "stop") == 0) ? "stop" : "start");
    cJSON* p = cJSON_AddObjectToObject(req, "params");
    cJSON_AddNumberToObject(p, "id", id);

    /* Optional duration (seconds) for variable-duration wiper services */
    cJSON* dur_j = cJSON_GetObjectItem(cfg, "duration");
    if (dur_j && dur_j->valuedouble > 0)
        cJSON_AddNumberToObject(p, "duration", dur_j->valuedouble);

    char* body = cJSON_PrintUnformatted(req); cJSON_Delete(req);
    char* resp = ACAP_VAPIX_Post("clearviewcontrol.cgi", body);
    free(body); if (resp) free(resp);
}

/* light_control — control an Axis illuminator (white/IR LED) */
static void action_light_control(cJSON* cfg) {
    const char* op = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "operation"));
    if (!op) op = "on";
    const char* light_id = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "light_id"));
    if (!light_id || !light_id[0]) light_id = "led0";

    cJSON* req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "apiVersion", "1.0");
    cJSON* p = cJSON_AddObjectToObject(req, "params");
    cJSON_AddStringToObject(p, "lightID", light_id);

    if (strcmp(op, "off") == 0) {
        cJSON_AddStringToObject(req, "method", "deactivateLight");
    } else if (strcmp(op, "auto") == 0) {
        cJSON_AddStringToObject(req, "method", "setAutomaticIntensityMode");
        cJSON_AddBoolToObject(p, "enabled", 1);
    } else {
        /* "on" — activate light; optionally set manual intensity first */
        cJSON* intensity_j = cJSON_GetObjectItem(cfg, "intensity");
        if (intensity_j) {
            cJSON_AddStringToObject(req, "method", "setManualIntensity");
            cJSON_AddNumberToObject(p, "intensity", intensity_j->valuedouble);
            char* body = cJSON_PrintUnformatted(req);
            char* resp = ACAP_VAPIX_Post("lightcontrol.cgi", body);
            free(body); if (resp) free(resp);
            /* Now activate */
            cJSON_SetValuestring(cJSON_GetObjectItem(req, "method"), "activateLight");
            cJSON_DeleteItemFromObject(p, "intensity");
        } else {
            cJSON_AddStringToObject(req, "method", "activateLight");
        }
    }

    char* body = cJSON_PrintUnformatted(req); cJSON_Delete(req);
    char* resp = ACAP_VAPIX_Post("lightcontrol.cgi", body);
    free(body); if (resp) free(resp);
}

/* aoa_get_counts — fetch accumulated crossline counts from an AOA scenario and inject
 * them into trigger_data so subsequent actions can use {{aoa_total}}, {{aoa_human}}, etc. */
static void action_aoa_get_counts(cJSON* cfg, cJSON* trigger_data) {
    cJSON* sid_j = cJSON_GetObjectItem(cfg, "scenario_id");
    if (!sid_j) { LOG_WARN("aoa_get_counts: no scenario_id"); return; }
    int scenario_id = (int)sid_j->valuedouble;

    char body[256];
    snprintf(body, sizeof(body),
        "{\"method\":\"getAccumulatedCounts\",\"apiVersion\":\"1.0\","
        "\"params\":{\"scenario\":%d}}", scenario_id);

    char* raw = ACAP_VAPIX_Post_Path("/local/objectanalytics/control.cgi", body);
    if (!raw) { LOG_WARN("aoa_get_counts: VAPIX call failed for scenario %d", scenario_id); return; }

    cJSON* root = cJSON_Parse(raw);
    free(raw);
    if (!root) { LOG_WARN("aoa_get_counts: invalid JSON response"); return; }

    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (data) {
        /* getAccumulatedCounts returns: total, totalHuman, totalCar, totalTruck,
         * totalBus, totalBike, totalOtherVehicle.
         * Map each to aoa_total, aoa_human, ... for template use. */
        static const struct { const char* api; const char* token; } map[] = {
            {"total",             "aoa_total"},
            {"totalHuman",        "aoa_human"},
            {"totalCar",          "aoa_car"},
            {"totalTruck",        "aoa_truck"},
            {"totalBus",          "aoa_bus"},
            {"totalBike",         "aoa_bike"},
            {"totalOtherVehicle", "aoa_otherVehicle"},
            {NULL, NULL}
        };
        for (int i = 0; map[i].api; i++) {
            cJSON* v = cJSON_GetObjectItem(data, map[i].api);
            if (v && cJSON_IsNumber(v)) {
                cJSON_DeleteItemFromObject(trigger_data, map[i].token);
                cJSON_AddNumberToObject(trigger_data, map[i].token, v->valuedouble);
            }
        }
        const char* ts = cJSON_GetStringValue(cJSON_GetObjectItem(data, "timestamp"));
        if (ts) {
            cJSON_DeleteItemFromObject(trigger_data, "aoa_timestamp");
            cJSON_AddStringToObject(trigger_data, "aoa_timestamp", ts);
        }
    }
    cJSON_Delete(root);

    cJSON* reset_j = cJSON_GetObjectItem(cfg, "reset_after");
    if (reset_j && cJSON_IsTrue(reset_j)) {
        char rbody[256];
        snprintf(rbody, sizeof(rbody),
            "{\"method\":\"resetAccumulatedCounts\",\"apiVersion\":\"1.0\","
            "\"params\":{\"scenario\":%d}}", scenario_id);
        char* rresp = ACAP_VAPIX_Post_Path("/local/objectanalytics/control.cgi", rbody);
        if (rresp) free(rresp);
    }
}

/* acap_control — start, stop, or restart another installed ACAP application */
static void action_acap_control(cJSON* cfg) {
    const char* package = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "package"));
    if (!package || !package[0]) { LOG_WARN("acap_control: no package specified"); return; }
    const char* op = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "operation"));
    if (!op) op = "start";
    char req[256];
    snprintf(req, sizeof(req), "applications/control.cgi?package=%s&action=%s", package, op);
    char* resp = ACAP_VAPIX_Get(req);
    if (resp) free(resp);
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

/*-----------------------------------------------------
 * Notification Digest — buffer messages and flush
 * periodically instead of sending one per event.
 *-----------------------------------------------------*/
#define MAX_DIGEST_BUFFERS 32
#define MAX_DIGEST_LINES   200

typedef struct {
    char   rule_id[37];
    char   deliver_via[16]; /* "slack", "teams", "email", "mqtt" */
    cJSON* config;          /* retained delivery config (webhook_url, etc.) */
    char*  lines[MAX_DIGEST_LINES];
    int    line_count;
    int    interval;        /* flush interval in seconds */
    time_t last_flush;
} DigestBuf;

static DigestBuf digest_bufs[MAX_DIGEST_BUFFERS];
static int       digest_buf_count = 0;

static DigestBuf* digest_find_or_create(const char* rule_id, cJSON* cfg) {
    for (int i = 0; i < digest_buf_count; i++)
        if (strcmp(digest_bufs[i].rule_id, rule_id) == 0) return &digest_bufs[i];
    if (digest_buf_count >= MAX_DIGEST_BUFFERS) return NULL;

    DigestBuf* d = &digest_bufs[digest_buf_count++];
    memset(d, 0, sizeof(*d));
    snprintf(d->rule_id, sizeof(d->rule_id), "%s", rule_id);
    const char* via = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "deliver_via"));
    snprintf(d->deliver_via, sizeof(d->deliver_via), "%s", via ? via : "slack");
    d->config = cJSON_Duplicate(cfg, 1);
    cJSON* iv = cJSON_GetObjectItem(cfg, "interval");
    d->interval = iv ? (int)iv->valuedouble : 300;
    if (d->interval < 30) d->interval = 30;
    d->last_flush = time(NULL);
    return d;
}

static void digest_flush(DigestBuf* d) {
    if (d->line_count == 0) return;

    /* Build combined message */
    size_t total_len = 0;
    for (int i = 0; i < d->line_count; i++)
        total_len += strlen(d->lines[i]) + 1;
    char* combined = malloc(total_len + 64);
    if (!combined) return;
    combined[0] = '\0';

    char header[64];
    snprintf(header, sizeof(header), "[%d events]\n", d->line_count);
    strcat(combined, header);

    for (int i = 0; i < d->line_count; i++) {
        strcat(combined, d->lines[i]);
        strcat(combined, "\n");
        free(d->lines[i]);
        d->lines[i] = NULL;
    }
    d->line_count = 0;
    d->last_flush = time(NULL);

    /* Deliver via configured method */
    cJSON* payload = cJSON_Duplicate(d->config, 1);
    cJSON_DeleteItemFromObject(payload, "type");
    cJSON_AddStringToObject(payload, "type", d->deliver_via);

    /* Inject the combined message */
    cJSON_DeleteItemFromObject(payload, "message");
    cJSON_AddStringToObject(payload, "message", combined);
    /* For email: inject into body instead */
    cJSON_DeleteItemFromObject(payload, "body");
    cJSON_AddStringToObject(payload, "body", combined);
    /* For MQTT: inject into payload */
    cJSON_DeleteItemFromObject(payload, "payload");
    cJSON_AddStringToObject(payload, "payload", combined);

    /* Dispatch via the appropriate action handler */
    cJSON* empty_td = cJSON_CreateObject();
    if (strcmp(d->deliver_via, "slack") == 0)
        action_slack_webhook(payload, empty_td);
    else if (strcmp(d->deliver_via, "teams") == 0)
        action_teams_webhook(payload, empty_td);
    else if (strcmp(d->deliver_via, "email") == 0)
        action_email(payload, empty_td);
    else if (strcmp(d->deliver_via, "mqtt") == 0)
        action_mqtt_publish(payload, empty_td);
    else if (strcmp(d->deliver_via, "telegram") == 0)
        action_telegram(payload, empty_td);
    cJSON_Delete(payload);
    cJSON_Delete(empty_td);
    free(combined);
}

static void action_digest(const char* rule_id, cJSON* cfg, cJSON* trigger_data) {
    DigestBuf* d = digest_find_or_create(rule_id, cfg);
    if (!d) { LOG_WARN("digest: too many buffers"); return; }

    const char* line_tmpl = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "line"));
    if (!line_tmpl) line_tmpl = "{{timestamp}} — {{trigger_json}}";
    char* line = Actions_Expand_Template(line_tmpl, trigger_data);

    if (d->line_count < MAX_DIGEST_LINES)
        d->lines[d->line_count++] = line;
    else
        free(line); /* buffer full, drop */
}

/* Called from Actions_Tick (via main loop 1s timer) */
void Actions_Digest_Tick(void) {
    time_t now = time(NULL);
    for (int i = 0; i < digest_buf_count; i++) {
        DigestBuf* d = &digest_bufs[i];
        if (d->line_count > 0 && (now - d->last_flush) >= d->interval)
            digest_flush(d);
    }
}

static void execute_from(const char* rule_id, cJSON* actions_array,
                         int start_index, cJSON* trigger_data) {
    g_current_rule_id = rule_id;
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
        else if (strcmp(type, "slack_webhook")    == 0) action_slack_webhook(action, trigger_data);
        else if (strcmp(type, "teams_webhook")    == 0) action_teams_webhook(action, trigger_data);
        else if (strcmp(type, "influxdb_write")   == 0) action_influxdb_write(action, trigger_data);
        else if (strcmp(type, "email")            == 0) action_email(action, trigger_data);
        else if (strcmp(type, "telegram")         == 0) action_telegram(action, trigger_data);
        else if (strcmp(type, "ftp_upload")       == 0) action_ftp_upload(action, trigger_data);
        else if (strcmp(type, "digest")           == 0) action_digest(rule_id, action, trigger_data);
        else if (strcmp(type, "run_rule")          == 0) action_run_rule(action);
        else if (strcmp(type, "vapix_query")       == 0) action_vapix_query(action, trigger_data);
        else if (strcmp(type, "guard_tour")        == 0) action_guard_tour(action);
        else if (strcmp(type, "set_device_param")  == 0) action_set_device_param(action);
        else if (strcmp(type, "snapshot_upload")   == 0) action_snapshot_upload(action, trigger_data);
        else if (strcmp(type, "ir_cut_filter")     == 0) action_ir_cut_filter(action);
        else if (strcmp(type, "privacy_mask")      == 0) action_privacy_mask(action);
        else if (strcmp(type, "wiper")             == 0) action_wiper(action);
        else if (strcmp(type, "light_control")     == 0) action_light_control(action);
        else if (strcmp(type, "acap_control")      == 0) action_acap_control(action);
        else if (strcmp(type, "aoa_get_counts")    == 0) action_aoa_get_counts(action, trigger_data);
        else LOG_WARN("unknown action type '%s'", type);
    }
}

void Actions_Execute(const char* rule_id, cJSON* actions_array, cJSON* trigger_data) {
    if (!actions_array) return;
    execute_from(rule_id, actions_array, 0, trigger_data);
}

int Actions_Test(const char* type, cJSON* config) {
    if (!type || !config) return -1;
    /* Only allow safe/notification action types for testing */
    const char* testable[] = {
        "http_request", "mqtt_publish", "slack_webhook", "teams_webhook",
        "telegram", "email", "send_syslog", "influxdb_write",
        "fire_vapix_event", NULL
    };
    int allowed = 0;
    for (int i = 0; testable[i]; i++) {
        if (strcmp(type, testable[i]) == 0) { allowed = 1; break; }
    }
    if (!allowed) return -2; /* not testable */

    /* Build a single-item actions array with type injected */
    cJSON* action = cJSON_Duplicate(config, 1);
    cJSON_DeleteItemFromObject(action, "type");
    cJSON_AddStringToObject(action, "type", type);
    cJSON* arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, action);

    /* Execute with empty trigger data; capture any errors via LOG_ACTION_ERR */
    g_action_error[0] = '\0';
    cJSON* td = cJSON_CreateObject();
    cJSON_AddStringToObject(td, "type", "test");
    execute_from(NULL, arr, 0, td);

    int failed = g_action_error[0] ? 1 : 0;
    char rname[128];
    snprintf(rname, sizeof(rname), "Test: %s", type);
    EventLog_Append("_test_", rname, 1, NULL, td, 1, failed);
    if (failed) EventLog_Set_Action_Error("_test_", g_action_error);

    cJSON_Delete(td);
    cJSON_Delete(arr);
    return failed ? -1 : 0;
}
