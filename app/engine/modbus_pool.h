#ifndef _ENGINE_MODBUS_POOL_H_
#define _ENGINE_MODBUS_POOL_H_

#include <stddef.h>
#include <stdint.h>
#include "../cJSON.h"
#include "modbus_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Modbus connection pool: Modbus TCP and RTU (RS-485) over raw POSIX sockets
 * and termios, no external library.
 *
 * cfg JSON fields:
 *   connection_type  "tcp" | "rtu" | "serial_gateway" (RTU framing over TCP)
 *   host             TCP hostname or IP
 *   port             TCP port (default 502)
 *   device           RTU device path (e.g. "/dev/ttyS1")
 *   baud             RTU baud rate (default 9600)
 *   parity           "N" | "E" | "O" (default "N")
 */

typedef struct mb_ctx mb_ctx_t;

/* Get (or create) a pooled connection. Returns NULL on failure. */
mb_ctx_t* modbus_pool_get(cJSON* cfg);

/* Mark a connection as broken; it will be reconnected on next use. */
void modbus_pool_invalidate(mb_ctx_t* ctx);

/* Close and free all pooled connections. Call at shutdown. */
void modbus_pool_release_all(void);

typedef enum {
    MB_ERR_NONE = 0,
    MB_ERR_CONNECTION,   /* connect failed, or the connection was closed */
    MB_ERR_TIMEOUT,      /* no (complete) response in time */
    MB_ERR_CRC,          /* RTU frame with a bad CRC */
    MB_ERR_EXCEPTION,    /* the device answered with a Modbus exception */
    MB_ERR_INVALID,      /* response doesn't match the request */
    MB_ERR_CONFIG        /* no connection available (pool full) */
} mb_err_kind_t;

typedef struct {
    mb_err_kind_t kind;
    int exception;       /* exception code, for MB_ERR_EXCEPTION */
    int sys_errno;       /* for MB_ERR_CONNECTION / MB_ERR_TIMEOUT */
} mb_err_t;

/* "timeout", "connection", "crc", "exception", "invalid_response", "config" */
const char* mb_err_kind_name(mb_err_kind_t kind);
/* Human-readable, e.g. "exception 02 Illegal Data Address" */
void mb_err_describe(const mb_err_t* err, char* buf, size_t size);

/* Read count registers (fc 0x03 holding, 0x04 input, count <= 125) or bits
 * (fc 0x01 coils, 0x02 discrete inputs, count <= 2000; out gets 0/1).
 * Return 0 on success, -1 with err set. */
int mb_read_registers(mb_ctx_t* ctx, int slave_id, int fc, int address, int count,
                      uint16_t* out, mb_err_t* err);
int mb_read_bits(mb_ctx_t* ctx, int slave_id, int fc, int address, int count,
                 uint16_t* out, mb_err_t* err);

/* Write registers (fc 6: one, fc 16: up to 123) or coils (fc 5: one, fc 15:
 * up to 1968; non-zero = on). Return 0 on success, -1 with err set. */
int mb_write_registers(mb_ctx_t* ctx, int slave_id, int fc, int address, int count,
                       const uint16_t* values, mb_err_t* err);
int mb_write_bits(mb_ctx_t* ctx, int slave_id, int fc, int address, int count,
                  const uint16_t* values, mb_err_t* err);

/*
 * Worker thread. A failing connect or read can block for seconds, so all
 * Modbus I/O runs on one worker thread instead of the GLib main loop. Jobs run
 * in the order they were queued.
 */

/* Outcome of one request, reported on the GLib main loop. */
typedef struct {
    char     device[160];  /* "192.168.1.50:502", "127.0.0.1:4001 (RTU)", "/dev/ttyS1" */
    int      slave_id;
    int      function;     /* function code of the request */
    int      address;
    int      count;
    int      write;        /* 1 = write, 0 = read */
    int      ok;
    mb_err_t err;
} mb_status_t;

typedef void (*mb_status_fn)(const mb_status_t* status);

/* Called on the main loop after every request, read or write. */
void modbus_set_status_callback(mb_status_fn fn);

#define MB_READ_MAX 2000

typedef struct {
    unsigned    tag;                 /* the caller's tag, returned unchanged */
    mb_status_t status;
    int         count;
    uint16_t    data[MB_READ_MAX];   /* registers, or 0/1 per bit */
} mb_read_result_t;

typedef void (*mb_read_done_fn)(const mb_read_result_t* result);

/* Queue a read of count registers or bits from address. spans are the
 * requests (from mb_plan_reads, within address..address+count-1); the
 * registers between them are left 0. NULL reads everything in one request.
 * conn is the connection cfg (see above) and is taken over by the worker.
 * reg_type: "holding", "input", "coil" or "discrete". done() is called on the
 * GLib main loop, after the status callback. */
void modbus_submit_read(cJSON* conn, int slave_id, const char* reg_type,
                        int address, int count, const mb_span_t* spans, int nspans,
                        unsigned tag, mb_read_done_fn done);

/* Queue a write (function 5, 6, 15 or 16) of count values starting at
 * address. conn is copied. Returns 0 if queued, -1 if the queue is full. */
int modbus_submit_write(cJSON* conn, int slave_id, int function, int address,
                        int count, const uint16_t* values);

/* Queue a write like modbus_submit_write and wait up to timeout_ms for the
 * device's answer, for testing an action. Not for the GLib main loop thread
 * if it can wait long. The status callback still runs. Returns 0 with
 * *status set, -1 if the write was not queued or was dropped, -2 on timeout. */
int modbus_write_wait(cJSON* conn, int slave_id, int function, int address, int count,
                      const uint16_t* values, int timeout_ms, mb_status_t* status);

/* Stop the worker (pending jobs are dropped) and close all connections. */
void modbus_worker_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* _ENGINE_MODBUS_POOL_H_ */
