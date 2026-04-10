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

/* Forward declarations for remote helpers defined in the http_check section */
static char* cond_remote_get(const char* host, const char* user, const char* pass, const char* cgi);
static char* cond_remote_post(const char* host, const char* user, const char* pass, const char* path, const char* body);

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

/* Forward declaration — defined after remote helpers */
static cJSON* cond_remote_pullpoint(const char* host, const char* user,
                                     const char* pass, const char* event_key);
static cJSON* cond_local_pullpoint(const char* event_key);

/*-----------------------------------------------------
 * vapix_event_state
 * Checks the current state of a VAPIX event by querying
 * the event instance list.
 *
 * Match modes (determined by "op" field):
 *   op absent / "eq_str"  → string equality (legacy "expected")
 *   "boolean"             → boolean match (expected "true"/"false"/"1"/"0")
 *   "gt","lt","gte","lte","eq","between" → numeric comparison
 *   "contains"            → substring match
 *-----------------------------------------------------*/
static int value_passes_cond(double actual, const char* op, double thr, double thr2) {
    if (strcmp(op, "gt")      == 0) return actual >  thr;
    if (strcmp(op, "lt")      == 0) return actual <  thr;
    if (strcmp(op, "gte")     == 0) return actual >= thr;
    if (strcmp(op, "lte")     == 0) return actual <= thr;
    if (strcmp(op, "eq")      == 0) return actual == thr;
    if (strcmp(op, "between") == 0) return actual >= thr && actual <= thr2;
    return 0;
}

static int cond_vapix_event_state(cJSON* cfg) {
    const char* event_key = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "event_key"));
    const char* data_key  = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "data_key"));
    if (!event_key || !data_key) return 0;

    const char* op        = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "op"));
    const char* expected  = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "expected"));
    double threshold  = 0, threshold2 = 0;
    if (op && (strcmp(op, "gt") == 0 || strcmp(op, "lt") == 0 ||
               strcmp(op, "gte") == 0 || strcmp(op, "lte") == 0 ||
               strcmp(op, "eq") == 0 || strcmp(op, "between") == 0)) {
        cJSON* thr = cJSON_GetObjectItem(cfg, "threshold");
        if (thr && cJSON_IsNumber(thr)) threshold = thr->valuedouble;
        cJSON* thr2 = cJSON_GetObjectItem(cfg, "threshold2");
        if (thr2 && cJSON_IsNumber(thr2)) threshold2 = thr2->valuedouble;
    }

    /* Legacy fallback: if no op specified, require expected for string equality */
    if (!op && !expected) return 0;

    const char* rh = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "remote_host"));
    const char* ru = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "remote_user"));
    const char* rp = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "remote_pass"));

    /* Remote: use ONVIF PullPoint to get current event state.
     * Local: use internal eventhandler.cgi (accessible on 127.0.0.12). */
    cJSON* root = NULL;
    cJSON* val  = NULL;
    int    remote_mode = (rh && rh[0]);

    if (remote_mode) {
        cJSON* pp_data = cond_remote_pullpoint(rh, ru, rp, event_key);
        if (!pp_data) return 0;
        val = cJSON_GetObjectItem(pp_data, data_key);
        /* val points inside pp_data — keep pp_data alive as root */
        root = pp_data;
    } else {
        const char* body =
            "{\"apiVersion\":\"1.0\",\"method\":\"getEventInstances\"}";
        char* resp = ACAP_VAPIX_Post("eventhandler.cgi", body);
        if (resp) {
            root = cJSON_Parse(resp);
            free(resp);
            if (root) {
                cJSON* data = cJSON_GetObjectItem(root, "data");
                cJSON* instances = data ? cJSON_GetObjectItem(data, "instances") : NULL;
                if (instances && cJSON_IsArray(instances)) {
                    cJSON* inst;
                    cJSON_ArrayForEach(inst, instances) {
                        const char* topic = cJSON_GetStringValue(cJSON_GetObjectItem(inst, "topic"));
                        if (!topic || !strstr(topic, event_key)) continue;
                        cJSON* d = cJSON_GetObjectItem(inst, "data");
                        if (!d) continue;
                        val = cJSON_GetObjectItem(d, data_key);
                        if (val) break;
                    }
                }
            }
        }
        /* Fallback: if eventhandler.cgi is unavailable (404 on some devices),
         * use local PullPoint via /vapix/services. */
        if (!val) {
            if (root) { cJSON_Delete(root); root = NULL; }
            cJSON* pp_data = cond_local_pullpoint(event_key);
            if (pp_data) {
                val = cJSON_GetObjectItem(pp_data, data_key);
                root = pp_data;
            }
        }
    }

    int result = 0;
    if (!val) { cJSON_Delete(root); return 0; }

    /* For remote mode, val is always a cJSON string (from PullPoint XML).
     * For local mode, val may be string, number, or bool from JSON. */
    if (!op || strcmp(op, "eq_str") == 0) {
        /* Legacy string equality (backward compatible) */
        if (expected) {
            char val_str[128] = "";
            if (cJSON_IsString(val))
                snprintf(val_str, sizeof(val_str), "%s", val->valuestring);
            else if (cJSON_IsNumber(val))
                snprintf(val_str, sizeof(val_str), "%g", val->valuedouble);
            else if (cJSON_IsBool(val))
                snprintf(val_str, sizeof(val_str), "%s", cJSON_IsTrue(val) ? "1" : "0");
            if (strcmp(val_str, expected) == 0) result = 1;
        }
    } else if (strcmp(op, "boolean") == 0) {
        /* Boolean match: expected is "true"/"1" or "false"/"0" */
        if (expected) {
            int want = (strcmp(expected, "true") == 0 || strcmp(expected, "1") == 0) ? 1 : 0;
            int actual = 0;
            if (cJSON_IsBool(val)) actual = cJSON_IsTrue(val) ? 1 : 0;
            else if (cJSON_IsString(val)) actual = (strcmp(val->valuestring, "true") == 0 || strcmp(val->valuestring, "1") == 0) ? 1 : 0;
            else if (cJSON_IsNumber(val)) actual = val->valuedouble != 0 ? 1 : 0;
            if (actual == want) result = 1;
        }
    } else if (strcmp(op, "contains") == 0) {
        /* Substring match */
        if (expected) {
            const char* sval = cJSON_GetStringValue(val);
            char num_buf[64] = "";
            if (!sval && cJSON_IsNumber(val)) {
                snprintf(num_buf, sizeof(num_buf), "%g", val->valuedouble);
                sval = num_buf;
            }
            if (sval && strstr(sval, expected) != NULL) result = 1;
        }
    } else {
        /* Numeric comparison */
        double actual_num;
        int valid = 0;
        if (cJSON_IsNumber(val)) {
            actual_num = val->valuedouble; valid = 1;
        } else if (cJSON_IsString(val)) {
            char* endp;
            actual_num = strtod(val->valuestring, &endp);
            if (endp != val->valuestring) valid = 1;
        } else if (cJSON_IsBool(val)) {
            actual_num = cJSON_IsTrue(val) ? 1.0 : 0.0; valid = 1;
        }
        if (valid && value_passes_cond(actual_num, op, threshold, threshold2))
            result = 1;
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

/* Remote device helpers — curl wrappers for condition evaluation on a remote Axis device */
static char* cond_remote_get(const char* host, const char* user, const char* pass, const char* cgi) {
    char url[512], userpwd[512];
    snprintf(url,     sizeof(url),     "http://%s/axis-cgi/%s", host, cgi);
    snprintf(userpwd, sizeof(userpwd), "%s:%s", user ? user : "", pass ? pass : "");
    CURL* curl = curl_easy_init();
    if (!curl) return NULL;
    struct curl_buf buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd);
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_check_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    explicit_bzero(userpwd, sizeof(userpwd));
    if (res != CURLE_OK) { free(buf.data); return NULL; }
    return buf.data;
}

static char* cond_remote_post(const char* host, const char* user, const char* pass,
                               const char* path, const char* body) {
    char url[512], userpwd[512];
    if (path[0] == '/')
        snprintf(url, sizeof(url), "http://%s%s", host, path);
    else
        snprintf(url, sizeof(url), "http://%s/axis-cgi/%s", host, path);
    snprintf(userpwd, sizeof(userpwd), "%s:%s", user ? user : "", pass ? pass : "");
    CURL* curl = curl_easy_init();
    if (!curl) return NULL;
    struct curl_buf buf = {NULL, 0};
    struct curl_slist* hdrs = curl_slist_append(NULL, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd);
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_check_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    explicit_bzero(userpwd, sizeof(userpwd));
    if (res != CURLE_OK) { free(buf.data); return NULL; }
    return buf.data;
}

/* POST SOAP to a remote Axis device (e.g. /vapix/services) */
static char* cond_remote_soap_post(const char* host, const char* user, const char* pass,
                                    const char* path, const char* soap_body) {
    char url[512], userpwd[512];
    snprintf(url, sizeof(url), "http://%s%s", host, path);
    snprintf(userpwd, sizeof(userpwd), "%s:%s", user ? user : "", pass ? pass : "");
    CURL* curl = curl_easy_init();
    if (!curl) return NULL;
    struct curl_buf buf = {NULL, 0};
    struct curl_slist* hdrs = curl_slist_append(NULL,
        "Content-Type: application/soap+xml; charset=utf-8");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd);
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, soap_body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_check_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    explicit_bzero(userpwd, sizeof(userpwd));
    if (res != CURLE_OK) { free(buf.data); return NULL; }
    return buf.data;
}

/* Extract an XML attribute value from within a tag region.
 * Looks for attr="value" between xml and end pointers. */
static size_t cond_xml_attr(const char* xml, const char* end,
                             const char* attr, char* out, size_t out_sz) {
    char needle[64];
    snprintf(needle, sizeof(needle), "%s=\"", attr);
    const char* p = strstr(xml, needle);
    if (!p || p >= end) return 0;
    p += strlen(needle);
    const char* q = strchr(p, '"');
    if (!q || q >= end) return 0;
    size_t len = (size_t)(q - p);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return len;
}

/* Query current event state from a remote device using ONVIF PullPoint.
 * Returns a cJSON object with all SimpleItem Name/Value pairs from the
 * first NotificationMessage whose Topic contains event_key, or NULL. */
static cJSON* cond_remote_pullpoint(const char* host, const char* user,
                                     const char* pass, const char* event_key) {
    static const char create_soap[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\">"
        "<soap:Body><tev:CreatePullPointSubscription>"
        "<tev:InitialTerminationTime>PT60S</tev:InitialTerminationTime>"
        "</tev:CreatePullPointSubscription></soap:Body></soap:Envelope>";

    char* resp = cond_remote_soap_post(host, user, pass, "/vapix/services", create_soap);
    if (!resp) return NULL;

    char* sid_tag = strstr(resp, "SubscriptionId");
    if (!sid_tag || strstr(resp, "Fault")) { free(resp); return NULL; }
    char* sid_gt = strchr(sid_tag, '>');
    if (!sid_gt) { free(resp); return NULL; }
    sid_gt++;
    char* sid_lt = strchr(sid_gt, '<');
    if (!sid_lt) { free(resp); return NULL; }
    char sub_id[32];
    size_t sid_len = (size_t)(sid_lt - sid_gt);
    if (sid_len >= sizeof(sub_id)) sid_len = sizeof(sub_id) - 1;
    memcpy(sub_id, sid_gt, sid_len);
    sub_id[sid_len] = '\0';
    free(resp);
    if (strspn(sub_id, "0123456789") != sid_len) return NULL;

    cJSON* result = NULL;
    int drained = 0;
    for (int attempt = 0; attempt < 53 && !result; attempt++) {
        const char* timeout_str = drained ? "PT5S" : "PT1S";
        char pull_soap[1024];
        snprintf(pull_soap, sizeof(pull_soap),
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
            " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\""
            " xmlns:dom0=\"http://www.axis.com/2009/event\">"
            "<soap:Header><dom0:SubscriptionId>%s</dom0:SubscriptionId></soap:Header>"
            "<soap:Body><tev:PullMessages>"
            "<tev:Timeout>%s</tev:Timeout>"
            "<tev:MessageLimit>200</tev:MessageLimit>"
            "</tev:PullMessages></soap:Body></soap:Envelope>",
            sub_id, timeout_str);

        resp = cond_remote_soap_post(host, user, pass, "/vapix/services", pull_soap);
        if (!resp) break;
        if (strstr(resp, "Fault")) { free(resp); break; }

        const char* pos = resp;
        while ((pos = strstr(pos, "<wsnt:NotificationMessage")) != NULL) {
            const char* msg_end = strstr(pos + 1, "</wsnt:NotificationMessage>");
            if (!msg_end) break;
            msg_end += strlen("</wsnt:NotificationMessage>");

            const char* t_start = strstr(pos, "<wsnt:Topic");
            if (!t_start || t_start >= msg_end) { pos = msg_end; continue; }
            t_start = strchr(t_start, '>');
            if (!t_start) { pos = msg_end; continue; }
            t_start++;
            const char* t_end = strchr(t_start, '<');
            if (!t_end || t_end >= msg_end) { pos = msg_end; continue; }

            char topic[512];
            size_t tlen = (size_t)(t_end - t_start);
            if (tlen >= sizeof(topic)) tlen = sizeof(topic) - 1;
            memcpy(topic, t_start, tlen);
            topic[tlen] = '\0';

            if (!strstr(topic, event_key)) { pos = msg_end; continue; }

            result = cJSON_CreateObject();
            const char* si = pos;
            while ((si = strstr(si, "SimpleItem")) != NULL && si < msg_end) {
                const char* tag_lt = si;
                while (tag_lt > pos && *tag_lt != '<') tag_lt--;
                const char* tag_gt = strchr(si, '>');
                if (!tag_gt || tag_gt >= msg_end) break;

                char name[128] = "", value[256] = "";
                cond_xml_attr(tag_lt, tag_gt + 1, "Name", name, sizeof(name));
                cond_xml_attr(tag_lt, tag_gt + 1, "Value", value, sizeof(value));
                if (name[0] && value[0]) {
                    cJSON_DeleteItemFromObject(result, name);
                    cJSON_AddStringToObject(result, name, value);
                }
                si = tag_gt + 1;
            }
            break;
        }

        if (!strstr(resp, "NotificationMessage")) {
            free(resp);
            if (drained) break;
            drained = attempt + 1;
            continue;
        }
        free(resp);
    }

    char unsub_soap[512];
    snprintf(unsub_soap, sizeof(unsub_soap),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\""
        " xmlns:dom0=\"http://www.axis.com/2009/event\">"
        "<soap:Header><dom0:SubscriptionId>%s</dom0:SubscriptionId></soap:Header>"
        "<soap:Body><wsnt:Unsubscribe/></soap:Body></soap:Envelope>",
        sub_id);
    char* unsub_resp = cond_remote_soap_post(host, user, pass, "/vapix/services", unsub_soap);
    if (!unsub_resp || strstr(unsub_resp, "Fault"))
        LOG_WARN("PullPoint unsubscribe failed for %s (sub %s)", host, sub_id);
    free(unsub_resp);

    return result;
}

/* Local PullPoint: uses ACAP_VAPIX_Soap_Post via the loopback (no external
 * credentials needed). Same drain-then-wait logic as the remote version but
 * the subscription is already authenticated by the ACAP framework. */
static cJSON* cond_local_pullpoint(const char* event_key) {
    static const char create_soap[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\">"
        "<soap:Body><tev:CreatePullPointSubscription>"
        "<tev:InitialTerminationTime>PT60S</tev:InitialTerminationTime>"
        "</tev:CreatePullPointSubscription></soap:Body></soap:Envelope>";

    char* resp = ACAP_VAPIX_Soap_Post("/vapix/services", create_soap);
    if (!resp) return NULL;

    char* sid_tag = strstr(resp, "SubscriptionId");
    if (!sid_tag || strstr(resp, "Fault")) { free(resp); return NULL; }
    char* sid_gt = strchr(sid_tag, '>');
    if (!sid_gt) { free(resp); return NULL; }
    sid_gt++;
    char* sid_lt = strchr(sid_gt, '<');
    if (!sid_lt) { free(resp); return NULL; }
    char sub_id[32];
    size_t sid_len = (size_t)(sid_lt - sid_gt);
    if (sid_len >= sizeof(sub_id)) sid_len = sizeof(sub_id) - 1;
    memcpy(sub_id, sid_gt, sid_len);
    sub_id[sid_len] = '\0';
    free(resp);
    if (strspn(sub_id, "0123456789") != sid_len) return NULL;

    cJSON* result = NULL;
    int drained = 0;
    for (int attempt = 0; attempt < 53 && !result; attempt++) {
        const char* timeout_str = drained ? "PT5S" : "PT1S";
        char pull_soap[1024];
        snprintf(pull_soap, sizeof(pull_soap),
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
            " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\""
            " xmlns:dom0=\"http://www.axis.com/2009/event\">"
            "<soap:Header><dom0:SubscriptionId>%s</dom0:SubscriptionId></soap:Header>"
            "<soap:Body><tev:PullMessages>"
            "<tev:Timeout>%s</tev:Timeout>"
            "<tev:MessageLimit>200</tev:MessageLimit>"
            "</tev:PullMessages></soap:Body></soap:Envelope>",
            sub_id, timeout_str);

        resp = ACAP_VAPIX_Soap_Post("/vapix/services", pull_soap);
        if (!resp) break;
        if (strstr(resp, "Fault")) { free(resp); break; }

        const char* pos = resp;
        while ((pos = strstr(pos, "<wsnt:NotificationMessage")) != NULL) {
            const char* msg_end = strstr(pos + 1, "</wsnt:NotificationMessage>");
            if (!msg_end) break;
            msg_end += strlen("</wsnt:NotificationMessage>");

            const char* t_start = strstr(pos, "<wsnt:Topic");
            if (!t_start || t_start >= msg_end) { pos = msg_end; continue; }
            t_start = strchr(t_start, '>');
            if (!t_start) { pos = msg_end; continue; }
            t_start++;
            const char* t_end = strchr(t_start, '<');
            if (!t_end || t_end >= msg_end) { pos = msg_end; continue; }

            char topic[512];
            size_t tlen = (size_t)(t_end - t_start);
            if (tlen >= sizeof(topic)) tlen = sizeof(topic) - 1;
            memcpy(topic, t_start, tlen);
            topic[tlen] = '\0';

            if (!strstr(topic, event_key)) { pos = msg_end; continue; }

            result = cJSON_CreateObject();
            const char* si = pos;
            while ((si = strstr(si, "SimpleItem")) != NULL && si < msg_end) {
                const char* tag_lt = si;
                while (tag_lt > pos && *tag_lt != '<') tag_lt--;
                const char* tag_gt = strchr(si, '>');
                if (!tag_gt || tag_gt >= msg_end) break;

                char name[128] = "", value[256] = "";
                cond_xml_attr(tag_lt, tag_gt + 1, "Name", name, sizeof(name));
                cond_xml_attr(tag_lt, tag_gt + 1, "Value", value, sizeof(value));
                if (name[0] && value[0]) {
                    cJSON_DeleteItemFromObject(result, name);
                    cJSON_AddStringToObject(result, name, value);
                }
                si = tag_gt + 1;
            }
            break;
        }

        if (!strstr(resp, "NotificationMessage")) {
            free(resp);
            if (drained) break;
            drained = attempt + 1;
            continue;
        }
        free(resp);
    }

    char unsub_soap[512];
    snprintf(unsub_soap, sizeof(unsub_soap),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\""
        " xmlns:dom0=\"http://www.axis.com/2009/event\">"
        "<soap:Header><dom0:SubscriptionId>%s</dom0:SubscriptionId></soap:Header>"
        "<soap:Body><wsnt:Unsubscribe/></soap:Body></soap:Envelope>",
        sub_id);
    char* unsub_resp = ACAP_VAPIX_Soap_Post("/vapix/services", unsub_soap);
    free(unsub_resp);

    return result;
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
    cJSON* allow_insecure = cJSON_GetObjectItem(cfg, "allow_insecure");
    if (allow_insecure && cJSON_IsTrue(allow_insecure)) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
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
    const char* rh = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "remote_host"));
    const char* ru = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "remote_user"));
    const char* rp = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "remote_pass"));

    char req[64];
    snprintf(req, sizeof(req), "io/port.cgi?checkactive=%d", port);
    char* resp = (rh && rh[0])
        ? cond_remote_get(rh, ru, rp, req)
        : ACAP_VAPIX_Get(req);
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

    const char* rh = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "remote_host"));
    const char* ru = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "remote_user"));
    const char* rp = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "remote_pass"));
    char* raw = (rh && rh[0])
        ? cond_remote_post(rh, ru, rp, "/local/objectanalytics/control.cgi", body)
        : ACAP_VAPIX_Post_Path("/local/objectanalytics/control.cgi", body);
    if (!raw) return 0;

    cJSON* root = cJSON_Parse(raw);
    free(raw);
    if (!root) return 0;

    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (!data) { cJSON_Delete(root); return 0; }
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
    /* NOTE: trigger_data is currently unused but available for future enhancements
     * (e.g., condition: "fire only if trigger value > 30") */
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
