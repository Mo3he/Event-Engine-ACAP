#ifndef _ENGINE_GPS_NMEA_H_
#define _ENGINE_GPS_NMEA_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GPS_NMEA_LINE_MAX 128

typedef enum {
    GPS_NMEA_IGNORED = 0,
    GPS_NMEA_UPDATED = 1,
    GPS_NMEA_BAD_FORMAT = -1,
    GPS_NMEA_BAD_CHECKSUM = -2
} gps_nmea_result_t;

typedef struct {
    double latitude;
    double longitude;
    double speed_knots;
    double speed_kmh;
    double heading;
    double altitude_m;
    double hdop;
    int satellites;
    int fix_quality;
    int has_position;
    int has_speed;
    int has_heading;
    int has_altitude;
    int has_fix;
    char utc[16];
    char date[8];
    unsigned long updates;
} gps_nmea_fix_t;

/* Parse one complete NMEA line into the accumulated state. Missing checksums
 * are accepted; a checksum that is present must be valid. */
gps_nmea_result_t gps_nmea_parse_line(const char* line, gps_nmea_fix_t* fix);

/* Convert NMEA ddmm.mmmm coordinates to signed decimal degrees. */
int gps_nmea_coordinate(const char* value, char hemisphere, int latitude,
                        double* degrees);

#ifdef __cplusplus
}
#endif
#endif
