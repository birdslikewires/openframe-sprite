WP = $(shell pkg-config --variable=pkgdatadir wayland-protocols)
CFLAGS = -O2 -Wall -Wno-unused-parameter -Wno-unused-result

wlbench: wlbench.c xdg-shell-protocol.c presentation-time-protocol.c xdg-shell-client-protocol.h presentation-time-client-protocol.h
	$(CC) $(CFLAGS) -I. -o $@ wlbench.c xdg-shell-protocol.c presentation-time-protocol.c $(shell pkg-config --cflags --libs wayland-client)

xdg-shell-client-protocol.h: ; wayland-scanner client-header $(WP)/stable/xdg-shell/xdg-shell.xml $@
xdg-shell-protocol.c: ; wayland-scanner private-code $(WP)/stable/xdg-shell/xdg-shell.xml $@
presentation-time-client-protocol.h: ; wayland-scanner client-header $(WP)/stable/presentation-time/presentation-time.xml $@
presentation-time-protocol.c: ; wayland-scanner private-code $(WP)/stable/presentation-time/presentation-time.xml $@
