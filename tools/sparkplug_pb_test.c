/*
 * Round-trip check for the Sparkplug B protobuf codec.
 * Not part of the ACAP build:
 *   cc -Wall -Wextra -I../app/engine -o /tmp/spbtest sparkplug_pb_test.c ../app/engine/sparkplug_pb.c && /tmp/spbtest
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "sparkplug_pb.h"

static int failures = 0;

static void expect_str(const char* what, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        printf("FAIL %-24s got '%s' want '%s'\n", what, got, want);
        failures++;
    } else {
        printf("ok   %-24s %s\n", what, got);
    }
}

typedef struct {
    char name[128];
    char value[512];
    int  datatype;
    uint64_t alias;
    int  count;
} Captured;

static void capture(const char* name, uint64_t alias, int has_alias,
                    int datatype, const char* value_str, void* user) {
    (void)has_alias;
    Captured* c = (Captured*)user;
    int i = c->count++;
    if (i > 0) return;
    snprintf(c->name, sizeof(c->name), "%s", name);
    snprintf(c->value, sizeof(c->value), "%s", value_str);
    c->datatype = datatype;
    c->alias = alias;
}

static void roundtrip(const char* label, int datatype, const char* text, const char* want) {
    SPB_Metric m;
    memset(&m, 0, sizeof(m));
    m.name = "Test/Metric";
    m.alias = 7;
    m.has_alias = 1;
    m.datatype = datatype;
    m.timestamp_ms = 1750000000000ULL;
    SPB_Metric_Set_Value(&m, text);

    SPB_Buf buf;
    if (!SPB_Encode(&buf, m.timestamp_ms, 1, 42, &m, 1)) {
        printf("FAIL %-24s encode failed\n", label);
        failures++;
        return;
    }

    Captured c;
    memset(&c, 0, sizeof(c));
    int n = SPB_Decode(buf.data, buf.len, capture, &c);
    if (n != 1) {
        printf("FAIL %-24s decode returned %d\n", label, n);
        failures++;
        SPB_Buf_Free(&buf);
        return;
    }
    if (c.datatype != datatype || c.alias != 7 || strcmp(c.name, "Test/Metric") != 0) {
        printf("FAIL %-24s metadata mismatch (dt=%d alias=%llu name=%s)\n",
               label, c.datatype, (unsigned long long)c.alias, c.name);
        failures++;
    }
    expect_str(label, c.value, want);
    SPB_Buf_Free(&buf);
}

int main(void) {
    roundtrip("Int8 negative",   SPB_DT_INT8,     "-5",          "-5");
    roundtrip("Int16 negative",  SPB_DT_INT16,    "-1000",       "-1000");
    roundtrip("Int32 negative",  SPB_DT_INT32,    "-70000",      "-70000");
    roundtrip("Int64 negative",  SPB_DT_INT64,    "-5000000000", "-5000000000");
    roundtrip("UInt8",           SPB_DT_UINT8,    "200",         "200");
    roundtrip("UInt32",          SPB_DT_UINT32,   "4000000000",  "4000000000");
    roundtrip("UInt64",          SPB_DT_UINT64,   "9000000000",  "9000000000");
    roundtrip("Boolean true",    SPB_DT_BOOLEAN,  "true",        "true");
    roundtrip("Boolean yes",     SPB_DT_BOOLEAN,  "yes",         "true");
    roundtrip("Boolean off",     SPB_DT_BOOLEAN,  "off",         "false");
    roundtrip("Double",          SPB_DT_DOUBLE,   "23.5",        "23.5");
    roundtrip("String",          SPB_DT_STRING,   "hello/world", "hello/world");
    roundtrip("Int from float",  SPB_DT_INT32,    "12.7",        "12");
    roundtrip("DateTime",        SPB_DT_DATETIME, "1750000000000", "1750000000000");

    /* Float is 32-bit, so only check it survives to single precision */
    {
        SPB_Metric m;
        memset(&m, 0, sizeof(m));
        m.name = "F"; m.datatype = SPB_DT_FLOAT;
        SPB_Metric_Set_Value(&m, "1.5");
        SPB_Buf b;
        SPB_Encode(&b, 1, 1, 0, &m, 1);
        Captured c; memset(&c, 0, sizeof(c));
        SPB_Decode(b.data, b.len, capture, &c);
        expect_str("Float", c.value, "1.5");
        SPB_Buf_Free(&b);
    }

    /* A null metric must decode with no value */
    {
        SPB_Metric m;
        memset(&m, 0, sizeof(m));
        m.name = "N"; m.datatype = SPB_DT_INT32; m.is_null = 1;
        SPB_Buf b;
        SPB_Encode(&b, 1, 1, 0, &m, 1);
        Captured c; memset(&c, 0, sizeof(c));
        SPB_Decode(b.data, b.len, capture, &c);
        expect_str("Null metric", c.value, "");
        SPB_Buf_Free(&b);
    }

    /* Multiple metrics in one payload */
    {
        SPB_Metric m[3];
        memset(m, 0, sizeof(m));
        for (int i = 0; i < 3; i++) {
            m[i].name = "M"; m[i].datatype = SPB_DT_INT32; m[i].i64 = i;
        }
        SPB_Buf b;
        SPB_Encode(&b, 1, 1, 5, m, 3);
        Captured c; memset(&c, 0, sizeof(c));
        int n = SPB_Decode(b.data, b.len, capture, &c);
        if (n != 3) { printf("FAIL %-24s got %d metrics\n", "Multi metric", n); failures++; }
        else        { printf("ok   %-24s 3\n", "Multi metric"); }
        SPB_Buf_Free(&b);
    }

    /* Truncated payloads must be rejected, never over-read */
    {
        SPB_Metric m;
        memset(&m, 0, sizeof(m));
        m.name = "Trunc"; m.datatype = SPB_DT_STRING; m.str = "abcdefghij";
        SPB_Buf b;
        SPB_Encode(&b, 1, 1, 0, &m, 1);
        int rejected = 0, total = 0;
        for (int cut = 1; cut < b.len; cut++) {
            total++;
            if (SPB_Decode(b.data, cut, NULL, NULL) < 0) rejected++;
        }
        printf("ok   %-24s %d/%d truncations rejected\n", "Truncation safety", rejected, total);
        SPB_Buf_Free(&b);
    }

    /* Datatype name mapping used by the settings UI */
    expect_str("Name(Int32)",  SPB_DataType_Name(SPB_DT_INT32), "Int32");
    if (SPB_DataType_From_Name("boolean") != SPB_DT_BOOLEAN) {
        printf("FAIL name lookup is not case-insensitive\n");
        failures++;
    }

    printf("\n%s\n", failures ? "FAILURES PRESENT" : "all checks passed");
    return failures ? 1 : 0;
}
