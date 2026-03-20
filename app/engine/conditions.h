#ifndef _ENGINE_CONDITIONS_H_
#define _ENGINE_CONDITIONS_H_

#include "../cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Condition evaluators.
 *
 * Conditions_Evaluate() runs through a conditions array applying AND or OR
 * logic and returns 1 (pass) or 0 (fail).
 *
 * Individual condition types:
 *   time_window      — current time within start/end on specified days
 *   event_state      — VAPIX event current state value
 *   counter          — compare a counter to a threshold
 *   http_check       — HTTP GET/POST response matches expected
 *   io_state         — current IO port state
 *   variable_compare — compare a stored variable
 *
 * trigger_data is passed through for potential template use in http_check.
 */

/* logic: 0 = AND, 1 = OR */
int Conditions_Evaluate(cJSON* conditions_array, int logic, cJSON* trigger_data);

#ifdef __cplusplus
}
#endif
#endif /* _ENGINE_CONDITIONS_H_ */
