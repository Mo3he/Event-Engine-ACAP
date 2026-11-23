#define _GNU_SOURCE
#include "gps.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <syslog.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <glib.h>

#define GPS_LOG(fmt, ...) syslog(LOG_INFO, "gps: " fmt, ##__VA_ARGS__)
#define GPS_WARN(fmt, ...) syslog(LOG_WARNING, "gps: " fmt, ##__VA_ARGS__)
#define GPS_RAW_LINES 20

typedef struct {
    int enabled;
    char source[24];
    char device[128];
    char host[128];
    int baud;
    int port;
    int stale_after_s;
} gps_config_t;

typedef struct {
    gps_nmea_fix_t fix;
    Gps_Fix_Callback callback;
} gps_dispatch_t;

static pthread_mutex_t lifecycle_lock = PTHREAD_MUTEX_INITIALIZER;
static GMutex state_lock;
static pthread_t worker;
static int worker_started;
static gint stop_worker;
static int initialized;
static gps_config_t current_config;
static Gps_Fix_Callback fix_callback;
static gps_snapshot_t current_state;
static int raw_next;
static gint64 last_fix_us;
static int have_fix_time;

static void set_state(const char* state, const char* error) {
    g_mutex_lock(&state_lock);
    g_snprintf(current_state.state, sizeof(current_state.state), "%s", state ? state : "unknown");
    if (error && error[0])
        g_snprintf(current_state.last_error, sizeof(current_state.last_error), "%s", error);
    else
        current_state.last_error[0] = '\0';
    g_mutex_unlock(&state_lock);
}

static void set_errno_state(const char* operation) {
    char error[160];
    g_snprintf(error, sizeof(error), "%s: %s", operation, strerror(errno));
    set_state("error", error);
    GPS_WARN("%s", error);
}

static int on_main_loop(gpointer data) {
    gps_dispatch_t* dispatch = data;
    if (dispatch->callback) dispatch->callback(&dispatch->fix);
    return G_SOURCE_REMOVE;
}

static void dispatch_fix(const gps_nmea_fix_t* fix) {
    gps_dispatch_t* dispatch = malloc(sizeof(*dispatch));
    if (!dispatch) {
        GPS_WARN("could not allocate fix dispatch");
        return;
    }
    dispatch->fix = *fix;
    dispatch->callback = fix_callback;
    g_idle_add_full(G_PRIORITY_DEFAULT, on_main_loop, dispatch, free);
}

static int baud_constant(int baud, speed_t* out) {
    switch (baud) {
        case 1200: *out = B1200; return 1;
        case 2400: *out = B2400; return 1;
        case 4800: *out = B4800; return 1;
        case 9600: *out = B9600; return 1;
        case 19200: *out = B19200; return 1;
        case 38400: *out = B38400; return 1;
        case 57600: *out = B57600; return 1;
        case 115200: *out = B115200; return 1;
        default: return 0;
    }
}

static int resolve_serial_device(const char* configured, char* out, size_t size) {
    if (configured && configured[0] && strcmp(configured, "auto") != 0) {
        g_snprintf(out, size, "%s", configured);
        return 1;
    }
    FILE* file = fopen("/usr/share/serial/serial_port.conf", "r");
    if (file) {
        char line[256];
        int serial_port_one = 0;
        while (fgets(line, sizeof(line), file)) {
            if (line[0] == '[') {
                serial_port_one = strcmp(line, "[SerialPort1]\n") == 0 ||
                                  strcmp(line, "[SerialPort1]") == 0;
            } else if (serial_port_one && strncmp(line, "Device=", 7) == 0) {
                char* value = line + 7;
                value[strcspn(value, "\r\n")] = '\0';
                if (value[0]) {
                    g_snprintf(out, size, "%s", value);
                    fclose(file);
                    return 1;
                }
            }
        }
        fclose(file);
    }
    g_snprintf(out, size, "/dev/ttyPCC1");
    return 0;
}

static int configure_serial(int fd, int baud) {
    struct termios options;
    if (tcgetattr(fd, &options) != 0) return -1;
    cfmakeraw(&options);
    options.c_cflag |= (CLOCAL | CREAD | CS8);
    options.c_cflag &= ~(CSTOPB | PARENB);
    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 1;
    if (baud) {
        speed_t speed;
        if (!baud_constant(baud, &speed)) {
            errno = EINVAL;
            return -1;
        }
        if (cfsetispeed(&options, speed) != 0 || cfsetospeed(&options, speed) != 0)
            return -1;
    }
    return tcsetattr(fd, TCSANOW, &options);
}

static int open_serial(const gps_config_t* cfg) {
    char path[160];
    resolve_serial_device(cfg->device, path, sizeof(path));
    int fd = open(path, O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        set_errno_state(path);
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    if (configure_serial(fd, cfg->baud) != 0) {
        set_errno_state("configure serial port");
        close(fd);
        return -1;
    }
    GPS_LOG("opened %s at %s baud", path, cfg->baud ? "configured" : "system");
    return fd;
}

static int open_udp(const gps_config_t* cfg) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)cfg->port);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

static int open_tcp_server(const gps_config_t* cfg) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)cfg->port);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(fd, 1) != 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

static int open_tcp_client(const gps_config_t* cfg) {
    struct addrinfo hints, *result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port[8];
    g_snprintf(port, sizeof(port), "%d", cfg->port);
    int rc = getaddrinfo(cfg->host, port, &hints, &result);
    if (rc != 0) {
        errno = EHOSTUNREACH;
        return -1;
    }
    int fd = -1;
    for (struct addrinfo* ai = result; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(fd);
            fd = -1;
            continue;
        }
        int connected = connect(fd, ai->ai_addr, ai->ai_addrlen) == 0;
        if (!connected && errno == EINPROGRESS) {
            fd_set writers;
            FD_ZERO(&writers);
            FD_SET(fd, &writers);
            struct timeval timeout = {3, 0};
            int ready = select(fd + 1, NULL, &writers, NULL, &timeout);
            int error = 0;
            socklen_t error_len = sizeof(error);
            connected = ready > 0 &&
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len) == 0 &&
                error == 0;
            if (!connected && ready > 0 && error) errno = error;
            else if (ready == 0) errno = ETIMEDOUT;
        }
        if (connected) {
            fcntl(fd, F_SETFL, flags);
            break;
        }
        if (fd >= 0) close(fd);
        fd = -1;
    }
    freeaddrinfo(result);
    return fd;
}

static void update_raw_line(const char* line) {
    g_mutex_lock(&state_lock);
    g_snprintf(current_state.raw_lines[raw_next], GPS_NMEA_LINE_MAX, "%s", line);
    raw_next = (raw_next + 1) % GPS_RAW_LINES;
    current_state.lines_received++;
    g_mutex_unlock(&state_lock);
}

static void parse_line(char* line) {
    if (!line[0]) return;
    update_raw_line(line);
    g_mutex_lock(&state_lock);
    gps_nmea_result_t result = gps_nmea_parse_line(line, &current_state.fix);
    if (result == GPS_NMEA_UPDATED) {
        current_state.sentences_parsed++;
        if (current_state.fix.has_position && current_state.fix.has_fix) {
            last_fix_us = g_get_monotonic_time();
            have_fix_time = 1;
        }
        gps_nmea_fix_t fix = current_state.fix;
        g_mutex_unlock(&state_lock);
        dispatch_fix(&fix);
        return;
    }
    if (result == GPS_NMEA_BAD_CHECKSUM) current_state.checksum_errors++;
    else if (result == GPS_NMEA_BAD_FORMAT) current_state.format_errors++;
    if (result < 0)
        g_snprintf(current_state.last_error, sizeof(current_state.last_error),
                   "%s", result == GPS_NMEA_BAD_CHECKSUM ? "Invalid NMEA checksum" : "Malformed NMEA sentence");
    g_mutex_unlock(&state_lock);
}

static void process_bytes(const unsigned char* data, size_t count,
                          char* line, size_t* line_len, int* overflow) {
    g_mutex_lock(&state_lock);
    current_state.bytes_received += count;
    g_mutex_unlock(&state_lock);
    for (size_t i = 0; i < count; i++) {
        unsigned char ch = data[i];
        if (ch == '\r' || ch == '\n') {
            if (!*overflow && *line_len > 0) {
                line[*line_len] = '\0';
                parse_line(line);
            }
            *line_len = 0;
            *overflow = 0;
        } else if (!*overflow) {
            if (*line_len + 1 >= GPS_NMEA_LINE_MAX) {
                *overflow = 1;
                *line_len = 0;
                g_mutex_lock(&state_lock);
                current_state.format_errors++;
                g_mutex_unlock(&state_lock);
            } else {
                line[(*line_len)++] = (char)ch;
            }
        }
    }
}

static int wait_readable(int fd, int timeout_s) {
    fd_set readers;
    FD_ZERO(&readers);
    FD_SET(fd, &readers);
    struct timeval timeout = { timeout_s, 0 };
    return select(fd + 1, &readers, NULL, NULL, &timeout);
}

static void interruptible_delay(int seconds) {
    for (int i = 0; i < seconds * 4 && !g_atomic_int_get(&stop_worker); i++)
        g_usleep(250000);
}

static gpointer gps_worker(gpointer unused) {
    (void)unused;
    gps_config_t cfg = current_config;
    char line[GPS_NMEA_LINE_MAX];
    size_t line_len = 0;
    int overflow = 0;
    int fd = -1;
    int listener = -1;

    while (!g_atomic_int_get(&stop_worker)) {
        if (fd < 0 && listener < 0) {
            errno = 0;
            if (strcmp(cfg.source, "serial") == 0) fd = open_serial(&cfg);
            else if (strcmp(cfg.source, "udp") == 0) fd = open_udp(&cfg);
            else if (strcmp(cfg.source, "tcp_client") == 0) fd = open_tcp_client(&cfg);
            else if (strcmp(cfg.source, "tcp_server") == 0) listener = open_tcp_server(&cfg);
            if (fd < 0 && listener < 0) {
                if (errno) set_errno_state("open GPS input");
                interruptible_delay(3);
                continue;
            }
            set_state("connected", NULL);
            GPS_LOG("input connected (%s)", cfg.source);
        }

        int read_fd = fd >= 0 ? fd : listener;
        int ready = wait_readable(read_fd, 1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            set_errno_state("select GPS input");
            if (fd >= 0) close(fd);
            if (listener >= 0) close(listener);
            fd = listener = -1;
            interruptible_delay(3);
            continue;
        }
        if (ready == 0) continue;

        if (fd < 0 && listener >= 0) {
            fd = accept(listener, NULL, NULL);
            if (fd < 0) {
                set_errno_state("accept GPS connection");
                continue;
            }
            set_state("connected", NULL);
            GPS_LOG("TCP GPS client connected");
            continue;
        }

        unsigned char buffer[1024];
        ssize_t got = strcmp(cfg.source, "udp") == 0
                    ? recvfrom(fd, buffer, sizeof(buffer), 0, NULL, NULL)
                    : read(fd, buffer, sizeof(buffer));
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            set_errno_state("read GPS input");
        }
        if (got <= 0) {
            if (strcmp(cfg.source, "udp") == 0) continue;
            if (fd >= 0) close(fd);
            fd = -1;
            if (listener < 0) set_state("waiting", "GPS input disconnected");
            line_len = 0;
            overflow = 0;
            continue;
        }
        process_bytes(buffer, (size_t)got, line, &line_len, &overflow);
    }

    if (fd >= 0) close(fd);
    if (listener >= 0) close(listener);
    return NULL;
}

static int config_from_json(cJSON* cfg, gps_config_t* out) {
    memset(out, 0, sizeof(*out));
    out->enabled = cJSON_IsTrue(cJSON_GetObjectItem(cfg, "enabled"));
    const char* source = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "source"));
    g_snprintf(out->source, sizeof(out->source), "%s", source && source[0] ? source : "serial");
    const char* device = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "device"));
    g_snprintf(out->device, sizeof(out->device), "%s", device && device[0] ? device : "auto");
    const char* host = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "host"));
    g_snprintf(out->host, sizeof(out->host), "%s", host ? host : "127.0.0.1");
    cJSON* baud = cJSON_GetObjectItem(cfg, "baud");
    out->baud = cJSON_IsNumber(baud) ? (int)baud->valuedouble : 0;
    cJSON* port = cJSON_GetObjectItem(cfg, "port");
    out->port = cJSON_IsNumber(port) ? (int)port->valuedouble : 5010;
    cJSON* stale = cJSON_GetObjectItem(cfg, "stale_after_s");
    out->stale_after_s = cJSON_IsNumber(stale) ? (int)stale->valuedouble : 5;

    if (strcmp(out->source, "serial") != 0 && strcmp(out->source, "udp") != 0 &&
        strcmp(out->source, "tcp_client") != 0 && strcmp(out->source, "tcp_server") != 0)
        return 0;
    if (out->port < 1 || out->port > 65535 ||
        out->stale_after_s < 1 || out->stale_after_s > 3600)
        return 0;
    if (out->baud != 0 && out->baud != 1200 && out->baud != 2400 &&
        out->baud != 4800 && out->baud != 9600 && out->baud != 19200 &&
        out->baud != 38400 && out->baud != 57600 && out->baud != 115200)
        return 0;
    if (strcmp(out->source, "tcp_client") == 0 && !out->host[0]) return 0;
    return 1;
}

int Gps_Reconfigure(cJSON* cfg) {
    gps_config_t next;
    if (!config_from_json(cfg, &next)) {
        GPS_WARN("rejected invalid GPS configuration");
        return 0;
    }

    pthread_mutex_lock(&lifecycle_lock);
    if (!initialized) {
        current_config = next;
        pthread_mutex_unlock(&lifecycle_lock);
        return 1;
    }
    if (worker_started) {
        g_atomic_int_set(&stop_worker, 1);
        pthread_join(worker, NULL);
        worker_started = 0;
    }
    current_config = next;
    g_mutex_lock(&state_lock);
    memset(&current_state, 0, sizeof(current_state));
    g_snprintf(current_state.source, sizeof(current_state.source), "%s", next.source);
    g_snprintf(current_state.state, sizeof(current_state.state), "%s",
               next.enabled ? "starting" : "disabled");
    current_state.enabled = next.enabled;
    current_state.baud = next.baud;
    current_state.port = next.port;
    current_state.stale_after_s = next.stale_after_s;
    raw_next = 0;
    last_fix_us = 0;
    have_fix_time = 0;
    g_mutex_unlock(&state_lock);
    g_atomic_int_set(&stop_worker, 0);
    if (next.enabled) {
        int rc = pthread_create(&worker, NULL, gps_worker, NULL);
        if (rc != 0) {
            errno = rc;
            set_errno_state("start GPS reader");
            pthread_mutex_unlock(&lifecycle_lock);
            return 0;
        }
        worker_started = 1;
    }
    pthread_mutex_unlock(&lifecycle_lock);
    return 1;
}

int Gps_Init(cJSON* cfg, Gps_Fix_Callback callback) {
    fix_callback = callback;
    initialized = 1;
    return Gps_Reconfigure(cfg);
}

void Gps_Cleanup(void) {
    if (!initialized) return;
    pthread_mutex_lock(&lifecycle_lock);
    if (worker_started) {
        g_atomic_int_set(&stop_worker, 1);
        pthread_join(worker, NULL);
        worker_started = 0;
    }
    set_state("disabled", NULL);
    initialized = 0;
    pthread_mutex_unlock(&lifecycle_lock);
}

void Gps_Get_Snapshot(gps_snapshot_t* snapshot) {
    if (!snapshot) return;
    g_mutex_lock(&state_lock);
    *snapshot = current_state;
    snapshot->raw_next = raw_next;
    gint64 last = last_fix_us;
    int has_time = have_fix_time;
    g_mutex_unlock(&state_lock);
    if (has_time) {
        snapshot->age_seconds = (double)(g_get_monotonic_time() - last) / G_USEC_PER_SEC;
        snapshot->fresh = snapshot->age_seconds <= snapshot->stale_after_s;
    } else {
        snapshot->age_seconds = -1.0;
        snapshot->fresh = 0;
    }
}

cJSON* Gps_Status_JSON(void) {
    gps_snapshot_t s;
    Gps_Get_Snapshot(&s);
    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "enabled", s.enabled);
    cJSON_AddStringToObject(result, "source", s.source);
    cJSON_AddStringToObject(result, "state", s.state);
    cJSON_AddStringToObject(result, "last_error", s.last_error);
    cJSON_AddNumberToObject(result, "baud", s.baud);
    cJSON_AddNumberToObject(result, "port", s.port);
    cJSON_AddNumberToObject(result, "stale_after_s", s.stale_after_s);
    cJSON_AddNumberToObject(result, "bytes_received", (double)s.bytes_received);
    cJSON_AddNumberToObject(result, "lines_received", (double)s.lines_received);
    cJSON_AddNumberToObject(result, "sentences_parsed", (double)s.sentences_parsed);
    cJSON_AddNumberToObject(result, "checksum_errors", (double)s.checksum_errors);
    cJSON_AddNumberToObject(result, "format_errors", (double)s.format_errors);
    cJSON_AddBoolToObject(result, "fresh", s.fresh);
    if (s.age_seconds >= 0.0) cJSON_AddNumberToObject(result, "age_seconds", s.age_seconds);
    cJSON_AddNumberToObject(result, "latitude", s.fix.latitude);
    cJSON_AddNumberToObject(result, "longitude", s.fix.longitude);
    cJSON_AddNumberToObject(result, "speed_knots", s.fix.speed_knots);
    cJSON_AddNumberToObject(result, "speed_kmh", s.fix.speed_kmh);
    cJSON_AddNumberToObject(result, "heading", s.fix.heading);
    cJSON_AddNumberToObject(result, "altitude_m", s.fix.altitude_m);
    cJSON_AddNumberToObject(result, "satellites", s.fix.satellites);
    cJSON_AddNumberToObject(result, "fix_quality", s.fix.fix_quality);
    cJSON_AddBoolToObject(result, "has_fix", s.fix.has_fix && s.fresh);
    cJSON_AddStringToObject(result, "utc", s.fix.utc);
    cJSON* raw = cJSON_CreateArray();
    for (int i = 0; i < GPS_RAW_LINES; i++) {
        int index = (s.raw_next + i) % GPS_RAW_LINES;
        if (s.raw_lines[index][0]) cJSON_AddItemToArray(raw, cJSON_CreateString(s.raw_lines[index]));
    }
    cJSON_AddItemToObject(result, "raw_lines", raw);
    return result;
}
