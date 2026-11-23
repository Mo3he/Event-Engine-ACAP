#include "gps_nmea.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define GPS_NMEA_FIELDS 24

static int parse_number(const char* s, double* out) {
    if (!s || !s[0]) return 0;
    errno = 0;
    char* end = NULL;
    double value = strtod(s, &end);
    if (errno || end == s || *end || !isfinite(value)) return 0;
    *out = value;
    return 1;
}

int gps_nmea_coordinate(const char* value, char hemisphere, int latitude,
                        double* degrees) {
    double raw;
    if (!degrees || !parse_number(value, &raw) || raw < 0.0) return 0;
    double whole = floor(raw / 100.0);
    double minutes = raw - whole * 100.0;
    if (minutes < 0.0 || minutes >= 60.0 ||
        (latitude && whole > 90.0) || (!latitude && whole > 180.0))
        return 0;
    double result = whole + minutes / 60.0;
    if ((latitude && whole == 90.0 && minutes != 0.0) ||
        (!latitude && whole == 180.0 && minutes != 0.0))
        return 0;
    if ((latitude && hemisphere == 'S') || (!latitude && hemisphere == 'W'))
        result = -result;
    else if ((latitude && hemisphere != 'N') || (!latitude && hemisphere != 'E'))
        return 0;
    *degrees = result;
    return 1;
}

static int valid_checksum(const char* line, size_t length, const char** body_end) {
    const char* star = memchr(line, '*', length);
    if (!star) {
        *body_end = line + length;
        return 1;
    }
    if ((size_t)(line + length - star) != 3 ||
        !isxdigit((unsigned char)star[1]) || !isxdigit((unsigned char)star[2]))
        return 0;
    unsigned char checksum = 0;
    for (const char* p = line + 1; p < star; p++) checksum ^= (unsigned char)*p;
    char expected[3] = { star[1], star[2], '\0' };
    if ((unsigned)strtoul(expected, NULL, 16) != checksum) return 0;
    *body_end = star;
    return 1;
}

static int parse_time(const char* value, char* out, size_t out_size) {
    if (!value || strlen(value) < 6) return 0;
    for (int i = 0; i < 6; i++)
        if (!isdigit((unsigned char)value[i])) return 0;
    if (value[6] && value[6] != '.') return 0;
    if (value[6] == '.') {
        for (const char* p = value + 7; *p; p++)
            if (!isdigit((unsigned char)*p)) return 0;
    }
    int hour = (value[0]-'0')*10 + value[1]-'0';
    int minute = (value[2]-'0')*10 + value[3]-'0';
    int second = (value[4]-'0')*10 + value[5]-'0';
    if (hour > 23 || minute > 59 || second > 60) return 0;
    snprintf(out, out_size, "%.6s", value);
    return 1;
}

static int split_fields(char* body, char** fields, int capacity) {
    int count = 0;
    fields[count++] = body;
    for (char* p = body; *p; p++) {
        if (*p == ',') {
            *p = '\0';
            if (count == capacity) return -1;
            fields[count++] = p + 1;
        }
    }
    return count;
}

static int update_rmc(char** f, int n, gps_nmea_fix_t* fix) {
    if (n < 10) return 0;
    char utc[16];
    if (!parse_time(f[1], utc, sizeof(utc))) return 0;
    fix->has_fix = (f[2][0] == 'A');
    fix->has_position = 0;
    if (fix->has_fix &&
        gps_nmea_coordinate(f[3], f[4][0], 1, &fix->latitude) &&
        gps_nmea_coordinate(f[5], f[6][0], 0, &fix->longitude))
        fix->has_position = 1;
    fix->has_fix = fix->has_fix && fix->has_position;

    double speed;
    fix->has_speed = parse_number(f[7], &speed) && speed >= 0.0;
    if (fix->has_speed) {
        fix->speed_knots = speed;
        fix->speed_kmh = speed * 1.852;
    }
    fix->has_heading = parse_number(f[8], &fix->heading);
    if (fix->has_heading && (fix->heading < 0.0 || fix->heading >= 360.0))
        fix->has_heading = 0;
    snprintf(fix->utc, sizeof(fix->utc), "%s", utc);
    if (strlen(f[9]) >= 6 && isdigit((unsigned char)f[9][0])) {
        memcpy(fix->date, f[9], 6);
        fix->date[6] = '\0';
    }
    return 1;
}

static int update_gga(char** f, int n, gps_nmea_fix_t* fix) {
    if (n < 10) return 0;
    char utc[16];
    double quality, sats, hdop;
    if (!parse_time(f[1], utc, sizeof(utc)) ||
        !parse_number(f[6], &quality) || quality < 0.0 ||
        !parse_number(f[7], &sats) || sats < 0.0 ||
        !parse_number(f[8], &hdop) || hdop < 0.0)
        return 0;
    fix->fix_quality = (int)quality;
    fix->satellites = (int)sats;
    fix->hdop = hdop;
    fix->has_fix = quality > 0.0;
    fix->has_position = 0;
    if (fix->has_fix &&
        gps_nmea_coordinate(f[2], f[3][0], 1, &fix->latitude) &&
        gps_nmea_coordinate(f[4], f[5][0], 0, &fix->longitude))
        fix->has_position = 1;
    fix->has_fix = fix->has_fix && fix->has_position;
    fix->has_altitude = parse_number(f[9], &fix->altitude_m);
    snprintf(fix->utc, sizeof(fix->utc), "%s", utc);
    return 1;
}

static int update_vtg(char** f, int n, gps_nmea_fix_t* fix) {
    if (n < 8) return 0;
    double heading, speed_knots, speed_kmh;
    int has_heading = parse_number(f[1], &heading) &&
                      heading >= 0.0 && heading < 360.0;
    int has_knots = parse_number(f[5], &speed_knots) && speed_knots >= 0.0;
    int has_kmh = parse_number(f[7], &speed_kmh) && speed_kmh >= 0.0;
    if (has_heading) {
        fix->heading = heading;
        fix->has_heading = 1;
    }
    if (has_kmh || has_knots) {
        fix->speed_kmh = has_kmh ? speed_kmh : speed_knots * 1.852;
        fix->speed_knots = has_knots ? speed_knots : speed_kmh / 1.852;
        fix->has_speed = 1;
    }
    return has_heading || has_kmh || has_knots;
}

gps_nmea_result_t gps_nmea_parse_line(const char* input, gps_nmea_fix_t* fix) {
    if (!input || !fix) return GPS_NMEA_BAD_FORMAT;
    size_t length = strcspn(input, "\r\n");
    if (length < 7 || length >= GPS_NMEA_LINE_MAX ||
        (input[0] != '$' && input[0] != '!'))
        return GPS_NMEA_BAD_FORMAT;

    char line[GPS_NMEA_LINE_MAX];
    memcpy(line, input, length);
    line[length] = '\0';
    const char* body_end = NULL;
    if (!valid_checksum(line, length, &body_end)) return GPS_NMEA_BAD_CHECKSUM;
    size_t body_length = (size_t)(body_end - line);
    line[body_length] = '\0';

    char* fields[GPS_NMEA_FIELDS];
    int count = split_fields(line, fields, GPS_NMEA_FIELDS);
    if (count <= 0 || strlen(fields[0]) != 6) return GPS_NMEA_BAD_FORMAT;
    for (int i = 1; i < 6; i++)
        if (!isalpha((unsigned char)fields[0][i])) return GPS_NMEA_BAD_FORMAT;

    const char* type = fields[0] + 3;
    int updated = 0;
    if (strcmp(type, "RMC") == 0) updated = update_rmc(fields, count, fix);
    else if (strcmp(type, "GGA") == 0) updated = update_gga(fields, count, fix);
    else if (strcmp(type, "VTG") == 0) updated = update_vtg(fields, count, fix);
    else return GPS_NMEA_IGNORED;

    if (!updated) return GPS_NMEA_BAD_FORMAT;
    fix->updates++;
    return GPS_NMEA_UPDATED;
}
