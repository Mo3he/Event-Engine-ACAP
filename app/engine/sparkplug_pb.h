#ifndef _ENGINE_SPARKPLUG_PB_H_
#define _ENGINE_SPARKPLUG_PB_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Minimal encoder/decoder for the Sparkplug B protobuf payload
 * (org.eclipse.tahu.protobuf.Payload).
 *
 * Only the subset Event Engine needs is implemented: Payload.timestamp,
 * Payload.seq, Payload.metrics, and scalar Metric values. DataSet, Template,
 * PropertySet and MetaData are not encoded and are skipped when decoding.
 */

/* Tahu DataType enum values — these are wire constants, do not renumber. */
typedef enum {
    SPB_DT_UNKNOWN  = 0,
    SPB_DT_INT8     = 1,
    SPB_DT_INT16    = 2,
    SPB_DT_INT32    = 3,
    SPB_DT_INT64    = 4,
    SPB_DT_UINT8    = 5,
    SPB_DT_UINT16   = 6,
    SPB_DT_UINT32   = 7,
    SPB_DT_UINT64   = 8,
    SPB_DT_FLOAT    = 9,
    SPB_DT_DOUBLE   = 10,
    SPB_DT_BOOLEAN  = 11,
    SPB_DT_STRING   = 12,
    SPB_DT_DATETIME = 13,
    SPB_DT_TEXT     = 14,
    SPB_DT_UUID     = 15
} SPB_DataType;

/* Canonical Sparkplug names ("Int32", "Boolean", ...) used in the UI and config */
const char* SPB_DataType_Name(int datatype);
int         SPB_DataType_From_Name(const char* name);

typedef struct {
    const char* name;          /* NULL or "" publishes by alias only (DATA messages) */
    uint64_t    alias;
    int         has_alias;
    uint64_t    timestamp_ms;  /* 0 omits the field */
    int         datatype;      /* SPB_DataType */
    int         is_null;
    int         is_historical;

    int64_t     i64;           /* integer, boolean and datetime types */
    double      dbl;           /* SPB_DT_FLOAT and SPB_DT_DOUBLE */
    const char* str;           /* SPB_DT_STRING, SPB_DT_TEXT, SPB_DT_UUID (not owned) */
} SPB_Metric;

/* Parse text into the value field matching m->datatype. Booleans accept
 * "true"/"1"/"on"/"yes" (case-insensitive). For string types m->str is set to
 * text itself, so text must outlive the metric. */
void SPB_Metric_Set_Value(SPB_Metric* m, const char* text);

typedef struct {
    uint8_t* data;
    int      len;
    int      cap;
} SPB_Buf;

void SPB_Buf_Free(SPB_Buf* b);

/* Encode a payload. Returns 1 on success and fills out (caller must SPB_Buf_Free).
 * DEATH certificates omit seq, every other message type includes it. */
int SPB_Encode(SPB_Buf* out, uint64_t timestamp_ms, int has_seq, uint64_t seq,
               const SPB_Metric* metrics, int metric_count);

/* value_str is a NUL-terminated rendering of the metric value ("true"/"false"
 * for booleans, empty for null metrics). Valid only for the duration of the call. */
typedef void (*SPB_Metric_Handler)(const char* name, uint64_t alias, int has_alias,
                                   int datatype, const char* value_str, void* user);

/* Returns the number of metrics handled, or -1 if the payload is malformed. */
int SPB_Decode(const uint8_t* data, int len, SPB_Metric_Handler handler, void* user);

#ifdef __cplusplus
}
#endif
#endif /* _ENGINE_SPARKPLUG_PB_H_ */
