/*
 * modbus_pool.c: self-contained Modbus TCP/RTU connection pool (no libmodbus).
 *
 * serial_gateway connects via TCP but speaks RTU framing (CRC, no MBAP), for
 * use with an Axis PortManager GenericTCPServer exposing a local RS-485 port.
 * Supports FC 0x01-0x04 (reads, blocks of registers/bits) and 0x05, 0x06,
 * 0x0F, 0x10 (single and multiple writes).
 *
 * All I/O runs on one worker thread (modbus_submit_*), never on the GLib main
 * loop: a device that doesn't answer blocks for the socket timeout.
 */

#include <glib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <syslog.h>
#include <pthread.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <termios.h>

/* The OS 13 SDK's glibc 2.42 gave cfsetispeed()/cfsetospeed() new symbol
 * versions, the only GLIBC_2.42 references, which break loading on older AXIS
 * OS. Behaviour is unchanged for standard baud rates, so pin the old versions. */
#if defined(__aarch64__)
__asm__(".symver cfsetispeed,cfsetispeed@GLIBC_2.17");
__asm__(".symver cfsetospeed,cfsetospeed@GLIBC_2.17");
#elif defined(__arm__)
__asm__(".symver cfsetispeed,cfsetispeed@GLIBC_2.4");
__asm__(".symver cfsetospeed,cfsetospeed@GLIBC_2.4");
#endif

#include "event_log.h"
#include "modbus_codec.h"
#include "modbus_pool.h"

#define LOG(fmt, args...)      syslog(LOG_INFO,    "modbus_pool: " fmt, ## args)
#define LOG_WARN(fmt, args...) EventLog_Syslog(LOG_WARNING, "modbus_pool", fmt, ## args)

#define POOL_MAX        16
#define MB_TIMEOUT_S     2   /* TCP socket timeout */
#define MB_RTU_VTIME    10   /* termios VTIME units (0.1s each) → 1s */
#define MB_RETRY_S       5   /* after a failed connect, wait before trying again */
#define MB_QUEUE_MAX    64   /* queued writes; reads are one per trigger at most */

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

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
    gint64 retry_at;    /* monotonic s; no connect attempt before this */
    int    last_err;    /* errno of the failed connect */
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
    if (getaddrinfo(c->host, portstr, &hints, &res) != 0 || !res) { errno = EHOSTUNREACH; return -1; }
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    struct timeval tv = { MB_TIMEOUT_S, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        int e = errno;
        if (e == EINPROGRESS || e == EAGAIN) e = ETIMEDOUT; /* SO_SNDTIMEO expired */
        close(fd); freeaddrinfo(res);
        errno = e;
        return -1;
    }
    freeaddrinfo(res);
    c->fd = fd; c->connected = 1;
    return 0;
}

static int rtu_connect(struct mb_ctx* c) {
    int fd = open(c->device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
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
    gint64 now = g_get_monotonic_time() / G_USEC_PER_SEC;
    if (c->retry_at && now < c->retry_at) {
        errno = c->last_err;
        return -1;
    }
    errno = 0;
    int rc = (c->transport == MB_RTU) ? rtu_connect(c) : tcp_connect(c);
    if (rc != 0) {
        c->last_err = errno ? errno : EIO;
        c->retry_at = now + MB_RETRY_S;
        errno = c->last_err;
    } else {
        c->retry_at = 0;
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Transport-level request/response                                    */
/* ------------------------------------------------------------------ */

const char* mb_err_kind_name(mb_err_kind_t kind) {
    switch (kind) {
        case MB_ERR_NONE:       return "none";
        case MB_ERR_CONNECTION: return "connection";
        case MB_ERR_TIMEOUT:    return "timeout";
        case MB_ERR_CRC:        return "crc";
        case MB_ERR_EXCEPTION:  return "exception";
        case MB_ERR_INVALID:    return "invalid_response";
        case MB_ERR_CONFIG:     return "config";
    }
    return "unknown";
}

void mb_err_describe(const mb_err_t* err, char* buf, size_t size) {
    switch (err->kind) {
        case MB_ERR_NONE:
            snprintf(buf, size, "ok");
            break;
        case MB_ERR_CONNECTION:
            snprintf(buf, size, "connection failed: %s", strerror(err->sys_errno ? err->sys_errno : EIO));
            break;
        case MB_ERR_TIMEOUT:
            snprintf(buf, size, "no response (timeout)");
            break;
        case MB_ERR_CRC:
            snprintf(buf, size, "CRC error in response");
            break;
        case MB_ERR_EXCEPTION:
            snprintf(buf, size, "exception %02d %s", err->exception, mb_exception_name(err->exception));
            break;
        case MB_ERR_INVALID:
            snprintf(buf, size, "invalid response");
            break;
        case MB_ERR_CONFIG:
            snprintf(buf, size, "no connection available (pool full)");
            break;
    }
}

static int fail(mb_err_t* err, mb_err_kind_t kind, int e) {
    err->kind = kind;
    err->sys_errno = e;
    return -1;
}

/* recv()/read() error or short read: closed connection or timeout. */
static int io_fail(mb_err_t* err, ssize_t n) {
    if (n == 0) return fail(err, MB_ERR_CONNECTION, ECONNRESET);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != ETIMEDOUT && errno != EINTR)
        return fail(err, MB_ERR_CONNECTION, errno);
    return fail(err, MB_ERR_TIMEOUT, ETIMEDOUT);
}

/* Send PDU over TCP (wraps in MBAP header), receive response PDU.
 * resp_pdu must be at least 256 bytes. Returns 0 on success. */
static int tcp_request(struct mb_ctx* c, int slave_id,
                       const uint8_t* pdu, int pdu_len,
                       uint8_t* resp_pdu, int* resp_pdu_len, mb_err_t* err) {
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
    if (send(c->fd, frame, total, MSG_NOSIGNAL) != total) return fail(err, MB_ERR_CONNECTION, errno ? errno : EIO);
    /* receive MBAP header (6 bytes) */
    uint8_t hdr[6];
    ssize_t n = recv(c->fd, hdr, 6, MSG_WAITALL);
    if (n != 6) return io_fail(err, n);
    uint16_t rxid = ((uint16_t)hdr[0] << 8) | hdr[1];
    uint16_t rlen = ((uint16_t)hdr[4] << 8) | hdr[5];
    if (rxid != txid || hdr[2] || hdr[3] || rlen < 2 || rlen > 254)
        return fail(err, MB_ERR_INVALID, EBADMSG);
    /* receive unit_id (1) + PDU (rlen-1) */
    uint8_t rbuf[256];
    n = recv(c->fd, rbuf, rlen, MSG_WAITALL);
    if (n != (ssize_t)rlen) return io_fail(err, n);
    *resp_pdu_len = rlen - 1;
    memcpy(resp_pdu, rbuf + 1, *resp_pdu_len);
    return 0;
}

/* Length of the RTU response frame (addr .. CRC) from its first 3 bytes. */
static int rtu_expected_len(const uint8_t* rbuf) {
    if (rbuf[1] & 0x80) return 5;                /* addr + fc + code + CRC */
    if (rbuf[1] <= 0x04) return 3 + rbuf[2] + 2;  /* addr + fc + count + data + CRC */
    return 8;                                     /* write echo (5, 6, 15, 16) */
}

/* Check an RTU response frame and copy out its PDU. */
static int rtu_finish(int slave_id, const uint8_t* rbuf, int got,
                      uint8_t* resp_pdu, int* resp_pdu_len, mb_err_t* err) {
    if (got == 0) return fail(err, MB_ERR_TIMEOUT, ETIMEDOUT);
    if (got < 5 || got < rtu_expected_len(rbuf)) return fail(err, MB_ERR_TIMEOUT, ETIMEDOUT);
    got = rtu_expected_len(rbuf);
    uint16_t rx_crc = ((uint16_t)rbuf[got - 1] << 8) | rbuf[got - 2];
    if (crc16(rbuf, got - 2) != rx_crc) return fail(err, MB_ERR_CRC, EBADMSG);
    if (rbuf[0] != (uint8_t)slave_id)   return fail(err, MB_ERR_INVALID, EBADMSG);
    *resp_pdu_len = got - 3;   /* strip addr + 2-byte CRC */
    memcpy(resp_pdu, rbuf + 1, *resp_pdu_len);
    return 0;
}

static int rtu_frame(int slave_id, const uint8_t* pdu, int pdu_len, uint8_t* frame) {
    frame[0] = (uint8_t)slave_id;
    memcpy(frame + 1, pdu, pdu_len);
    uint16_t crc = crc16(frame, 1 + pdu_len);
    frame[1 + pdu_len]     =  crc & 0xFF;
    frame[1 + pdu_len + 1] = (crc >> 8) & 0xFF;
    return 1 + pdu_len + 2;
}

/* Send PDU over RTU serial (adds addr + CRC), receive and validate response PDU.
 * resp_pdu must be at least 256 bytes. Returns 0 on success. */
static int rtu_request(struct mb_ctx* c, int slave_id,
                       const uint8_t* pdu, int pdu_len,
                       uint8_t* resp_pdu, int* resp_pdu_len, mb_err_t* err) {
    uint8_t frame[260];
    int flen = rtu_frame(slave_id, pdu, pdu_len, frame);
    tcflush(c->fd, TCIOFLUSH);
    if (write(c->fd, frame, flen) != flen) return fail(err, MB_ERR_CONNECTION, errno ? errno : EIO);
    /* read response with timeout via VTIME */
    uint8_t rbuf[256];
    int got = 0;
    while (got < (int)sizeof(rbuf)) {
        int n = (int)read(c->fd, rbuf + got, sizeof(rbuf) - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += n;
        if (got >= 5 && got >= rtu_expected_len(rbuf)) break;
    }
    return rtu_finish(slave_id, rbuf, got, resp_pdu, resp_pdu_len, err);
}

/* Send RTU-framed PDU over a raw TCP socket (serial_gateway mode).
 * Same framing as rtu_request() but no tcflush; uses send()/recv(). */
static int sgw_request(struct mb_ctx* c, int slave_id,
                       const uint8_t* pdu, int pdu_len,
                       uint8_t* resp_pdu, int* resp_pdu_len, mb_err_t* err) {
    uint8_t frame[260];
    int flen = rtu_frame(slave_id, pdu, pdu_len, frame);
    if (send(c->fd, frame, flen, MSG_NOSIGNAL) != flen) return fail(err, MB_ERR_CONNECTION, errno ? errno : EIO);
    uint8_t rbuf[256];
    int got = 0;
    while (got < (int)sizeof(rbuf)) {
        ssize_t n = recv(c->fd, rbuf + got, sizeof(rbuf) - got, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n == 0 && got == 0) return fail(err, MB_ERR_CONNECTION, ECONNRESET);
        if (n <= 0) break;
        got += (int)n;
        if (got >= 5 && got >= rtu_expected_len(rbuf)) break;
    }
    return rtu_finish(slave_id, rbuf, got, resp_pdu, resp_pdu_len, err);
}

/* A pooled TCP connection the device has closed, e.g. after being idle.
 * Bytes waiting on an open connection are late answers to earlier requests
 * and are dropped. */
static int peer_closed(int fd) {
    for (;;) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, 0) <= 0) return 0;
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) return 1;
        uint8_t buf[256];
        ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n == 0) return 1;
        if (n < 0) return errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR;
    }
}

static void disconnect(struct mb_ctx* c) {
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    c->connected = 0;
}

static int send_request(struct mb_ctx* c, int slave_id, const uint8_t* pdu, int pdu_len,
                        uint8_t* resp, int* rlen, mb_err_t* err) {
    memset(err, 0, sizeof(*err));
    if (c->transport == MB_TCP)       return tcp_request(c, slave_id, pdu, pdu_len, resp, rlen, err);
    if (c->transport == MB_SERIAL_GW) return sgw_request(c, slave_id, pdu, pdu_len, resp, rlen, err);
    return rtu_request(c, slave_id, pdu, pdu_len, resp, rlen, err);
}

/* Connect if needed, send the request and check the response is for this
 * function: an exception gives MB_ERR_EXCEPTION. On a transport error the
 * connection is closed so the next request starts clean. A request on a
 * reused TCP connection that turns out to be closed is sent once more on a
 * new connection. */
static int transact(struct mb_ctx* c, int slave_id, const uint8_t* pdu, int pdu_len,
                    uint8_t* resp, int* rlen, mb_err_t* err) {
    memset(err, 0, sizeof(*err));
    pthread_mutex_lock(&pool_mutex);
    int tcp = c->transport != MB_RTU;
    int reused = c->connected && c->fd >= 0;
    if (reused && tcp && peer_closed(c->fd)) {
        LOG("%s: connection closed by the device, reconnecting", c->key);
        disconnect(c);
        reused = 0;
    }
    if (ensure_connected(c) != 0) {
        int e = errno;
        pthread_mutex_unlock(&pool_mutex);
        return fail(err, MB_ERR_CONNECTION, e ? e : EIO);
    }
    int rc = send_request(c, slave_id, pdu, pdu_len, resp, rlen, err);
    if (rc != 0 && reused && tcp && err->kind == MB_ERR_CONNECTION) {
        LOG("%s: connection lost, sending again on a new connection", c->key);
        disconnect(c);
        if (ensure_connected(c) == 0)
            rc = send_request(c, slave_id, pdu, pdu_len, resp, rlen, err);
        else
            fail(err, MB_ERR_CONNECTION, errno ? errno : EIO);
    }
    if (rc != 0 && (tcp || err->kind == MB_ERR_CONNECTION))
        disconnect(c);   /* a late answer would be read as the response to the next request */
    pthread_mutex_unlock(&pool_mutex);
    if (rc != 0) return -1;
    if (*rlen < 1) return fail(err, MB_ERR_INVALID, EBADMSG);
    if (resp[0] == (pdu[0] | 0x80)) {
        if (*rlen < 2) return fail(err, MB_ERR_INVALID, EBADMSG);
        err->kind = MB_ERR_EXCEPTION;
        err->exception = resp[1];
        return -1;
    }
    if (resp[0] != pdu[0]) return fail(err, MB_ERR_INVALID, EBADMSG);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

static void put16(uint8_t* p, int v) {
    p[0] = (uint8_t)((v >> 8) & 0xFF);
    p[1] = (uint8_t)(v & 0xFF);
}

static int get16(const uint8_t* p) {
    return ((int)p[0] << 8) | p[1];
}

static int read_request(mb_ctx_t* ctx, int slave_id, int fc, int address, int count,
                        int data_bytes, uint8_t* resp, mb_err_t* err) {
    uint8_t pdu[5] = { (uint8_t)fc };
    put16(pdu + 1, address);
    put16(pdu + 3, count);
    int rlen = 0;
    if (transact(ctx, slave_id, pdu, 5, resp, &rlen, err) != 0) return -1;
    if (rlen != 2 + data_bytes || resp[1] != data_bytes) return fail(err, MB_ERR_INVALID, EBADMSG);
    return 0;
}

int mb_read_registers(mb_ctx_t* ctx, int slave_id, int fc, int address, int count,
                      uint16_t* out, mb_err_t* err) {
    memset(err, 0, sizeof(*err));
    if (!ctx || !out || count < 1 || count > 125 || address < 0 || address + count > 65536)
        return fail(err, MB_ERR_CONFIG, EINVAL);
    uint8_t resp[256];
    if (read_request(ctx, slave_id, fc, address, count, count * 2, resp, err) != 0) return -1;
    for (int i = 0; i < count; i++) out[i] = (uint16_t)get16(resp + 2 + i * 2);
    return 0;
}

int mb_read_bits(mb_ctx_t* ctx, int slave_id, int fc, int address, int count,
                 uint16_t* out, mb_err_t* err) {
    memset(err, 0, sizeof(*err));
    if (!ctx || !out || count < 1 || count > 2000 || address < 0 || address + count > 65536)
        return fail(err, MB_ERR_CONFIG, EINVAL);
    uint8_t resp[256];
    if (read_request(ctx, slave_id, fc, address, count, (count + 7) / 8, resp, err) != 0) return -1;
    for (int i = 0; i < count; i++) out[i] = (resp[2 + i / 8] >> (i % 8)) & 1;
    return 0;
}

/* Write responses echo the address and the value (fc 5/6) or count (15/16). */
static int write_request(mb_ctx_t* ctx, int slave_id, const uint8_t* pdu, int pdu_len, mb_err_t* err) {
    uint8_t resp[256];
    int rlen = 0;
    if (transact(ctx, slave_id, pdu, pdu_len, resp, &rlen, err) != 0) return -1;
    if (rlen != 5 || memcmp(resp, pdu, 5) != 0) return fail(err, MB_ERR_INVALID, EBADMSG);
    return 0;
}

int mb_write_registers(mb_ctx_t* ctx, int slave_id, int fc, int address, int count,
                       const uint16_t* values, mb_err_t* err) {
    memset(err, 0, sizeof(*err));
    if (!ctx || !values || count < 1 || address < 0 || address + count > 65536 ||
        (fc == 0x06 && count != 1) || (fc == 0x10 && count > 123) || (fc != 0x06 && fc != 0x10))
        return fail(err, MB_ERR_CONFIG, EINVAL);
    uint8_t pdu[256];
    pdu[0] = (uint8_t)fc;
    put16(pdu + 1, address);
    if (fc == 0x06) {
        put16(pdu + 3, values[0]);
        return write_request(ctx, slave_id, pdu, 5, err);
    }
    put16(pdu + 3, count);
    pdu[5] = (uint8_t)(count * 2);
    for (int i = 0; i < count; i++) put16(pdu + 6 + i * 2, values[i]);
    return write_request(ctx, slave_id, pdu, 6 + count * 2, err);
}

int mb_write_bits(mb_ctx_t* ctx, int slave_id, int fc, int address, int count,
                  const uint16_t* values, mb_err_t* err) {
    memset(err, 0, sizeof(*err));
    if (!ctx || !values || count < 1 || address < 0 || address + count > 65536 ||
        (fc == 0x05 && count != 1) || (fc == 0x0F && count > 1968) || (fc != 0x05 && fc != 0x0F))
        return fail(err, MB_ERR_CONFIG, EINVAL);
    uint8_t pdu[256];
    pdu[0] = (uint8_t)fc;
    put16(pdu + 1, address);
    if (fc == 0x05) {
        put16(pdu + 3, values[0] ? 0xFF00 : 0x0000);
        return write_request(ctx, slave_id, pdu, 5, err);
    }
    int nbytes = (count + 7) / 8;
    put16(pdu + 3, count);
    pdu[5] = (uint8_t)nbytes;
    memset(pdu + 6, 0, nbytes);
    for (int i = 0; i < count; i++)
        if (values[i]) pdu[6 + i / 8] |= (uint8_t)(1 << (i % 8));
    return write_request(ctx, slave_id, pdu, 6 + nbytes, err);
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

    for (int i = 0; i < POOL_MAX; i++) {
        if (pool_used[i] && strcmp(pool[i].key, key) == 0) {
            struct mb_ctx* c = &pool[i];
            pthread_mutex_unlock(&pool_mutex);
            return c;
        }
    }

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

/* ------------------------------------------------------------------ */
/* Worker thread                                                       */
/* ------------------------------------------------------------------ */

typedef enum { JOB_READ, JOB_WRITE, JOB_STOP } mb_job_kind_t;

/* Someone waiting for a write's result (modbus_write_wait). Freed by
 * whichever of the waiter and the worker lets go last. */
typedef struct {
    GMutex      lock;
    GCond       cond;
    int         refs;
    int         done;      /* 1 = status set, -1 = dropped */
    mb_status_t status;
} mb_waiter_t;

typedef struct {
    mb_job_kind_t    kind;
    cJSON*           cfg;
    int              slave_id;
    int              function;
    int              address;
    int              count;
    mb_span_t        spans[MB_MAX_REQUESTS];   /* reads: the requests */
    int              nspans;
    mb_waiter_t*     waiter;    /* writes only, may be NULL */
    mb_read_done_fn  done;      /* reads only */
    mb_read_result_t res;       /* status for both; data = values to write */
} mb_job_t;

static GAsyncQueue*    jobs        = NULL;
static GThread*        worker      = NULL;
static int             pending_writes = 0;   /* queued or running, under worker_lock */
static pthread_mutex_t worker_lock = PTHREAD_MUTEX_INITIALIZER;
static mb_status_fn    status_fn   = NULL;   /* main loop only */

void modbus_set_status_callback(mb_status_fn fn) {
    status_fn = fn;
}

/* Hand the result to the waiter (status NULL: the job was dropped) and let
 * go of it. */
static void waiter_finish(mb_waiter_t* w, const mb_status_t* status) {
    g_mutex_lock(&w->lock);
    if (!w->done) {
        if (status) w->status = *status;
        w->done = status ? 1 : -1;
        g_cond_signal(&w->cond);
    }
    int last = --w->refs == 0;
    g_mutex_unlock(&w->lock);
    if (last) {
        g_mutex_clear(&w->lock);
        g_cond_clear(&w->cond);
        free(w);
    }
}

static void job_free(mb_job_t* j) {
    if (j->waiter) waiter_finish(j->waiter, NULL);
    if (j->cfg) cJSON_Delete(j->cfg);
    free(j);
}

/* Main loop: report the outcome, then hand a read result to its owner. */
static gboolean deliver(gpointer data) {
    mb_job_t* j = data;
    if (status_fn) status_fn(&j->res.status);
    if (j->kind == JOB_READ && j->done) j->done(&j->res);
    job_free(j);
    return G_SOURCE_REMOVE;
}

/* "192.168.1.50:502", "192.168.1.50:4001 (RTU)" or "/dev/ttyS1", as the user
 * entered it, for the event log and the modbus_error trigger. */
static void describe_cfg(cJSON* cfg, char* buf, size_t size) {
    const char* ctype = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "connection_type"));
    if (ctype && strcmp(ctype, "rtu") == 0) {
        const char* dev = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "device"));
        snprintf(buf, size, "%s", (dev && dev[0]) ? dev : "/dev/ttyS1");
        return;
    }
    const char* host = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "host"));
    cJSON* port_j = cJSON_GetObjectItem(cfg, "port");
    int port = cJSON_IsNumber(port_j) && port_j->valuedouble > 0 ? (int)port_j->valuedouble : 502;
    snprintf(buf, size, "%s:%d%s", (host && host[0]) ? host : "127.0.0.1", port,
             (ctype && strcmp(ctype, "serial_gateway") == 0) ? " (RTU)" : "");
}

static void run_job(mb_job_t* j) {
    mb_status_t* st = &j->res.status;
    describe_cfg(j->cfg, st->device, sizeof(st->device));
    st->slave_id = j->slave_id;
    st->function = j->function;
    st->address  = j->address;
    st->count    = j->count;
    st->write    = j->kind == JOB_WRITE;

    mb_ctx_t* ctx = modbus_pool_get(j->cfg);
    int rc;
    if (!ctx) {
        st->err.kind = MB_ERR_CONFIG;
        rc = -1;
    } else if (j->function >= 0x01 && j->function <= 0x04) {
        /* One request per span; a failure reports that span */
        rc = 0;
        for (int i = 0; i < j->nspans && rc == 0; i++) {
            const mb_span_t* sp = &j->spans[i];
            uint16_t* out = j->res.data + (sp->address - j->address);
            rc = j->function >= 0x03
                ? mb_read_registers(ctx, j->slave_id, j->function, sp->address, sp->count, out, &st->err)
                : mb_read_bits(ctx, j->slave_id, j->function, sp->address, sp->count, out, &st->err);
            if (rc != 0) { st->address = sp->address; st->count = sp->count; }
        }
    } else if (j->function == 0x06 || j->function == 0x10) {
        rc = mb_write_registers(ctx, j->slave_id, j->function, j->address, j->count, j->res.data, &st->err);
    } else {
        rc = mb_write_bits(ctx, j->slave_id, j->function, j->address, j->count, j->res.data, &st->err);
    }
    st->ok = rc == 0;
    j->res.count = rc == 0 && !st->write ? j->count : 0;
}

static gpointer worker_main(gpointer unused) {
    (void)unused;
    for (;;) {
        mb_job_t* j = g_async_queue_pop(jobs);
        if (j->kind == JOB_STOP) { job_free(j); break; }
        run_job(j);
        cJSON_Delete(j->cfg);
        j->cfg = NULL;
        if (j->waiter) {
            waiter_finish(j->waiter, &j->res.status);
            j->waiter = NULL;
        }
        if (j->kind == JOB_WRITE) {
            pthread_mutex_lock(&worker_lock);
            pending_writes--;
            pthread_mutex_unlock(&worker_lock);
        }
        g_idle_add_full(G_PRIORITY_DEFAULT, deliver, j, NULL);
    }
    return NULL;
}

/* Returns 0 if queued; the job is freed otherwise. */
static int submit(mb_job_t* j) {
    pthread_mutex_lock(&worker_lock);
    if (!jobs) {
        jobs   = g_async_queue_new();
        worker = g_thread_new("modbus", worker_main, NULL);
    }
    if (j->kind == JOB_WRITE) {
        if (pending_writes >= MB_QUEUE_MAX) {
            pthread_mutex_unlock(&worker_lock);
            job_free(j);
            return -1;
        }
        pending_writes++;
    }
    g_async_queue_push(jobs, j);
    pthread_mutex_unlock(&worker_lock);
    return 0;
}

void modbus_submit_read(cJSON* conn, int slave_id, const char* reg_type,
                        int address, int count, const mb_span_t* spans, int nspans,
                        unsigned tag, mb_read_done_fn done) {
    if (count < 1 || count > MB_READ_MAX || nspans > MB_MAX_REQUESTS) { cJSON_Delete(conn); return; }
    mb_job_t* j = calloc(1, sizeof(*j));
    if (!j) { cJSON_Delete(conn); return; }
    if (spans && nspans > 0) {
        memcpy(j->spans, spans, (size_t)nspans * sizeof(*spans));
        j->nspans = nspans;
    } else {
        j->spans[0].address = address;
        j->spans[0].count = count;
        j->nspans = 1;
    }
    if (!reg_type) reg_type = "holding";
    j->kind     = JOB_READ;
    j->cfg      = conn;
    j->slave_id = slave_id;
    j->function = strcmp(reg_type, "input") == 0    ? 0x04
                : strcmp(reg_type, "coil") == 0     ? 0x01
                : strcmp(reg_type, "discrete") == 0 ? 0x02 : 0x03;
    j->address  = address;
    j->count    = count;
    j->done     = done;
    j->res.tag  = tag;
    submit(j);
}

/* On failure the job's reference to waiter is released too. */
static int submit_write(cJSON* conn, int slave_id, int function, int address,
                        int count, const uint16_t* values, mb_waiter_t* waiter) {
    mb_job_t* j = count >= 1 && count <= MB_READ_MAX ? calloc(1, sizeof(*j)) : NULL;
    if (!j) {
        if (waiter) waiter_finish(waiter, NULL);
        return -1;
    }
    j->waiter   = waiter;
    j->kind     = JOB_WRITE;
    j->cfg      = cJSON_Duplicate(conn, 1);
    j->slave_id = slave_id;
    j->function = function;
    j->address  = address;
    j->count    = count;
    memcpy(j->res.data, values, (size_t)count * sizeof(uint16_t));
    if (!j->cfg) { job_free(j); return -1; }
    if (submit(j) != 0) {
        LOG_WARN("modbus_write: queue full (%d writes waiting), write dropped", MB_QUEUE_MAX);
        return -1;
    }
    return 0;
}

int modbus_submit_write(cJSON* conn, int slave_id, int function, int address,
                        int count, const uint16_t* values) {
    return submit_write(conn, slave_id, function, address, count, values, NULL);
}

int modbus_write_wait(cJSON* conn, int slave_id, int function, int address, int count,
                      const uint16_t* values, int timeout_ms, mb_status_t* status) {
    mb_waiter_t* w = calloc(1, sizeof(*w));
    if (!w) return -1;
    g_mutex_init(&w->lock);
    g_cond_init(&w->cond);
    w->refs = 2;
    if (submit_write(conn, slave_id, function, address, count, values, w) != 0) {
        waiter_finish(w, NULL);
        return -1;
    }
    gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
    g_mutex_lock(&w->lock);
    while (!w->done && g_cond_wait_until(&w->cond, &w->lock, deadline)) {}
    int done = w->done;
    if (done == 1 && status) *status = w->status;
    g_mutex_unlock(&w->lock);
    waiter_finish(w, NULL);
    return done == 1 ? 0 : done == 0 ? -2 : -1;
}

void modbus_worker_stop(void) {
    pthread_mutex_lock(&worker_lock);
    GAsyncQueue* q = jobs;
    GThread*     t = worker;
    if (q) {
        mb_job_t* j;
        while ((j = g_async_queue_try_pop(q))) job_free(j);
        mb_job_t* stop = calloc(1, sizeof(*stop));
        if (stop) {
            stop->kind = JOB_STOP;
            g_async_queue_push(q, stop);
        } else {
            t = NULL;   /* can't signal the worker; don't wait for it */
        }
    }
    pthread_mutex_unlock(&worker_lock);
    if (t) g_thread_join(t);
    pthread_mutex_lock(&worker_lock);
    if (q) g_async_queue_unref(q);
    jobs = NULL;
    worker = NULL;
    pending_writes = 0;
    pthread_mutex_unlock(&worker_lock);
    modbus_pool_release_all();
}
