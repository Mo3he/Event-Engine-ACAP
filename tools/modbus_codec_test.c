/*
 * Tests for the Modbus codec: data types, word orders, scaling, strings and
 * the validation of Modbus rule steps. Not part of the ACAP build:
 *   cc -std=c11 -Wall -Wextra -Iapp/engine -o /tmp/modbus_codec_test \
 *      tools/modbus_codec_test.c app/engine/modbus_codec.c app/cJSON.c -lm && /tmp/modbus_codec_test
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "modbus_codec.h"

static int failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static void expect_regs(const char* what, const uint16_t* got, const uint16_t* want, int n) {
    for (int i = 0; i < n; i++) {
        if (got[i] != want[i]) {
            printf("FAIL %s: reg %d is 0x%04X, want 0x%04X\n", what, i, got[i], want[i]);
            failures++;
            return;
        }
    }
}

static void test_orders(void) {
    /* float32 21.3 = 0x41AA6666 in every order */
    struct { const char* order; uint16_t regs[2]; } f32[] = {
        { "ABCD", { 0x41AA, 0x6666 } }, { "CDAB", { 0x6666, 0x41AA } },
        { "BADC", { 0xAA41, 0x6666 } }, { "DCBA", { 0x6666, 0xAA41 } },
    };
    for (int i = 0; i < 4; i++) {
        mb_order_t o = mb_order_parse(f32[i].order);
        double v = mb_decode_number(f32[i].regs, MB_T_FLOAT32, o);
        CHECK(v == 21.3, "float32 %s decodes to %.17g", f32[i].order, v);
        uint16_t out[2];
        CHECK(mb_encode_number(21.3, MB_T_FLOAT32, o, out) == 0, "encode float32 %s", f32[i].order);
        expect_regs(f32[i].order, out, f32[i].regs, 2);
    }

    /* 64-bit 0x0001020304050607 (exact in a double) */
    struct { const char* order; uint16_t regs[4]; } i64[] = {
        { "ABCD", { 0x0001, 0x0203, 0x0405, 0x0607 } },
        { "CDAB", { 0x0607, 0x0405, 0x0203, 0x0001 } },
        { "BADC", { 0x0100, 0x0302, 0x0504, 0x0706 } },
        { "DCBA", { 0x0706, 0x0504, 0x0302, 0x0100 } },
    };
    for (int i = 0; i < 4; i++) {
        uint16_t out[4];
        CHECK(mb_encode_number((double)0x0001020304050607LL, MB_T_INT64, mb_order_parse(i64[i].order), out) == 0,
              "encode int64 %s", i64[i].order);
        expect_regs(i64[i].order, out, i64[i].regs, 4);
    }

    uint16_t r16 = 0xFFFB;
    CHECK(mb_decode_number(&r16, MB_T_INT16, MB_O_ABCD) == -5, "int16 0xFFFB");
    CHECK(mb_decode_number(&r16, MB_T_UINT16, MB_O_ABCD) == 65531, "uint16 0xFFFB");
    uint16_t swapped = 0xFBFF;
    CHECK(mb_decode_number(&swapped, MB_T_INT16, MB_O_BADC) == -5, "int16 BADC");

    uint16_t u32[2] = { 0x0002, 0x0001 };
    CHECK(mb_decode_number(u32, MB_T_UINT32, MB_O_CDAB) == 65538, "uint32 CDAB");
    uint16_t i32[2] = { 0xFFFF, 0xFFFE };
    CHECK(mb_decode_number(i32, MB_T_INT32, MB_O_ABCD) == -2, "int32 -2");

    uint16_t f64[4] = { 0x3FF8, 0, 0, 0 };
    CHECK(mb_decode_number(f64, MB_T_FLOAT64, MB_O_ABCD) == 1.5, "float64 1.5");
}

static void test_round_trip(void) {
    const double samples[] = { 0, 1, -1, 123, -123.5, 32767, 65535, 1e6, -2.5e9, 3.25 };
    for (int t = MB_T_UINT16; t <= MB_T_FLOAT64; t++) {
        for (int o = MB_O_ABCD; o <= MB_O_DCBA; o++) {
            for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
                uint16_t regs[4];
                double v = samples[i];
                if (mb_encode_number(v, (mb_type_t)t, (mb_order_t)o, regs) != 0) continue;
                double back = mb_decode_number(regs, (mb_type_t)t, (mb_order_t)o);
                double want = (t == MB_T_FLOAT32 || t == MB_T_FLOAT64) ? v : round(v);
                CHECK(back == want, "%s order %d: %g -> %g", mb_type_name((mb_type_t)t), o, v, back);
            }
        }
    }
    uint16_t regs[4];
    CHECK(mb_encode_number(70000, MB_T_UINT16, MB_O_ABCD, regs) == -1, "uint16 70000 out of range");
    CHECK(mb_encode_number(-1, MB_T_UINT32, MB_O_ABCD, regs) == -1, "uint32 -1 out of range");
    CHECK(mb_encode_number(-40000, MB_T_INT16, MB_O_ABCD, regs) == -1, "int16 -40000 out of range");
    CHECK(mb_encode_number(NAN, MB_T_FLOAT32, MB_O_ABCD, regs) == -1, "NaN rejected");
    CHECK(mb_encode_number(1e39, MB_T_FLOAT32, MB_O_ABCD, regs) == -1, "float32 overflow");
    mb_field_t u16 = { .nregs = 1, .type = MB_T_UINT16, .scale = 1 };
    CHECK(mb_encode_field(-5, &u16, regs) == 0 && regs[0] == 0xFFFB, "uint16 write -5 as 0xFFFB");
    CHECK(mb_encode_field(-0.5, &u16, regs) == 0 && regs[0] == 0, "uint16 write -0.5 as legacy truncation");
    CHECK(mb_encode_field(-32768.9, &u16, regs) == 0 && regs[0] == 0x8000, "uint16 write negative fraction at signed boundary");
}

static void test_strings(void) {
    uint16_t regs[3];
    CHECK(mb_encode_string("HELLO", 3, MB_O_ABCD, MB_CS_ASCII, MB_FILL_NUL, regs, NULL) == 0, "encode HELLO");
    uint16_t want[3] = { 0x4845, 0x4C4C, 0x4F00 };
    expect_regs("string ABCD", regs, want, 3);
    char out[16];
    mb_decode_string(regs, 3, MB_O_ABCD, MB_CS_ASCII, out, sizeof(out));
    CHECK(strcmp(out, "HELLO") == 0, "decode HELLO got '%s'", out);

    CHECK(mb_encode_string("HELLO", 3, MB_O_BADC, MB_CS_ASCII, MB_FILL_NUL, regs, NULL) == 0, "encode HELLO BADC");
    uint16_t want_sw[3] = { 0x4548, 0x4C4C, 0x004F };
    expect_regs("string BADC", regs, want_sw, 3);
    mb_decode_string(regs, 3, MB_O_BADC, MB_CS_ASCII, out, sizeof(out));
    CHECK(strcmp(out, "HELLO") == 0, "decode HELLO BADC got '%s'", out);

    CHECK(mb_encode_string("TOO LONG", 3, MB_O_ABCD, MB_CS_ASCII, MB_FILL_NUL, regs, NULL) == -1, "string too long");

    uint16_t padded[3] = { 0x4F4B, 0x2020, 0x2020 };   /* "OK    " */
    mb_decode_string(padded, 3, MB_O_ABCD, MB_CS_ASCII, out, sizeof(out));
    CHECK(strcmp(out, "OK") == 0, "trailing spaces trimmed, got '%s'", out);
    uint16_t ctrl[1] = { 0x4101 };
    mb_decode_string(ctrl, 1, MB_O_ABCD, MB_CS_ASCII, out, sizeof(out));
    CHECK(strcmp(out, "A?") == 0, "control byte replaced, got '%s'", out);
    mb_decode_string(want, 3, MB_O_ABCD, MB_CS_ASCII, out, 4);
    CHECK(strcmp(out, "HEL") == 0, "truncated to buffer, got '%s'", out);

    /* The rest of the registers is NUL, whatever was there before */
    uint16_t pad[4] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
    CHECK(mb_encode_string("ABC", 4, MB_O_ABCD, MB_CS_ASCII, MB_FILL_NUL, pad, NULL) == 0, "encode ABC");
    uint16_t want_pad[4] = { 0x4142, 0x4300, 0x0000, 0x0000 };
    expect_regs("NUL padding", pad, want_pad, 4);
}

static void test_fill(void) {
    CHECK(mb_fill_parse(NULL) == MB_FILL_NUL && mb_fill_parse("") == MB_FILL_NUL, "fill defaults to nul");
    CHECK(mb_fill_parse("space_center") == MB_FILL_SPACE_CENTER, "fill space_center");
    CHECK(mb_fill_parse("spaces") == MB_FILL_INVALID, "unknown fill");

    struct { mb_fill_t fill; mb_order_t o; uint16_t want[3]; const char* name; } cases[] = {
        { MB_FILL_SPACE_LEFT,   MB_O_ABCD, { 0x4142, 0x4320, 0x2020 }, "space_left"   },   /* "ABC   " */
        { MB_FILL_SPACE_RIGHT,  MB_O_ABCD, { 0x2020, 0x2041, 0x4243 }, "space_right"  },   /* "   ABC" */
        { MB_FILL_SPACE_CENTER, MB_O_ABCD, { 0x2041, 0x4243, 0x2020 }, "space_center" },   /* " ABC  " */
        { MB_FILL_SPACE_RIGHT,  MB_O_BADC, { 0x2020, 0x4120, 0x4342 }, "space_right BADC" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint16_t regs[3] = { 0xFFFF, 0xFFFF, 0xFFFF };
        CHECK(mb_encode_string("ABC", 3, cases[i].o, MB_CS_ASCII, cases[i].fill, regs, NULL) == 0, "encode %s", cases[i].name);
        expect_regs(cases[i].name, regs, cases[i].want, 3);
    }
    /* Centring an even gap splits it evenly; full-length text has no fill */
    uint16_t regs[3];
    CHECK(mb_encode_string("AB", 2, MB_O_ABCD, MB_CS_ASCII, MB_FILL_SPACE_CENTER, regs, NULL) == 0, "encode AB centred");
    uint16_t want_even[2] = { 0x2041, 0x4220 };
    expect_regs("space_center even", regs, want_even, 2);
    CHECK(mb_encode_string("ABCDEF", 3, MB_O_ABCD, MB_CS_ASCII, MB_FILL_SPACE_RIGHT, regs, NULL) == 0, "encode full length");
    uint16_t want_full[3] = { 0x4142, 0x4344, 0x4546 };
    expect_regs("full length right", regs, want_full, 3);
    CHECK(mb_encode_string("ABCDEFG", 3, MB_O_ABCD, MB_CS_ASCII, MB_FILL_SPACE_LEFT, regs, NULL) == -1, "too long with fill");
    /* Bytes, not characters: "ż" is 2 bytes in UTF-8, so "żA" leaves 1 */
    CHECK(mb_encode_string("\xC5\xBC" "A", 2, MB_O_ABCD, MB_CS_UTF8, MB_FILL_SPACE_RIGHT, regs, NULL) == 0, "encode UTF-8 right");
    uint16_t want_utf8[2] = { 0x20C5, 0xBC41 };
    expect_regs("UTF-8 right", regs, want_utf8, 2);
    /* An empty text fills the registers */
    CHECK(mb_encode_string("", 2, MB_O_ABCD, MB_CS_ASCII, MB_FILL_SPACE_CENTER, regs, NULL) == 0, "encode empty");
    uint16_t want_empty[2] = { 0x2020, 0x2020 };
    expect_regs("empty with spaces", regs, want_empty, 2);
    /* Right-aligned text reads back with its leading spaces */
    CHECK(mb_encode_string("ABC", 3, MB_O_ABCD, MB_CS_ASCII, MB_FILL_SPACE_RIGHT, regs, NULL) == 0, "encode for read back");
    char out[16];
    mb_decode_string(regs, 3, MB_O_ABCD, MB_CS_ASCII, out, sizeof(out));
    CHECK(strcmp(out, "   ABC") == 0, "right-aligned reads back as '%s'", out);
}

static void test_encodings(void) {
    const char* pl = "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87";   /* "Zażółć" in UTF-8 */
    struct { mb_charset_t cs; const char* bytes; int n; } cases[] = {
        { MB_CS_CP1250, "Za\xBF\xF3\xB3\xE6", 6 },
        { MB_CS_LATIN2, "Za\xBF\xF3\xB3\xE6", 6 },
        { MB_CS_UTF8,   "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87", 10 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t buf[16];
        int bad = -1;
        size_t n = mb_text_encode(pl, cases[i].cs, buf, sizeof(buf), &bad, NULL);
        CHECK(n == (size_t)cases[i].n && bad == 0 && memcmp(buf, cases[i].bytes, n) == 0,
              "%s encodes to %d bytes, bad %d", mb_charset_name(cases[i].cs), (int)n, bad);
        uint16_t regs[8];
        CHECK(mb_encode_string(pl, 8, MB_O_ABCD, cases[i].cs, MB_FILL_NUL, regs, NULL) == 0, "encode %s", mb_charset_name(cases[i].cs));
        char out[64];
        mb_decode_string(regs, 8, MB_O_ABCD, cases[i].cs, out, sizeof(out));
        CHECK(strcmp(out, pl) == 0, "%s round trip got '%s'", mb_charset_name(cases[i].cs), out);
    }
    /* Latin-2 and Windows-1250 differ for some letters: Ś is 0xA6 / 0x8C */
    uint8_t b;
    mb_text_encode("\xC5\x9A", MB_CS_LATIN2, &b, 1, NULL, NULL);
    CHECK(b == 0xA6, "latin2 S-acute 0x%02X", b);
    mb_text_encode("\xC5\x9A", MB_CS_CP1250, &b, 1, NULL, NULL);
    CHECK(b == 0x8C, "cp1250 S-acute 0x%02X", b);
    mb_text_encode("\xE2\x82\xAC", MB_CS_CP1252, &b, 1, NULL, NULL);
    CHECK(b == 0x80, "cp1252 euro 0x%02X", b);

    int bad = 0;
    uint32_t first = 0;
    uint8_t buf[16];
    size_t n = mb_text_encode(pl, MB_CS_ASCII, buf, sizeof(buf), &bad, &first);
    CHECK(n == 6 && bad == 4 && first == 0x17C && memcmp(buf, "Za????", 6) == 0, "ascii replaces, bad %d first U+%04X", bad, first);
    n = mb_text_encode("a\xFF" "b", MB_CS_UTF8, buf, sizeof(buf), &bad, &first);
    CHECK(n == 3 && bad == 1 && first == 0xFFFD && memcmp(buf, "a?b", 3) == 0, "invalid UTF-8 input");
    /* The byte length counts against the registers: 6 characters, 10 bytes */
    uint16_t regs[5];
    CHECK(mb_encode_string(pl, 4, MB_O_ABCD, MB_CS_UTF8, MB_FILL_NUL, regs, NULL) == -1, "utf-8 too long for 4 registers");
    CHECK(mb_encode_string(pl, 5, MB_O_ABCD, MB_CS_UTF8, MB_FILL_NUL, regs, NULL) == 0, "utf-8 fits 5 registers");

    char out[32];
    uint16_t raw[2] = { 0x41C5, 0x0000 };   /* 'A' then a lone UTF-8 lead byte */
    mb_decode_string(raw, 2, MB_O_ABCD, MB_CS_UTF8, out, sizeof(out));
    CHECK(strcmp(out, "A?") == 0, "invalid UTF-8 read as '?', got '%s'", out);
    mb_decode_string(raw, 2, MB_O_ABCD, MB_CS_ASCII, out, sizeof(out));
    CHECK(strcmp(out, "A?") == 0, "ascii read replaces 0xC5, got '%s'", out);
    mb_decode_string(raw, 2, MB_O_ABCD, MB_CS_LATIN1, out, sizeof(out));
    CHECK(strcmp(out, "A\xC3\x85") == 0, "latin1 0xC5 is A-ring, got '%s'", out);
    /* A multi-byte character that doesn't fit the buffer is left out whole */
    uint16_t zz[2] = { 0x5ABF, 0x0000 };   /* "Zż" in Windows-1250 */
    mb_decode_string(zz, 2, MB_O_ABCD, MB_CS_CP1250, out, 3);
    CHECK(strcmp(out, "Z") == 0, "no partial UTF-8, got '%s'", out);

    CHECK(mb_charset_parse(NULL) == MB_CS_UTF8, "default utf-8");
    CHECK(mb_charset_parse("CP1250") == MB_CS_CP1250, "alias cp1250");
    CHECK(mb_charset_parse("latin9") == MB_CS_INVALID, "unknown encoding");
}

static int spans_are(const mb_span_t* got, int n, const int* want, int want_n) {
    if (n != want_n) return 0;
    for (int i = 0; i < n; i++)
        if (got[i].address != want[2 * i] || got[i].count != want[2 * i + 1]) return 0;
    return 1;
}

static void test_plans(void) {
    mb_span_t sp[MB_MAX_REQUESTS];
    /* Reads: nearby fields share a request, gaps included, up to 125 */
    mb_field_t r1[] = { { .reg = 10, .nregs = 2 }, { .reg = 0, .nregs = 1 }, { .reg = 100, .nregs = 2 } };
    int n = mb_plan_reads(r1, 3, MB_MAX_READ_REGS, sp, MB_MAX_REQUESTS);
    CHECK(spans_are(sp, n, (int[]){ 0, 102 }, 1), "one read, got %d spans", n);
    mb_field_t r2[] = { { .reg = 0, .nregs = 2 }, { .reg = 200, .nregs = 4 } };
    n = mb_plan_reads(r2, 2, MB_MAX_READ_REGS, sp, MB_MAX_REQUESTS);
    CHECK(spans_are(sp, n, (int[]){ 0, 2, 200, 4 }, 2), "two reads, got %d spans", n);
    mb_field_t r3[] = { { .reg = 5, .nregs = 125 }, { .reg = 130, .nregs = 125 } };
    n = mb_plan_reads(r3, 2, MB_MAX_READ_REGS, sp, MB_MAX_REQUESTS);
    CHECK(spans_are(sp, n, (int[]){ 5, 125, 130, 125 }, 2), "two full strings, got %d spans", n);
    n = mb_plan_reads(r3, 2, 100, sp, MB_MAX_REQUESTS);
    CHECK(spans_are(sp, n, (int[]){ 5, 100, 105, 25, 130, 100, 230, 25 }, 4), "split fields, got %d spans", n);
    CHECK(mb_plan_reads(r3, 2, 10, sp, 4) == -1, "too many read requests");

    /* Writes: adjacent values share a request; a value is split only when
     * it is longer than one request */
    mb_field_t w1[] = { { .reg = 0, .nregs = 60 }, { .reg = 60, .nregs = 60 }, { .reg = 120, .nregs = 4 } };
    n = mb_plan_writes(w1, 3, MB_MAX_WRITE_REGS, sp, MB_MAX_REQUESTS);
    CHECK(spans_are(sp, n, (int[]){ 0, 120, 120, 4 }, 2), "float64 not split, got %d spans", n);
    mb_field_t w2[] = { { .reg = 0, .nregs = 1 }, { .reg = 1, .nregs = 125 } };
    n = mb_plan_writes(w2, 2, MB_MAX_WRITE_REGS, sp, MB_MAX_REQUESTS);
    CHECK(spans_are(sp, n, (int[]){ 0, 1, 1, 123, 124, 2 }, 3), "long string split, got %d spans", n);
    mb_field_t w3[] = { { .reg = 0, .nregs = 2 }, { .reg = 2, .nregs = 2 }, { .reg = 10, .nregs = 1 } };
    n = mb_plan_writes(w3, 3, MB_MAX_WRITE_REGS, sp, MB_MAX_REQUESTS);
    CHECK(spans_are(sp, n, (int[]){ 0, 4, 10, 1 }, 2), "gap starts a request, got %d spans", n);
}

static void test_scaling(void) {
    CHECK(mb_scale(213, 0.01, 0, -1) == 2.13, "213 * 0.01 = %.17g", mb_scale(213, 0.01, 0, -1));
    CHECK(mb_scale(21.456, 1, 0, 1) == 21.5, "decimals 1");
    CHECK(mb_scale(1000, 0.1, -40, -1) == 60, "offset");
    CHECK(mb_unscale(60, 0.1, -40) == 1000, "unscale");
    CHECK(strcmp(mb_exception_name(2), "Illegal Data Address") == 0, "exception 2");
    char buf[32];
    mb_format_number(2.5, 2, buf, sizeof(buf));
    CHECK(strcmp(buf, "2.50") == 0, "format decimals got %s", buf);
    mb_format_number(2.13, -1, buf, sizeof(buf));
    CHECK(strcmp(buf, "2.13") == 0, "format shortest got %s", buf);
}

static int valid(int (*fn)(cJSON*, char*, size_t), const char* json, const char* want_err) {
    cJSON* j = cJSON_Parse(json);
    char err[256] = "";
    int ok = fn(j, err, sizeof(err));
    cJSON_Delete(j);
    if (want_err && (ok || !strstr(err, want_err))) {
        printf("FAIL %s: expected error containing '%s', got %s '%s'\n", json, want_err, ok ? "valid" : "error", err);
        failures++;
    } else if (!want_err && !ok) {
        printf("FAIL %s: expected valid, got '%s'\n", json, err);
        failures++;
    }
    return ok;
}

static void test_validation(void) {
    valid(mb_validate_read, "{\"register\":0}", NULL);
    valid(mb_validate_read, "{\"register\":0,\"data_type\":\"float32\",\"word_order\":\"CDAB\",\"scale\":\"0.1\"}", NULL);
    valid(mb_validate_read, "{\"data_type\":\"float16\"}", "unknown data type");
    valid(mb_validate_read, "{\"word_order\":\"XYZW\"}", "unknown word order");
    valid(mb_validate_read, "{\"scale\":0}", "scale");
    valid(mb_validate_read, "{\"register\":65535,\"data_type\":\"uint32\"}", "past 65535");
    valid(mb_validate_read, "{\"data_type\":\"string\",\"op\":\"gt\"}", "threshold needs a number");
    valid(mb_validate_read, "{\"register_type\":\"coil\",\"data_type\":\"float32\",\"op\":\"gt\"}", NULL);
    valid(mb_validate_read,
          "{\"map\":[{\"register\":0,\"type\":\"float32\",\"order\":\"CDAB\",\"var\":\"t\"},"
          "{\"register\":2,\"type\":\"int16\",\"scale\":0.1,\"offset\":-5,\"var\":\"p\"}],\"compare\":\"p\",\"op\":\"gt\"}", NULL);
    valid(mb_validate_read, "{\"map\":[{\"register\":0}]}", "var");
    valid(mb_validate_read, "{\"map\":[{\"var\":\"a\"}]}", "register is required");
    valid(mb_validate_read, "{\"map\":[{\"register\":0,\"var\":\"a\"},{\"register\":1,\"var\":\"a\"}]}", "used twice");
    valid(mb_validate_read, "{\"map\":[{\"register\":0,\"var\":\"a\"},{\"register\":200,\"var\":\"b\"}]}", NULL);
    valid(mb_validate_read, "{\"map\":[{\"register\":0,\"var\":\"a\"},{\"register\":2000,\"var\":\"b\"}]}", "spans 2001 registers");
    valid(mb_validate_read, "{\"map\":[{\"register\":0,\"type\":\"string\",\"length\":125,\"var\":\"a\"},"
                            "{\"register\":125,\"type\":\"string\",\"length\":125,\"var\":\"b\"}]}", NULL);
    valid(mb_validate_read, "{\"data_type\":\"string\",\"encoding\":\"windows-1250\"}", NULL);
    valid(mb_validate_read, "{\"data_type\":\"string\",\"encoding\":\"ebcdic\"}", "unknown encoding");
    valid(mb_validate_read, "{\"register_type\":\"coil\",\"map\":[{\"register\":0,\"var\":\"a\"},{\"register\":200,\"var\":\"b\"}]}", NULL);
    valid(mb_validate_read, "{\"map\":[{\"register\":0,\"var\":\"a\"}],\"compare\":\"b\"}", "compare 'b'");
    valid(mb_validate_read, "{\"map\":[{\"register\":0,\"type\":\"string\",\"var\":\"a\"}],\"op\":\"gt\"}", "string");

    valid(mb_validate_write, "{\"register\":10,\"value\":1}", NULL);
    valid(mb_validate_write, "{\"register\":10,\"value\":70000}", "out of range");
    valid(mb_validate_write, "{\"register\":10,\"value\":-5}", NULL);
    valid(mb_validate_write, "{\"register\":10,\"value\":-0.5}", NULL);
    valid(mb_validate_write, "{\"register\":10,\"value\":-32768.9}", NULL);
    valid(mb_validate_write, "{\"register\":10,\"value\":-40000}", "out of range");
    valid(mb_validate_write, "{\"register\":10,\"value\":\"{{var.sp}}\"}", NULL);
    valid(mb_validate_write, "{\"register\":10,\"value\":\"abc\"}", "not a number");
    valid(mb_validate_write, "{\"register\":10,\"value\":700,\"scale\":0.1}", NULL);
    valid(mb_validate_write, "{\"register\":10,\"value\":7000,\"scale\":0.1}", "out of range");
    valid(mb_validate_write, "{\"data_type\":\"float32\",\"function\":\"fc06\",\"value\":1}", "fc06 writes one register");
    valid(mb_validate_write, "{\"register_type\":\"coil\",\"function\":\"fc16\"}", "function must be");
    valid(mb_validate_write, "{\"data_type\":\"string\",\"length\":2,\"value\":\"HELLO\"}", "text is 5 bytes in utf-8");
    valid(mb_validate_write, "{\"data_type\":\"string\",\"length\":2,\"value\":\"{{trigger.text}}\"}", NULL);
    valid(mb_validate_write,
          "{\"values\":[{\"register\":0,\"value\":1},{\"register\":1,\"type\":\"float32\",\"value\":2.5},"
          "{\"register\":10,\"value\":\"{{var.x}}\"}]}", NULL);
    valid(mb_validate_write, "{\"values\":[{\"register\":0,\"type\":\"uint32\",\"value\":1},{\"register\":1,\"value\":2}]}", "overlap");
    valid(mb_validate_write, "{\"values\":[{\"register\":0}]}", "value is required");
    valid(mb_validate_write, "{\"function\":\"fc06\",\"values\":[{\"register\":0,\"value\":1},{\"register\":1,\"value\":2}]}", "fc06");
    valid(mb_validate_write,
          "{\"values\":[{\"register\":0,\"type\":\"string\",\"length\":120,\"value\":\"\"},"
          "{\"register\":120,\"type\":\"uint64\",\"value\":1}]}", NULL);
    valid(mb_validate_write, "{\"data_type\":\"string\",\"length\":125,\"value\":\"\"}", NULL);
    valid(mb_validate_write, "{\"values\":[{\"register\":0,\"type\":\"string\",\"length\":125,\"value\":\"\"}]}", NULL);
    valid(mb_validate_write, "{\"data_type\":\"string\",\"length\":126,\"value\":\"\"}", "1 to 125");

    /* Empty values: numbers and coils need one, text may be empty */
    valid(mb_validate_write, "{\"register\":10,\"value\":\"\"}", "value is required");
    valid(mb_validate_write, "{\"register\":10,\"value\":\" \"}", "value is required");
    valid(mb_validate_write, "{\"register\":10}", "value is required");
    valid(mb_validate_write, "{\"register_type\":\"coil\",\"value\":\"\"}", "value is required");
    valid(mb_validate_write, "{\"register_type\":\"coil\",\"value\":\"ON\"}", NULL);
    valid(mb_validate_write, "{\"register_type\":\"coil\",\"value\":\"maybe\"}", "not on/off");
    valid(mb_validate_write, "{\"values\":[{\"register\":0,\"value\":\"\"}]}", "value is required");
    valid(mb_validate_write, "{\"data_type\":\"string\",\"value\":\"\"}", NULL);
    valid(mb_validate_write, "{\"register\":10,\"value\":\" 12 \"}", NULL);

    /* Encodings: characters and byte length are checked in the chosen one */
    valid(mb_validate_write, "{\"data_type\":\"string\",\"length\":3,\"encoding\":\"windows-1250\","
                             "\"value\":\"Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87\"}", NULL);
    valid(mb_validate_write, "{\"data_type\":\"string\",\"length\":3,\"encoding\":\"utf-8\","
                             "\"value\":\"Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87\"}", "10 bytes in utf-8");
    valid(mb_validate_write, "{\"data_type\":\"string\",\"length\":8,\"encoding\":\"ascii\","
                             "\"value\":\"Za\xC5\xBC\"}", "can't be written in ascii");
    valid(mb_validate_write, "{\"register_type\":\"coil\",\"values\":[{\"register\":0,\"value\":1},{\"register\":1,\"value\":0}]}", NULL);

    /* Fill */
    valid(mb_validate_write, "{\"data_type\":\"string\",\"fill\":\"space_right\",\"value\":\"A\"}", NULL);
    valid(mb_validate_write, "{\"values\":[{\"register\":0,\"type\":\"string\",\"fill\":\"space_center\",\"value\":\"A\"}]}", NULL);
    valid(mb_validate_write, "{\"data_type\":\"string\",\"fill\":\"spaces\",\"value\":\"A\"}", "unknown fill");
    valid(mb_validate_write, "{\"data_type\":\"string\",\"length\":2,\"fill\":\"space_left\",\"value\":\"HELLO\"}", "text is 5 bytes");
}

int main(void) {
    test_orders();
    test_round_trip();
    test_strings();
    test_fill();
    test_encodings();
    test_plans();
    test_scaling();
    test_validation();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all Modbus codec tests passed\n");
    return 0;
}
