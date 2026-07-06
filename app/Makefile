PROG1 = acap_event_engine
OBJS1 = main.c ACAP.c cJSON.c \
        engine/rule_engine.c \
        engine/triggers.c \
        engine/conditions.c \
        engine/actions.c \
        engine/scheduler.c \
        engine/variables.c \
        engine/event_log.c \
        engine/mqtt_client.c \
        engine/alert_stream.c \
        engine/modbus_pool.c
PROGS = $(PROG1)

PKGS = glib-2.0 gio-2.0 axevent fcgi
CFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(PKGS))
CFLAGS += -fstack-protector-strong -D_FORTIFY_SOURCE=2
# Cap the GLib API version so headers never emit symbols newer than the target
# firmware's GLib (e.g. g_string_free_and_steal, added in GLib 2.76). Building
# against the OS 13 SDK (Ubuntu 24.04 / GLib 2.80) would otherwise produce a
# binary that fails at runtime on older firmware with:
#   undefined symbol: g_string_free_and_steal
CFLAGS += -DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_66 -DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_66
LDLIBS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs $(PKGS))
LDLIBS += -s -lm -ldl -laxparameter -lfcgi -lcurl -lpthread -lssl -lcrypto
LDFLAGS += -Wl,-z,relro,-z,now

.PHONY: all clean

all: $(PROGS)

$(PROG1): $(OBJS1)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

clean:
	rm -rf $(PROGS) *.o engine/*.o *.d engine/*.d $(LIBDIR) *.eap* *_LICENSE.txt manifest.json package.conf* param.conf
