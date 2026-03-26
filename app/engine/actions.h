#ifndef _ENGINE_ACTIONS_H_
#define _ENGINE_ACTIONS_H_

#include "../cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Action executor.
 *
 * Actions_Execute() runs through the actions array sequentially.
 * The "delay" action is asynchronous — it suspends execution and
 * resumes via a GLib timeout (g_timeout_add_seconds).
 *
 * Template variables ({{camera.name}}, {{timestamp}}, etc.) are
 * expanded in string fields before use. Use Actions_Expand_Template()
 * to expand a string — caller must free() the result.
 *
 * Supported action types:
 *   http_request      — HTTP GET/POST/PUT to external URL
 *   recording         — start/stop local recording
 *   overlay_text      — set dynamic overlay text
 *   ptz_preset        — move camera to PTZ preset
 *   io_output         — set digital output port state
 *   audio_clip        — play an audio clip
 *   send_syslog       — write syslog message
 *   fire_vapix_event  — emit a declared VAPIX event
 *   delay             — wait N seconds (async)
 *   set_variable      — store a variable value
 *   increment_counter — increment/decrement a counter
 *   run_rule          — trigger another rule by ID
 *   guard_tour        — start/stop a PTZ guard tour
 *   set_device_param  — update a camera parameter via param.cgi
 *   snapshot_upload   — capture JPEG and POST/PUT to a URL
 *   ir_cut_filter     — force day/night mode or restore auto switching
 *   privacy_mask      — enable/disable a named privacy mask
 *   wiper             — trigger the windshield wiper
 *   light_control     — control an Axis illuminator (white/IR LED)
 *   acap_control      — start/stop/restart another ACAP application
 */

void  Actions_Init(void);
void  Actions_Execute(const char* rule_id, cJSON* actions_array, cJSON* trigger_data);
char* Actions_Expand_Template(const char* tmpl, cJSON* trigger_data); /* caller must free() */
void  Actions_Stop_Active_Siren(const char* rule_id); /* stop while_active siren if running */
void  Actions_ForEach_Active_Siren(int (*cb)(const char* rule_id, void* userdata), void* userdata);
void  Actions_Set_Proxy(const char* proxy); /* set SOCKS5 proxy for outbound HTTP (e.g. "socks5h://localhost:1055") */
void  Actions_Digest_Tick(void);           /* call from 1s timer to flush digest buffers */
int         Actions_Test(const char* type, cJSON* config); /* test a single action without a rule */
const char* Actions_Get_Last_Error(void);              /* error from last Actions_Test(); NULL if none */

#ifdef __cplusplus
}
#endif
#endif /* _ENGINE_ACTIONS_H_ */
