#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <glib.h>
#include <glib-unix.h>

#include "ACAP.h"
#include "cJSON.h"
#include "engine/rule_engine.h"
#include "engine/event_log.h"
#include "engine/variables.h"
#include "engine/triggers.h"
#include "engine/actions.h"
#include "engine/conditions.h"
#include "engine/mqtt_client.h"

#define APP_PACKAGE "acap_event_engine"

#define LOG(fmt, args...)      { syslog(LOG_INFO,    fmt, ## args); printf(fmt "\n", ## args); }
#define LOG_WARN(fmt, args...) { syslog(LOG_WARNING, fmt, ## args); printf("WARN: " fmt "\n", ## args); }

static GMainLoop* main_loop = NULL;

/*=====================================================
 * VAPIX event callback (GMainLoop thread)
 *=====================================================*/
static void Event_Callback(cJSON* event, void* user_data) {
    (void)user_data;
    RuleEngine_Dispatch_Event(event);
}

/*=====================================================
 * GLib timers
 *=====================================================*/
static gboolean Engine_Tick(gpointer user_data) {
    (void)user_data;
    RuleEngine_Tick();
    Actions_Digest_Tick();
    return G_SOURCE_CONTINUE;
}

/*=====================================================
 * Settings callback
 *=====================================================*/
/* Password is stored separately — never returned by the settings endpoint */
#define MQTT_PASS_FILE "localdata/mqtt_password.txt"

static void save_mqtt_password(const char* pw) {
    if (!pw || !pw[0]) return;
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "pw", pw);
    ACAP_FILE_Write(MQTT_PASS_FILE, obj);
    cJSON_Delete(obj);
}

static void load_mqtt_password(char* out, size_t out_size) {
    cJSON* obj = ACAP_FILE_Read(MQTT_PASS_FILE);
    if (!obj) return;
    const char* pw = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "pw"));
    if (pw) snprintf(out, out_size, "%s", pw);
    cJSON_Delete(obj);
}

static void apply_mqtt_config(cJSON* mqtt_json) {
    if (!mqtt_json) return;
    MQTT_Config mc;
    memset(&mc, 0, sizeof(mc));
    const char* host = cJSON_GetStringValue(cJSON_GetObjectItem(mqtt_json, "host"));
    snprintf(mc.host,      sizeof(mc.host),      "%s", host ? host : "");
    const char* cid  = cJSON_GetStringValue(cJSON_GetObjectItem(mqtt_json, "client_id"));
    snprintf(mc.client_id, sizeof(mc.client_id), "%s", cid  ? cid  : "acap_event_engine");
    const char* user = cJSON_GetStringValue(cJSON_GetObjectItem(mqtt_json, "username"));
    snprintf(mc.username,  sizeof(mc.username),  "%s", user ? user : "");

    /* Password: if provided in the update, save it; otherwise use stored value */
    const char* pass = cJSON_GetStringValue(cJSON_GetObjectItem(mqtt_json, "password"));
    if (pass && pass[0]) {
        save_mqtt_password(pass);
        snprintf(mc.password, sizeof(mc.password), "%s", pass);
        /* Strip from in-memory settings so GET /settings never returns it */
        cJSON* pw_item = cJSON_GetObjectItem(mqtt_json, "password");
        if (pw_item) cJSON_SetValuestring(pw_item, "");
    } else {
        load_mqtt_password(mc.password, sizeof(mc.password));
    }

    cJSON* port = cJSON_GetObjectItem(mqtt_json, "port");
    mc.port = port ? (int)port->valuedouble : 1883;
    cJSON* ka   = cJSON_GetObjectItem(mqtt_json, "keepalive");
    mc.keepalive = ka ? (int)ka->valuedouble : 60;
    mc.enabled = cJSON_IsTrue(cJSON_GetObjectItem(mqtt_json, "enabled")) ? 1 : 0;
    MQTT_Reconfigure(&mc);
}

static void mqtt_message_cb(const char* topic, const char* payload,
                             int payload_len, void* user_data) {
    (void)user_data;
    Triggers_On_MQTT_Message(topic, payload, payload_len);
}

static void apply_proxy_config(cJSON* engine_cfg) {
    const char* proxy = cJSON_GetStringValue(cJSON_GetObjectItem(engine_cfg, "socks5_proxy"));
    if (!proxy) proxy = "";

    Actions_Set_Proxy(proxy);
    Conditions_Set_Proxy(proxy);

    /* Parse "host:port" for MQTT (raw socket client needs host and port separately) */
    char ph[256] = "";
    int  pp = 1080;
    if (proxy[0]) {
        const char* colon = strrchr(proxy, ':');
        if (colon) {
            size_t hlen = (size_t)(colon - proxy);
            if (hlen >= sizeof(ph)) hlen = sizeof(ph) - 1;
            memcpy(ph, proxy, hlen); ph[hlen] = '\0';
            pp = atoi(colon + 1);
        } else {
            snprintf(ph, sizeof(ph), "%s", proxy);
        }
    }
    MQTT_Set_Proxy(ph[0] ? ph : NULL, pp);
}

#define SMTP_PASS_FILE "localdata/smtp_password.txt"

static void load_smtp_password(void) {
    cJSON* obj = ACAP_FILE_Read(SMTP_PASS_FILE);
    if (!obj) return;
    const char* pw = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "pw"));
    if (pw && pw[0]) {
        /* Inject into in-memory smtp config so actions.c can read it */
        cJSON* smtp_cfg = ACAP_Get_Config("smtp");
        if (smtp_cfg) {
            cJSON* pw_item = cJSON_GetObjectItem(smtp_cfg, "password");
            if (pw_item) cJSON_SetValuestring(pw_item, pw);
            else         cJSON_AddStringToObject(smtp_cfg, "password", pw);
        }
    }
    cJSON_Delete(obj);
}

static void apply_smtp_config(cJSON* smtp_json) {
    if (!smtp_json) return;
    const char* pass = cJSON_GetStringValue(cJSON_GetObjectItem(smtp_json, "password"));
    if (pass && pass[0]) {
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "pw", pass);
        ACAP_FILE_Write(SMTP_PASS_FILE, obj);
        cJSON_Delete(obj);
        /* Strip from in-memory settings so GET never returns it */
        cJSON* pw_item = cJSON_GetObjectItem(smtp_json, "password");
        if (pw_item) cJSON_SetValuestring(pw_item, "");
    }
    /* Reload so in-memory config has the password for action_email() */
    load_smtp_password();
}

static void Settings_Updated(const char* service, cJSON* data) {
    if (strcmp(service, "mqtt") == 0)
        apply_mqtt_config(data);
    else if (strcmp(service, "engine") == 0)
        apply_proxy_config(data);
    else if (strcmp(service, "smtp") == 0)
        apply_smtp_config(data);
}

/*=====================================================
 * HTTP Helpers
 *=====================================================*/
static const char* get_method(const ACAP_HTTP_Request req) {
    return ACAP_HTTP_Get_Method(req);
}

static cJSON* get_body_json(const ACAP_HTTP_Request req) {
    const char* body = ACAP_HTTP_Get_Body(req);
    if (!body || !*body) return NULL;
    return cJSON_Parse(body);
}

/*=====================================================
 * GET/POST/PUT/DELETE /local/acap_event_engine/rules
 *=====================================================*/
static void HTTP_Rules(ACAP_HTTP_Response resp, const ACAP_HTTP_Request req) {
    const char* method = get_method(req);

    if (strcmp(method, "GET") == 0) {
        char* id = ACAP_HTTP_Request_Param(req, "id");
        if (id && *id) {
            cJSON* rule = RuleEngine_Get(id);
            free(id);
            if (!rule) { ACAP_HTTP_Respond_Error(resp, 404, "Rule not found"); return; }
            ACAP_HTTP_Respond_JSON(resp, rule);
            cJSON_Delete(rule);
        } else {
            if (id) free(id);
            cJSON* list = RuleEngine_List();
            ACAP_HTTP_Respond_JSON(resp, list);
            cJSON_Delete(list);
        }

    } else if (strcmp(method, "POST") == 0) {
        cJSON* body = get_body_json(req);
        if (!body) { ACAP_HTTP_Respond_Error(resp, 400, "Invalid JSON body"); return; }

        char* id = ACAP_HTTP_Request_Param(req, "id");
        if (id && *id) {
            /* POST ?id=... → update existing rule (PUT body not supported by ACAP) */
            cJSON* en   = cJSON_GetObjectItem(body, "enabled");
            cJSON* name = cJSON_GetObjectItem(body, "name");
            if (en && !name) {
                /* Enable/disable shortcut */
                int result = RuleEngine_SetEnabled(id, cJSON_IsTrue(en) ? 1 : 0);
                free(id); cJSON_Delete(body);
                if (!result) { ACAP_HTTP_Respond_Error(resp, 404, "Rule not found"); return; }
                ACAP_HTTP_Respond_Text(resp, "OK");
                return;
            }
            int result = RuleEngine_Update(id, body);
            free(id); cJSON_Delete(body);
            if (!result) { ACAP_HTTP_Respond_Error(resp, 404, "Rule not found"); return; }
            ACAP_HTTP_Respond_Text(resp, "OK");
        } else {
            /* POST (no id) → create new rule */
            if (id) free(id);
            char new_id[37] = "";
            if (!RuleEngine_Add(body, new_id)) {
                cJSON_Delete(body);
                ACAP_HTTP_Respond_Error(resp, 500, "Failed to add rule");
                return;
            }
            cJSON_Delete(body);
            cJSON* result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "id", new_id);
            cJSON_AddStringToObject(result, "status", "created");
            ACAP_HTTP_Respond_JSON(resp, result);
            cJSON_Delete(result);
        }

    } else if (strcmp(method, "DELETE") == 0) {
        char* id = ACAP_HTTP_Request_Param(req, "id");
        if (!id || !*id) {
            if (id) free(id);
            ACAP_HTTP_Respond_Error(resp, 400, "Missing id parameter");
            return;
        }
        int result = RuleEngine_Delete(id);
        free(id);
        if (!result) { ACAP_HTTP_Respond_Error(resp, 404, "Rule not found"); return; }
        ACAP_HTTP_Respond_Text(resp, "OK");

    } else {
        ACAP_HTTP_Respond_Error(resp, 405, "Method not allowed");
    }
}

/*=====================================================
 * GET /local/acap_event_engine/triggers
 * Returns catalog of available trigger types
 *=====================================================*/
static void HTTP_Triggers(ACAP_HTTP_Response resp, const ACAP_HTTP_Request req) {
    (void)req;
    cJSON* catalog = Triggers_Catalog();
    ACAP_HTTP_Respond_JSON(resp, catalog);
    cJSON_Delete(catalog);
}

/*=====================================================
 * GET /local/acap_event_engine/actions
 * Returns catalog of available action types
 *=====================================================*/
static void HTTP_Actions(ACAP_HTTP_Response resp, const ACAP_HTTP_Request req) {
    (void)req;
    cJSON* arr = cJSON_CreateArray();

    const struct { const char* type; const char* label; const char* desc; } action_types[] = {
        {"http_request",      "HTTP Request",      "Make an HTTP GET/POST/PUT request to any URL"},
        {"mqtt_publish",      "MQTT Publish",      "Publish a message to an MQTT topic"},
        {"slack_webhook",     "Slack",             "Send a message to a Slack channel via incoming webhook"},
        {"teams_webhook",     "Teams",             "Send an Adaptive Card to Microsoft Teams via webhook"},
        {"telegram",          "Telegram",          "Send a message via Telegram Bot API"},
        {"email",             "Email (SMTP)",      "Send an email via SMTP with optional TLS"},
        {"snapshot_upload",   "Snapshot Upload",   "Capture a JPEG and POST/PUT it to a URL"},
        {"ftp_upload",        "FTP Upload",        "Capture a JPEG and upload it to an FTP/SFTP server"},
        {"send_syslog",       "Send Syslog",       "Write a message to the system log"},
        {"recording",         "Recording",         "Start or stop local camera recording"},
        {"overlay_text",      "Overlay Text",      "Set dynamic text overlay on video stream"},
        {"ptz_preset",        "PTZ Preset",        "Move camera to a PTZ preset position"},
        {"guard_tour",        "Guard Tour",        "Start or stop a PTZ guard tour"},
        {"ir_cut_filter",     "IR Cut Filter",     "Force IR cut filter On, Off, or Auto"},
        {"privacy_mask",      "Privacy Mask",      "Enable or disable a named privacy mask"},
        {"wiper",             "Wiper",             "Trigger the windshield wiper"},
        {"light_control",     "Light Control",     "Control an Axis illuminator"},
        {"audio_clip",        "Audio Clip",        "Play an audio clip on the camera speaker"},
        {"siren_light",       "Siren / Light",     "Start or stop a siren/LED profile"},
        {"io_output",         "I/O Output",        "Activate or deactivate a digital output port"},
        {"digest",            "Notification Digest","Buffer events and send a batched summary at an interval"},
        {"delay",             "Delay",             "Wait N seconds before continuing action sequence"},
        {"set_variable",      "Set Variable",      "Store a named variable value"},
        {"increment_counter", "Increment Counter", "Increment, decrement, reset, or set a counter"},
        {"run_rule",          "Run Rule",          "Trigger another rule (rule chaining)"},
        {"fire_vapix_event",  "Fire VAPIX Event",  "Emit a declared VAPIX event"},
        {"vapix_query",       "VAPIX Event Query", "Fetch latest cached VAPIX event data as template variables"},
        {"set_device_param",  "Set Device Parameter", "Update a camera parameter via param.cgi"},
        {"acap_control",      "ACAP Control",      "Start, stop, or restart another ACAP application"},
        {"influxdb_write",    "InfluxDB Write",    "Write a data point to InfluxDB (v1 or v2) in line protocol"},
        {"aoa_get_counts",    "AOA Get Counts",    "Fetch accumulated crossline counts from an Object Analytics scenario"},
        {NULL, NULL, NULL}
    };

    for (int i = 0; action_types[i].type; i++) {
        cJSON* t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "type",        action_types[i].type);
        cJSON_AddStringToObject(t, "label",       action_types[i].label);
        cJSON_AddStringToObject(t, "description", action_types[i].desc);
        cJSON_AddItemToArray(arr, t);
    }

    ACAP_HTTP_Respond_JSON(resp, arr);
    cJSON_Delete(arr);
}

/*=====================================================
 * GET /local/acap_event_engine/events
 *=====================================================*/
static void HTTP_Events(ACAP_HTTP_Response resp, const ACAP_HTTP_Request req) {
    int limit = 50;
    char* limit_str = ACAP_HTTP_Request_Param(req, "limit");
    if (limit_str) { limit = atoi(limit_str); free(limit_str); }
    if (limit <= 0 || limit > EVENT_LOG_SIZE) limit = 50;

    char* rule_id = ACAP_HTTP_Request_Param(req, "rule");
    cJSON* result;
    if (rule_id && *rule_id) {
        result = EventLog_Get_For_Rule(rule_id, limit);
    } else {
        result = EventLog_Get_Recent(limit);
    }
    if (rule_id) free(rule_id);

    ACAP_HTTP_Respond_JSON(resp, result);
    cJSON_Delete(result);
}

/*=====================================================
 * GET /local/acap_event_engine/status
 *=====================================================*/
static void HTTP_Status(ACAP_HTTP_Response resp, const ACAP_HTTP_Request req) {
    (void)req;
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "uptime",         ACAP_DEVICE_Uptime());
    cJSON_AddNumberToObject(obj, "rules_total",    RuleEngine_Count());
    cJSON_AddNumberToObject(obj, "rules_enabled",  RuleEngine_Count_Enabled());
    cJSON_AddNumberToObject(obj, "events_today",   EventLog_Count_Today());
    cJSON_AddStringToObject(obj, "time",           ACAP_DEVICE_ISOTime());
    cJSON_AddStringToObject(obj, "engine_version", "1.7.2");

    cJSON* mqtt_st = MQTT_Status();
    cJSON_AddItemToObject(obj, "mqtt", mqtt_st);

    cJSON* dev = cJSON_AddObjectToObject(obj, "device");
    cJSON_AddStringToObject(dev, "serial",  ACAP_DEVICE_Prop("serial")   ? ACAP_DEVICE_Prop("serial")   : "");
    cJSON_AddStringToObject(dev, "model",   ACAP_DEVICE_Prop("model")    ? ACAP_DEVICE_Prop("model")    : "");
    cJSON_AddStringToObject(dev, "ip",      ACAP_DEVICE_Prop("IPv4")     ? ACAP_DEVICE_Prop("IPv4")     : "");
    cJSON_AddStringToObject(dev, "firmware",ACAP_DEVICE_Prop("firmware") ? ACAP_DEVICE_Prop("firmware") : "");

    ACAP_HTTP_Respond_JSON(resp, obj);
    cJSON_Delete(obj);
}

/*=====================================================
 * POST /local/acap_event_engine/fire
 * Manual rule fire or webhook dispatch
 *=====================================================*/
static void HTTP_Fire(ACAP_HTTP_Response resp, const ACAP_HTTP_Request req) {
    const char* method = get_method(req);

    if (strcmp(method, "POST") != 0) {
        ACAP_HTTP_Respond_Error(resp, 405, "POST required");
        return;
    }

    cJSON* body = get_body_json(req);

    /* Webhook mode: ?token=TOKEN or body.token */
    char* token_param = ACAP_HTTP_Request_Param(req, "token");
    const char* token = token_param;
    if (!token && body) token = cJSON_GetStringValue(cJSON_GetObjectItem(body, "token"));

    if (token) {
        cJSON* payload = body ? cJSON_GetObjectItem(body, "payload") : NULL;
        int fired = RuleEngine_Dispatch_Webhook(token, payload);
        if (token_param) free(token_param);
        if (body) cJSON_Delete(body);
        cJSON* result = cJSON_CreateObject();
        cJSON_AddNumberToObject(result, "rules_fired", fired);
        ACAP_HTTP_Respond_JSON(resp, result);
        cJSON_Delete(result);
        return;
    }
    if (token_param) free(token_param);

    /* Manual fire mode: body.id */
    const char* id = body ? cJSON_GetStringValue(cJSON_GetObjectItem(body, "id")) : NULL;
    if (!id) {
        if (body) cJSON_Delete(body);
        ACAP_HTTP_Respond_Error(resp, 400, "Missing 'id' or 'token' in request");
        return;
    }

    cJSON* trigger_data = body ? cJSON_GetObjectItem(body, "trigger_data") : NULL;
    int ok = RuleEngine_Fire(id, trigger_data);
    if (body) cJSON_Delete(body);

    if (!ok) { ACAP_HTTP_Respond_Error(resp, 404, "Rule not found"); return; }
    ACAP_HTTP_Respond_Text(resp, "OK");
}

/*=====================================================
 * GET /local/acap_event_engine/aoa
 * Returns list of configured AOA scenarios (id, name, type)
 * by proxying getConfiguration to objectanalytics/control.cgi
 *=====================================================*/
static void HTTP_AOA(ACAP_HTTP_Response resp, const ACAP_HTTP_Request req) {
    (void)req;
    const char* body = "{\"apiVersion\":\"1.0\",\"method\":\"getConfiguration\"}";
    char* raw = ACAP_VAPIX_Post_Path("/local/objectanalytics/control.cgi", body);
    if (!raw) {
        ACAP_HTTP_Respond_Error(resp, 502, "AOA not available");
        return;
    }
    cJSON* root = cJSON_Parse(raw);
    free(raw);
    if (!root) {
        ACAP_HTTP_Respond_Error(resp, 502, "Invalid AOA response");
        return;
    }

    cJSON* arr = cJSON_CreateArray();
    cJSON* data = cJSON_GetObjectItem(root, "data");
    /* Scenarios are at data.scenarios in the AOA API */
    cJSON* scenarios = data ? cJSON_GetObjectItem(data, "scenarios") : NULL;
    if (scenarios && cJSON_IsArray(scenarios)) {
        cJSON* s;
        cJSON_ArrayForEach(s, scenarios) {
            cJSON* id_j   = cJSON_GetObjectItem(s, "id");
            cJSON* name_j = cJSON_GetObjectItem(s, "name");
            cJSON* type_j = cJSON_GetObjectItem(s, "type");
            if (!id_j) continue;
            cJSON* item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "id",   id_j->valuedouble);
            cJSON_AddStringToObject(item, "name", name_j ? name_j->valuestring : "");
            cJSON_AddStringToObject(item, "type", type_j ? type_j->valuestring : "");
            cJSON_AddItemToArray(arr, item);
        }
    }
    cJSON_Delete(root);
    ACAP_HTTP_Respond_JSON(resp, arr);
    cJSON_Delete(arr);
}

/*=====================================================
 * GET/POST/DELETE /local/acap_event_engine/variables
 *=====================================================*/
static void HTTP_Variables(ACAP_HTTP_Response resp, const ACAP_HTTP_Request req) {
    const char* method = get_method(req);

    if (strcmp(method, "GET") == 0) {
        cJSON* vars = Variables_List();
        ACAP_HTTP_Respond_JSON(resp, vars);
        cJSON_Delete(vars);

    } else if (strcmp(method, "POST") == 0) {
        cJSON* body = get_body_json(req);
        if (!body) { ACAP_HTTP_Respond_Error(resp, 400, "Invalid JSON"); return; }
        const char* name  = cJSON_GetStringValue(cJSON_GetObjectItem(body, "name"));
        const char* value = cJSON_GetStringValue(cJSON_GetObjectItem(body, "value"));
        cJSON* is_counter = cJSON_GetObjectItem(body, "is_counter");
        if (!name) { cJSON_Delete(body); ACAP_HTTP_Respond_Error(resp, 400, "Missing name"); return; }
        if (is_counter && cJSON_IsTrue(is_counter)) {
            double v = value ? atof(value) : 0.0;
            Counter_Set(name, v);
        } else if (value) {
            Variables_Set(name, value);
        }
        cJSON_Delete(body);
        ACAP_HTTP_Respond_Text(resp, "OK");

    } else if (strcmp(method, "DELETE") == 0) {
        char* name = ACAP_HTTP_Request_Param(req, "name");
        if (!name || !*name) {
            if (name) free(name);
            ACAP_HTTP_Respond_Error(resp, 400, "Missing name parameter");
            return;
        }
        Variables_Delete(name);
        free(name);
        ACAP_HTTP_Respond_Text(resp, "OK");

    } else {
        ACAP_HTTP_Respond_Error(resp, 405, "Method not allowed");
    }
}

/*=====================================================
 * Signal handler
 *=====================================================*/
static gboolean Signal_Handler(gpointer user_data) {
    (void)user_data;
    LOG("Received SIGTERM, shutting down");
    if (main_loop && g_main_loop_is_running(main_loop))
        g_main_loop_quit(main_loop);
    return G_SOURCE_REMOVE;
}

/*=====================================================
 * main
 *=====================================================*/
int main(void) {
    openlog(APP_PACKAGE, LOG_PID | LOG_CONS, LOG_USER);
    srand((unsigned)time(NULL));

    LOG("------ Starting %s ------", APP_PACKAGE);

    cJSON* settings = ACAP_Init(APP_PACKAGE, Settings_Updated);
    if (!settings) {
        LOG_WARN("ACAP_Init failed");
        return EXIT_FAILURE;
    }

    /* Apply proxy config from engine settings */
    cJSON* eng_cfg = cJSON_GetObjectItem(settings, "engine");
    if (eng_cfg) apply_proxy_config(eng_cfg);

    /* Register event callback */
    ACAP_EVENTS_SetCallback(Event_Callback);

    /* Initialize subsystems */
    Variables_Init();
    EventLog_Init();

    /* MQTT — must init before RuleEngine so subscriptions can be set up */
    {
        MQTT_Config mc;
        memset(&mc, 0, sizeof(mc));
        cJSON* mqtt_cfg = cJSON_GetObjectItem(settings, "mqtt");
        if (mqtt_cfg) {
            const char* h = cJSON_GetStringValue(cJSON_GetObjectItem(mqtt_cfg, "host"));
            snprintf(mc.host,      sizeof(mc.host),      "%s", h ? h : "");
            const char* c = cJSON_GetStringValue(cJSON_GetObjectItem(mqtt_cfg, "client_id"));
            snprintf(mc.client_id, sizeof(mc.client_id), "%s", c ? c : "acap_event_engine");
            const char* u = cJSON_GetStringValue(cJSON_GetObjectItem(mqtt_cfg, "username"));
            snprintf(mc.username,  sizeof(mc.username),  "%s", u ? u : "");
            /* Password: migrate from settings to separate file if present */
            const char* p = cJSON_GetStringValue(cJSON_GetObjectItem(mqtt_cfg, "password"));
            if (p && p[0]) {
                save_mqtt_password(p);
                snprintf(mc.password, sizeof(mc.password), "%s", p);
                cJSON* pw_item = cJSON_GetObjectItem(mqtt_cfg, "password");
                if (pw_item) cJSON_SetValuestring(pw_item, "");
            } else {
                load_mqtt_password(mc.password, sizeof(mc.password));
            }
            cJSON* port = cJSON_GetObjectItem(mqtt_cfg, "port");
            mc.port = port ? (int)port->valuedouble : 1883;
            cJSON* ka   = cJSON_GetObjectItem(mqtt_cfg, "keepalive");
            mc.keepalive = ka ? (int)ka->valuedouble : 60;
            mc.enabled = cJSON_IsTrue(cJSON_GetObjectItem(mqtt_cfg, "enabled")) ? 1 : 0;
        } else {
            mc.port = 1883; mc.keepalive = 60;
            snprintf(mc.client_id, sizeof(mc.client_id), "acap_event_engine");
        }
        MQTT_Init(&mc, mqtt_message_cb, NULL);
    }

    /* SMTP — load saved password into in-memory config */
    load_smtp_password();

    RuleEngine_Init();

    /* Set status */
    ACAP_STATUS_SetString("app", "status", "Running");
    ACAP_STATUS_SetNumber("app", "rules",  RuleEngine_Count());

    /* Register HTTP endpoints */
    /* NOTE: ACAP_Init() internally registers "app", "settings", and "status" first.
     *       Any attempt to re-register those names is silently dropped.
     *       Our status/info endpoint is therefore registered as "engine". */
    ACAP_HTTP_Node("engine",    HTTP_Status);
    ACAP_HTTP_Node("rules",     HTTP_Rules);
    ACAP_HTTP_Node("triggers",  HTTP_Triggers);
    ACAP_HTTP_Node("actions",   HTTP_Actions);
    ACAP_HTTP_Node("events",    HTTP_Events);
    ACAP_HTTP_Node("fire",      HTTP_Fire);
    ACAP_HTTP_Node("variables", HTTP_Variables);
    ACAP_HTTP_Node("aoa",       HTTP_AOA);

    /* 1-second engine tick */
    g_timeout_add_seconds(1, Engine_Tick, NULL);

    /* Fire EngineReady event */
    ACAP_EVENTS_Fire_State("EngineReady", 1);

    /* Main loop */
    main_loop = g_main_loop_new(NULL, FALSE);
    GSource* sig = g_unix_signal_source_new(SIGTERM);
    if (sig) {
        g_source_set_callback(sig, Signal_Handler, NULL, NULL);
        g_source_attach(sig, NULL);
        g_source_unref(sig);
    }

    LOG("Event Engine running — %d rules loaded", RuleEngine_Count());
    g_main_loop_run(main_loop);

    LOG("Shutting down %s", APP_PACKAGE);
    ACAP_EVENTS_Fire_State("EngineReady", 0);
    RuleEngine_Cleanup();
    MQTT_Cleanup();
    EventLog_Cleanup();
    Variables_Cleanup();
    ACAP_Cleanup();
    g_main_loop_unref(main_loop);
    closelog();
    return EXIT_SUCCESS;
}
