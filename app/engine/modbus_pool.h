#ifndef _ENGINE_MODBUS_POOL_H_
#define _ENGINE_MODBUS_POOL_H_

#include <stdint.h>
#include "../cJSON.h"

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

/* Read one 16-bit register (fc 0x03 holding, 0x04 input). Returns 0 on success, -1 on error. */
int mb_read_register(mb_ctx_t* ctx, int slave_id, int fc, int address, uint16_t* out);

/* Read one coil or discrete input (fc 0x01 coil, 0x02 input). Returns 0 on success, -1 on error. */
int mb_read_bit(mb_ctx_t* ctx, int slave_id, int fc, int address, uint8_t* out);

/* Write one 16-bit holding register (FC06). Returns 0 on success, -1 on error. */
int mb_write_register(mb_ctx_t* ctx, int slave_id, int address, uint16_t value);

/* Write one coil (FC05). value: 0 = off, non-zero = on. Returns 0 on success, -1 on error. */
int mb_write_bit(mb_ctx_t* ctx, int slave_id, int address, int value);

/*
 * Worker thread. A failing connect or read can block for seconds, so all
 * Modbus I/O runs on one worker thread instead of the GLib main loop. Jobs run
 * in the order they were queued.
 */

typedef struct {
    unsigned tag;    /* the caller's tag, returned unchanged */
    int      ok;     /* 1 = read succeeded */
    double   value;  /* register value, or 0/1 for coils and discrete inputs */
    int      err;    /* errno on failure */
} mb_read_result_t;

typedef void (*mb_read_done_fn)(const mb_read_result_t* result);

/* Queue a read of one register or bit. conn is the connection cfg (see above)
 * and is taken over by the worker. reg_type: "holding", "input", "coil" or
 * "discrete". done() is called on the GLib main loop. */
void modbus_submit_read(cJSON* conn, int slave_id, const char* reg_type,
                        int address, unsigned tag, mb_read_done_fn done);

/* Queue a modbus_write action (copied). Failures are logged. */
void modbus_submit_write(cJSON* action_cfg);

/* Stop the worker (pending jobs are dropped) and close all connections. */
void modbus_worker_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* _ENGINE_MODBUS_POOL_H_ */
