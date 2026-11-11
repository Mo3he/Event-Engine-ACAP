#ifndef _ENGINE_EVENT_LOG_H_
#define _ENGINE_EVENT_LOG_H_

#include <time.h>
#include "../cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Event Log: in-memory ring buffer of rule execution history (oldest entries
 * are overwritten). Thread-safe: used from both GMainLoop and FastCGI threads.
 */

#define EVENT_LOG_SIZE 500
#define SYSTEM_LOG_SIZE 200

/* Current time as epoch seconds with millisecond resolution. */
double EventLog_Now(void);

/* Add a system entry (source e.g. "mqtt", level "info"/"warning"/"error").
 * Repeats of a recent message are counted on the existing entry, and bursts
 * are capped per minute, so a failing loop cannot flood the log. */
void   EventLog_System(const char* source, const char* level, const char* fmt, ...)
       __attribute__((format(__printf__, 3, 4)));
/* syslog() plus, for warnings and errors, a system entry. Used by LOG_WARN. */
void   EventLog_Syslog(int priority, const char* source, const char* fmt, ...)
       __attribute__((format(__printf__, 3, 4)));
/* Most recent system entries first — caller must cJSON_Delete */
cJSON* EventLog_Get_System(int limit);
void   EventLog_System_Clear(void);

int    EventLog_Init(void);
void   EventLog_Cleanup(void);

void   EventLog_Append(const char* rule_id, const char* rule_name,
                       int fired, const char* block_reason,
                       cJSON* trigger_data,
                       int actions_run, int actions_failed);

void   EventLog_Set_Action_Error(const char* rule_id, const char* error_msg);

/* Returns new cJSON array — caller must cJSON_Delete */
cJSON* EventLog_Get_Recent(int limit);
cJSON* EventLog_Get_For_Rule(const char* rule_id, int limit);

/* Today's fire count */
int    EventLog_Count_Today(void);

void   EventLog_Clear(void);

void   EventLog_Load(void);
void   EventLog_Persist(void);

#ifdef __cplusplus
}
#endif
#endif /* _ENGINE_EVENT_LOG_H_ */
