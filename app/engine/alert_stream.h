#ifndef _ENGINE_ALERT_STREAM_H_
#define _ENGINE_ALERT_STREAM_H_

#include "../cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Alert Stream: real-time HTTP multipart/mixed event stream at
 * /local/acap_event_engine/alertStream. Apache (Digest Auth) reverse-proxies
 * it to a TCP server on 127.0.0.1:8888; each client gets its own thread and
 * each event is one JSON part. A slow client only gets the newest pending event.
 */

/* Start the loopback HTTP server and its accept thread. */
int  AlertStream_Init(void);

/* Shut down all stream connections and stop the accept thread. */
void AlertStream_Cleanup(void);

/* Broadcast an event to all connected stream clients. Non-blocking; any thread. */
void AlertStream_Broadcast(const char* rule_id, const char* rule_name,
                           cJSON* trigger_data);

/* Turn the stream on or off (engine.alert_stream_enabled). While off, new
 * connections get 503 and open streams are closed. */
void AlertStream_Set_Enabled(int enabled);
int  AlertStream_Is_Enabled(void);

/* Returns the number of currently connected stream clients. */
int  AlertStream_Client_Count(void);

#ifdef __cplusplus
}
#endif
#endif /* _ENGINE_ALERT_STREAM_H_ */
