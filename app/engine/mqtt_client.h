#ifndef _ENGINE_MQTT_CLIENT_H_
#define _ENGINE_MQTT_CLIENT_H_

#include "../cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Minimal MQTT 3.1.1 client over POSIX TCP sockets.
 *
 * Features:
 *   - QoS 0 and QoS 1 publish (per-message, optional)
 *   - QoS 1 subscribe (broker delivers with acknowledgment)
 *   - QoS 1 retransmission: unacknowledged publishes are resent with DUP set
 *     until the broker acknowledges them or the retry budget runs out
 *   - PUBACK sent to broker for incoming QoS 1 messages
 *   - UNSUBSCRIBE packet sent on MQTT_Unsubscribe()
 *   - Automatic reconnection with exponential backoff
 *   - Keepalive PINGREQ/PINGRESP
 *   - Multiple topic subscriptions with wildcard support (+ and #)
 *   - Optional TLS transport with hostname verification
 *   - Incoming message dispatch via callback (on GMainLoop thread)
 *   - Thread-safe publish (mutex protected)
 *   - Binary-safe publish and Last Will & Testament (required by Sparkplug B)
 *
 * Limitations:
 *   - Sessions are always clean, so unacknowledged QoS 1 messages are dropped
 *     on disconnect rather than replayed on the next connection
 *   - on_failure fires on socket/connection failure only, not on QoS 1 delivery timeout
 *   - Single broker
 */

typedef void (*MQTT_Message_Callback)(const char* topic, const char* payload,
                                      int payload_len, void* user_data);

/* Connection lifecycle states reported to MQTT_State_Callback. */
#define MQTT_STATE_PRECONNECT   0  /* CONNECT is about to be sent — set the will now */
#define MQTT_STATE_CONNECTED    1  /* CONNACK accepted and subscriptions restored */
#define MQTT_STATE_DISCONNECTED 2  /* connection lost */

/* Invoked on the MQTT worker thread, not the GMainLoop thread. Publishing from
 * MQTT_STATE_CONNECTED is safe and is the only way to guarantee a message goes
 * out before anything else on a fresh session. */
typedef void (*MQTT_State_Callback)(int state, void* user_data);

typedef struct {
    char host[256];
    int  port;
    char client_id[128];
    char username[128];
    char password[128];
    int  keepalive;       /* seconds, 0 = disabled */
    int  use_tls;         /* 1 = TLS transport, 0 = plain TCP */
    int  enabled;
} MQTT_Config;

int  MQTT_Init(MQTT_Config* config, MQTT_Message_Callback cb, void* user_data);
void MQTT_Cleanup(void);

/* Update config (triggers reconnect if host/port changed) */
int  MQTT_Reconfigure(MQTT_Config* config);

/* Publish — thread-safe, returns 1 on success.
 * qos: 0 = fire and forget, 1 = at-least-once (PUBACK expected from broker) */
int  MQTT_Publish(const char* topic, const char* payload, int retain, int qos);

/* Publish an arbitrary byte payload (may contain NUL). Thread-safe. */
int  MQTT_Publish_Binary(const char* topic, const void* payload, int payload_len,
                         int retain, int qos);

/* Register the Last Will & Testament used by the NEXT CONNECT. Set it from an
 * MQTT_STATE_PRECONNECT callback so the will matches the session it belongs to.
 * payload may be binary. Pass topic = NULL to clear. Max payload 1024 bytes. */
int  MQTT_Set_Will(const char* topic, const void* payload, int payload_len,
                   int qos, int retain);

/* Connection lifecycle notifications. Pass cb = NULL to unregister. */
void MQTT_Set_State_Callback(MQTT_State_Callback cb, void* user_data);

/* Drop the current connection so the worker reconnects (and re-runs the will /
 * birth sequence). No-op when not connected. */
void MQTT_Force_Reconnect(void);

/* Subscribe to a topic filter — call before or after connect, resubscribed on reconnect */
int  MQTT_Subscribe(const char* topic_filter);
void MQTT_Unsubscribe(const char* topic_filter);

/* Set SOCKS5 proxy for the broker connection (e.g. "localhost:1055").
 * Pass NULL or "" to disable. Triggers reconnect if currently connected. */
void MQTT_Set_Proxy(const char* proxy_host, int proxy_port);

/* Current connection state */
int  MQTT_Is_Connected(void);

/* Returns status JSON for /status endpoint — caller cJSON_Delete */
cJSON* MQTT_Status(void);

#ifdef __cplusplus
}
#endif
#endif /* _ENGINE_MQTT_CLIENT_H_ */
