#ifndef _ENGINE_GPS_H_
#define _ENGINE_GPS_H_

#include "../cJSON.h"
#include "gps_nmea.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*Gps_Fix_Callback)(const gps_nmea_fix_t* fix);

typedef struct {
    gps_nmea_fix_t fix;
    char source[24];
    char state[24];
    char last_error[160];
    char raw_lines[20][GPS_NMEA_LINE_MAX];
    unsigned long bytes_received;
    unsigned long lines_received;
    unsigned long sentences_parsed;
    unsigned long checksum_errors;
    unsigned long format_errors;
    double age_seconds;
    int fresh;
    int enabled;
    int baud;
    int port;
    int stale_after_s;
    int raw_next;
} gps_snapshot_t;

int  Gps_Init(cJSON* cfg, Gps_Fix_Callback callback);
int  Gps_Reconfigure(cJSON* cfg);
void Gps_Cleanup(void);
void Gps_Get_Snapshot(gps_snapshot_t* snapshot);
cJSON* Gps_Status_JSON(void); /* caller owns result */

#ifdef __cplusplus
}
#endif
#endif
