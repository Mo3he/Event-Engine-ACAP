PROG1 = acap_event_engine
OBJS1 = main.c ACAP.c cJSON.c \
        engine/rule_engine.c \
        engine/triggers.c \
        engine/conditions.c \
        engine/actions.c \
        engine/scheduler.c \
        engine/variables.c \
        engine/event_log.c \
        engine/mqtt_client.c
PROGS = $(PROG1)

PKGS = glib-2.0 gio-2.0 axevent fcgi
CFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(PKGS))
LDLIBS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs $(PKGS))
LDLIBS += -s -lm -ldl -laxparameter -lfcgi -lcurl -lpthread

all: $(PROGS)

$(PROG1): $(OBJS1)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

clean:
	rm -rf $(PROGS) *.o engine/*.o $(LIBDIR) *.eap* *_LICENSE.txt manifest.json package.conf* param.conf
