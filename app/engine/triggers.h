#ifndef _ENGINE_TRIGGERS_H_
#define _ENGINE_TRIGGERS_H_

#include "../cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Trigger subscription manager: routes events, webhooks, schedules, MQTT and
 * Sparkplug commands, Modbus polls and rule chaining to the rule engine.
 *
 * ACAP_EVENTS_Subscribe/Unsubscribe MUST run on the GMainLoop thread, not the
 * FastCGI thread; rule_engine.c defers subscription work with g_idle_add().
 *
 * Trigger types: vapix_event, io_input, http_webhook, schedule,
 * counter_threshold, rule_fired, mqtt_message, sparkplug_command,
 * aoa_scenario, modbus_read, manual.
 */

typedef void (*Trigger_Fire_Fn)(const char* rule_id, int trigger_index, cJSON* trigger_data);

int  Triggers_Init(Trigger_Fire_Fn fire_fn);
void Triggers_Cleanup(void);

/* Subscribe/unsubscribe for a rule — call from GMainLoop thread only */
int  Triggers_Subscribe_Rule(const char* rule_id, cJSON* triggers_array);
void Triggers_Unsubscribe_Rule(const char* rule_id);

/* Called from ACAP single event callback (main loop thread) */
void Triggers_On_VAPIX_Event(cJSON* event);

/* Called from /fire HTTP endpoint — matches token against registered webhooks */
int  Triggers_On_Webhook(const char* token, cJSON* payload);

/* Called when another rule fires — matches rule_fired triggers */
void Triggers_On_Rule_Fired(const char* fired_rule_id);

/* Called by MQTT client when a message arrives (dispatched on GMainLoop) */
void Triggers_On_MQTT_Message(const char* topic, const char* payload, int payload_len);

/* Dispatch a metric write received from a Sparkplug host (NCMD/DCMD) */
void Triggers_On_Sparkplug_Command(const char* metric, const char* value);

/* Called every 1s from main loop: scheduler, I/O hold timers, counter thresholds, Modbus polls */
void Triggers_Tick(void);

/* Register a passive subscription for a vapix_query action (caches data, never fires rules).
 * action_cfg carries topic0..topic3 like a VAPIX trigger. GMainLoop thread only. */
void Triggers_Subscribe_Passive(const char* rule_id, int action_idx, cJSON* action_cfg);

/* Return the last-seen event data cached by a passive subscription matching topic_cfg.
 * Returns a borrowed reference (do not free); NULL if not yet received. */
cJSON* Triggers_Get_Cached(cJSON* topic_cfg);

/* Returns 1 if any stateful trigger for rule_id is currently active
 * (e.g. a VAPIX threshold that has been crossed and not yet reset). */
int Triggers_Any_Active(const char* rule_id);

/* Returns 1 if ALL triggers for rule_id are currently in their active state.
 * fired_trigger_index is the index of the trigger that just fired (counts as
 * active regardless of type); momentary triggers at other indices return 0.
 * Pass -1 for fired_trigger_index when no specific trigger initiated the call. */
int Triggers_All_Currently_Active(const char* rule_id, int fired_trigger_index);

/* Returns catalog of subscribable trigger types for /triggers API */
cJSON* Triggers_Catalog(void);

#ifdef __cplusplus
}
#endif
#endif /* _ENGINE_TRIGGERS_H_ */
