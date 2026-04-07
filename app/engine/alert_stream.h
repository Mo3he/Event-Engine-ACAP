#ifndef _ENGINE_ALERT_STREAM_H_
#define _ENGINE_ALERT_STREAM_H_

#include "../cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Alert Stream — real-time HTTP multipart event stream.
 *
 * Provides a long-lived HTTP endpoint at /local/acap_event_engine/alertStream.
 * Clients perform a GET request (authenticated via the camera's Digest Auth
 * reverse proxy) and receive a multipart/mixed response where each part is
 * a JSON object describing a rule-fire event.
 *
 * Implementation:
 *   A dedicated thread runs its own FCGX_Accept_r loop on the shared FCGI
 *   socket. When a client connects, a per-client thread is spawned that
 *   writes the multipart header and then blocks on a condition variable.
 *   AlertStream_Broadcast() signals all waiting client threads to write
 *   the latest event data as a new multipart chunk.
 *
 * Thread safety:
 *   AlertStream_Broadcast() may be called from any thread.
 */

/* Initialize the alert stream subsystem and start the accept thread.
 * Must be called after ACAP_Init() (needs the FCGI socket). */
int  AlertStream_Init(void);

/* Shut down all stream connections and stop the accept thread. */
void AlertStream_Cleanup(void);

/* Broadcast an event to all connected stream clients.
 * May be called from any thread. Non-blocking (queues data and signals). */
void AlertStream_Broadcast(const char* rule_id, const char* rule_name,
                           cJSON* trigger_data);

/* Returns the number of currently connected stream clients. */
int  AlertStream_Client_Count(void);

#ifdef __cplusplus
}
#endif
#endif /* _ENGINE_ALERT_STREAM_H_ */
