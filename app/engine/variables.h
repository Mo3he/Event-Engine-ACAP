#ifndef _ENGINE_VARIABLES_H_
#define _ENGINE_VARIABLES_H_

#include "../cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Variables & Counters store, persisted in localdata/variables.json by
 * Variables_Flush(). Counters are numeric variables. Thread-safe.
 */

int           Variables_Init(void);
void          Variables_Cleanup(void);

/* Generic variables */
int           Variables_Set(const char* name, const char* value);
char*         Variables_Get(const char* name);          /* caller must free() the returned string */
int           Variables_Delete(const char* name);
int           Variables_Compare(const char* name, const char* op, const char* value);
cJSON*        Variables_List(void);                     /* caller must cJSON_Delete */
/* [{name, value, is_counter, persistent, initial}] — caller must cJSON_Delete */
cJSON*        Variables_List_Detail(void);
/* persistent: 1 keep across restarts, 0 volatile (reset to initial at startup),
 * -1 unchanged. initial: value a volatile variable resets to (NULL unchanged).
 * Creates the variable at its initial value if it does not exist. */
int           Variables_Set_Options(const char* name, int persistent, const char* initial);
/* One-line description of what was loaded (and reset) at startup. */
void          Variables_Startup_Summary(char* buf, size_t size);

/* Numeric counters */
int           Counter_Set(const char* name, double value);
double        Counter_Get(const char* name);
int           Counter_Increment(const char* name, double delta);
int           Counter_Reset(const char* name);
int           Counter_Compare(const char* name, const char* op, double threshold);

/* Persist dirty data (called every 1 s from RuleEngine_Tick) */
void          Variables_Flush(void);

#ifdef __cplusplus
}
#endif
#endif /* _ENGINE_VARIABLES_H_ */
