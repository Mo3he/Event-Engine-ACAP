#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <syslog.h>
#include <curl/curl.h>

#include "conditions.h"
#include "variables.h"
#include "../ACAP.h"

#define LOG(fmt, args...)      syslog(LOG_INFO,    "conditions: " fmt, ## args)
#define LOG_WARN(fmt, args...) syslog(LOG_WARNING, "conditions: " fmt, ## args)

static char g_socks5_proxy[256] = "";

void Conditions_Set_Proxy(const char* proxy) {
    snprintf(g_socks5_proxy, sizeof(g_socks5_proxy), "%s", proxy ? proxy : "");
}

/*-----------------------------------------------------
 * time_window
 * config: { "start": "HH:MM", "end": "HH:MM", "days": [0-6,...] }
 * days: 0=Sun, 1=Mon ... 6=Sat
 *-----------------------------------------------------*/
static int cond_time_window(cJSON* cfg) {
    const char* start_s = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "start"));
    const char* end_s   = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "end"));
    cJSON*      days    = cJSON_GetObjectItem(cfg, "days");

    if (!start_s || !end_s) return 0;

    int sh = 0, sm = 0, eh = 0, em = 0;
    sscanf(start_s, "%d:%d", &sh, &sm);
    sscanf(end_s,   "%d:%d", &eh, &em);
    int start_sod = sh * 3600 + sm * 60;
    int end_sod   = eh * 3600 + em * 60;

    int sod = ACAP_DEVICE_Seconds_Since_Midnight();

    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    int dow = tm->tm_wday; /* 0=Sun */

    /* Check day-of-week */
    if (days && cJSON_IsArray(days)) {
        int day_ok = 0;
        cJSON* d;
        cJSON_ArrayForEach(d, days) {
            if ((int)d->valuedouble == dow) { day_ok = 1; break; }
        }
        if (!day_ok) return 0;
    }

    /* Handle wraparound (e.g. 22:00 – 06:00) */
    if (start_sod <= end_sod) {
        return (sod >= start_sod && sod < end_sod);
    } else {
        return (sod >= start_sod || sod < end_sod);
    }
}

/*-----------------------------------------------------
 * event_state
 * config: { "topic0": {...}, "topic1": {...}, "key": "active", "value": true/false/"string" }
 * Polls current VAPIX event state via VAPIX list endpoint.
 *-----------------------------------------------------*/
static int cond_event_state(cJSON* cfg) {
    /* Use VAPIX to query current event state */
    const char* key = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "key"));
    cJSON* expected = cJSON_GetObjectItem(cfg, "value");
    if (!key || !expected) return 0;

    /* Build a VAPIX eventhandler query to get current state.
     * This is a best-effort poll; stateful events return their current value. */
    const char* body =
        "{\"apiVersion\":\"1.0\",\"method\":\"getEventInstances\","
        "\"params\":{\"topicFilter\":\"onvif:\"}}";
    char* resp = ACAP_VAPIX_Post("eventhandler.cgi", body);
    if (!resp) return 0;
    /* Simplified: we can't easily parse the full event instance tree here.
     * Return 0 for now — this condition type is most useful with user-supplied
     * topic filters. A full implementation would parse the XML/JSON response. */
    free(resp);
    return 0;
}

/*-----------------------------------------------------
 * counter
 * config: { "name": "my_counter", "op": "gt", "value": 5 }
 *-----------------------------------------------------*/
static int cond_counter(cJSON* cfg) {
    const char* name = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "name"));
    const char* op   = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "op"));
    cJSON* val_j     = cJSON_GetObjectItem(cfg, "value");
    if (!name || !op || !val_j) return 0;
    double threshold = val_j->valuedouble;
    return Counter_Compare(name, op, threshold);
}

/*-----------------------------------------------------
 * http_check
 * config: { "url": "...", "method": "GET", "expected_status": 200,
 *            "expected_body": "ok" }
 * Makes a quick HTTP request and checks the response.
 *-----------------------------------------------------*/

struct curl_buf { char* data; size_t size; };

static size_t http_check_write(void* ptr, size_t sz, size_t nmemb, void* userdata) {
    struct curl_buf* buf = (struct curl_buf*)userdata;
    size_t new_size = buf->size + sz * nmemb;
    if (new_size > 4096) return 0; /* cap response size */
    buf->data = realloc(buf->data, new_size + 1);
    if (!buf->data) return 0;
    memcpy(buf->data + buf->size, ptr, sz * nmemb);
    buf->size = new_size;
    buf->data[buf->size] = '\0';
    return sz * nmemb;
}

static int cond_http_check(cJSON* cfg) {
    const char* url    = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "url"));
    cJSON* exp_status  = cJSON_GetObjectItem(cfg, "expected_status");
    const char* exp_body = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "expected_body"));
    if (!url) return 0;

    CURL* curl = curl_easy_init();
    if (!curl) return 0;

    struct curl_buf buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_check_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    if (g_socks5_proxy[0]) {
        curl_easy_setopt(curl, CURLOPT_PROXY, g_socks5_proxy);
        curl_easy_setopt(curl, CURLOPT_PROXYTYPE, (long)CURLPROXY_SOCKS5_HOSTNAME);
    }

    const char* method = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "method"));
    if (method && strcmp(method, "POST") == 0)
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) { free(buf.data); return 0; }

    int ok = 1;
    if (exp_status && (long)exp_status->valuedouble != http_code) ok = 0;
    if (ok && exp_body && buf.data && strstr(buf.data, exp_body) == NULL) ok = 0;

    /* JSONPath check: dot-notation traversal into parsed JSON response */
    const char* json_path     = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "json_path"));
    const char* json_expected = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "json_expected"));
    if (ok && json_path && json_expected && buf.data) {
        cJSON* root = cJSON_Parse(buf.data);
        if (!root) {
            ok = 0; /* expected JSON but body is not valid JSON */
        } else {
            /* Walk the dot-separated path */
            char path_copy[256];
            strncpy(path_copy, json_path, sizeof(path_copy) - 1);
            path_copy[sizeof(path_copy) - 1] = '\0';
            cJSON* node = root;
            char* token = strtok(path_copy, ".");
            while (token && node) {
                node = cJSON_GetObjectItem(node, token);
                token = strtok(NULL, ".");
            }
            if (!node) {
                ok = 0; /* path not found */
            } else {
                char val_str[256] = {0};
                if (cJSON_IsString(node))
                    strncpy(val_str, node->valuestring, sizeof(val_str) - 1);
                else if (cJSON_IsNumber(node))
                    snprintf(val_str, sizeof(val_str), "%g", node->valuedouble);
                else if (cJSON_IsBool(node))
                    strncpy(val_str, cJSON_IsTrue(node) ? "true" : "false", sizeof(val_str) - 1);
                if (strcmp(val_str, json_expected) != 0) ok = 0;
            }
            cJSON_Delete(root);
        }
    }

    free(buf.data);
    return ok;
}

/*-----------------------------------------------------
 * io_state
 * config: { "port": 1, "state": "active" }
 * Checks current IO port state via VAPIX.
 *-----------------------------------------------------*/
static int cond_io_state(cJSON* cfg) {
    cJSON* port_j = cJSON_GetObjectItem(cfg, "port");
    const char* expected = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "state"));
    if (!port_j || !expected) return 0;

    int port = (int)port_j->valuedouble;
    char req[64];
    snprintf(req, sizeof(req), "io/port.cgi?checkactive=%d", port);
    char* resp = ACAP_VAPIX_Get(req);
    if (!resp) return 0;

    int result = 0;
    if (strcmp(expected, "active") == 0)
        result = (strstr(resp, "active=yes") != NULL);
    else
        result = (strstr(resp, "active=no") != NULL);
    free(resp);
    return result;
}

/*-----------------------------------------------------
 * variable_compare
 * config: { "name": "my_var", "op": "eq", "value": "hello" }
 *-----------------------------------------------------*/
static int cond_variable_compare(cJSON* cfg) {
    const char* name  = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "name"));
    const char* op    = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "op"));
    const char* value = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "value"));
    if (!name || !op || !value) return 0;
    return Variables_Compare(name, op, value);
}

/*-----------------------------------------------------
 * Public
 *-----------------------------------------------------*/

static int conditions_evaluate_internal(cJSON* conditions_array, int logic, cJSON* trigger_data, int skip_expensive) {
    (void)trigger_data;
    if (!conditions_array || cJSON_GetArraySize(conditions_array) == 0)
        return 1; /* no conditions → always pass */

    /* logic: 0=AND, 1=OR */
    int result = (logic == 0) ? 1 : 0;

    cJSON* cond;
    cJSON_ArrayForEach(cond, conditions_array) {
        const char* type = cJSON_GetStringValue(cJSON_GetObjectItem(cond, "type"));
        if (!type) continue;

        int pass = 0;
        if      (strcmp(type, "time_window")      == 0) pass = cond_time_window(cond);
        else if (strcmp(type, "counter")          == 0) pass = cond_counter(cond);
        else if (strcmp(type, "io_state")         == 0) pass = cond_io_state(cond);
        else if (strcmp(type, "variable_compare") == 0) pass = cond_variable_compare(cond);
        else if (strcmp(type, "event_state")      == 0) pass = skip_expensive ? 1 : cond_event_state(cond);
        else if (strcmp(type, "http_check")       == 0) pass = skip_expensive ? 1 : cond_http_check(cond);
        else { LOG_WARN("unknown condition type '%s'", type); continue; }

        if (logic == 0) { /* AND */
            if (!pass) return 0;
        } else { /* OR */
            if (pass) return 1;
        }
    }
    return result;
}

int Conditions_Evaluate(cJSON* conditions_array, int logic, cJSON* trigger_data) {
    return conditions_evaluate_internal(conditions_array, logic, trigger_data, 0);
}

int Conditions_Evaluate_Lightweight(cJSON* conditions_array, int logic) {
    return conditions_evaluate_internal(conditions_array, logic, NULL, 1);
}
