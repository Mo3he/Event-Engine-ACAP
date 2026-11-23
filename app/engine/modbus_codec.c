/*
 * modbus_codec.c: Modbus data types, word order, scaling and strings, plus the
 * validation of Modbus rule steps. No GLib, so tools/modbus_codec_test.c can
 * build it on the host.
 */

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modbus_codec.h"

static const char* const type_names[] = {
    "uint16", "int16", "uint32", "int32", "float32",
    "uint64", "int64", "float64", "string", NULL
};
static const char* const order_names[] = { "ABCD", "CDAB", "BADC", "DCBA", NULL };

mb_type_t mb_type_parse(const char* s) {
    if (!s || !s[0]) return MB_T_UINT16;
    for (int i = 0; type_names[i]; i++)
        if (strcmp(s, type_names[i]) == 0) return (mb_type_t)i;
    return MB_T_INVALID;
}

mb_order_t mb_order_parse(const char* s) {
    if (!s || !s[0]) return MB_O_ABCD;
    for (int i = 0; order_names[i]; i++)
        if (strcmp(s, order_names[i]) == 0) return (mb_order_t)i;
    return MB_O_INVALID;
}

const char* mb_type_name(mb_type_t t) {
    return (t >= MB_T_UINT16 && t <= MB_T_STRING) ? type_names[t] : "invalid";
}

int mb_type_regs(mb_type_t t, int length) {
    switch (t) {
        case MB_T_UINT16: case MB_T_INT16:                     return 1;
        case MB_T_UINT32: case MB_T_INT32: case MB_T_FLOAT32:  return 2;
        case MB_T_UINT64: case MB_T_INT64: case MB_T_FLOAT64:  return 4;
        case MB_T_STRING: return length > 0 ? length : 1;
        default: return 1;
    }
}

/* Wire bytes (register 0 high byte first) <-> value bytes (most significant
 * first) for n registers. The mapping is its own inverse for every order. */
static int wire_index(int k, int n, mb_order_t o) {
    switch (o) {
        case MB_O_DCBA: return 2 * n - 1 - k;
        case MB_O_BADC: return k ^ 1;
        case MB_O_CDAB: return 2 * (n - 1 - k / 2) + (k & 1);
        default:        return k;
    }
}

static uint64_t regs_to_u64(const uint16_t* regs, int n, mb_order_t o) {
    uint8_t w[8];
    for (int i = 0; i < n; i++) { w[2 * i] = regs[i] >> 8; w[2 * i + 1] = regs[i] & 0xFF; }
    uint64_t v = 0;
    for (int k = 0; k < 2 * n; k++) v = (v << 8) | w[wire_index(k, n, o)];
    return v;
}

static void u64_to_regs(uint64_t v, int n, mb_order_t o, uint16_t* regs) {
    uint8_t w[8];
    for (int k = 2 * n - 1; k >= 0; k--) { w[wire_index(k, n, o)] = v & 0xFF; v >>= 8; }
    for (int i = 0; i < n; i++) regs[i] = (uint16_t)((w[2 * i] << 8) | w[2 * i + 1]);
}

static double round_sig(double v, int digits) {
    char buf[40];
    snprintf(buf, sizeof(buf), "%.*g", digits, v);
    return strtod(buf, NULL);
}

double mb_decode_number(const uint16_t* regs, mb_type_t t, mb_order_t o) {
    int n = mb_type_regs(t, 1);
    uint64_t u = regs_to_u64(regs, n, o);
    switch (t) {
        case MB_T_UINT16: return (double)(uint16_t)u;
        case MB_T_INT16:  return (double)(int16_t)(uint16_t)u;
        case MB_T_UINT32: return (double)(uint32_t)u;
        case MB_T_INT32:  return (double)(int32_t)(uint32_t)u;
        case MB_T_UINT64: return (double)u;
        case MB_T_INT64:  return (double)(int64_t)u;
        case MB_T_FLOAT32: {
            uint32_t b = (uint32_t)u;
            float f;
            memcpy(&f, &b, sizeof(f));
            return isfinite(f) ? round_sig((double)f, 7) : (double)f;
        }
        case MB_T_FLOAT64: {
            double d;
            memcpy(&d, &u, sizeof(d));
            return d;
        }
        default: return 0;
    }
}

void mb_decode_string(const uint16_t* regs, int nregs, mb_order_t o, char* out, size_t out_size) {
    if (!out || !out_size) return;
    int swap = (o == MB_O_BADC || o == MB_O_DCBA);
    size_t len = 0;
    for (int i = 0; i < nregs && len + 1 < out_size; i++) {
        uint8_t b[2] = { regs[i] >> 8, regs[i] & 0xFF };
        if (swap) { uint8_t t = b[0]; b[0] = b[1]; b[1] = t; }
        for (int j = 0; j < 2 && len + 1 < out_size; j++) {
            if (b[j] == 0) { i = nregs; break; }
            out[len++] = (b[j] >= 0x20 && b[j] < 0x7F) ? (char)b[j] : '?';
        }
    }
    while (len > 0 && out[len - 1] == ' ') len--;
    out[len] = '\0';
}

int mb_encode_number(double v, mb_type_t t, mb_order_t o, uint16_t* regs) {
    if (isnan(v) || isinf(v)) return -1;
    int n = mb_type_regs(t, 1);
    uint64_t u = 0;
    double r = round(v);
    switch (t) {
        case MB_T_UINT16: if (r < 0 || r > 65535) return -1; u = (uint64_t)r; break;
        case MB_T_INT16:  if (r < -32768 || r > 32767) return -1; u = (uint16_t)(int16_t)r; break;
        case MB_T_UINT32: if (r < 0 || r > 4294967295.0) return -1; u = (uint64_t)r; break;
        case MB_T_INT32:  if (r < -2147483648.0 || r > 2147483647.0) return -1; u = (uint32_t)(int32_t)r; break;
        case MB_T_UINT64: if (r < 0 || r >= 18446744073709551616.0) return -1; u = (uint64_t)r; break;
        case MB_T_INT64:  if (r < -9223372036854775808.0 || r >= 9223372036854775808.0) return -1;
                          u = (uint64_t)(int64_t)r; break;
        case MB_T_FLOAT32: {
            if (fabs(v) > FLT_MAX) return -1;
            float f = (float)v;
            uint32_t b;
            memcpy(&b, &f, sizeof(b));
            u = b;
            break;
        }
        case MB_T_FLOAT64: memcpy(&u, &v, sizeof(u)); break;
        default: return -1;
    }
    u64_to_regs(u, n, o, regs);
    return 0;
}

int mb_encode_string(const char* s, int nregs, mb_order_t o, uint16_t* regs) {
    size_t len = s ? strlen(s) : 0;
    if (nregs < 1 || len > (size_t)nregs * 2) return -1;
    int swap = (o == MB_O_BADC || o == MB_O_DCBA);
    for (int i = 0; i < nregs; i++) {
        uint8_t hi = (size_t)(2 * i) < len ? (uint8_t)s[2 * i] : 0;
        uint8_t lo = (size_t)(2 * i + 1) < len ? (uint8_t)s[2 * i + 1] : 0;
        if (swap) { uint8_t t = hi; hi = lo; lo = t; }
        regs[i] = (uint16_t)((hi << 8) | lo);
    }
    return 0;
}

double mb_scale(double raw, double scale, double offset, int decimals) {
    double v = raw * scale + offset;
    if (!isfinite(v)) return v;
    if (decimals >= 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.*f", decimals > 15 ? 15 : decimals, v);
        return strtod(buf, NULL);
    }
    return round_sig(v, 15);
}

double mb_unscale(double value, double scale, double offset) {
    return (value - offset) / (scale != 0 ? scale : 1);
}

const char* mb_exception_name(int code) {
    switch (code) {
        case 0x01: return "Illegal Function";
        case 0x02: return "Illegal Data Address";
        case 0x03: return "Illegal Data Value";
        case 0x04: return "Server Device Failure";
        case 0x05: return "Acknowledge";
        case 0x06: return "Server Device Busy";
        case 0x08: return "Memory Parity Error";
        case 0x0A: return "Gateway Path Unavailable";
        case 0x0B: return "Gateway Target Device Failed to Respond";
        default:   return "Unknown exception";
    }
}

void mb_format_number(double v, int decimals, char* out, size_t out_size) {
    if (decimals >= 0) snprintf(out, out_size, "%.*f", decimals > 15 ? 15 : decimals, v);
    else               snprintf(out, out_size, "%.15g", v);
}

/* ------------------------------------------------------------------ */
/* Rule step parsing and validation                                    */
/* ------------------------------------------------------------------ */

/* Number from a JSON number or numeric string; def when absent or empty. */
static int get_num(cJSON* obj, const char* key, double def, double* out) {
    cJSON* j = cJSON_GetObjectItem(obj, key);
    if (!j || cJSON_IsNull(j)) { *out = def; return 0; }
    if (cJSON_IsNumber(j)) { *out = j->valuedouble; return 0; }
    if (cJSON_IsString(j)) {
        const char* s = j->valuestring;
        while (*s == ' ') s++;
        if (!*s) { *out = def; return 0; }
        char* end;
        double d = strtod(s, &end);
        while (*end == ' ') end++;
        if (end != s && *end == '\0') { *out = d; return 0; }
    }
    return -1;
}

int mb_field_parse(cJSON* obj, const char* type_key, const char* order_key,
                   int bits, mb_field_t* f, char* err, size_t err_size) {
    memset(f, 0, sizeof(*f));
    double d;
    if (get_num(obj, "register", 0, &d) != 0 || d < 0 || d > 65535 || d != floor(d)) {
        snprintf(err, err_size, "register must be a whole number from 0 to 65535");
        return -1;
    }
    f->reg = (int)d;
    f->scale = 1;
    f->decimals = -1;
    f->nregs = 1;
    f->type = MB_T_UINT16;
    if (bits) return 0;

    const char* ts = cJSON_GetStringValue(cJSON_GetObjectItem(obj, type_key));
    f->type = mb_type_parse(ts);
    if (f->type == MB_T_INVALID) {
        snprintf(err, err_size, "unknown data type '%s' (use uint16, int16, uint32, int32, float32, "
                 "uint64, int64, float64 or string)", ts);
        return -1;
    }
    const char* os = cJSON_GetStringValue(cJSON_GetObjectItem(obj, order_key));
    f->order = mb_order_parse(os);
    if (f->order == MB_O_INVALID) {
        snprintf(err, err_size, "unknown word order '%s' (use ABCD, CDAB, BADC or DCBA)", os);
        return -1;
    }
    if (get_num(obj, "scale", 1, &f->scale) != 0 || f->scale == 0 || !isfinite(f->scale)) {
        snprintf(err, err_size, "scale must be a number other than 0");
        return -1;
    }
    if (get_num(obj, "offset", 0, &f->offset) != 0 || !isfinite(f->offset)) {
        snprintf(err, err_size, "offset must be a number");
        return -1;
    }
    if (get_num(obj, "decimals", -1, &d) != 0 || d < -1 || d > 10 || d != floor(d)) {
        snprintf(err, err_size, "decimals must be a whole number from 0 to 10");
        return -1;
    }
    f->decimals = (int)d;
    if (f->type == MB_T_STRING) {
        if (get_num(obj, "length", 8, &d) != 0 || d < 1 || d > MB_MAX_STRING_REGS || d != floor(d)) {
            snprintf(err, err_size, "string length must be 1 to %d registers", MB_MAX_STRING_REGS);
            return -1;
        }
        f->length = (int)d;
    }
    f->nregs = mb_type_regs(f->type, f->length);
    if (f->reg + f->nregs - 1 > 65535) {
        snprintf(err, err_size, "register %d + %d registers goes past 65535", f->reg, f->nregs);
        return -1;
    }
    return 0;
}

static int is_bit_type(const char* rtype) {
    return rtype && (strcmp(rtype, "coil") == 0 || strcmp(rtype, "discrete") == 0);
}

static int valid_var_name(const char* s) {
    return s && s[0] && strlen(s) < 64;
}

int mb_validate_read(cJSON* trig, char* err, size_t err_size) {
    const char* rtype = cJSON_GetStringValue(cJSON_GetObjectItem(trig, "register_type"));
    if (rtype && strcmp(rtype, "holding") != 0 && strcmp(rtype, "input") != 0 && !is_bit_type(rtype)) {
        snprintf(err, err_size, "register_type must be holding, input, coil or discrete");
        return 0;
    }
    int bits = is_bit_type(rtype);
    const char* op = cJSON_GetStringValue(cJSON_GetObjectItem(trig, "op"));
    int has_op = op && op[0];
    char ferr[160];
    mb_field_t f;

    cJSON* map = cJSON_GetObjectItem(trig, "map");
    if (map && !cJSON_IsArray(map)) {
        snprintf(err, err_size, "map must be an array");
        return 0;
    }
    if (map && cJSON_GetArraySize(map) > 0) {
        const char* cmp = cJSON_GetStringValue(cJSON_GetObjectItem(trig, "compare"));
        int lo = 65536, hi = -1, idx = 0, found_cmp = 0;
        mb_type_t cmp_type = MB_T_UINT16;
        cJSON* e;
        cJSON_ArrayForEach(e, map) {
            idx++;
            if (!cJSON_IsObject(e)) {
                snprintf(err, err_size, "map entry %d must be an object", idx);
                return 0;
            }
            if (!cJSON_GetObjectItem(e, "register")) {
                snprintf(err, err_size, "map entry %d: register is required", idx);
                return 0;
            }
            if (mb_field_parse(e, "type", "order", bits, &f, ferr, sizeof(ferr)) != 0) {
                snprintf(err, err_size, "map entry %d: %s", idx, ferr);
                return 0;
            }
            const char* var = cJSON_GetStringValue(cJSON_GetObjectItem(e, "var"));
            if (!valid_var_name(var)) {
                snprintf(err, err_size, "map entry %d: var (variable name, up to 63 characters) is required", idx);
                return 0;
            }
            int j = 0;
            cJSON* o;
            cJSON_ArrayForEach(o, map) {
                if (++j >= idx) break;
                const char* ov = cJSON_GetStringValue(cJSON_GetObjectItem(o, "var"));
                if (ov && strcmp(ov, var) == 0) {
                    snprintf(err, err_size, "map entry %d: variable '%s' is used twice", idx, var);
                    return 0;
                }
            }
            if (f.reg < lo) lo = f.reg;
            if (f.reg + f.nregs - 1 > hi) hi = f.reg + f.nregs - 1;
            if (cmp && strcmp(cmp, var) == 0) { found_cmp = 1; cmp_type = f.type; }
            if (!cmp && idx == 1) cmp_type = f.type;
        }
        int max = bits ? MB_MAX_READ_BITS : MB_MAX_READ_REGS;
        if (hi - lo + 1 > max) {
            snprintf(err, err_size, "map spans %d %s (%d to %d); one read covers at most %d",
                     hi - lo + 1, bits ? "bits" : "registers", lo, hi, max);
            return 0;
        }
        if (cmp && cmp[0] && !found_cmp) {
            snprintf(err, err_size, "compare '%s' is not a variable in the map", cmp);
            return 0;
        }
        if (has_op && cmp_type == MB_T_STRING) {
            snprintf(err, err_size, "a threshold needs a number; the compared map entry is a string");
            return 0;
        }
        return 1;
    }

    if (mb_field_parse(trig, "data_type", "word_order", bits, &f, ferr, sizeof(ferr)) != 0) {
        snprintf(err, err_size, "%s", ferr);
        return 0;
    }
    if (has_op && f.type == MB_T_STRING) {
        snprintf(err, err_size, "a threshold needs a number; string values fire on every change");
        return 0;
    }
    const char* var = cJSON_GetStringValue(cJSON_GetObjectItem(trig, "variable"));
    if (var && var[0] && !valid_var_name(var)) {
        snprintf(err, err_size, "variable name must be up to 63 characters");
        return 0;
    }
    return 1;
}

static int valid_function(const char* fn, int bits, int* forced) {
    *forced = 0;
    if (!fn || !fn[0] || strcmp(fn, "auto") == 0) return 1;
    if (bits) {
        if (strcmp(fn, "fc05") == 0) { *forced = 5; return 1; }
        if (strcmp(fn, "fc15") == 0) { *forced = 15; return 1; }
    } else {
        if (strcmp(fn, "fc06") == 0) { *forced = 6; return 1; }
        if (strcmp(fn, "fc16") == 0) { *forced = 16; return 1; }
    }
    return 0;
}

/* A literal number or string can be checked now; templates only at run time. */
static int check_literal(cJSON* v, const mb_field_t* f, int bits, char* err, size_t err_size) {
    if (bits || !v) return 0;
    if (f->type == MB_T_STRING) {
        const char* s = cJSON_GetStringValue(v);
        if (s && !strstr(s, "{{") && strlen(s) > (size_t)f->length * 2) {
            snprintf(err, err_size, "text is %d characters; %d registers hold %d",
                     (int)strlen(s), f->length, f->length * 2);
            return -1;
        }
        return 0;
    }
    double d;
    if (cJSON_IsNumber(v)) d = v->valuedouble;
    else if (cJSON_IsString(v) && v->valuestring[0] && !strstr(v->valuestring, "{{")) {
        char* end;
        d = strtod(v->valuestring, &end);
        if (end == v->valuestring || *end) {
            snprintf(err, err_size, "value '%s' is not a number", v->valuestring);
            return -1;
        }
    } else {
        return 0;
    }
    uint16_t regs[4];
    if (mb_encode_number(mb_unscale(d, f->scale, f->offset), f->type, f->order, regs) != 0) {
        snprintf(err, err_size, "value %g is out of range for %s", d, mb_type_name(f->type));
        return -1;
    }
    return 0;
}

static int field_cmp(const void* a, const void* b) {
    return ((const mb_field_t*)a)->reg - ((const mb_field_t*)b)->reg;
}

int mb_validate_write(cJSON* action, char* err, size_t err_size) {
    const char* rtype = cJSON_GetStringValue(cJSON_GetObjectItem(action, "register_type"));
    if (rtype && strcmp(rtype, "holding") != 0 && strcmp(rtype, "coil") != 0) {
        snprintf(err, err_size, "register_type must be holding or coil");
        return 0;
    }
    int bits = rtype && strcmp(rtype, "coil") == 0;
    const char* fn = cJSON_GetStringValue(cJSON_GetObjectItem(action, "function"));
    int forced;
    if (!valid_function(fn, bits, &forced)) {
        snprintf(err, err_size, "function must be auto, %s", bits ? "fc05 or fc15 for coils" : "fc06 or fc16 for holding registers");
        return 0;
    }
    char ferr[160];
    mb_field_t f;

    cJSON* values = cJSON_GetObjectItem(action, "values");
    if (values && !cJSON_IsArray(values)) {
        snprintf(err, err_size, "values must be an array");
        return 0;
    }
    if (values && cJSON_GetArraySize(values) > 0) {
        int n = cJSON_GetArraySize(values);
        mb_field_t* fs = calloc((size_t)n, sizeof(*fs));
        if (!fs) { snprintf(err, err_size, "out of memory"); return 0; }
        int idx = 0, ok = 1;
        cJSON* e;
        cJSON_ArrayForEach(e, values) {
            if (!cJSON_IsObject(e)) { snprintf(err, err_size, "values entry %d must be an object", idx + 1); ok = 0; break; }
            if (!cJSON_GetObjectItem(e, "register")) {
                snprintf(err, err_size, "values entry %d: register is required", idx + 1); ok = 0; break;
            }
            if (!cJSON_GetObjectItem(e, "value")) {
                snprintf(err, err_size, "values entry %d: value is required", idx + 1); ok = 0; break;
            }
            if (mb_field_parse(e, "type", "order", bits, &fs[idx], ferr, sizeof(ferr)) != 0 ||
                check_literal(cJSON_GetObjectItem(e, "value"), &fs[idx], bits, ferr, sizeof(ferr)) != 0) {
                snprintf(err, err_size, "values entry %d: %s", idx + 1, ferr); ok = 0; break;
            }
            for (int j = 0; j < idx; j++) {
                if (fs[idx].reg < fs[j].reg + fs[j].nregs && fs[j].reg < fs[idx].reg + fs[idx].nregs) {
                    snprintf(err, err_size, "values entries %d and %d overlap (register %d)",
                             j + 1, idx + 1, fs[idx].reg > fs[j].reg ? fs[idx].reg : fs[j].reg);
                    ok = 0; break;
                }
            }
            if (!ok) break;
            idx++;
        }
        int total = 0;
        if (ok) {
            /* Gaps split the block into separate requests; each must fit one. */
            qsort(fs, (size_t)n, sizeof(*fs), field_cmp);
            int max = bits ? MB_MAX_WRITE_BITS : MB_MAX_WRITE_REGS;
            for (int i = 0, run_start = 0; i < n; i++) {
                total += fs[i].nregs;
                if (i > 0 && fs[i].reg != fs[i - 1].reg + fs[i - 1].nregs) run_start = i;
                int run = fs[i].reg + fs[i].nregs - fs[run_start].reg;
                if (run > max) {
                    snprintf(err, err_size, "registers %d to %d are %d %s in one request; at most %d",
                             fs[run_start].reg, fs[i].reg + fs[i].nregs - 1, run,
                             bits ? "coils" : "registers", max);
                    ok = 0;
                    break;
                }
            }
        }
        free(fs);
        if (!ok) return 0;
        if ((forced == 5 || forced == 6) && (n > 1 || total > 1)) {
            snprintf(err, err_size, "fc%02d writes a single %s; use fc%d or auto for several",
                     forced, bits ? "coil" : "register", bits ? 15 : 16);
            return 0;
        }
        return 1;
    }

    if (mb_field_parse(action, "data_type", "word_order", bits, &f, ferr, sizeof(ferr)) != 0 ||
        check_literal(cJSON_GetObjectItem(action, "value"), &f, bits, ferr, sizeof(ferr)) != 0) {
        snprintf(err, err_size, "%s", ferr);
        return 0;
    }
    if (forced == 6 && f.nregs > 1) {
        snprintf(err, err_size, "fc06 writes one register; %s needs %d, use fc16 or auto",
                 mb_type_name(f.type), f.nregs);
        return 0;
    }
    if (!bits && f.nregs > MB_MAX_WRITE_REGS) {
        snprintf(err, err_size, "one write covers at most %d registers", MB_MAX_WRITE_REGS);
        return 0;
    }
    return 1;
}
