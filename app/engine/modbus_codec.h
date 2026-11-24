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

/* Per request (Modbus frame limits) */
#define MB_MAX_READ_REGS   125   /* FC03/FC04 */
#define MB_MAX_READ_BITS  2000   /* FC01/FC02 */
#define MB_MAX_WRITE_REGS  123   /* FC16 */
#define MB_MAX_WRITE_BITS 1968   /* FC15 */
#define MB_MAX_STRING_REGS 125
/* Larger reads and writes are split into several requests, up to these. */
#define MB_MAX_SPAN       2000   /* registers or bits from the first to the last address read */
#define MB_MAX_REQUESTS     32   /* requests for one poll or one write action */

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

/* Character encoding of string registers. */
typedef enum {
    MB_CS_INVALID = -1,
    MB_CS_UTF8 = 0,
    MB_CS_ASCII,
    MB_CS_LATIN1,    /* ISO-8859-1 */
    MB_CS_LATIN2,    /* ISO-8859-2 */
    MB_CS_CP1250,    /* Windows-1250 */
    MB_CS_CP1252     /* Windows-1252 */
} mb_charset_t;

/* NULL or "" give the default (uint16, ABCD, utf-8). Encodings are matched
 * without case, and also as utf8, latin1, latin2, cp1250 and cp1252. */
mb_type_t    mb_type_parse(const char* s);
mb_order_t   mb_order_parse(const char* s);
mb_charset_t mb_charset_parse(const char* s);
const char*  mb_type_name(mb_type_t t);
const char*  mb_charset_name(mb_charset_t cs);

/* Registers a value occupies; length is the string length in registers. */
int mb_type_regs(mb_type_t t, int length);

/* Registers to a number (raw, before scaling). float32 is rounded to 7
 * significant digits so 21.3 doesn't read back as 21.299999237. */
double mb_decode_number(const uint16_t* regs, mb_type_t t, mb_order_t o);

/* Registers to UTF-8 text: bytes in register order, low byte first for
 * BADC/DCBA, converted from cs. Stops at the first NUL; trailing spaces are
 * dropped, and control characters and bytes that aren't valid in cs become
 * '?'. */
void mb_decode_string(const uint16_t* regs, int nregs, mb_order_t o, mb_charset_t cs,
                      char* out, size_t out_size);

/* UTF-8 text to bytes in cs. Writes up to out_size bytes and returns the
 * number of bytes the whole text needs. Characters cs can't represent, and
 * invalid UTF-8, become '?'; *bad counts them and *first_bad (if not NULL)
 * gets the first one's code point (0xFFFD for invalid UTF-8). */
size_t mb_text_encode(const char* s, mb_charset_t cs, uint8_t* out, size_t out_size,
                      int* bad, uint32_t* first_bad);

/* Number to registers. Integer types round to the nearest integer. Returns
 * -1 if the value is NaN or out of range for the type. */
int mb_encode_number(double v, mb_type_t t, mb_order_t o, uint16_t* regs);

/* UTF-8 text to nregs registers in cs, padded with NUL to the end. Returns
 * -1 if the encoded text is longer than nregs * 2 bytes. *bad as for
 * mb_text_encode (may be NULL). */
int mb_encode_string(const char* s, int nregs, mb_order_t o, mb_charset_t cs,
                     uint16_t* regs, int* bad);

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
    mb_charset_t charset; /* strings */
} mb_field_t;

/* Field from a JSON object. type_key/order_key name the keys ("data_type"/
 * "word_order" at the top level of a rule step, "type"/"order" in a map
 * entry). bits: coils/discrete inputs, which have no type. Returns 0, or -1
 * with err set. */
int mb_field_parse(cJSON* obj, const char* type_key, const char* order_key,
                   int bits, mb_field_t* f, char* err, size_t err_size);

/* A number written to a numeric field: unscaled, then encoded. A uint16 also
 * takes -32768..-1 as two's complement, as 1.9.29 wrote them. Returns -1 if
 * it is out of range. */
int mb_encode_field(double v, const mb_field_t* f, uint16_t* regs);

/* One request: count registers or bits from address. */
typedef struct {
    int address;
    int count;
} mb_span_t;

/* Requests that read all fields. A request also covers the gaps between
 * fields as long as it stays within max; a field longer than max is split.
 * Returns the number of spans, or -1 if more than out_max are needed. */
int mb_plan_reads(const mb_field_t* fs, int n, int max, mb_span_t* out, int out_max);

/* Requests that write the fields, which must be sorted by address and must
 * not overlap. Adjacent fields share a request up to max registers; a field
 * is only split if it is longer than max. Returns the number of spans, or -1
 * if more than out_max are needed. */
int mb_plan_writes(const mb_field_t* fs, int n, int max, mb_span_t* out, int out_max);

/* Check a modbus_read trigger / modbus_write action as the API receives it.
 * Returns 1 if valid, else 0 with err set. */
int mb_validate_read(cJSON* trig, char* err, size_t err_size);
int mb_validate_write(cJSON* action, char* err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif /* _ENGINE_MODBUS_CODEC_H_ */
