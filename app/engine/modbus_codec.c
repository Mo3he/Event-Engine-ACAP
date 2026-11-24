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
#include <strings.h>

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

/* ------------------------------------------------------------------ */
/* Character encodings                                                 */
/* ------------------------------------------------------------------ */

static const char* const charset_names[] = {
    "utf-8", "ascii", "iso-8859-1", "iso-8859-2", "windows-1250", "windows-1252", NULL
};
static const char* const charset_aliases[] = {
    "utf8", "us-ascii", "latin1", "latin2", "cp1250", "cp1252", NULL
};

mb_charset_t mb_charset_parse(const char* s) {
    if (!s || !s[0]) return MB_CS_UTF8;
    for (int i = 0; charset_names[i]; i++)
        if (strcasecmp(s, charset_names[i]) == 0 || strcasecmp(s, charset_aliases[i]) == 0)
            return (mb_charset_t)i;
    return MB_CS_INVALID;
}

const char* mb_charset_name(mb_charset_t cs) {
    return (cs >= MB_CS_UTF8 && cs <= MB_CS_CP1252) ? charset_names[cs] : "invalid";
}

/* Unicode code points of bytes 0x80-0xFF; 0 = not defined. 0x80-0x9F of
 * ISO-8859-2 are C1 controls, which are treated as not printable. */
static const uint16_t cs_latin2[128] = {
    0x0080, 0x0081, 0x0082, 0x0083, 0x0084, 0x0085, 0x0086, 0x0087,
    0x0088, 0x0089, 0x008A, 0x008B, 0x008C, 0x008D, 0x008E, 0x008F,
    0x0090, 0x0091, 0x0092, 0x0093, 0x0094, 0x0095, 0x0096, 0x0097,
    0x0098, 0x0099, 0x009A, 0x009B, 0x009C, 0x009D, 0x009E, 0x009F,
    0x00A0, 0x0104, 0x02D8, 0x0141, 0x00A4, 0x013D, 0x015A, 0x00A7,
    0x00A8, 0x0160, 0x015E, 0x0164, 0x0179, 0x00AD, 0x017D, 0x017B,
    0x00B0, 0x0105, 0x02DB, 0x0142, 0x00B4, 0x013E, 0x015B, 0x02C7,
    0x00B8, 0x0161, 0x015F, 0x0165, 0x017A, 0x02DD, 0x017E, 0x017C,
    0x0154, 0x00C1, 0x00C2, 0x0102, 0x00C4, 0x0139, 0x0106, 0x00C7,
    0x010C, 0x00C9, 0x0118, 0x00CB, 0x011A, 0x00CD, 0x00CE, 0x010E,
    0x0110, 0x0143, 0x0147, 0x00D3, 0x00D4, 0x0150, 0x00D6, 0x00D7,
    0x0158, 0x016E, 0x00DA, 0x0170, 0x00DC, 0x00DD, 0x0162, 0x00DF,
    0x0155, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x013A, 0x0107, 0x00E7,
    0x010D, 0x00E9, 0x0119, 0x00EB, 0x011B, 0x00ED, 0x00EE, 0x010F,
    0x0111, 0x0144, 0x0148, 0x00F3, 0x00F4, 0x0151, 0x00F6, 0x00F7,
    0x0159, 0x016F, 0x00FA, 0x0171, 0x00FC, 0x00FD, 0x0163, 0x02D9,
};
static const uint16_t cs_cp1250[128] = {
    0x20AC, 0x0000, 0x201A, 0x0000, 0x201E, 0x2026, 0x2020, 0x2021,
    0x0000, 0x2030, 0x0160, 0x2039, 0x015A, 0x0164, 0x017D, 0x0179,
    0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x0000, 0x2122, 0x0161, 0x203A, 0x015B, 0x0165, 0x017E, 0x017A,
    0x00A0, 0x02C7, 0x02D8, 0x0141, 0x00A4, 0x0104, 0x00A6, 0x00A7,
    0x00A8, 0x00A9, 0x015E, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x017B,
    0x00B0, 0x00B1, 0x02DB, 0x0142, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
    0x00B8, 0x0105, 0x015F, 0x00BB, 0x013D, 0x02DD, 0x013E, 0x017C,
    0x0154, 0x00C1, 0x00C2, 0x0102, 0x00C4, 0x0139, 0x0106, 0x00C7,
    0x010C, 0x00C9, 0x0118, 0x00CB, 0x011A, 0x00CD, 0x00CE, 0x010E,
    0x0110, 0x0143, 0x0147, 0x00D3, 0x00D4, 0x0150, 0x00D6, 0x00D7,
    0x0158, 0x016E, 0x00DA, 0x0170, 0x00DC, 0x00DD, 0x0162, 0x00DF,
    0x0155, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x013A, 0x0107, 0x00E7,
    0x010D, 0x00E9, 0x0119, 0x00EB, 0x011B, 0x00ED, 0x00EE, 0x010F,
    0x0111, 0x0144, 0x0148, 0x00F3, 0x00F4, 0x0151, 0x00F6, 0x00F7,
    0x0159, 0x016F, 0x00FA, 0x0171, 0x00FC, 0x00FD, 0x0163, 0x02D9,
};
static const uint16_t cs_cp1252[128] = {
    0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000,
    0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178,
    0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7,
    0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
    0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,
    0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7,
    0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
    0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7,
    0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7,
    0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,
    0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7,
    0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF,
};

static const uint16_t* cs_table(mb_charset_t cs) {
    switch (cs) {
        case MB_CS_LATIN2: return cs_latin2;
        case MB_CS_CP1250: return cs_cp1250;
        case MB_CS_CP1252: return cs_cp1252;
        default:           return NULL;
    }
}

/* Code point of an 8-bit byte in cs; 0 if it isn't a printable character. */
static uint32_t cs_byte_to_cp(mb_charset_t cs, uint8_t b) {
    if (b < 0x80) return (b >= 0x20 && b != 0x7F) ? b : 0;
    if (cs == MB_CS_ASCII) return 0;
    const uint16_t* t = cs_table(cs);
    uint32_t cp = t ? t[b - 0x80] : b;
    return (cp >= 0x80 && cp < 0xA0) ? 0 : cp;
}

/* Byte for a code point in an 8-bit cs; -1 if there is none. */
static int cs_cp_to_byte(mb_charset_t cs, uint32_t cp) {
    if (cp < 0x80) return (int)cp;
    if (cs == MB_CS_ASCII || (cp >= 0x80 && cp < 0xA0)) return -1;
    const uint16_t* t = cs_table(cs);
    if (!t) return cp <= 0xFF ? (int)cp : -1;
    for (int i = 0; i < 128; i++)
        if (t[i] == cp) return 0x80 + i;
    return -1;
}

/* Next code point of UTF-8 text; *len gets its byte length. Invalid or
 * overlong sequences give 0xFFFD with length 1. */
static uint32_t utf8_next(const uint8_t* s, int* len) {
    uint8_t c = s[0];
    if (c < 0x80) { *len = 1; return c; }
    int n = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    *len = 1;
    if (!n) return 0xFFFD;
    uint32_t cp = c & (0x7F >> n);
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    static const uint32_t min_cp[] = { 0, 0, 0x80, 0x800, 0x10000 };
    if (cp < min_cp[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0xFFFD;
    *len = n;
    return cp;
}

static int utf8_put(uint32_t cp, char* out) {
    if (cp < 0x80)    { out[0] = (char)cp; return 1; }
    if (cp < 0x800)   { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

size_t mb_text_encode(const char* s, mb_charset_t cs, uint8_t* out, size_t out_size,
                      int* bad, uint32_t* first_bad) {
    size_t n = 0;
    int nbad = 0;
    const uint8_t* p = (const uint8_t*)(s ? s : "");
    while (*p) {
        int len;
        uint32_t cp = utf8_next(p, &len);
        uint8_t buf[4];
        int blen;
        if (cp == 0xFFFD && !(len == 3 && p[0] == 0xEF)) {
            buf[0] = '?'; blen = 1;
        } else if (cs == MB_CS_UTF8) {
            memcpy(buf, p, (size_t)len); blen = len;
            cp = 0;
        } else {
            int b = cs_cp_to_byte(cs, cp);
            buf[0] = b < 0 ? '?' : (uint8_t)b; blen = 1;
            if (b >= 0) cp = 0;
        }
        if (cp) {
            if (!nbad && first_bad) *first_bad = cp;
            nbad++;
        }
        for (int i = 0; i < blen; i++, n++)
            if (out && n < out_size) out[n] = buf[i];
        p += len;
    }
    if (bad) *bad = nbad;
    return n;
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

void mb_decode_string(const uint16_t* regs, int nregs, mb_order_t o, mb_charset_t cs,
                      char* out, size_t out_size) {
    if (!out || !out_size) return;
    int swap = (o == MB_O_BADC || o == MB_O_DCBA);
    uint8_t bytes[2 * MB_MAX_STRING_REGS + 4];
    int nb = 0;
    for (int i = 0; i < nregs && nb < 2 * MB_MAX_STRING_REGS; i++) {
        uint8_t b[2] = { regs[i] >> 8, regs[i] & 0xFF };
        if (swap) { uint8_t t = b[0]; b[0] = b[1]; b[1] = t; }
        if (b[0] == 0) break;
        bytes[nb++] = b[0];
        if (b[1] == 0) break;
        bytes[nb++] = b[1];
    }
    memset(bytes + nb, 0, 4);   /* utf8_next stops at the NUL */

    size_t len = 0;
    for (int i = 0; i < nb;) {
        uint32_t cp;
        int step = 1;
        if (cs == MB_CS_UTF8) {
            cp = utf8_next(bytes + i, &step);
            if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) cp = '?';
            if (cp == 0xFFFD && step == 1) cp = '?';
        } else {
            cp = cs_byte_to_cp(cs, bytes[i]);
            if (!cp) cp = '?';
        }
        char u[4];
        int ulen = utf8_put(cp, u);
        if (len + (size_t)ulen >= out_size) break;
        memcpy(out + len, u, (size_t)ulen);
        len += (size_t)ulen;
        i += step;
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

int mb_encode_string(const char* s, int nregs, mb_order_t o, mb_charset_t cs,
                     uint16_t* regs, int* bad) {
    if (nregs < 1 || nregs > MB_MAX_STRING_REGS) return -1;
    uint8_t bytes[2 * MB_MAX_STRING_REGS];
    memset(bytes, 0, sizeof(bytes));
    size_t len = mb_text_encode(s, cs, bytes, sizeof(bytes), bad, NULL);
    if (len > (size_t)nregs * 2) return -1;
    int swap = (o == MB_O_BADC || o == MB_O_DCBA);
    for (int i = 0; i < nregs; i++) {
        uint8_t hi = bytes[2 * i], lo = bytes[2 * i + 1];
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

int mb_encode_field(double v, const mb_field_t* f, uint16_t* regs) {
    double raw = mb_unscale(v, f->scale, f->offset);
    if (f->type == MB_T_UINT16 && raw < 0)
        return mb_encode_number(trunc(raw), MB_T_INT16, f->order, regs);
    return mb_encode_number(raw, f->type, f->order, regs);
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
        const char* cs = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "encoding"));
        f->charset = mb_charset_parse(cs);
        if (f->charset == MB_CS_INVALID) {
            snprintf(err, err_size, "unknown encoding '%s' (use ascii, utf-8, iso-8859-1, iso-8859-2, "
                     "windows-1250 or windows-1252)", cs);
            return -1;
        }
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

static int field_cmp(const void* a, const void* b) {
    return ((const mb_field_t*)a)->reg - ((const mb_field_t*)b)->reg;
}

static int add_span(mb_span_t* out, int* n, int out_max, int address, int count) {
    if (*n >= out_max) return -1;
    out[*n].address = address;
    out[*n].count = count;
    (*n)++;
    return 0;
}

int mb_plan_reads(const mb_field_t* fs, int n, int max, mb_span_t* out, int out_max) {
    if (n < 1 || max < 1) return 0;
    mb_field_t* sorted = malloc((size_t)n * sizeof(*sorted));
    if (!sorted) return -1;
    memcpy(sorted, fs, (size_t)n * sizeof(*sorted));
    qsort(sorted, (size_t)n, sizeof(*sorted), field_cmp);
    int count = 0, have = 0, start = 0, end = 0, rc = 0;
    for (int i = 0; i < n && rc == 0; i++) {
        int fstart = sorted[i].reg, fend = sorted[i].reg + sorted[i].nregs - 1;
        if (have) {
            int new_end = fend > end ? fend : end;
            if (new_end - start + 1 <= max) { end = new_end; continue; }
            rc = add_span(out, &count, out_max, start, end - start + 1);
            have = 0;
        }
        while (rc == 0 && fend - fstart + 1 > max) {
            rc = add_span(out, &count, out_max, fstart, max);
            fstart += max;
        }
        start = fstart; end = fend; have = 1;
    }
    if (rc == 0 && have) rc = add_span(out, &count, out_max, start, end - start + 1);
    free(sorted);
    return rc == 0 ? count : -1;
}

int mb_plan_writes(const mb_field_t* fs, int n, int max, mb_span_t* out, int out_max) {
    int count = 0, start = 0, len = 0;
    for (int i = 0; i < n; i++) {
        const mb_field_t* f = &fs[i];
        if (len > 0 && (f->reg != start + len || len + f->nregs > max)) {
            if (add_span(out, &count, out_max, start, len) != 0) return -1;
            len = 0;
        }
        for (int off = 0; off < f->nregs;) {
            if (len == 0) start = f->reg + off;
            int take = f->nregs - off < max - len ? f->nregs - off : max - len;
            len += take;
            off += take;
            if (len == max && off < f->nregs) {
                if (add_span(out, &count, out_max, start, len) != 0) return -1;
                len = 0;
            }
        }
    }
    if (len > 0 && add_span(out, &count, out_max, start, len) != 0) return -1;
    return count;
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
        int n = cJSON_GetArraySize(map);
        mb_field_t* fs = calloc((size_t)n, sizeof(*fs));
        if (!fs) { snprintf(err, err_size, "out of memory"); return 0; }
        int lo = 65536, hi = -1, idx = 0, found_cmp = 0, ok = 1;
        mb_type_t cmp_type = MB_T_UINT16;
        cJSON* e;
        cJSON_ArrayForEach(e, map) {
            idx++;
            if (!cJSON_IsObject(e)) {
                snprintf(err, err_size, "map entry %d must be an object", idx);
                ok = 0; break;
            }
            if (!cJSON_GetObjectItem(e, "register")) {
                snprintf(err, err_size, "map entry %d: register is required", idx);
                ok = 0; break;
            }
            mb_field_t* f = &fs[idx - 1];
            if (mb_field_parse(e, "type", "order", bits, f, ferr, sizeof(ferr)) != 0) {
                snprintf(err, err_size, "map entry %d: %s", idx, ferr);
                ok = 0; break;
            }
            const char* var = cJSON_GetStringValue(cJSON_GetObjectItem(e, "var"));
            if (!valid_var_name(var)) {
                snprintf(err, err_size, "map entry %d: var (variable name, up to 63 characters) is required", idx);
                ok = 0; break;
            }
            int j = 0;
            cJSON* o;
            cJSON_ArrayForEach(o, map) {
                if (++j >= idx) break;
                const char* ov = cJSON_GetStringValue(cJSON_GetObjectItem(o, "var"));
                if (ov && strcmp(ov, var) == 0) {
                    snprintf(err, err_size, "map entry %d: variable '%s' is used twice", idx, var);
                    ok = 0; break;
                }
            }
            if (!ok) break;
            if (f->reg < lo) lo = f->reg;
            if (f->reg + f->nregs - 1 > hi) hi = f->reg + f->nregs - 1;
            if (cmp && strcmp(cmp, var) == 0) { found_cmp = 1; cmp_type = f->type; }
            if (!cmp && idx == 1) cmp_type = f->type;
        }
        if (ok && hi - lo + 1 > MB_MAX_SPAN) {
            snprintf(err, err_size, "map spans %d %s (%d to %d); at most %d",
                     hi - lo + 1, bits ? "bits" : "registers", lo, hi, MB_MAX_SPAN);
            ok = 0;
        }
        if (ok) {
            mb_span_t spans[MB_MAX_REQUESTS];
            if (mb_plan_reads(fs, n, bits ? MB_MAX_READ_BITS : MB_MAX_READ_REGS, spans, MB_MAX_REQUESTS) < 0) {
                snprintf(err, err_size, "map needs more than %d read requests; split it into several rules",
                         MB_MAX_REQUESTS);
                ok = 0;
            }
        }
        free(fs);
        if (!ok) return 0;
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

/* Number from text with optional surrounding spaces. */
static int text_number(const char* s, double* out) {
    while (*s == ' ') s++;
    char* end;
    *out = strtod(s, &end);
    while (*end == ' ') end++;
    return end != s && *end == '\0' ? 0 : -1;
}

/* A literal value can be checked now; templates only at run time. Numbers
 * and coils need a value; an empty string clears a text register. */
static int check_literal(cJSON* v, const mb_field_t* f, int bits, char* err, size_t err_size) {
    const char* s = cJSON_GetStringValue(v);
    if (s && strstr(s, "{{")) return 0;
    if (!bits && f->type == MB_T_STRING) {
        if (!v) return 0;
        if (!s) {
            snprintf(err, err_size, "value must be text");
            return -1;
        }
        int bad = 0;
        uint32_t first_bad = 0;
        size_t len = mb_text_encode(s, f->charset, NULL, 0, &bad, &first_bad);
        if (bad) {
            char ch[8] = "";
            if (first_bad != 0xFFFD) ch[utf8_put(first_bad, ch)] = '\0';
            snprintf(err, err_size, "'%s' can't be written in %s; choose another encoding",
                     ch[0] ? ch : "invalid UTF-8", mb_charset_name(f->charset));
            return -1;
        }
        if (len > (size_t)f->length * 2) {
            snprintf(err, err_size, "text is %d bytes in %s; %d registers hold %d",
                     (int)len, mb_charset_name(f->charset), f->length, f->length * 2);
            return -1;
        }
        return 0;
    }
    int empty = !v || cJSON_IsNull(v) || (s && !s[strspn(s, " ")]);
    if (empty) {
        snprintf(err, err_size, "value is required");
        return -1;
    }
    double d;
    if (cJSON_IsNumber(v)) d = v->valuedouble;
    else if (cJSON_IsBool(v)) d = cJSON_IsTrue(v) ? 1 : 0;
    else if (!s) {
        snprintf(err, err_size, "value must be a number or text");
        return -1;
    } else if (bits && (strcasecmp(s, "true") == 0 || strcasecmp(s, "on") == 0 ||
                        strcasecmp(s, "false") == 0 || strcasecmp(s, "off") == 0)) {
        return 0;
    } else if (text_number(s, &d) != 0) {
        snprintf(err, err_size, bits ? "value '%.40s' is not on/off, true/false or a number"
                                     : "value '%.40s' is not a number", s);
        return -1;
    }
    if (bits) return 0;
    uint16_t regs[4];
    if (mb_encode_field(d, f, regs) != 0) {
        snprintf(err, err_size, "value %g is out of range for %s", d, mb_type_name(f->type));
        return -1;
    }
    return 0;
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
            qsort(fs, (size_t)n, sizeof(*fs), field_cmp);
            for (int i = 0; i < n; i++) total += fs[i].nregs;
            mb_span_t spans[MB_MAX_REQUESTS];
            if (mb_plan_writes(fs, n, bits ? MB_MAX_WRITE_BITS : MB_MAX_WRITE_REGS, spans, MB_MAX_REQUESTS) < 0) {
                snprintf(err, err_size, "the values need more than %d write requests", MB_MAX_REQUESTS);
                ok = 0;
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
    return 1;
}
