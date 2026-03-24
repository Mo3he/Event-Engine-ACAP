#ifndef _ENGINE_MQTT_CLIENT_H_
#define _ENGINE_MQTT_CLIENT_H_

#include "../cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Minimal MQTT 3.1.1 client over raw POSIX TCP sockets.
 * No external library required.
 *
 * Features:
 *   - QoS 0 and QoS 1 publish (per-message, optional)
 *   - QoS 1 subscribe (broker delivers with acknowledgment)
 *   - PUBACK sent to broker for incoming QoS 1 messages
 *   - UNSUBSCRIBE packet sent on MQTT_Unsubscribe()
 *   - Automatic reconnection with exponential backoff
 *   - Keepalive PINGREQ/PINGRESP
 *   - Multiple topic subscriptions with wildcard support (+ and #)
 *   - Incoming message dispatch via callback (on GMainLoop thread)
 *   - Thread-safe publish (mutex protected)
 *
 * Limitations:
 *   - No QoS 1 retransmit (message sent once; PUBACK acknowledged but not enforced)
 *   - No TLS (plain TCP, port 1883)
 *   - Single broker
 */

typedef void (*MQTT_Message_Callback)(const char* topic, const char* payload,
                                      int payload_len, void* user_data);

typedef struct {
    char host[256];
    int  port;
    char client_id[128];
    char username[128];
    char password[128];
    int  keepalive;       /* seconds, 0 = disabled */
    int  enabled;
} MQTT_Config;

int  MQTT_Init(MQTT_Config* config, MQTT_Message_Callback cb, void* user_data);
void MQTT_Cleanup(void);

/* Update config (triggers reconnect if host/port changed) */
int  MQTT_Reconfigure(MQTT_Config* config);

/* Publish — thread-safe, returns 1 on success.
 * qos: 0 = fire and forget, 1 = at-least-once (PUBACK expected from broker) */
int  MQTT_Publish(const char* topic, const char* payload, int retain, int qos);

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
