#ifndef _ENGINE_RULE_ENGINE_H_
#define _ENGINE_RULE_ENGINE_H_

#include "../cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Rule Engine — core coordinator.
 *
 * Owns the rule store, wires triggers → conditions → actions.
 *
 * Thread safety:
 *   CRUD operations (Add/Update/Delete) may be called from the FastCGI
 *   thread. Event dispatch and Tick are called from the GMainLoop thread.
 *   An internal mutex protects the rule store.
 *
 *   VAPIX event subscriptions are scheduled back onto the main loop via
 *   g_idle_add() from the HTTP handlers — do NOT call
 *   Triggers_Subscribe_Rule directly from a FastCGI callback.
 */

int    RuleEngine_Init(void);
void   RuleEngine_Cleanup(void);

/* CRUD — callable from any thread */
cJSON* RuleEngine_List(void);                                   /* caller cJSON_Delete */
cJSON* RuleEngine_Get(const char* id);                          /* caller cJSON_Delete */
int    RuleEngine_Add(cJSON* rule_json, char* id_out_37);       /* id_out_37 receives the assigned UUID */
int    RuleEngine_Update(const char* id, cJSON* rule_json);
int    RuleEngine_Delete(const char* id);
int    RuleEngine_SetEnabled(const char* id, int enabled);

/* Dispatch — call from GMainLoop thread */
void   RuleEngine_Dispatch_Event(cJSON* event);                 /* VAPIX event */
int    RuleEngine_Dispatch_Webhook(const char* token, cJSON* payload); /* returns rules fired */
void   RuleEngine_Dispatch_RuleFired(const char* rule_id);

/* Tick — call every 1s from GMainLoop */
void   RuleEngine_Tick(void);

/* Force-execute a rule (test/manual fire — bypasses cooldown) */
int    RuleEngine_Fire(const char* id, cJSON* trigger_data);

/* Stats */
int    RuleEngine_Count(void);
int    RuleEngine_Count_Enabled(void);

#ifdef __cplusplus
}
#endif
#endif /* _ENGINE_RULE_ENGINE_H_ */
