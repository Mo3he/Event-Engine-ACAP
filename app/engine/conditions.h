#ifndef _ENGINE_CONDITIONS_H_
#define _ENGINE_CONDITIONS_H_

#include "../cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Condition evaluators: time_window, day_night, counter, io_state,
 * variable_compare, vapix_event_state, http_check, aoa_occupancy.
 */

/* logic: 0 = AND, 1 = OR. Returns 1 (pass) or 0 (fail). */
int Conditions_Evaluate(cJSON* conditions_array, int logic, cJSON* trigger_data);
/* Treats http_check, vapix_event_state and aoa_occupancy as pass (not evaluated) */
int Conditions_Evaluate_Lightweight(cJSON* conditions_array, int logic);
void Conditions_Set_Proxy(const char* proxy); /* set SOCKS5 proxy for http_check (e.g. "socks5h://localhost:1055") */

#ifdef __cplusplus
}
#endif
#endif /* _ENGINE_CONDITIONS_H_ */
