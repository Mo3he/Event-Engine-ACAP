#ifndef _ENGINE_TRIGGERS_H_
#define _ENGINE_TRIGGERS_H_

#include "../cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Trigger subscription manager.
 *
 * Manages all trigger subscriptions for every rule.
 * Routes incoming events/webhooks/schedules to the rule engine.
 *
 * IMPORTANT: ACAP_EVENTS_Subscribe and ACAP_EVENTS_Unsubscribe MUST be
 * called from the GMainLoop thread, not the FastCGI thread. Use
 * Triggers_Subscribe_Rule_Idle() from HTTP handlers to safely schedule
 * subscription work on the main loop.
 *
 * Trigger types handled:
 *   vapix_event       — subscribes to a VAPIX event topic
 *   http_webhook      — matched by token in the /fire HTTP endpoint
 *   schedule          — delegated to scheduler.c
 *   io_input          — VAPIX IO state change event
 *   counter_threshold — polled every tick against counter value
 *   rule_fired        — fired when another rule executes
 *   mqtt_message      — MQTT topic message received from broker
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

/* Called every 1s from main loop — checks counter threshold triggers */
void Triggers_Tick(void);

/* Register a passive subscription for a vapix_query action (caches data; never fires rules).
 * action_cfg must contain topic0/topic1/topic2/topic3 keys identical to a VAPIX trigger.
 * Must be called from the GMainLoop thread. */
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
