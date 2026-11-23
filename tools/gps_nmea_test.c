#include "../app/engine/gps_nmea.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

static void with_checksum(const char* sentence, char* out, size_t size) {
    unsigned char checksum = 0;
    for (const char* p = sentence + 1; *p; p++) checksum ^= (unsigned char)*p;
    snprintf(out, size, "%s*%02X\r\n", sentence, checksum);
}

int main(void) {
    gps_nmea_fix_t fix = {0};
    char line[GPS_NMEA_LINE_MAX];
    const char* rmc = "$GPRMC,163503.00,A,6325.279162,N,01024.042748,E,12.5,23.6,020726,0.3,W,A,V";
    with_checksum(rmc, line, sizeof(line));
    CHECK(gps_nmea_parse_line(line, &fix) == GPS_NMEA_UPDATED);
    CHECK(fix.has_fix && fix.has_position);
    CHECK(fabs(fix.latitude - 63.4213193667) < 1e-7);
    CHECK(fabs(fix.longitude - 10.4007124667) < 1e-7);
    CHECK(fabs(fix.speed_kmh - 23.15) < 1e-8);
    CHECK(fix.has_heading && fabs(fix.heading - 23.6) < 1e-8);

    const char* gga = "$GNGGA,163503.00,6325.279162,N,01024.042748,E,1,08,0.6,45.0,M,41.0,M,,";
    with_checksum(gga, line, sizeof(line));
    CHECK(gps_nmea_parse_line(line, &fix) == GPS_NMEA_UPDATED);
    CHECK(fix.fix_quality == 1 && fix.satellites == 8);
    CHECK(fix.has_altitude && fabs(fix.altitude_m - 45.0) < 1e-8);

    const char* vtg = "$GNVTG,123.4,T,,M,10.0,N,18.5,K";
    CHECK(gps_nmea_parse_line(vtg, &fix) == GPS_NMEA_UPDATED);
    CHECK(fix.has_speed && fabs(fix.speed_kmh - 18.5) < 1e-8);

    const char* invalid_fix = "$GPRMC,163503.00,V,6325.279162,N,01024.042748,E,0.0,23.6,020726,,,A,V";
    CHECK(gps_nmea_parse_line(invalid_fix, &fix) == GPS_NMEA_UPDATED);
    CHECK(!fix.has_fix && !fix.has_position);

    char bad[GPS_NMEA_LINE_MAX];
    snprintf(bad, sizeof(bad), "%s", line);
    size_t n = strlen(bad);
    bad[n - 3] = bad[n - 3] == '0' ? '1' : '0';
    CHECK(gps_nmea_parse_line(bad, &fix) == GPS_NMEA_BAD_CHECKSUM);

    const char* gn_rmc = "$GNRMC,163503.00,A,6325.279162,N,01024.042748,E,0.0,23.6,020726,,,A,V\n";
    CHECK(gps_nmea_parse_line(gn_rmc, &fix) == GPS_NMEA_UPDATED);
    const char* no_checksum = "$GPRMC,163503.00,A,6325.279162,N,01024.042748,E,0.0,23.6,020726,,,A,V\n";
    CHECK(gps_nmea_parse_line(no_checksum, &fix) == GPS_NMEA_UPDATED);
    const char* unknown = "$GPGSA,A,3,01,02,,,,,,,,,,,1.0,0.8,0.6\r\n";
    CHECK(gps_nmea_parse_line(unknown, &fix) == GPS_NMEA_IGNORED);
    CHECK(gps_nmea_coordinate("9000.0000", 'N', 1, &fix.latitude));
    CHECK(!gps_nmea_coordinate("9060.0000", 'N', 1, &fix.latitude));

    if (failures) return 1;
    puts("gps_nmea_test: all checks passed");
    return 0;
}
