WP = $(shell pkg-config --variable=pkgdatadir wayland-protocols)
UNITS ?= 1
SRCS := main.c $(shell find lvgl/src lvgl/demos/benchmark lvgl/demos/widgets -name '*.c') gen/wayland_xdg_shell.c
OBJS := $(patsubst %.c,obj$(UNITS)/%.o,$(SRCS))
CFLAGS = -O2 -march=atom -I. -Ilvgl -Igen -DLV_CONF_INCLUDE_SIMPLE -DLVB_UNITS=$(UNITS) -w
lvbench$(UNITS): $(OBJS) ; $(CC) -o $@ $^ -lwayland-client -lwayland-cursor -lxkbcommon -lpthread -lm
obj$(UNITS)/%.o: %.c lv_conf.h gen/wayland_xdg_shell.h ; @mkdir -p $(dir $@); $(CC) $(CFLAGS) -c $< -o $@
gen/wayland_xdg_shell.h: ; mkdir -p gen; wayland-scanner client-header $(WP)/stable/xdg-shell/xdg-shell.xml $@
gen/wayland_xdg_shell.c: ; mkdir -p gen; wayland-scanner private-code $(WP)/stable/xdg-shell/xdg-shell.xml $@
