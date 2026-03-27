#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <syslog.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <glib.h>

#include "mqtt_client.h"

#define LOG(fmt, args...)      syslog(LOG_INFO,    "mqtt: " fmt, ## args)
#define LOG_WARN(fmt, args...) syslog(LOG_WARNING, "mqtt: " fmt, ## args)

/* =====================================================
 * MQTT 3.1.1 packet constants
 * ===================================================== */
#define MQTT_PKT_CONNECT     0x10
#define MQTT_PKT_CONNACK     0x20
#define MQTT_PKT_PUBLISH     0x30
#define MQTT_PKT_PUBACK      0x40
#define MQTT_PKT_SUBSCRIBE   0x82  /* type 0x80 | reserved 0x02 */
#define MQTT_PKT_SUBACK      0x90
#define MQTT_PKT_UNSUBSCRIBE 0xA2  /* type 0xA0 | reserved 0x02 */
#define MQTT_PKT_UNSUBACK    0xB0
#define MQTT_PKT_PINGREQ     0xC0
#define MQTT_PKT_PINGRESP    0xD0
#define MQTT_PKT_DISCONNECT  0xE0

#define CONNECT_TIMEOUT_SEC  10
#define RECV_TIMEOUT_SEC      5
#define RECONNECT_DELAY_MIN   2
#define RECONNECT_DELAY_MAX  60
#define MAX_SUBSCRIPTIONS    32
#define MQTT_BUF_SIZE      4096

/* =====================================================
 * Internal state
 * ===================================================== */
static MQTT_Config          cfg;
static MQTT_Message_Callback msg_cb   = NULL;
static void*                 msg_udata = NULL;
static int                   sockfd    = -1;
static SSL*                  ssl_conn  = NULL;
static SSL_CTX*              tls_ctx   = NULL;
static int                   connected = 0;
static char                  proxy_host[256] = "";
static int                   proxy_port      = 0;
static uint16_t              packet_id = 1;
static pthread_t             worker_thread;
static int                   thread_running = 0;
static int                   shutdown_pipe[2] = {-1, -1};
static pthread_mutex_t       send_lock = PTHREAD_MUTEX_INITIALIZER;
static char                  subscriptions[MAX_SUBSCRIPTIONS][256];
static int                   sub_count = 0;
static pthread_mutex_t       sub_lock  = PTHREAD_MUTEX_INITIALIZER;

/* =====================================================
 * Buffer helpers
 * ===================================================== */
typedef struct {
    uint8_t* data;
    int      len;
    int      cap;
} Buf;

static void buf_init(Buf* b) { b->data = malloc(64); b->cap = 64; b->len = 0; }
static void buf_free(Buf* b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

static void buf_append(Buf* b, const uint8_t* src, int n) {
    if (b->len + n > b->cap) {
        b->cap = b->len + n + 64;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
}

static void buf_append_u8(Buf* b, uint8_t v) { buf_append(b, &v, 1); }

static void buf_append_u16(Buf* b, uint16_t v) {
    uint8_t tmp[2] = { (uint8_t)(v >> 8), (uint8_t)(v & 0xFF) };
    buf_append(b, tmp, 2);
}

static void buf_append_str(Buf* b, const char* s) {
    uint16_t len = s ? (uint16_t)strlen(s) : 0;
    buf_append_u16(b, len);
    if (len) buf_append(b, (const uint8_t*)s, len);
}

static int encode_remaining_length(uint8_t* out, int length) {
    int idx = 0;
    do {
        uint8_t byte = (uint8_t)(length % 128);
        length /= 128;
        if (length > 0) byte |= 0x80;
        out[idx++] = byte;
    } while (length > 0 && idx < 4);
    return idx;
}

static int decode_remaining_length(const uint8_t* buf, int buf_len, int* out_len) {
    int multiplier = 1, value = 0, idx = 0;
    uint8_t byte;
    do {
        if (idx >= buf_len || idx >= 4) return -1;
        byte = buf[idx++];
        value += (byte & 0x7F) * multiplier;
        multiplier *= 128;
    } while (byte & 0x80);
    *out_len = value;
    return idx;
}

/* =====================================================
 * TCP helpers
 * ===================================================== */
static int recv_exact(int fd, SSL* ssl, uint8_t* buf, int n, int timeout_sec);  /* forward decl */

static void log_tls_error(const char* context) {
    unsigned long err = ERR_get_error();
    if (!err) {
        LOG_WARN("%s failed", context);
        return;
    }
    char buf[256] = "";
    ERR_error_string_n(err, buf, sizeof(buf));
    LOG_WARN("%s failed: %s", context, buf);
}

static int tls_init(void) {
    if (tls_ctx) return 1;
    OPENSSL_init_ssl(0, NULL);
    ERR_clear_error();
    tls_ctx = SSL_CTX_new(TLS_client_method());
    if (!tls_ctx) {
        log_tls_error("SSL_CTX_new");
        return 0;
    }
    SSL_CTX_set_min_proto_version(tls_ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(tls_ctx, SSL_VERIFY_PEER, NULL);
    if (SSL_CTX_set_default_verify_paths(tls_ctx) != 1) {
        log_tls_error("SSL_CTX_set_default_verify_paths");
        SSL_CTX_free(tls_ctx);
        tls_ctx = NULL;
        return 0;
    }
    return 1;
}

static SSL* tls_connect(int fd, const char* host) {
    if (!tls_init()) return NULL;

    ERR_clear_error();
    SSL* ssl = SSL_new(tls_ctx);
    if (!ssl) {
        log_tls_error("SSL_new");
        return NULL;
    }

    SSL_set_verify(ssl, SSL_VERIFY_PEER, NULL);
    if (host && host[0]) {
        X509_VERIFY_PARAM* param = SSL_get0_param(ssl);
        if (!param || X509_VERIFY_PARAM_set1_host(param, host, 0) != 1) {
            log_tls_error("X509_VERIFY_PARAM_set1_host");
            SSL_free(ssl);
            return NULL;
        }
        SSL_set_tlsext_host_name(ssl, host);
    }

    if (SSL_set_fd(ssl, fd) != 1) {
        log_tls_error("SSL_set_fd");
        SSL_free(ssl);
        return NULL;
    }
    if (SSL_connect(ssl) != 1) {
        log_tls_error("SSL_connect");
        SSL_free(ssl);
        return NULL;
    }
    if (SSL_get_verify_result(ssl) != X509_V_OK) {
        LOG_WARN("TLS certificate verification failed for %s", host ? host : "broker");
        SSL_free(ssl);
        return NULL;
    }
    return ssl;
}

static void tls_close(SSL** ssl_ptr) {
    if (!ssl_ptr || !*ssl_ptr) return;
    SSL_shutdown(*ssl_ptr);
    SSL_free(*ssl_ptr);
    *ssl_ptr = NULL;
}

static int tcp_connect_raw(const char* host, int port) {
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        LOG_WARN("getaddrinfo failed for %s", host);
        return -1;
    }

    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;

        struct timeval tv = { CONNECT_TIMEOUT_SEC, 0 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int flag = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* SOCKS5 CONNECT handshake — fd is already connected to the proxy.
 * Asks the proxy to tunnel to target_host:target_port.
 * Returns 0 on success, -1 on failure. */
static int socks5_handshake(int fd, const char* target_host, int target_port) {
    uint8_t buf[512];
    size_t host_len = strlen(target_host);
    if (host_len > 255) return -1;

    /* Greeting: VER=5, NMETHODS=1, METHOD=0 (no auth) */
    buf[0] = 0x05; buf[1] = 0x01; buf[2] = 0x00;
    if (send(fd, buf, 3, MSG_NOSIGNAL) != 3) return -1;

    /* Server choice */
    if (recv_exact(fd, NULL, buf, 2, CONNECT_TIMEOUT_SEC) != 2) return -1;
    if (buf[0] != 0x05 || buf[1] != 0x00) {
        LOG_WARN("SOCKS5: server rejected no-auth (method 0x%02x)", buf[1]);
        return -1;
    }

    /* CONNECT request: VER=5, CMD=1 (CONNECT), RSV=0, ATYP=3 (hostname) */
    int idx = 0;
    buf[idx++] = 0x05;
    buf[idx++] = 0x01;
    buf[idx++] = 0x00;
    buf[idx++] = 0x03;                         /* ATYP: domain name */
    buf[idx++] = (uint8_t)host_len;
    memcpy(buf + idx, target_host, host_len); idx += (int)host_len;
    buf[idx++] = (uint8_t)(target_port >> 8);
    buf[idx++] = (uint8_t)(target_port & 0xFF);
    if (send(fd, buf, idx, MSG_NOSIGNAL) != idx) return -1;

    /* Response: VER, REP, RSV, ATYP, BNDADDR, BNDPORT */
    if (recv_exact(fd, NULL, buf, 4, CONNECT_TIMEOUT_SEC) != 4) return -1;
    if (buf[0] != 0x05 || buf[1] != 0x00) {
        LOG_WARN("SOCKS5: CONNECT failed, REP=0x%02x", buf[1]);
        return -1;
    }
    /* Drain bound address */
    int drain = 0;
    if      (buf[3] == 0x01) drain = 4 + 2;   /* IPv4 + port */
    else if (buf[3] == 0x03) {
        uint8_t alen;
        if (recv_exact(fd, NULL, &alen, 1, CONNECT_TIMEOUT_SEC) != 1) return -1;
        drain = alen + 2;
    }
    else if (buf[3] == 0x04) drain = 16 + 2;  /* IPv6 + port */
    while (drain > 0) {
        int chunk = drain > (int)sizeof(buf) ? (int)sizeof(buf) : drain;
        if (recv_exact(fd, NULL, buf, chunk, CONNECT_TIMEOUT_SEC) != chunk) return -1;
        drain -= chunk;
    }
    return 0;
}

static int tcp_connect(const char* host, int port) {
    if (proxy_host[0]) {
        int fd = tcp_connect_raw(proxy_host, proxy_port);
        if (fd < 0) {
            LOG_WARN("SOCKS5 proxy connect failed (%s:%d)", proxy_host, proxy_port);
            return -1;
        }
        if (socks5_handshake(fd, host, port) != 0) {
            LOG_WARN("SOCKS5 handshake to %s:%d failed", host, port);
            close(fd);
            return -1;
        }
        LOG("SOCKS5 tunnel to %s:%d via %s:%d", host, port, proxy_host, proxy_port);
        return fd;
    }
    return tcp_connect_raw(host, port);
}

static int send_all(int fd, SSL* ssl, const uint8_t* data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = ssl
            ? SSL_write(ssl, data + sent, len - sent)
            : (int)send(fd, data + sent, (size_t)(len - sent), MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += n;
    }
    return sent;
}

/* recv exactly n bytes with timeout — returns n on success, -1 on error/timeout */
static int recv_exact(int fd, SSL* ssl, uint8_t* buf, int n, int timeout_sec) {
    int received = 0;
    while (received < n) {
        fd_set fds;
        FD_ZERO(&fds); FD_SET(fd, &fds);
        struct timeval tv = { timeout_sec, 0 };
        int r = select(fd + 1, &fds, NULL, NULL, &tv);
        if (r <= 0) return -1;
        int got = ssl
            ? SSL_read(ssl, buf + received, n - received)
            : (int)recv(fd, buf + received, (size_t)(n - received), 0);
        if (got <= 0) return -1;
        received += got;
    }
    return received;
}

/* =====================================================
 * MQTT packet builders
 * ===================================================== */
static int send_connect(int fd, SSL* ssl) {
    Buf payload; buf_init(&payload);
    buf_append_str(&payload, "MQTT");           /* protocol name */
    buf_append_u8(&payload, 4);                 /* protocol level 3.1.1 */

    uint8_t connect_flags = 0x02;               /* clean session */
    if (cfg.username[0]) connect_flags |= 0x80;
    if (cfg.username[0] && cfg.password[0]) connect_flags |= 0x40;
    buf_append_u8(&payload, connect_flags);
    buf_append_u16(&payload, (uint16_t)(cfg.keepalive > 0 ? cfg.keepalive : 60));
    buf_append_str(&payload, cfg.client_id[0] ? cfg.client_id : "acap_event_engine");
    if (cfg.username[0]) buf_append_str(&payload, cfg.username);
    if (cfg.username[0] && cfg.password[0]) buf_append_str(&payload, cfg.password);

    uint8_t rl[4];
    int rl_len = encode_remaining_length(rl, payload.len);

    Buf pkt; buf_init(&pkt);
    buf_append_u8(&pkt, MQTT_PKT_CONNECT);
    buf_append(&pkt, rl, rl_len);
    buf_append(&pkt, payload.data, payload.len);
    buf_free(&payload);

    int ret = send_all(fd, ssl, pkt.data, pkt.len);
    buf_free(&pkt);
    return ret > 0 ? 1 : 0;
}

static int recv_connack(int fd, SSL* ssl) {
    uint8_t buf[4];
    if (recv_exact(fd, ssl, buf, 4, CONNECT_TIMEOUT_SEC) != 4) return 0;
    if (buf[0] != MQTT_PKT_CONNACK || buf[1] != 2) return 0;
    if (buf[3] != 0) {
        LOG_WARN("CONNACK returned code %d", buf[3]);
        return 0;
    }
    return 1;
}

static int send_subscribe(int fd, SSL* ssl, const char* topic_filter) {
    pthread_mutex_lock(&send_lock);
    uint16_t pid = packet_id++;
    if (packet_id == 0) packet_id = 1;

    Buf payload; buf_init(&payload);
    buf_append_u16(&payload, pid);
    buf_append_str(&payload, topic_filter);
    buf_append_u8(&payload, 1);   /* Request QoS 1 */

    uint8_t rl[4];
    int rl_len = encode_remaining_length(rl, payload.len);
    Buf pkt; buf_init(&pkt);
    buf_append_u8(&pkt, MQTT_PKT_SUBSCRIBE);
    buf_append(&pkt, rl, rl_len);
    buf_append(&pkt, payload.data, payload.len);
    buf_free(&payload);

    int ret = send_all(fd, ssl, pkt.data, pkt.len);
    buf_free(&pkt);
    pthread_mutex_unlock(&send_lock);
    return ret > 0 ? 1 : 0;
}

static int send_unsubscribe(int fd, SSL* ssl, const char* topic_filter) {
    pthread_mutex_lock(&send_lock);
    uint16_t pid = packet_id++;
    if (packet_id == 0) packet_id = 1;

    Buf payload; buf_init(&payload);
    buf_append_u16(&payload, pid);
    buf_append_str(&payload, topic_filter);

    uint8_t rl[4];
    int rl_len = encode_remaining_length(rl, payload.len);
    Buf pkt; buf_init(&pkt);
    buf_append_u8(&pkt, MQTT_PKT_UNSUBSCRIBE);
    buf_append(&pkt, rl, rl_len);
    buf_append(&pkt, payload.data, payload.len);
    buf_free(&payload);

    int ret = send_all(fd, ssl, pkt.data, pkt.len);
    buf_free(&pkt);
    pthread_mutex_unlock(&send_lock);
    return ret > 0 ? 1 : 0;
}

static void send_puback(int fd, SSL* ssl, uint16_t pid) {
    uint8_t pkt[4] = { MQTT_PKT_PUBACK, 2,
                       (uint8_t)(pid >> 8), (uint8_t)(pid & 0xFF) };
    pthread_mutex_lock(&send_lock);
    send_all(fd, ssl, pkt, 4);
    pthread_mutex_unlock(&send_lock);
}

static int send_pingreq(int fd, SSL* ssl) {
    uint8_t pkt[2] = { MQTT_PKT_PINGREQ, 0 };
    return send_all(fd, ssl, pkt, 2) > 0 ? 1 : 0;
}

static int send_disconnect(int fd, SSL* ssl) {
    uint8_t pkt[2] = { MQTT_PKT_DISCONNECT, 0 };
    return send_all(fd, ssl, pkt, 2) > 0 ? 1 : 0;
}

/* =====================================================
 * Dispatch incoming PUBLISH to GMainLoop via g_idle_add
 * ===================================================== */
typedef struct {
    char* topic;
    char* payload;
    int   payload_len;
} IncomingMsg;

static gboolean dispatch_message(gpointer data) {
    IncomingMsg* m = (IncomingMsg*)data;
    if (msg_cb) msg_cb(m->topic, m->payload, m->payload_len, msg_udata);
    free(m->topic);
    free(m->payload);
    free(m);
    return G_SOURCE_REMOVE;
}

/* Parse and dispatch a PUBLISH packet.
 * buf holds the variable header + payload (remaining_len bytes).
 * qos is extracted from the fixed header flags.
 * For QoS 1 we send a PUBACK back on fd. */
static void handle_publish(const uint8_t* buf, int remaining_len, uint8_t qos, int fd, SSL* ssl) {
    if (remaining_len < 2) return;
    int pos = 0;
    uint16_t topic_len = ((uint16_t)buf[pos] << 8) | buf[pos + 1];
    pos += 2;
    if (pos + topic_len > remaining_len) return;

    char* topic = malloc(topic_len + 1);
    memcpy(topic, buf + pos, topic_len);
    topic[topic_len] = '\0';
    pos += topic_len;

    /* QoS 1: variable header contains a 2-byte packet ID after the topic */
    if (qos == 1) {
        if (pos + 2 > remaining_len) { free(topic); return; }
        uint16_t pid = ((uint16_t)buf[pos] << 8) | buf[pos + 1];
        pos += 2;
        send_puback(fd, ssl, pid);
    }

    int payload_len = remaining_len - pos;
    char* payload = malloc(payload_len + 1);
    memcpy(payload, buf + pos, payload_len);
    payload[payload_len] = '\0';

    IncomingMsg* m = malloc(sizeof(IncomingMsg));
    m->topic       = topic;
    m->payload     = payload;
    m->payload_len = payload_len;
    g_idle_add(dispatch_message, m);
}

/* =====================================================
 * Worker thread — connect, receive loop, keepalive
 * ===================================================== */
static int do_subscribe_all(int fd, SSL* ssl) {
    pthread_mutex_lock(&sub_lock);
    for (int i = 0; i < sub_count; i++) {
        if (!send_subscribe(fd, ssl, subscriptions[i])) {
            LOG_WARN("subscribe failed for '%s'", subscriptions[i]);
        }
    }
    pthread_mutex_unlock(&sub_lock);
    return 1;
}

static void* worker_fn(void* arg) {
    (void)arg;
    int reconnect_delay = RECONNECT_DELAY_MIN;
    uint8_t recv_buf[MQTT_BUF_SIZE];

    while (1) {
        /* Check shutdown */
        fd_set check; FD_ZERO(&check); FD_SET(shutdown_pipe[0], &check);
        struct timeval tv_zero = {0, 0};
        if (select(shutdown_pipe[0] + 1, &check, NULL, NULL, &tv_zero) > 0) break;

        if (!cfg.enabled || !cfg.host[0]) {
            sleep(2);
            continue;
        }

        LOG("connecting to %s:%d%s", cfg.host, cfg.port, cfg.use_tls ? " with TLS" : "");
        int fd = tcp_connect(cfg.host, cfg.port);
        if (fd < 0) {
            LOG_WARN("TCP connect failed, retry in %ds", reconnect_delay);
            sleep(reconnect_delay);
            reconnect_delay = reconnect_delay < RECONNECT_DELAY_MAX ? reconnect_delay * 2 : RECONNECT_DELAY_MAX;
            continue;
        }

        SSL* ssl = NULL;
        if (cfg.use_tls) {
            ssl = tls_connect(fd, cfg.host);
            if (!ssl) {
                close(fd);
                sleep(reconnect_delay);
                reconnect_delay = reconnect_delay < RECONNECT_DELAY_MAX ? reconnect_delay * 2 : RECONNECT_DELAY_MAX;
                continue;
            }
        }

        if (!send_connect(fd, ssl) || !recv_connack(fd, ssl)) {
            LOG_WARN("MQTT handshake failed, retry in %ds", reconnect_delay);
            tls_close(&ssl);
            close(fd); fd = -1;
            sleep(reconnect_delay);
            reconnect_delay = reconnect_delay < RECONNECT_DELAY_MAX ? reconnect_delay * 2 : RECONNECT_DELAY_MAX;
            continue;
        }

        LOG("connected to %s:%d", cfg.host, cfg.port);
        pthread_mutex_lock(&send_lock);
        sockfd    = fd;
        ssl_conn  = ssl;
        connected = 1;
        pthread_mutex_unlock(&send_lock);
        reconnect_delay = RECONNECT_DELAY_MIN;

        do_subscribe_all(fd, ssl);

        time_t last_ping = time(NULL);
        int keepalive = cfg.keepalive > 0 ? cfg.keepalive : 60;

        /* Receive loop */
        while (1) {
            /* Check shutdown pipe */
            fd_set rfds; FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            FD_SET(shutdown_pipe[0], &rfds);
            int maxfd = fd > shutdown_pipe[0] ? fd : shutdown_pipe[0];
            struct timeval tv = { 1, 0 }; /* 1s tick */
            int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);

            if (FD_ISSET(shutdown_pipe[0], &rfds)) goto cleanup;

            /* Keepalive */
            time_t now = time(NULL);
            if (keepalive > 0 && (now - last_ping) >= keepalive) {
                if (!send_pingreq(fd, ssl)) { LOG_WARN("pingreq failed"); break; }
                last_ping = now;
            }

            if (r <= 0 || !FD_ISSET(fd, &rfds)) continue;

            /* Read fixed header byte */
            uint8_t fhdr;
            if (recv_exact(fd, ssl, &fhdr, 1, RECV_TIMEOUT_SEC) != 1) break;

            /* Read remaining length (VLE) */
            int remaining_len = 0;
            for (int i = 0; i < 4; i++) {
                uint8_t b;
                if (recv_exact(fd, ssl, &b, 1, RECV_TIMEOUT_SEC) != 1) goto disconnect;
                remaining_len += (b & 0x7F) << (7 * i);
                if (!(b & 0x80)) break;
            }

            /* Read variable header + payload */
            if (remaining_len > 0) {
                int buf_sz = remaining_len < MQTT_BUF_SIZE ? remaining_len : MQTT_BUF_SIZE;
                if (recv_exact(fd, ssl, recv_buf, buf_sz, RECV_TIMEOUT_SEC) != buf_sz) break;
            }

            uint8_t pkt_type = fhdr & 0xF0;
            if (pkt_type == MQTT_PKT_PUBLISH) {
                uint8_t qos_level = (fhdr >> 1) & 0x03;
                handle_publish(recv_buf, remaining_len, qos_level, fd, ssl);
            } else if (pkt_type == MQTT_PKT_PUBACK) {
                /* QoS 1 delivery confirmed by broker — no retransmit needed */
            }
            /* PINGRESP, SUBACK, UNSUBACK — no action needed */
            continue;

        disconnect:
            break;
        }

        LOG_WARN("disconnected from %s", cfg.host);
        pthread_mutex_lock(&send_lock);
        if (sockfd == fd) {
            sockfd = -1;
            ssl_conn = NULL;
            connected = 0;
        }
        pthread_mutex_unlock(&send_lock);
        tls_close(&ssl);
        close(fd); fd = -1;

        sleep(reconnect_delay);
        reconnect_delay = reconnect_delay < RECONNECT_DELAY_MAX ? reconnect_delay * 2 : RECONNECT_DELAY_MAX;
        continue;

    cleanup:
        pthread_mutex_lock(&send_lock);
        if (sockfd >= 0) {
            send_disconnect(sockfd, ssl_conn);
            tls_close(&ssl_conn);
            close(sockfd);
            sockfd = -1;
        }
        connected = 0;
        pthread_mutex_unlock(&send_lock);
        break;
    }
    return NULL;
}

/* =====================================================
 * Public API
 * ===================================================== */
int MQTT_Init(MQTT_Config* config, MQTT_Message_Callback cb, void* user_data) {
    memcpy(&cfg, config, sizeof(MQTT_Config));
    msg_cb    = cb;
    msg_udata = user_data;

    if (pipe(shutdown_pipe) < 0) {
        LOG_WARN("failed to create shutdown pipe");
        return 0;
    }

    thread_running = 1;
    if (pthread_create(&worker_thread, NULL, worker_fn, NULL) != 0) {
        LOG_WARN("failed to start worker thread");
        thread_running = 0;
        return 0;
    }
    return 1;
}

void MQTT_Cleanup(void) {
    if (!thread_running) return;
    /* Signal shutdown */
    uint8_t sig = 1;
    write(shutdown_pipe[1], &sig, 1);
    pthread_join(worker_thread, NULL);
    thread_running = 0;
    close(shutdown_pipe[0]); close(shutdown_pipe[1]);
    shutdown_pipe[0] = shutdown_pipe[1] = -1;
    if (tls_ctx) {
        SSL_CTX_free(tls_ctx);
        tls_ctx = NULL;
    }
}

int MQTT_Reconfigure(MQTT_Config* config) {
    int host_changed = strcmp(cfg.host, config->host) != 0 || cfg.port != config->port || cfg.use_tls != config->use_tls;
    memcpy(&cfg, config, sizeof(MQTT_Config));

    if (host_changed && connected) {
        /* Force reconnect — worker thread owns SSL teardown */
        pthread_mutex_lock(&send_lock);
        if (sockfd >= 0) {
            close(sockfd);
            sockfd = -1;
            ssl_conn = NULL;
            connected = 0;
        }
        pthread_mutex_unlock(&send_lock);
    }
    return 1;
}

void MQTT_Set_Proxy(const char* ph, int pp) {
    int changed = (strcmp(proxy_host, ph ? ph : "") != 0 || proxy_port != pp);
    snprintf(proxy_host, sizeof(proxy_host), "%s", ph ? ph : "");
    proxy_port = pp;
    if (changed && connected) {
        pthread_mutex_lock(&send_lock);
        if (sockfd >= 0) {
            close(sockfd);
            sockfd = -1;
            ssl_conn = NULL;
            connected = 0;
        }
        pthread_mutex_unlock(&send_lock);
    }
}

int MQTT_Publish(const char* topic, const char* payload, int retain, int qos) {
    if (!topic || !connected) return 0;
    if (qos < 0 || qos > 1) qos = 0;

    pthread_mutex_lock(&send_lock);
    if (sockfd < 0) { pthread_mutex_unlock(&send_lock); return 0; }

    int topic_len   = (int)strlen(topic);
    int payload_len = payload ? (int)strlen(payload) : 0;
    /* QoS 1 adds a 2-byte packet ID after the topic */
    int var_len = 2 + topic_len + (qos > 0 ? 2 : 0) + payload_len;

    uint16_t pid = 0;
    if (qos > 0) {
        pid = packet_id++;
        if (packet_id == 0) packet_id = 1;
    }

    Buf pkt; buf_init(&pkt);
    uint8_t fhdr = (uint8_t)(MQTT_PKT_PUBLISH | ((qos & 0x03) << 1) | (retain ? 0x01 : 0x00));
    buf_append_u8(&pkt, fhdr);
    uint8_t rl[4];
    int rl_len = encode_remaining_length(rl, var_len);
    buf_append(&pkt, rl, rl_len);
    buf_append_u16(&pkt, (uint16_t)topic_len);
    buf_append(&pkt, (const uint8_t*)topic, topic_len);
    if (qos > 0) buf_append_u16(&pkt, pid);
    if (payload_len) buf_append(&pkt, (const uint8_t*)payload, payload_len);

    int ret = send_all(sockfd, ssl_conn, pkt.data, pkt.len);
    buf_free(&pkt);
    pthread_mutex_unlock(&send_lock);
    return ret > 0 ? 1 : 0;
}

int MQTT_Subscribe(const char* topic_filter) {
    if (!topic_filter) return 0;
    pthread_mutex_lock(&sub_lock);
    /* Dedup */
    for (int i = 0; i < sub_count; i++) {
        if (strcmp(subscriptions[i], topic_filter) == 0) {
            pthread_mutex_unlock(&sub_lock);
            return 1;
        }
    }
    if (sub_count >= MAX_SUBSCRIPTIONS) {
        pthread_mutex_unlock(&sub_lock);
        return 0;
    }
    snprintf(subscriptions[sub_count++], 256, "%s", topic_filter);
    pthread_mutex_unlock(&sub_lock);

    /* Subscribe immediately if connected */
    pthread_mutex_lock(&send_lock);
    int fd = sockfd;
    SSL* ssl = ssl_conn;
    pthread_mutex_unlock(&send_lock);
    if (fd >= 0) send_subscribe(fd, ssl, topic_filter);
    return 1;
}

void MQTT_Unsubscribe(const char* topic_filter) {
    if (!topic_filter) return;
    pthread_mutex_lock(&sub_lock);
    for (int i = 0; i < sub_count; i++) {
        if (strcmp(subscriptions[i], topic_filter) == 0) {
            if (i < sub_count - 1)
                memcpy(subscriptions[i], subscriptions[sub_count - 1], 256);
            sub_count--;
            break;
        }
    }
    pthread_mutex_unlock(&sub_lock);

    /* Send UNSUBSCRIBE to broker if connected */
    pthread_mutex_lock(&send_lock);
    int fd = sockfd;
    SSL* ssl = ssl_conn;
    pthread_mutex_unlock(&send_lock);
    if (fd >= 0) send_unsubscribe(fd, ssl, topic_filter);
}

int MQTT_Is_Connected(void) {
    return connected;
}

cJSON* MQTT_Status(void) {
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj,   "enabled",   cfg.enabled);
    cJSON_AddBoolToObject(obj,   "connected", connected);
    cJSON_AddBoolToObject(obj,   "use_tls",   cfg.use_tls);
    cJSON_AddStringToObject(obj, "host",      cfg.host);
    cJSON_AddNumberToObject(obj, "port",      cfg.port);
    cJSON_AddStringToObject(obj, "client_id", cfg.client_id);
    cJSON_AddNumberToObject(obj, "subscriptions", sub_count);
    return obj;
}
