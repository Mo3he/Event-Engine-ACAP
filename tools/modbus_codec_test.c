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
}

static void test_strings(void) {
    uint16_t regs[3];
    CHECK(mb_encode_string("HELLO", 3, MB_O_ABCD, regs) == 0, "encode HELLO");
    uint16_t want[3] = { 0x4845, 0x4C4C, 0x4F00 };
    expect_regs("string ABCD", regs, want, 3);
    char out[16];
    mb_decode_string(regs, 3, MB_O_ABCD, out, sizeof(out));
    CHECK(strcmp(out, "HELLO") == 0, "decode HELLO got '%s'", out);

    CHECK(mb_encode_string("HELLO", 3, MB_O_BADC, regs) == 0, "encode HELLO BADC");
    uint16_t want_sw[3] = { 0x4548, 0x4C4C, 0x004F };
    expect_regs("string BADC", regs, want_sw, 3);
    mb_decode_string(regs, 3, MB_O_BADC, out, sizeof(out));
    CHECK(strcmp(out, "HELLO") == 0, "decode HELLO BADC got '%s'", out);

    CHECK(mb_encode_string("TOO LONG", 3, MB_O_ABCD, regs) == -1, "string too long");

    uint16_t padded[3] = { 0x4F4B, 0x2020, 0x2020 };   /* "OK    " */
    mb_decode_string(padded, 3, MB_O_ABCD, out, sizeof(out));
    CHECK(strcmp(out, "OK") == 0, "trailing spaces trimmed, got '%s'", out);
    uint16_t ctrl[1] = { 0x4101 };
    mb_decode_string(ctrl, 1, MB_O_ABCD, out, sizeof(out));
    CHECK(strcmp(out, "A?") == 0, "control byte replaced, got '%s'", out);
    mb_decode_string(want, 3, MB_O_ABCD, out, 4);
    CHECK(strcmp(out, "HEL") == 0, "truncated to buffer, got '%s'", out);
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
    valid(mb_validate_read, "{\"map\":[{\"register\":0,\"var\":\"a\"},{\"register\":200,\"var\":\"b\"}]}", "spans 201 registers");
    valid(mb_validate_read, "{\"register_type\":\"coil\",\"map\":[{\"register\":0,\"var\":\"a\"},{\"register\":200,\"var\":\"b\"}]}", NULL);
    valid(mb_validate_read, "{\"map\":[{\"register\":0,\"var\":\"a\"}],\"compare\":\"b\"}", "compare 'b'");
    valid(mb_validate_read, "{\"map\":[{\"register\":0,\"type\":\"string\",\"var\":\"a\"}],\"op\":\"gt\"}", "string");

    valid(mb_validate_write, "{\"register\":10,\"value\":1}", NULL);
    valid(mb_validate_write, "{\"register\":10,\"value\":70000}", "out of range");
    valid(mb_validate_write, "{\"register\":10,\"value\":\"{{var.sp}}\"}", NULL);
    valid(mb_validate_write, "{\"register\":10,\"value\":\"abc\"}", "not a number");
    valid(mb_validate_write, "{\"register\":10,\"value\":700,\"scale\":0.1}", NULL);
    valid(mb_validate_write, "{\"register\":10,\"value\":7000,\"scale\":0.1}", "out of range");
    valid(mb_validate_write, "{\"data_type\":\"float32\",\"function\":\"fc06\",\"value\":1}", "fc06 writes one register");
    valid(mb_validate_write, "{\"register_type\":\"coil\",\"function\":\"fc16\"}", "function must be");
    valid(mb_validate_write, "{\"data_type\":\"string\",\"length\":2,\"value\":\"HELLO\"}", "text is 5 characters");
    valid(mb_validate_write, "{\"data_type\":\"string\",\"length\":2,\"value\":\"{{trigger.text}}\"}", NULL);
    valid(mb_validate_write,
          "{\"values\":[{\"register\":0,\"value\":1},{\"register\":1,\"type\":\"float32\",\"value\":2.5},"
          "{\"register\":10,\"value\":\"{{var.x}}\"}]}", NULL);
    valid(mb_validate_write, "{\"values\":[{\"register\":0,\"type\":\"uint32\",\"value\":1},{\"register\":1,\"value\":2}]}", "overlap");
    valid(mb_validate_write, "{\"values\":[{\"register\":0}]}", "value is required");
    valid(mb_validate_write, "{\"function\":\"fc06\",\"values\":[{\"register\":0,\"value\":1},{\"register\":1,\"value\":2}]}", "fc06");
    valid(mb_validate_write,
          "{\"values\":[{\"register\":0,\"type\":\"string\",\"length\":120,\"value\":\"\"},"
          "{\"register\":120,\"type\":\"uint64\",\"value\":1}]}", "at most 123");
    valid(mb_validate_write, "{\"register_type\":\"coil\",\"values\":[{\"register\":0,\"value\":1},{\"register\":1,\"value\":0}]}", NULL);
}

int main(void) {
    test_orders();
    test_round_trip();
    test_strings();
    test_scaling();
    test_validation();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all Modbus codec tests passed\n");
    return 0;
}
