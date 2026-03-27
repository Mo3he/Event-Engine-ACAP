#ifndef _ENGINE_VARIABLES_H_
#define _ENGINE_VARIABLES_H_

#include "../cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Variables & Counters store.
 *
 * Variables are named string values that persist across engine restarts.
 * Counters are numeric variables optimized for increment/decrement operations.
 * Both are stored in localdata/variables.json and flushed periodically.
 *
 * Thread-safe: all functions acquire an internal mutex.
 */

int           Variables_Init(void);
void          Variables_Cleanup(void);

/* Generic variables */
int           Variables_Set(const char* name, const char* value);
char*         Variables_Get(const char* name);          /* caller must free() the returned string */
int           Variables_Delete(const char* name);
int           Variables_Compare(const char* name, const char* op, const char* value);
cJSON*        Variables_List(void);                     /* caller must cJSON_Delete */

/* Numeric counters */
int           Counter_Set(const char* name, double value);
double        Counter_Get(const char* name);
int           Counter_Increment(const char* name, double delta);
int           Counter_Reset(const char* name);
int           Counter_Compare(const char* name, const char* op, double threshold);

/* Persist dirty data (called from housekeeping tick) */
void          Variables_Flush(void);

#ifdef __cplusplus
}
#endif
#endif /* _ENGINE_VARIABLES_H_ */
