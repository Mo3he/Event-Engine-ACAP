#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <syslog.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>

#include "alert_stream.h"
#include "../ACAP.h"

#define LOG(fmt, args...)      syslog(LOG_INFO,    "alert_stream: " fmt, ## args)
#define LOG_WARN(fmt, args...) syslog(LOG_WARNING, "alert_stream: " fmt, ## args)

#define MAX_STREAM_CLIENTS 8
#define BOUNDARY "----AxisEventBoundary"
#define ALERT_STREAM_PORT 8888

/*-----------------------------------------------------
 * Per-client state
 *-----------------------------------------------------*/
typedef struct {
    int              active;
    int              fd;             /* accepted TCP socket */
    pthread_t        thread;
    int              has_pending;    /* new event waiting to be written */
    char*            pending_json;   /* JSON string to write */
} StreamClient;

static StreamClient    clients[MAX_STREAM_CLIENTS];
static pthread_mutex_t clients_lock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  broadcast_cond = PTHREAD_COND_INITIALIZER;

static int             server_sock   = -1;
static pthread_t       accept_thread;
static int             accept_running = 0;

/*-----------------------------------------------------
 * Build a multipart chunk for one event
 *-----------------------------------------------------*/
static char* build_event_json(const char* rule_id, const char* rule_name,
                              cJSON* trigger_data) {
    cJSON* obj = cJSON_CreateObject();

    /* Device info — best effort */
    const char* ip = ACAP_DEVICE_Prop("IPv4");
    if (ip) cJSON_AddStringToObject(obj, "ipAddress", ip);

    const char* serial = ACAP_DEVICE_Prop("serial");
    if (serial) cJSON_AddStringToObject(obj, "macAddress", serial);

    /* ISO 8601 timestamp */
    time_t now = time(NULL);
    struct tm tm_buf;
    gmtime_r(&now, &tm_buf);
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%S+00:00", &tm_buf);
    cJSON_AddStringToObject(obj, "dateTime", timebuf);

    cJSON_AddStringToObject(obj, "eventType", "RuleFired");
    cJSON_AddStringToObject(obj, "eventState", "active");
    cJSON_AddStringToObject(obj, "eventDescription", rule_name ? rule_name : "");

    if (rule_id)   cJSON_AddStringToObject(obj, "rule_id",   rule_id);
    if (rule_name) cJSON_AddStringToObject(obj, "rule_name", rule_name);

    if (trigger_data)
        cJSON_AddItemToObject(obj, "trigger_data", cJSON_Duplicate(trigger_data, 1));

    char* json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return json;
}

/*-----------------------------------------------------
 * Per-client stream thread
 *
 * Writes multipart header, then loops waiting for events.
 *-----------------------------------------------------*/
static void* client_thread_fn(void* arg) {
    StreamClient* c = (StreamClient*)arg;

    /* Send HTTP response headers (real HTTP — Apache reverse-proxies this to the client) */
    {
        const char* hdr =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: multipart/mixed; boundary=" BOUNDARY "\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";
        if (send(c->fd, hdr, strlen(hdr), MSG_NOSIGNAL) < 0) goto done;

        /* Initial keepalive chunk — 18-byte JSON: {"connected":true} */
        const char* init_chunk =
            "--" BOUNDARY "\r\n"
            "Content-Type: application/json; charset=UTF-8\r\n"
            "Content-Length: 18\r\n"
            "\r\n"
            "{\"connected\":true}\r\n";
        if (send(c->fd, init_chunk, strlen(init_chunk), MSG_NOSIGNAL) < 0) goto done;
    }

    LOG("stream client connected (slot %d)", (int)(c - clients));

    while (1) {
        pthread_mutex_lock(&clients_lock);

        /* Wait for a broadcast signal */
        while (c->active && !c->has_pending)
            pthread_cond_wait(&broadcast_cond, &clients_lock);

        if (!c->active) {
            pthread_mutex_unlock(&clients_lock);
            break;
        }

        /* Take ownership of the pending JSON */
        char* json = c->pending_json;
        c->pending_json = NULL;
        c->has_pending = 0;
        pthread_mutex_unlock(&clients_lock);

        if (!json) continue;

        /* Write multipart chunk */
        int len = (int)strlen(json);
        char chunk_hdr[256];
        int hdr_len = snprintf(chunk_hdr, sizeof(chunk_hdr),
            "--" BOUNDARY "\r\n"
            "Content-Type: application/json; charset=UTF-8\r\n"
            "Content-Length: %d\r\n"
            "\r\n", len);

        ssize_t rc = send(c->fd, chunk_hdr, hdr_len, MSG_NOSIGNAL);
        if (rc >= 0) rc = send(c->fd, json, len, MSG_NOSIGNAL);
        if (rc >= 0) rc = send(c->fd, "\r\n", 2, MSG_NOSIGNAL);

        free(json);

        if (rc < 0) {
            LOG("stream client disconnected (slot %d)", (int)(c - clients));
            break;
        }
    }

done:
    /* Cleanup — take lock before clearing state so broadcast won't write to closed fd */
    pthread_mutex_lock(&clients_lock);
    int fd = c->fd;
    c->active = 0;
    c->fd = -1;
    if (c->pending_json) { free(c->pending_json); c->pending_json = NULL; }
    pthread_mutex_unlock(&clients_lock);

    close(fd);

    LOG("stream client thread exiting (slot %d)", (int)(c - clients));
    return NULL;
}

/*-----------------------------------------------------
 * Accept thread — TCP HTTP server on 127.0.0.1:ALERT_STREAM_PORT
 * Apache reverseProxy routes /alertStream requests here.
 *-----------------------------------------------------*/
static void* accept_thread_fn(void* arg) {
    (void)arg;
    LOG("accept thread started on port %d", ALERT_STREAM_PORT);

    while (accept_running) {
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof(addr);
        int fd = accept(server_sock, (struct sockaddr*)&addr, &addrlen);
        if (fd < 0) {
            if (accept_running) usleep(100000);
            continue;
        }

        /* Drain the HTTP request headers (we don't need them — Apache handles auth) */
        char buf[2048];
        int total = 0;
        while (total < (int)sizeof(buf) - 1) {
            int n = (int)recv(fd, buf + total, sizeof(buf) - 1 - total, 0);
            if (n <= 0) break;
            total += n;
            buf[total] = '\0';
            if (strstr(buf, "\r\n\r\n")) break;
        }

        /* Find a free client slot */
        pthread_mutex_lock(&clients_lock);
        int slot = -1;
        for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
            if (!clients[i].active) { slot = i; break; }
        }

        if (slot < 0) {
            pthread_mutex_unlock(&clients_lock);
            const char* err =
                "HTTP/1.1 503 Service Unavailable\r\n"
                "Content-Length: 0\r\n\r\n";
            send(fd, err, strlen(err), MSG_NOSIGNAL);
            close(fd);
            LOG_WARN("max stream clients reached");
            continue;
        }

        StreamClient* c = &clients[slot];
        c->fd          = fd;
        c->active      = 1;
        c->has_pending = 0;
        c->pending_json = NULL;
        pthread_mutex_unlock(&clients_lock);

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&c->thread, &attr, client_thread_fn, c) != 0) {
            LOG_WARN("failed to create client thread for slot %d", slot);
            close(fd);
            pthread_mutex_lock(&clients_lock);
            c->active = 0;
            c->fd     = -1;
            pthread_mutex_unlock(&clients_lock);
        }
        pthread_attr_destroy(&attr);
    }

    LOG("accept thread exiting");
    return NULL;
}

/*-----------------------------------------------------
 * Public API
 *-----------------------------------------------------*/

int AlertStream_Init(void) {
    memset(clients, 0, sizeof(clients));
    for (int i = 0; i < MAX_STREAM_CLIENTS; i++) clients[i].fd = -1;

    signal(SIGPIPE, SIG_IGN);  /* prevent broken-pipe from killing the process */

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        LOG_WARN("failed to create server socket: %s", strerror(errno));
        return 0;
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(ALERT_STREAM_PORT);

    if (bind(server_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_WARN("failed to bind port %d: %s", ALERT_STREAM_PORT, strerror(errno));
        close(server_sock);
        server_sock = -1;
        return 0;
    }

    if (listen(server_sock, 8) < 0) {
        LOG_WARN("listen failed: %s", strerror(errno));
        close(server_sock);
        server_sock = -1;
        return 0;
    }

    accept_running = 1;
    if (pthread_create(&accept_thread, NULL, accept_thread_fn, NULL) != 0) {
        LOG_WARN("failed to create accept thread");
        close(server_sock);
        server_sock = -1;
        accept_running = 0;
        return 0;
    }

    LOG("initialized — HTTP server on 127.0.0.1:%d", ALERT_STREAM_PORT);
    return 1;
}

void AlertStream_Cleanup(void) {
    /* Stop accept loop and close server socket */
    accept_running = 0;
    if (server_sock >= 0) {
        close(server_sock);
        server_sock = -1;
    }
    pthread_cancel(accept_thread);
    pthread_join(accept_thread, NULL);

    /* Signal all active clients to exit and close their sockets */
    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
        clients[i].active = 0;
        if (clients[i].fd >= 0) {
            close(clients[i].fd);
            clients[i].fd = -1;
        }
        if (clients[i].pending_json) {
            free(clients[i].pending_json);
            clients[i].pending_json = NULL;
        }
    }
    pthread_cond_broadcast(&broadcast_cond);
    pthread_mutex_unlock(&clients_lock);

    LOG("cleaned up");
}

void AlertStream_Broadcast(const char* rule_id, const char* rule_name,
                           cJSON* trigger_data) {
    pthread_mutex_lock(&clients_lock);

    /* Count active clients — skip work if nobody is listening */
    int active = 0;
    for (int i = 0; i < MAX_STREAM_CLIENTS; i++)
        if (clients[i].active) active++;

    if (active == 0) {
        pthread_mutex_unlock(&clients_lock);
        return;
    }

    /* Build the JSON once, duplicate for each client */
    char* json = build_event_json(rule_id, rule_name, trigger_data);
    if (!json) {
        pthread_mutex_unlock(&clients_lock);
        return;
    }

    for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
        if (!clients[i].active) continue;
        /* Free any unread previous event (client was too slow) */
        if (clients[i].pending_json) free(clients[i].pending_json);
        clients[i].pending_json = strdup(json);
        clients[i].has_pending = 1;
    }

    free(json);
    pthread_cond_broadcast(&broadcast_cond);
    pthread_mutex_unlock(&clients_lock);
}

int AlertStream_Client_Count(void) {
    pthread_mutex_lock(&clients_lock);
    int count = 0;
    for (int i = 0; i < MAX_STREAM_CLIENTS; i++)
        if (clients[i].active) count++;
    pthread_mutex_unlock(&clients_lock);
    return count;
}
