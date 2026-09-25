#ifndef _ENGINE_SCHEDULER_H_
#define _ENGINE_SCHEDULER_H_

#include "../cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Scheduler: evaluates time-based triggers every 1 s from the GMainLoop timer.
 * Schedule types:
 *   "cron"         5-field cron expression ("* * * * *")
 *   "interval"     fire every N seconds
 *   "daily_time"   fire once per day at HH:MM on selected days
 *   "astronomical" sunrise/sunset/dawn/dusk/solar_noon plus an offset
 *
 * The callback receives rule_id and trigger_index.
 */

typedef void (*Scheduler_Callback)(const char* rule_id, int trigger_index);

int  Scheduler_Init(Scheduler_Callback cb);
void Scheduler_Cleanup(void);

/* Called when a rule is added or updated */
int  Scheduler_Register(const char* rule_id, int trigger_index, cJSON* schedule_config);

/* Remove all schedule entries for a rule */
void Scheduler_Unregister_Rule(const char* rule_id);

/* Called every 1 second from main loop */
void Scheduler_Tick(void);

/* Returns 1 if the current time is between sunrise and sunset for the given
 * latitude/longitude, 0 if it is nighttime, or -1 on error (polar day/night). */
int Scheduler_Is_Daytime(double lat, double lon);

#ifdef __cplusplus
}
#endif
#endif /* _ENGINE_SCHEDULER_H_ */
