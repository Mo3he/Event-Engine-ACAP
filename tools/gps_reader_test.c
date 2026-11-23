#include "../app/engine/gps.h"

#include <arpa/inet.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <glib.h>

static int callback_count;

static void on_fix(const gps_nmea_fix_t* fix) {
    if (fix->has_fix) callback_count++;
}

static unsigned checksum(const char* body) {
    unsigned value = 0;
    for (const char* p = body + 1; *p; p++) value ^= (unsigned char)*p;
    return value;
}

static void append_sentence(char* out, size_t size, const char* body) {
    size_t used = strlen(out);
    snprintf(out + used, size - used, "%s*%02X\r\n", body, checksum(body));
}

int main(void) {
    int reserve = socket(AF_INET, SOCK_DGRAM, 0);
    if (reserve < 0) return 1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(reserve, (struct sockaddr*)&addr, sizeof(addr)) != 0) return 1;
    socklen_t addr_len = sizeof(addr);
    if (getsockname(reserve, (struct sockaddr*)&addr, &addr_len) != 0) return 1;
    int port = ntohs(addr.sin_port);
    close(reserve);

    cJSON* cfg = cJSON_CreateObject();
    cJSON_AddBoolToObject(cfg, "enabled", 1);
    cJSON_AddStringToObject(cfg, "source", "udp");
    cJSON_AddNumberToObject(cfg, "port", port);
    cJSON_AddNumberToObject(cfg, "stale_after_s", 1);
    if (!Gps_Init(cfg, on_fix)) {
        fprintf(stderr, "Gps_Init failed\n");
        cJSON_Delete(cfg);
        return 1;
    }
    cJSON_Delete(cfg);

    gps_snapshot_t snapshot;
    for (int i = 0; i < 30; i++) {
        Gps_Get_Snapshot(&snapshot);
        if (strcmp(snapshot.state, "connected") == 0) break;
        g_usleep(100000);
    }
    if (strcmp(snapshot.state, "connected") != 0) {
        fprintf(stderr, "UDP listener did not start: %s (%s)\n",
                snapshot.state, snapshot.last_error);
        Gps_Cleanup();
        return 1;
    }

    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    char payload[256] = "";
    append_sentence(payload, sizeof(payload),
        "$GNRMC,163503.00,A,6325.279162,N,01024.042748,E,12.5,23.6,020726,,,A,V");
    append_sentence(payload, sizeof(payload),
        "$GNGGA,163503.00,6325.279162,N,01024.042748,E,1,08,0.6,45.0,M,41.0,M,,");
    ssize_t sent = sendto(sender, payload, strlen(payload), 0,
                          (struct sockaddr*)&addr, sizeof(addr));
    close(sender);
    if (sent != (ssize_t)strlen(payload)) {
        fprintf(stderr, "UDP send failed\n");
        Gps_Cleanup();
        return 1;
    }

    for (int i = 0; i < 30; i++) {
        while (g_main_context_iteration(NULL, FALSE)) {}
        Gps_Get_Snapshot(&snapshot);
        if (snapshot.sentences_parsed >= 2 && callback_count >= 2) break;
        g_usleep(100000);
    }
    int ok = snapshot.sentences_parsed == 2 && snapshot.lines_received == 2 &&
             snapshot.bytes_received == (unsigned long)sent && snapshot.fix.has_fix &&
             snapshot.fix.has_position && snapshot.fix.satellites == 8 &&
             fabs(snapshot.fix.latitude - 63.4213193667) < 1e-7 &&
             fabs(snapshot.fix.speed_kmh - 23.15) < 1e-8 &&
             callback_count >= 2;
    if (!ok) {
        fprintf(stderr, "GPS UDP reader mismatch: lines=%lu parsed=%lu bytes=%lu "
                "fix=%d callbacks=%d error=%s\n",
                snapshot.lines_received, snapshot.sentences_parsed,
                snapshot.bytes_received, snapshot.fix.has_fix,
                callback_count, snapshot.last_error);
        Gps_Cleanup();
        return 1;
    }

    g_usleep(1200000);
    Gps_Get_Snapshot(&snapshot);
    ok = !snapshot.fresh;
    Gps_Cleanup();
    if (!ok) {
        fprintf(stderr, "GPS fix did not become stale\n");
        return 1;
    }
    puts("gps_reader_test: UDP input, multi-line framing, dispatch, and staleness passed");
    return 0;
}
