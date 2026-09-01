#ifndef _ENGINE_SPARKPLUG_H_
#define _ENGINE_SPARKPLUG_H_

#include "../cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Sparkplug B edge node.
 *
 * Turns the device into a Sparkplug edge node on top of the shared MQTT broker
 * connection configured in Settings, so no external IIoT gateway is needed to
 * expose device data to a Sparkplug host (Ignition, Honeywell EBI, ...).
 *
 * Owns the whole session state machine: bdSeq, the 0-255 sequence number,
 * NBIRTH before any NDATA, NDEATH registered as the MQTT will, and rebirth on
 * request. Sequence numbers are allocated here and nowhere else.
 *
 * Metrics are declared centrally (Settings -> Sparkplug) so the birth
 * certificate is stable. A metric is published either by a rule action or,
 * when bound to a device event, automatically whenever its value changes.
 */

#define SPB_SPEC_2_2 0
#define SPB_SPEC_3_0 1

int  Sparkplug_Init(void);

/* Publish NDEATH and stop. Must run before MQTT_Cleanup(): a clean MQTT
 * DISCONNECT suppresses the will, so a graceful shutdown has to announce its
 * own death or the host would keep showing the node as online. */
void Sparkplug_Cleanup(void);

/* Apply the "sparkplug" settings block. Identity or metric-list changes force a
 * new session (bdSeq bump + rebirth). Safe to call from any thread. */
int  Sparkplug_Configure(cJSON* cfg);

/* Publish declared metrics as NDATA (or DDATA when a device id is configured).
 * metrics: array of { "name": "...", "value": "..." }. Unknown metric names are
 * rejected because they are absent from the birth certificate.
 * Returns 1 if published or buffered for replay, 0 on error. */
int  Sparkplug_Publish(cJSON* metrics);

/* Inbound hooks. Both are no-ops when Sparkplug is disabled. */
void Sparkplug_On_MQTT_Message(const char* topic, const char* payload, int payload_len);
void Sparkplug_On_Device_Event(cJSON* event);

/* Invoked when a host writes a metric via NCMD/DCMD. */
typedef void (*Sparkplug_Command_Fn)(const char* metric, const char* value);
void Sparkplug_Set_Command_Callback(Sparkplug_Command_Fn fn);

int    Sparkplug_Is_Enabled(void);

/* Session state for the /status endpoint — caller cJSON_Delete */
cJSON* Sparkplug_Status(void);

/* Declared metrics with their current values — caller cJSON_Delete */
cJSON* Sparkplug_Metrics(void);

#ifdef __cplusplus
}
#endif
#endif /* _ENGINE_SPARKPLUG_H_ */
