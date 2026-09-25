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
        engine/sparkplug_pb.c \
        engine/sparkplug.c \
        engine/alert_stream.c \
        engine/modbus_pool.c
PROGS = $(PROG1)

PKGS = glib-2.0 gio-2.0 axevent fcgi
CFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(PKGS))
CFLAGS += -fstack-protector-strong -D_FORTIFY_SOURCE=2
# Cap the GLib API so the OS 13 SDK (GLib 2.80) cannot emit symbols missing on
# older firmware, e.g. g_string_free_and_steal (2.76) => "undefined symbol".
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
