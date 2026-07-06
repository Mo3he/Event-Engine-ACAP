/*
 * modbus_pool.c — self-contained Modbus TCP/RTU connection pool.
 *
 * No external libmodbus dependency. Uses POSIX sockets (TCP) and
 * termios (RTU/RS-485) to implement the Modbus application protocol.
 *
 * serial_gateway: connects via TCP but speaks RTU framing (CRC, no MBAP).
 * Use with Axis PortManager GenericTCPServer to expose a local RS-485 port.
 *
 * Supported function codes:
 *   0x01  Read Coils
 *   0x02  Read Discrete Inputs
 *   0x03  Read Holding Registers
 *   0x04  Read Input Registers
 *   0x05  Write Single Coil
 *   0x06  Write Single Register
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <syslog.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <termios.h>

/* --- glibc backward-compat shim (OS 13 SDK -> older AXIS OS) ---
 * glibc 2.42 (shipped in the OS 13 SDK / Ubuntu 24.04) introduced new symbol
 * versions for cfsetispeed()/cfsetospeed(). Left alone, the binary would
 * require GLIBC_2.42 and fail to load on older firmware (e.g. AXIS OS 11.x):
 *   libc.so.6: version `GLIBC_2.42' not found
 * These are the ONLY two symbols pulling in GLIBC_2.42, and their behaviour is
 * unchanged for the standard baud rates we use, so pin the references to each
 * architecture's glibc baseline to keep the app loadable on older AXIS OS. */
#if defined(__aarch64__)
__asm__(".symver cfsetispeed,cfsetispeed@GLIBC_2.17");
__asm__(".symver cfsetospeed,cfsetospeed@GLIBC_2.17");
#elif defined(__arm__)
__asm__(".symver cfsetispeed,cfsetispeed@GLIBC_2.4");
__asm__(".symver cfsetospeed,cfsetospeed@GLIBC_2.4");
#endif

#include "modbus_pool.h"

#define LOG(fmt, args...)      syslog(LOG_INFO,    "modbus_pool: " fmt, ## args)
#define LOG_WARN(fmt, args...) syslog(LOG_WARNING, "modbus_pool: " fmt, ## args)

#define POOL_MAX        16
#define MB_TIMEOUT_S     2   /* TCP socket timeout */
#define MB_RTU_VTIME    10   /* termios VTIME units (0.1s each) → 1s */

typedef enum { MB_TCP, MB_RTU, MB_SERIAL_GW } mb_transport_t;

struct mb_ctx {
    mb_transport_t transport;
    /* TCP */
    char   host[128];
    int    port;
    /* RTU */
    char   device[64];
    int    baud;
    char   parity;      /* 'N', 'E', 'O' */
    /* state */
    int    fd;          /* socket or serial fd; -1 = disconnected */
    int    connected;
    char   key[256];
};

static struct mb_ctx    pool[POOL_MAX];
static int              pool_used[POOL_MAX];
static pthread_mutex_t  pool_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint16_t         g_txid = 1;

/* ------------------------------------------------------------------ */
/* CRC-16/ANSI (Modbus RTU)                                           */
/* ------------------------------------------------------------------ */

static uint16_t crc16(const uint8_t* buf, int len) {
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 1) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
    }
    return crc;
}

/* ------------------------------------------------------------------ */
/* Connection helpers                                                  */
/* ------------------------------------------------------------------ */

static speed_t baud_to_speed(int baud) {
    switch (baud) {
        case 1200:   return B1200;
        case 2400:   return B2400;
        case 4800:   return B4800;
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        default:     return B9600;
    }
}

static int tcp_connect(struct mb_ctx* c) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", c->port);
    if (getaddrinfo(c->host, portstr, &hints, &res) != 0 || !res) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    struct timeval tv = { MB_TIMEOUT_S, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        close(fd); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);
    c->fd = fd; c->connected = 1;
    return 0;
}

static int rtu_connect(struct mb_ctx* c) {
    int fd = open(c->device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    /* switch to blocking mode */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    struct termios tio;
    memset(&tio, 0, sizeof(tio));
    cfmakeraw(&tio);
    cfsetispeed(&tio, baud_to_speed(c->baud));
    cfsetospeed(&tio, baud_to_speed(c->baud));
    tio.c_cflag |= (CLOCAL | CREAD | CS8);
    tio.c_cflag &= ~CSTOPB;
    if      (c->parity == 'E') { tio.c_cflag |= PARENB;  tio.c_cflag &= ~PARODD; }
    else if (c->parity == 'O') { tio.c_cflag |= (PARENB | PARODD); }
    else                       { tio.c_cflag &= ~PARENB; }
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = MB_RTU_VTIME;
    if (tcsetattr(fd, TCSANOW, &tio) != 0) { close(fd); return -1; }
    tcflush(fd, TCIOFLUSH);
    c->fd = fd; c->connected = 1;
    return 0;
}

static int ensure_connected(struct mb_ctx* c) {
    if (c->connected && c->fd >= 0) return 0;
    if (c->fd >= 0) { close(c->fd); c->fd = -1; }
    return (c->transport == MB_RTU) ? rtu_connect(c) : tcp_connect(c);
}

/* ------------------------------------------------------------------ */
/* Transport-level request/response                                    */
/* ------------------------------------------------------------------ */

/* Send PDU over TCP (wraps in MBAP header), receive response PDU.
 * resp_pdu must be at least 256 bytes. Returns 0 on success. */
static int tcp_request(struct mb_ctx* c, int slave_id,
                       const uint8_t* pdu, int pdu_len,
                       uint8_t* resp_pdu, int* resp_pdu_len) {
    uint8_t frame[264];
    uint16_t txid = g_txid++;
    frame[0] = (txid >> 8) & 0xFF;
    frame[1] =  txid & 0xFF;
    frame[2] = 0; frame[3] = 0;   /* protocol ID = 0 */
    uint16_t len = (uint16_t)(1 + pdu_len);
    frame[4] = (len >> 8) & 0xFF;
    frame[5] =  len & 0xFF;
    frame[6] = (uint8_t)slave_id;
    memcpy(frame + 7, pdu, pdu_len);
    int total = 7 + pdu_len;
    if (send(c->fd, frame, total, 0) != total) return -1;
    /* receive MBAP header (6 bytes) */
    uint8_t hdr[6];
    if (recv(c->fd, hdr, 6, MSG_WAITALL) != 6) return -1;
    uint16_t rlen = ((uint16_t)hdr[4] << 8) | hdr[5];
    if (rlen < 2 || rlen > 253) return -1;
    /* receive unit_id (1) + PDU (rlen-1) */
    uint8_t rbuf[256];
    if (recv(c->fd, rbuf, rlen, MSG_WAITALL) != (int)rlen) return -1;
    *resp_pdu_len = rlen - 1;
    memcpy(resp_pdu, rbuf + 1, *resp_pdu_len);
    return 0;
}

/* Send PDU over RTU serial (adds addr + CRC), receive and validate response PDU.
 * resp_pdu must be at least 256 bytes. Returns 0 on success. */
static int rtu_request(struct mb_ctx* c, int slave_id,
                       const uint8_t* pdu, int pdu_len,
                       uint8_t* resp_pdu, int* resp_pdu_len) {
    uint8_t frame[256];
    frame[0] = (uint8_t)slave_id;
    memcpy(frame + 1, pdu, pdu_len);
    uint16_t crc = crc16(frame, 1 + pdu_len);
    frame[1 + pdu_len]     =  crc & 0xFF;
    frame[1 + pdu_len + 1] = (crc >> 8) & 0xFF;
    tcflush(c->fd, TCIOFLUSH);
    int flen = 1 + pdu_len + 2;
    if (write(c->fd, frame, flen) != flen) return -1;
    /* read response with timeout via VTIME */
    uint8_t rbuf[256];
    int got = 0;
    while (got < 256) {
        int n = (int)read(c->fd, rbuf + got, sizeof(rbuf) - got);
        if (n <= 0) break;
        got += n;
        /* estimate expected frame length once we have enough bytes */
        if (got >= 5) {
            uint8_t fc = rbuf[1] & 0x7F;
            int expected;
            if (rbuf[1] & 0x80) {
                expected = 5;   /* exception: addr+FC+code+CRC = 5 */
            } else if (fc <= 0x04) {
                expected = (got >= 3) ? 3 + rbuf[2] + 2 : 5;
            } else {
                expected = 8;   /* write echo responses */
            }
            if (got >= expected) break;
        }
    }
    if (got < 4) { errno = ETIMEDOUT; return -1; }
    uint16_t rx_crc = ((uint16_t)rbuf[got - 1] << 8) | rbuf[got - 2];
    if (crc16(rbuf, got - 2) != rx_crc) { errno = EBADMSG; return -1; }
    if (rbuf[0] != (uint8_t)slave_id)   { errno = EBADMSG; return -1; }
    *resp_pdu_len = got - 3;   /* strip addr + 2-byte CRC */
    memcpy(resp_pdu, rbuf + 1, *resp_pdu_len);
    return 0;
}

/* Send RTU-framed PDU over a raw TCP socket (serial_gateway mode).
 * Same framing as rtu_request() but no tcflush; uses send()/recv(). */
static int sgw_request(struct mb_ctx* c, int slave_id,
                       const uint8_t* pdu, int pdu_len,
                       uint8_t* resp_pdu, int* resp_pdu_len) {
    uint8_t frame[256];
    frame[0] = (uint8_t)slave_id;
    memcpy(frame + 1, pdu, pdu_len);
    uint16_t crc = crc16(frame, 1 + pdu_len);
    frame[1 + pdu_len]     =  crc & 0xFF;
    frame[1 + pdu_len + 1] = (crc >> 8) & 0xFF;
    int flen = 1 + pdu_len + 2;
    if (send(c->fd, frame, flen, 0) != flen) return -1;
    uint8_t rbuf[256];
    int got = 0;
    while (got < (int)sizeof(rbuf)) {
        int n = (int)recv(c->fd, rbuf + got, sizeof(rbuf) - got, 0);
        if (n <= 0) break;
        got += n;
        if (got >= 5) {
            uint8_t fc = rbuf[1] & 0x7F;
            int expected;
            if (rbuf[1] & 0x80) {
                expected = 5;
            } else if (fc <= 0x04) {
                expected = (got >= 3) ? 3 + rbuf[2] + 2 : 5;
            } else {
                expected = 8;
            }
            if (got >= expected) break;
        }
    }
    if (got < 4) { errno = ETIMEDOUT; return -1; }
    uint16_t rx_crc = ((uint16_t)rbuf[got - 1] << 8) | rbuf[got - 2];
    if (crc16(rbuf, got - 2) != rx_crc) { errno = EBADMSG; return -1; }
    if (rbuf[0] != (uint8_t)slave_id)   { errno = EBADMSG; return -1; }
    *resp_pdu_len = got - 3;
    memcpy(resp_pdu, rbuf + 1, *resp_pdu_len);
    return 0;
}

static int do_request(struct mb_ctx* c, int slave_id,
                      const uint8_t* pdu, int pdu_len,
                      uint8_t* resp_pdu, int* resp_pdu_len) {
    if (c->transport == MB_TCP)       return tcp_request(c, slave_id, pdu, pdu_len, resp_pdu, resp_pdu_len);
    if (c->transport == MB_SERIAL_GW) return sgw_request(c, slave_id, pdu, pdu_len, resp_pdu, resp_pdu_len);
    return rtu_request(c, slave_id, pdu, pdu_len, resp_pdu, resp_pdu_len);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int mb_read_register(mb_ctx_t* ctx, int slave_id, int fc, int address, uint16_t* out) {
    if (!ctx || !out) return -1;
    pthread_mutex_lock(&pool_mutex);
    if (ensure_connected(ctx) != 0) { pthread_mutex_unlock(&pool_mutex); return -1; }
    uint8_t pdu[5] = {
        (uint8_t)fc,
        (address >> 8) & 0xFF, address & 0xFF,
        0x00, 0x01
    };
    uint8_t resp[256]; int rlen = 0;
    int rc = do_request(ctx, slave_id, pdu, 5, resp, &rlen);
    pthread_mutex_unlock(&pool_mutex);
    if (rc != 0 || rlen < 4) return -1;
    if (resp[0] & 0x80) { errno = EIO; return -1; }  /* Modbus exception */
    *out = ((uint16_t)resp[2] << 8) | resp[3];
    return 0;
}

int mb_read_bit(mb_ctx_t* ctx, int slave_id, int fc, int address, uint8_t* out) {
    if (!ctx || !out) return -1;
    pthread_mutex_lock(&pool_mutex);
    if (ensure_connected(ctx) != 0) { pthread_mutex_unlock(&pool_mutex); return -1; }
    uint8_t pdu[5] = {
        (uint8_t)fc,
        (address >> 8) & 0xFF, address & 0xFF,
        0x00, 0x01
    };
    uint8_t resp[256]; int rlen = 0;
    int rc = do_request(ctx, slave_id, pdu, 5, resp, &rlen);
    pthread_mutex_unlock(&pool_mutex);
    if (rc != 0 || rlen < 3) return -1;
    if (resp[0] & 0x80) { errno = EIO; return -1; }
    *out = resp[2] & 0x01;
    return 0;
}

int mb_write_register(mb_ctx_t* ctx, int slave_id, int address, uint16_t value) {
    if (!ctx) return -1;
    pthread_mutex_lock(&pool_mutex);
    if (ensure_connected(ctx) != 0) { pthread_mutex_unlock(&pool_mutex); return -1; }
    uint8_t pdu[5] = {
        0x06,
        (address >> 8) & 0xFF, address & 0xFF,
        (value >> 8) & 0xFF, value & 0xFF
    };
    uint8_t resp[256]; int rlen = 0;
    int rc = do_request(ctx, slave_id, pdu, 5, resp, &rlen);
    pthread_mutex_unlock(&pool_mutex);
    return rc;
}

int mb_write_bit(mb_ctx_t* ctx, int slave_id, int address, int value) {
    if (!ctx) return -1;
    pthread_mutex_lock(&pool_mutex);
    if (ensure_connected(ctx) != 0) { pthread_mutex_unlock(&pool_mutex); return -1; }
    uint16_t v = value ? 0xFF00 : 0x0000;
    uint8_t pdu[5] = {
        0x05,
        (address >> 8) & 0xFF, address & 0xFF,
        (v >> 8) & 0xFF, v & 0xFF
    };
    uint8_t resp[256]; int rlen = 0;
    int rc = do_request(ctx, slave_id, pdu, 5, resp, &rlen);
    pthread_mutex_unlock(&pool_mutex);
    return rc;
}

mb_ctx_t* modbus_pool_get(cJSON* cfg) {
    if (!cfg) return NULL;
    const char* ctype = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "connection_type"));
    if (!ctype) ctype = "tcp";
    mb_transport_t transport;
    if      (strcmp(ctype, "rtu")            == 0) transport = MB_RTU;
    else if (strcmp(ctype, "serial_gateway") == 0) transport = MB_SERIAL_GW;
    else                                            transport = MB_TCP;

    char key[256];
    if (transport == MB_TCP || transport == MB_SERIAL_GW) {
        const char* host = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "host"));
        cJSON* port_j = cJSON_GetObjectItem(cfg, "port");
        int port = port_j ? (int)port_j->valuedouble : 502;
        const char* pfx = (transport == MB_SERIAL_GW) ? "sgw" : "tcp";
        snprintf(key, sizeof(key), "%s:%s:%d", pfx, host ? host : "127.0.0.1", port);
    } else {
        const char* dev = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "device"));
        cJSON* baud_j = cJSON_GetObjectItem(cfg, "baud");
        int baud = baud_j ? (int)baud_j->valuedouble : 9600;
        const char* parity = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "parity"));
        snprintf(key, sizeof(key), "rtu:%s:%d:%s",
                 dev ? dev : "/dev/ttyS1", baud,
                 (parity && parity[0]) ? parity : "N");
    }

    pthread_mutex_lock(&pool_mutex);

    /* search for existing entry */
    for (int i = 0; i < POOL_MAX; i++) {
        if (pool_used[i] && strcmp(pool[i].key, key) == 0) {
            struct mb_ctx* c = &pool[i];
            pthread_mutex_unlock(&pool_mutex);
            return c;
        }
    }

    /* find a free slot */
    int slot = -1;
    for (int i = 0; i < POOL_MAX; i++) {
        if (!pool_used[i]) { slot = i; break; }
    }
    if (slot < 0) {
        LOG_WARN("pool full (%d entries); cannot add new connection", POOL_MAX);
        pthread_mutex_unlock(&pool_mutex);
        return NULL;
    }

    struct mb_ctx* c = &pool[slot];
    memset(c, 0, sizeof(*c));
    c->transport = transport;
    c->fd = -1;
    snprintf(c->key, sizeof(c->key), "%s", key);
    pool_used[slot] = 1;

    if (transport == MB_TCP || transport == MB_SERIAL_GW) {
        const char* host = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "host"));
        cJSON* port_j = cJSON_GetObjectItem(cfg, "port");
        snprintf(c->host, sizeof(c->host), "%s", host ? host : "127.0.0.1");
        c->port = port_j ? (int)port_j->valuedouble : 502;
    } else {
        const char* dev = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "device"));
        cJSON* baud_j = cJSON_GetObjectItem(cfg, "baud");
        const char* parity = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "parity"));
        snprintf(c->device, sizeof(c->device), "%s", dev ? dev : "/dev/ttyS1");
        c->baud   = baud_j ? (int)baud_j->valuedouble : 9600;
        c->parity = (parity && parity[0]) ? parity[0] : 'N';
    }

    pthread_mutex_unlock(&pool_mutex);
    return c;
}

void modbus_pool_invalidate(mb_ctx_t* ctx) {
    if (!ctx) return;
    pthread_mutex_lock(&pool_mutex);
    if (ctx->fd >= 0) { close(ctx->fd); ctx->fd = -1; }
    ctx->connected = 0;
    pthread_mutex_unlock(&pool_mutex);
}

void modbus_pool_release_all(void) {
    pthread_mutex_lock(&pool_mutex);
    for (int i = 0; i < POOL_MAX; i++) {
        if (pool_used[i] && pool[i].fd >= 0) close(pool[i].fd);
        pool_used[i] = 0;
    }
    pthread_mutex_unlock(&pool_mutex);
}
