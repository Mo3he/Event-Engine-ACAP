#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>

#include "sparkplug_pb.h"

/* =====================================================
 * Protobuf field numbers (Tahu Payload / Payload.Metric)
 * ===================================================== */
#define PAYLOAD_TIMESTAMP   1
#define PAYLOAD_METRICS     2
#define PAYLOAD_SEQ         3

#define METRIC_NAME          1
#define METRIC_ALIAS         2
#define METRIC_TIMESTAMP     3
#define METRIC_DATATYPE      4
#define METRIC_IS_HISTORICAL 5
#define METRIC_IS_NULL       7
#define METRIC_INT_VALUE     10
#define METRIC_LONG_VALUE    11
#define METRIC_FLOAT_VALUE   12
#define METRIC_DOUBLE_VALUE  13
#define METRIC_BOOL_VALUE    14
#define METRIC_STRING_VALUE  15
#define METRIC_BYTES_VALUE   16

#define WT_VARINT 0
#define WT_64BIT  1
#define WT_LEN    2
#define WT_32BIT  5

/* Guards against a hostile or corrupt payload allocating/looping unbounded */
#define SPB_MAX_METRICS 4096

/* =====================================================
 * Datatype names
 * ===================================================== */
static const struct { int dt; const char* name; } DT_NAMES[] = {
    { SPB_DT_INT8,     "Int8"     },
    { SPB_DT_INT16,    "Int16"    },
    { SPB_DT_INT32,    "Int32"    },
    { SPB_DT_INT64,    "Int64"    },
    { SPB_DT_UINT8,    "UInt8"    },
    { SPB_DT_UINT16,   "UInt16"   },
    { SPB_DT_UINT32,   "UInt32"   },
    { SPB_DT_UINT64,   "UInt64"   },
    { SPB_DT_FLOAT,    "Float"    },
    { SPB_DT_DOUBLE,   "Double"   },
    { SPB_DT_BOOLEAN,  "Boolean"  },
    { SPB_DT_STRING,   "String"   },
    { SPB_DT_DATETIME, "DateTime" },
    { SPB_DT_TEXT,     "Text"     },
    { SPB_DT_UUID,     "UUID"     },
    { 0, NULL }
};

const char* SPB_DataType_Name(int datatype) {
    for (int i = 0; DT_NAMES[i].name; i++)
        if (DT_NAMES[i].dt == datatype) return DT_NAMES[i].name;
    return "Unknown";
}

int SPB_DataType_From_Name(const char* name) {
    if (!name || !name[0]) return SPB_DT_UNKNOWN;
    for (int i = 0; DT_NAMES[i].name; i++)
        if (strcasecmp(DT_NAMES[i].name, name) == 0) return DT_NAMES[i].dt;
    return SPB_DT_UNKNOWN;
}

static int dt_is_signed_int(int dt) {
    return dt == SPB_DT_INT8 || dt == SPB_DT_INT16 ||
           dt == SPB_DT_INT32 || dt == SPB_DT_INT64;
}

static int dt_is_unsigned_int(int dt) {
    return dt == SPB_DT_UINT8 || dt == SPB_DT_UINT16 ||
           dt == SPB_DT_UINT32 || dt == SPB_DT_UINT64 || dt == SPB_DT_DATETIME;
}

static int dt_is_string(int dt) {
    return dt == SPB_DT_STRING || dt == SPB_DT_TEXT || dt == SPB_DT_UUID;
}

void SPB_Metric_Set_Value(SPB_Metric* m, const char* text) {
    if (!m) return;
    if (!text) text = "";

    if (dt_is_string(m->datatype)) {
        m->str = text;
        return;
    }
    if (m->datatype == SPB_DT_BOOLEAN) {
        m->i64 = (strcasecmp(text, "true") == 0 || strcasecmp(text, "1") == 0 ||
                  strcasecmp(text, "on")   == 0 || strcasecmp(text, "yes") == 0) ? 1 : 0;
        return;
    }
    if (m->datatype == SPB_DT_FLOAT || m->datatype == SPB_DT_DOUBLE) {
        m->dbl = strtod(text, NULL);
        return;
    }
    if (dt_is_unsigned_int(m->datatype)) {
        m->i64 = strpbrk(text, ".eE") ? (int64_t)(uint64_t)strtod(text, NULL)
                                      : (int64_t)strtoull(text, NULL, 10);
        return;
    }
    /* Parse integers exactly: routing them through a double would silently
     * round anything beyond 2^53. Only decimal input falls back to strtod, so
     * that "12.7" still yields a reading rather than nothing. */
    m->i64 = strpbrk(text, ".eE") ? (int64_t)strtod(text, NULL)
                                  : (int64_t)strtoll(text, NULL, 10);
}

/* =====================================================
 * Buffer
 * ===================================================== */
static int buf_reserve(SPB_Buf* b, int extra) {
    if (b->len + extra <= b->cap) return 1;
    int cap = b->cap ? b->cap : 128;
    while (cap < b->len + extra) cap *= 2;
    uint8_t* p = realloc(b->data, (size_t)cap);
    if (!p) return 0;
    b->data = p;
    b->cap  = cap;
    return 1;
}

static int buf_put(SPB_Buf* b, const void* src, int n) {
    if (n <= 0) return 1;
    if (!buf_reserve(b, n)) return 0;
    memcpy(b->data + b->len, src, (size_t)n);
    b->len += n;
    return 1;
}

static int buf_put_u8(SPB_Buf* b, uint8_t v) { return buf_put(b, &v, 1); }

void SPB_Buf_Free(SPB_Buf* b) {
    if (!b) return;
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

/* =====================================================
 * Protobuf writers
 * ===================================================== */
static int pb_varint(SPB_Buf* b, uint64_t v) {
    uint8_t tmp[10];
    int n = 0;
    do {
        uint8_t byte = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v) byte |= 0x80;
        tmp[n++] = byte;
    } while (v && n < 10);
    return buf_put(b, tmp, n);
}

static int pb_tag(SPB_Buf* b, int field, int wire_type) {
    return pb_varint(b, ((uint64_t)field << 3) | (uint64_t)wire_type);
}

static int pb_varint_field(SPB_Buf* b, int field, uint64_t v) {
    return pb_tag(b, field, WT_VARINT) && pb_varint(b, v);
}

static int pb_bool_field(SPB_Buf* b, int field, int v) {
    return pb_tag(b, field, WT_VARINT) && buf_put_u8(b, v ? 1 : 0);
}

static int pb_fixed32_field(SPB_Buf* b, int field, uint32_t v) {
    uint8_t le[4] = { (uint8_t)(v), (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return pb_tag(b, field, WT_32BIT) && buf_put(b, le, 4);
}

static int pb_fixed64_field(SPB_Buf* b, int field, uint64_t v) {
    uint8_t le[8];
    for (int i = 0; i < 8; i++) le[i] = (uint8_t)(v >> (8 * i));
    return pb_tag(b, field, WT_64BIT) && buf_put(b, le, 8);
}

static int pb_bytes_field(SPB_Buf* b, int field, const void* data, int len) {
    if (len < 0) len = 0;
    return pb_tag(b, field, WT_LEN) && pb_varint(b, (uint64_t)len) && buf_put(b, data, len);
}

static int pb_string_field(SPB_Buf* b, int field, const char* s) {
    return pb_bytes_field(b, field, s ? s : "", s ? (int)strlen(s) : 0);
}

/* =====================================================
 * Metric encoding
 * ===================================================== */
static int encode_value(SPB_Buf* b, const SPB_Metric* m) {
    switch (m->datatype) {
        /* Tahu stores small signed ints as their two's complement in a uint32 */
        case SPB_DT_INT8:   return pb_varint_field(b, METRIC_INT_VALUE, (uint64_t)(uint32_t)(uint8_t)(int8_t)m->i64);
        case SPB_DT_INT16:  return pb_varint_field(b, METRIC_INT_VALUE, (uint64_t)(uint32_t)(uint16_t)(int16_t)m->i64);
        case SPB_DT_INT32:  return pb_varint_field(b, METRIC_INT_VALUE, (uint64_t)(uint32_t)(int32_t)m->i64);
        case SPB_DT_UINT8:  return pb_varint_field(b, METRIC_INT_VALUE, (uint64_t)(uint32_t)(m->i64 & 0xFF));
        case SPB_DT_UINT16: return pb_varint_field(b, METRIC_INT_VALUE, (uint64_t)(uint32_t)(m->i64 & 0xFFFF));
        case SPB_DT_UINT32: return pb_varint_field(b, METRIC_INT_VALUE, (uint64_t)(uint32_t)m->i64);

        case SPB_DT_INT64:
        case SPB_DT_UINT64:
        case SPB_DT_DATETIME:
            return pb_varint_field(b, METRIC_LONG_VALUE, (uint64_t)m->i64);

        case SPB_DT_FLOAT: {
            float f = (float)m->dbl;
            uint32_t bits;
            memcpy(&bits, &f, sizeof(bits));
            return pb_fixed32_field(b, METRIC_FLOAT_VALUE, bits);
        }
        case SPB_DT_DOUBLE: {
            double d = m->dbl;
            uint64_t bits;
            memcpy(&bits, &d, sizeof(bits));
            return pb_fixed64_field(b, METRIC_DOUBLE_VALUE, bits);
        }
        case SPB_DT_BOOLEAN:
            return pb_bool_field(b, METRIC_BOOL_VALUE, m->i64 != 0);

        case SPB_DT_STRING:
        case SPB_DT_TEXT:
        case SPB_DT_UUID:
            return pb_string_field(b, METRIC_STRING_VALUE, m->str);

        default:
            return 1; /* unknown datatype — emit the metric with no value set */
    }
}

static int encode_metric(SPB_Buf* out, const SPB_Metric* m) {
    SPB_Buf mb = { NULL, 0, 0 };
    int ok = 1;

    if (m->name && m->name[0]) ok = ok && pb_string_field(&mb, METRIC_NAME, m->name);
    if (m->has_alias)          ok = ok && pb_varint_field(&mb, METRIC_ALIAS, m->alias);
    if (m->timestamp_ms)       ok = ok && pb_varint_field(&mb, METRIC_TIMESTAMP, m->timestamp_ms);
    ok = ok && pb_varint_field(&mb, METRIC_DATATYPE, (uint64_t)(uint32_t)m->datatype);
    if (m->is_historical)      ok = ok && pb_bool_field(&mb, METRIC_IS_HISTORICAL, 1);

    if (m->is_null) ok = ok && pb_bool_field(&mb, METRIC_IS_NULL, 1);
    else            ok = ok && encode_value(&mb, m);

    if (ok) ok = pb_bytes_field(out, PAYLOAD_METRICS, mb.data, mb.len);
    SPB_Buf_Free(&mb);
    return ok;
}

int SPB_Encode(SPB_Buf* out, uint64_t timestamp_ms, int has_seq, uint64_t seq,
               const SPB_Metric* metrics, int metric_count) {
    if (!out) return 0;
    out->data = NULL; out->len = 0; out->cap = 0;

    int ok = pb_varint_field(out, PAYLOAD_TIMESTAMP, timestamp_ms);
    for (int i = 0; ok && i < metric_count; i++)
        ok = encode_metric(out, &metrics[i]);
    if (ok && has_seq) ok = pb_varint_field(out, PAYLOAD_SEQ, seq);

    if (!ok) SPB_Buf_Free(out);
    return ok;
}

/* =====================================================
 * Protobuf readers
 * ===================================================== */
typedef struct {
    const uint8_t* data;
    int            len;
    int            pos;
} PbReader;

static int pb_read_varint(PbReader* r, uint64_t* out) {
    uint64_t v = 0;
    int shift = 0;
    while (r->pos < r->len && shift <= 63) {
        uint8_t byte = r->data[r->pos++];
        v |= (uint64_t)(byte & 0x7F) << shift;
        if (!(byte & 0x80)) { *out = v; return 1; }
        shift += 7;
    }
    return 0;
}

/* Advance past a field whose contents we do not care about */
static int pb_skip(PbReader* r, int wire_type) {
    uint64_t v;
    switch (wire_type) {
        case WT_VARINT: return pb_read_varint(r, &v);
        case WT_64BIT:  if (r->len - r->pos < 8) return 0; r->pos += 8; return 1;
        case WT_32BIT:  if (r->len - r->pos < 4) return 0; r->pos += 4; return 1;
        case WT_LEN:
            if (!pb_read_varint(r, &v)) return 0;
            if (v > (uint64_t)(r->len - r->pos)) return 0;
            r->pos += (int)v;
            return 1;
        default: return 0;
    }
}

typedef struct {
    char     name[256];
    int      has_name;
    uint64_t alias;
    int      has_alias;
    int      datatype;
    int      is_null;
    char     value[512];
    int      has_value;
} DecodedMetric;

static void render_int(DecodedMetric* dm, uint64_t raw, int is_long) {
    int dt = dm->datatype;
    if (dt_is_signed_int(dt)) {
        int64_t sv;
        if (is_long)                 sv = (int64_t)raw;
        else if (dt == SPB_DT_INT8)  sv = (int8_t)(raw & 0xFF);
        else if (dt == SPB_DT_INT16) sv = (int16_t)(raw & 0xFFFF);
        else                         sv = (int32_t)(uint32_t)raw;
        snprintf(dm->value, sizeof(dm->value), "%" PRId64, sv);
    } else {
        snprintf(dm->value, sizeof(dm->value), "%" PRIu64, raw);
    }
    dm->has_value = 1;
}

static int decode_metric(const uint8_t* data, int len, DecodedMetric* dm) {
    PbReader r = { data, len, 0 };
    memset(dm, 0, sizeof(*dm));

    /* Values can precede the datatype field, so remember the raw integer and
     * render it once the whole metric has been read. */
    uint64_t pending_int = 0;
    int      pending_kind = 0; /* 0 none, 1 int32-ish, 2 int64-ish */

    while (r.pos < r.len) {
        uint64_t key;
        if (!pb_read_varint(&r, &key)) return 0;
        int field = (int)(key >> 3);
        int wt    = (int)(key & 0x07);

        uint64_t v;
        switch (field) {
            case METRIC_NAME: {
                if (wt != WT_LEN || !pb_read_varint(&r, &v)) return 0;
                if (v > (uint64_t)(r.len - r.pos)) return 0;
                size_t n = (size_t)v;
                if (n >= sizeof(dm->name)) n = sizeof(dm->name) - 1;
                memcpy(dm->name, r.data + r.pos, n);
                dm->name[n] = '\0';
                dm->has_name = 1;
                r.pos += (int)v;
                break;
            }
            case METRIC_ALIAS:
                if (wt != WT_VARINT || !pb_read_varint(&r, &v)) return 0;
                dm->alias = v; dm->has_alias = 1;
                break;
            case METRIC_DATATYPE:
                if (wt != WT_VARINT || !pb_read_varint(&r, &v)) return 0;
                dm->datatype = (int)v;
                break;
            case METRIC_IS_NULL:
                if (wt != WT_VARINT || !pb_read_varint(&r, &v)) return 0;
                dm->is_null = v ? 1 : 0;
                break;
            case METRIC_INT_VALUE:
                if (wt != WT_VARINT || !pb_read_varint(&r, &v)) return 0;
                pending_int = v; pending_kind = 1;
                break;
            case METRIC_LONG_VALUE:
                if (wt != WT_VARINT || !pb_read_varint(&r, &v)) return 0;
                pending_int = v; pending_kind = 2;
                break;
            case METRIC_FLOAT_VALUE: {
                if (wt != WT_32BIT || r.len - r.pos < 4) return 0;
                uint32_t bits = (uint32_t)r.data[r.pos] | ((uint32_t)r.data[r.pos + 1] << 8) |
                                ((uint32_t)r.data[r.pos + 2] << 16) | ((uint32_t)r.data[r.pos + 3] << 24);
                r.pos += 4;
                float f;
                memcpy(&f, &bits, sizeof(f));
                snprintf(dm->value, sizeof(dm->value), "%.9g", (double)f);
                dm->has_value = 1;
                pending_kind = 0;
                break;
            }
            case METRIC_DOUBLE_VALUE: {
                if (wt != WT_64BIT || r.len - r.pos < 8) return 0;
                uint64_t bits = 0;
                for (int i = 0; i < 8; i++) bits |= (uint64_t)r.data[r.pos + i] << (8 * i);
                r.pos += 8;
                double d;
                memcpy(&d, &bits, sizeof(d));
                snprintf(dm->value, sizeof(dm->value), "%.17g", d);
                dm->has_value = 1;
                pending_kind = 0;
                break;
            }
            case METRIC_BOOL_VALUE:
                if (wt != WT_VARINT || !pb_read_varint(&r, &v)) return 0;
                snprintf(dm->value, sizeof(dm->value), "%s", v ? "true" : "false");
                dm->has_value = 1;
                pending_kind = 0;
                break;
            case METRIC_STRING_VALUE: {
                if (wt != WT_LEN || !pb_read_varint(&r, &v)) return 0;
                if (v > (uint64_t)(r.len - r.pos)) return 0;
                size_t n = (size_t)v;
                if (n >= sizeof(dm->value)) n = sizeof(dm->value) - 1;
                memcpy(dm->value, r.data + r.pos, n);
                dm->value[n] = '\0';
                dm->has_value = 1;
                pending_kind = 0;
                r.pos += (int)v;
                break;
            }
            default:
                if (!pb_skip(&r, wt)) return 0;
                break;
        }
    }

    if (pending_kind && !dm->has_value) render_int(dm, pending_int, pending_kind == 2);
    if (dm->is_null) { dm->value[0] = '\0'; dm->has_value = 0; }
    return 1;
}

int SPB_Decode(const uint8_t* data, int len, SPB_Metric_Handler handler, void* user) {
    if (!data || len < 0) return -1;

    PbReader r = { data, len, 0 };
    int count = 0;

    while (r.pos < r.len) {
        uint64_t key;
        if (!pb_read_varint(&r, &key)) return -1;
        int field = (int)(key >> 3);
        int wt    = (int)(key & 0x07);

        if (field == PAYLOAD_METRICS && wt == WT_LEN) {
            uint64_t mlen;
            if (!pb_read_varint(&r, &mlen)) return -1;
            if (mlen > (uint64_t)(r.len - r.pos)) return -1;

            DecodedMetric dm;
            if (!decode_metric(r.data + r.pos, (int)mlen, &dm)) return -1;
            r.pos += (int)mlen;

            if (++count > SPB_MAX_METRICS) return -1;
            if (handler)
                handler(dm.has_name ? dm.name : "", dm.alias, dm.has_alias,
                        dm.datatype, dm.value, user);
        } else if (!pb_skip(&r, wt)) {
            return -1;
        }
    }
    return count;
}
