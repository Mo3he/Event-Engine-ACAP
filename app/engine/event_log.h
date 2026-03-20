#ifndef _ENGINE_EVENT_LOG_H_
#define _ENGINE_EVENT_LOG_H_

#include <time.h>
#include "../cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Event Log — in-memory ring buffer of rule execution history.
 *
 * Stores up to EVENT_LOG_SIZE entries. When full, oldest entries are
 * overwritten. Thread-safe: accessed from both GMainLoop and FastCGI threads.
 */

#define EVENT_LOG_SIZE 500

int    EventLog_Init(void);
void   EventLog_Cleanup(void);

void   EventLog_Append(const char* rule_id, const char* rule_name,
                       int fired, const char* block_reason,
                       cJSON* trigger_data);

/* Returns new cJSON array — caller must cJSON_Delete */
cJSON* EventLog_Get_Recent(int limit);
cJSON* EventLog_Get_For_Rule(const char* rule_id, int limit);

/* Today's fire count */
int    EventLog_Count_Today(void);

#ifdef __cplusplus
}
#endif
#endif /* _ENGINE_EVENT_LOG_H_ */
