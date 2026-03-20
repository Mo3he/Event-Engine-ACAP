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
 */

void  Actions_Init(void);
void  Actions_Execute(const char* rule_id, cJSON* actions_array, cJSON* trigger_data);
char* Actions_Expand_Template(const char* tmpl, cJSON* trigger_data); /* caller must free() */

#ifdef __cplusplus
}
#endif
#endif /* _ENGINE_ACTIONS_H_ */
