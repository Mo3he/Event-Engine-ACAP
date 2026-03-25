#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <syslog.h>
#include <curl/curl.h>

#include "conditions.h"
#include "variables.h"
#include "scheduler.h"
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
 * day_night
 * config: { "state": "day"|"night", "lat": 51.5, "lon": -0.12 }
 * Uses the sunrise/sunset engine from scheduler.c.
 * If no lat/lon provided, reads from engine settings.
 *-----------------------------------------------------*/
static int cond_day_night(cJSON* cfg) {
    const char* want = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "state"));
    if (!want) want = "day";

    double lat = 0.0, lon = 0.0;
    cJSON* lat_j = cJSON_GetObjectItem(cfg, "lat");
    cJSON* lon_j = cJSON_GetObjectItem(cfg, "lon");
    if (lat_j) lat = lat_j->valuedouble;
    if (lon_j) lon = lon_j->valuedouble;

    /* Fallback to engine settings if not specified per-condition */
    if (!lat_j && !lon_j) {
        cJSON* eng = ACAP_Get_Config("engine");
        if (eng) {
            cJSON* sl = cJSON_GetObjectItem(eng, "latitude");
            cJSON* so = cJSON_GetObjectItem(eng, "longitude");
            if (sl) lat = sl->valuedouble;
            if (so) lon = so->valuedouble;
        }
    }

    int is_day = Scheduler_Is_Daytime(lat, lon);
    if (is_day < 0) return 0; /* polar — can't determine */
    return (strcmp(want, "day") == 0) ? is_day : !is_day;
}

/*-----------------------------------------------------
 * vapix_event_state
 * config: { "event_key": "tns1:Device/tnsaxis:IO/VirtualInput",
 *            "data_key": "active", "expected": "1" }
 * Checks the current state of a VAPIX event by querying
 * the event instance list.
 *-----------------------------------------------------*/
static int cond_vapix_event_state(cJSON* cfg) {
    const char* event_key = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "event_key"));
    const char* data_key  = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "data_key"));
    const char* expected  = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "expected"));
    if (!event_key || !data_key || !expected) return 0;

    const char* body =
        "{\"apiVersion\":\"1.0\",\"method\":\"getEventInstances\"}";
    char* resp = ACAP_VAPIX_Post("eventhandler.cgi", body);
    if (!resp) return 0;

    cJSON* root = cJSON_Parse(resp);
    free(resp);
    if (!root) return 0;

    int result = 0;
    /* Response: { "data": { "instances": [ { "topic": "...", "data": { "key": "value" } } ] } } */
    cJSON* data = cJSON_GetObjectItem(root, "data");
    cJSON* instances = data ? cJSON_GetObjectItem(data, "instances") : NULL;
    if (instances && cJSON_IsArray(instances)) {
        cJSON* inst;
        cJSON_ArrayForEach(inst, instances) {
            const char* topic = cJSON_GetStringValue(cJSON_GetObjectItem(inst, "topic"));
            if (!topic || !strstr(topic, event_key)) continue;
            cJSON* d = cJSON_GetObjectItem(inst, "data");
            if (!d) continue;
            cJSON* val = cJSON_GetObjectItem(d, data_key);
            if (!val) continue;
            char val_str[128] = "";
            if (cJSON_IsString(val))
                snprintf(val_str, sizeof(val_str), "%s", val->valuestring);
            else if (cJSON_IsNumber(val))
                snprintf(val_str, sizeof(val_str), "%g", val->valuedouble);
            else if (cJSON_IsBool(val))
                snprintf(val_str, sizeof(val_str), "%s", cJSON_IsTrue(val) ? "1" : "0");
            if (strcmp(val_str, expected) == 0) { result = 1; break; }
        }
    }
    cJSON_Delete(root);
    return result;
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
 * aoa_occupancy
 * config: { "scenario_id": 1, "object_class": "human"|"car"|...|"any",
 *            "op": "gt"|"gte"|"lt"|"lte"|"eq", "value": 3 }
 * Calls AOA getOccupancy and compares the count against a threshold.
 *-----------------------------------------------------*/
static int cond_aoa_occupancy(cJSON* cfg) {
    cJSON* sid_j     = cJSON_GetObjectItem(cfg, "scenario_id");
    const char* op   = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "op"));
    cJSON* thresh_j  = cJSON_GetObjectItem(cfg, "value");
    if (!sid_j || !op || !thresh_j) return 0;

    int    scenario_id = (int)sid_j->valuedouble;
    double threshold   = thresh_j->valuedouble;

    char body[256];
    snprintf(body, sizeof(body),
        "{\"method\":\"getOccupancy\",\"apiVersion\":\"1.0\","
        "\"params\":{\"scenario\":%d}}", scenario_id);

    char* raw = ACAP_VAPIX_Post_Path("/local/objectanalytics/control.cgi", body);
    if (!raw) return 0;

    cJSON* root = cJSON_Parse(raw);
    free(raw);
    if (!root) return 0;

    cJSON* data = cJSON_GetObjectItem(root, "data");
    const char* cls = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "object_class"));
    cJSON* count_j = NULL;
    if (cls && cls[0] && strcmp(cls, "any") != 0)
        count_j = cJSON_GetObjectItem(data, cls);
    else
        count_j = cJSON_GetObjectItem(data, "total");

    int result = 0;
    if (count_j && cJSON_IsNumber(count_j)) {
        double count = count_j->valuedouble;
        if      (strcmp(op, "gt")  == 0) result = count >  threshold;
        else if (strcmp(op, "gte") == 0) result = count >= threshold;
        else if (strcmp(op, "lt")  == 0) result = count <  threshold;
        else if (strcmp(op, "lte") == 0) result = count <= threshold;
        else if (strcmp(op, "eq")  == 0) result = count == threshold;
    }
    cJSON_Delete(root);
    return result;
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
        else if (strcmp(type, "day_night")         == 0) pass = cond_day_night(cond);
        else if (strcmp(type, "vapix_event_state")== 0) pass = skip_expensive ? 1 : cond_vapix_event_state(cond);
        else if (strcmp(type, "http_check")       == 0) pass = skip_expensive ? 1 : cond_http_check(cond);
        else if (strcmp(type, "aoa_occupancy")    == 0) pass = skip_expensive ? 1 : cond_aoa_occupancy(cond);
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
