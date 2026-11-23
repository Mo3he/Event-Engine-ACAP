#ifndef _ENGINE_MODBUS_CODEC_H_
#define _ENGINE_MODBUS_CODEC_H_

#include <stddef.h>
#include <stdint.h>
#include "../cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Modbus value encoding, independent of the transport (no GLib): data types,
 * word/byte order, scaling, strings, exception names and the validation of
 * the modbus_read map and modbus_write values used by the API.
 */

#define MB_MAX_READ_REGS   125   /* FC03/FC04 */
#define MB_MAX_READ_BITS  2000   /* FC01/FC02 */
#define MB_MAX_WRITE_REGS  123   /* FC16 */
#define MB_MAX_WRITE_BITS 1968   /* FC15 */
#define MB_MAX_STRING_REGS 125

typedef enum {
    MB_T_INVALID = -1,
    MB_T_UINT16 = 0,
    MB_T_INT16,
    MB_T_UINT32,
    MB_T_INT32,
    MB_T_FLOAT32,
    MB_T_UINT64,
    MB_T_INT64,
    MB_T_FLOAT64,
    MB_T_STRING
} mb_type_t;

/* Order of the bytes of a value, A = most significant, as sent on the wire. */
typedef enum {
    MB_O_INVALID = -1,
    MB_O_ABCD = 0,   /* big-endian */
    MB_O_CDAB,       /* word swap */
    MB_O_BADC,       /* byte swap within each register */
    MB_O_DCBA        /* little-endian */
} mb_order_t;

/* NULL or "" give the default (uint16, ABCD). */
mb_type_t   mb_type_parse(const char* s);
mb_order_t  mb_order_parse(const char* s);
const char* mb_type_name(mb_type_t t);

/* Registers a value occupies; length is the string length in registers. */
int mb_type_regs(mb_type_t t, int length);

/* Registers to a number (raw, before scaling). float32 is rounded to 7
 * significant digits so 21.3 doesn't read back as 21.299999237. */
double mb_decode_number(const uint16_t* regs, mb_type_t t, mb_order_t o);

/* Registers to text: bytes in register order, low byte first for BADC/DCBA.
 * Stops at the first NUL; trailing spaces are dropped and bytes outside
 * printable ASCII become '?'. */
void mb_decode_string(const uint16_t* regs, int nregs, mb_order_t o, char* out, size_t out_size);

/* Number to registers. Integer types round to the nearest integer. Returns
 * -1 if the value is NaN or out of range for the type. */
int mb_encode_number(double v, mb_type_t t, mb_order_t o, uint16_t* regs);

/* Text to nregs registers, padded with NUL. Returns -1 if it doesn't fit. */
int mb_encode_string(const char* s, int nregs, mb_order_t o, uint16_t* regs);

/* value = raw * scale + offset, rounded to decimals (< 0: noise of the double
 * arithmetic only, 15 significant digits). */
double mb_scale(double raw, double scale, double offset, int decimals);
/* Inverse for writes: raw = (value - offset) / scale. */
double mb_unscale(double value, double scale, double offset);

/* "Illegal Data Address" etc., or "Unknown exception". */
const char* mb_exception_name(int code);

/* Number formatted for a variable: decimals >= 0 fixed, else shortest. */
void mb_format_number(double v, int decimals, char* out, size_t out_size);

/* One decoded field: where it is and how to read it. */
typedef struct {
    int        reg;       /* absolute address */
    int        nregs;     /* registers (1 for bits) */
    mb_type_t  type;
    mb_order_t order;
    double     scale;
    double     offset;
    int        decimals;  /* -1 = not set */
    int        length;    /* string length in registers */
} mb_field_t;

/* Field from a JSON object. type_key/order_key name the keys ("data_type"/
 * "word_order" at the top level of a rule step, "type"/"order" in a map
 * entry). bits: coils/discrete inputs, which have no type. Returns 0, or -1
 * with err set. */
int mb_field_parse(cJSON* obj, const char* type_key, const char* order_key,
                   int bits, mb_field_t* f, char* err, size_t err_size);

/* Check a modbus_read trigger / modbus_write action as the API receives it.
 * Returns 1 if valid, else 0 with err set. */
int mb_validate_read(cJSON* trig, char* err, size_t err_size);
int mb_validate_write(cJSON* action, char* err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif /* _ENGINE_MODBUS_CODEC_H_ */
