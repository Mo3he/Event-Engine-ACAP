#ifndef _ENGINE_MODBUS_POOL_H_
#define _ENGINE_MODBUS_POOL_H_

#include <stdint.h>
#include "../cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Modbus connection pool — no external library dependency.
 * Implements Modbus TCP and Modbus RTU (RS-485) using raw
 * POSIX sockets and termios respectively.
 *
 * cfg JSON fields:
 *   connection_type  "tcp" | "rtu"
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

/*
 * Read one 16-bit register.
 *   fc: 0x03 = holding register, 0x04 = input register
 * Returns 0 on success, -1 on error.
 */
int mb_read_register(mb_ctx_t* ctx, int slave_id, int fc, int address, uint16_t* out);

/*
 * Read one coil or discrete input bit.
 *   fc: 0x01 = coil, 0x02 = discrete input
 * Returns 0 on success, -1 on error.
 */
int mb_read_bit(mb_ctx_t* ctx, int slave_id, int fc, int address, uint8_t* out);

/* Write one 16-bit holding register (FC06). Returns 0 on success, -1 on error. */
int mb_write_register(mb_ctx_t* ctx, int slave_id, int address, uint16_t value);

/* Write one coil (FC05). value: 0 = off, non-zero = on. Returns 0 on success, -1 on error. */
int mb_write_bit(mb_ctx_t* ctx, int slave_id, int address, int value);

#ifdef __cplusplus
}
#endif

#endif /* _ENGINE_MODBUS_POOL_H_ */
