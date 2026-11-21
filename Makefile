# sprite: build inside the i386 Trixie build image (scripts/build.sh), against the
# Mesa-free wlroots in /opt/sprite.
PREFIX ?= /opt/sprite
PKGS = wlroots-0.18 wayland-server xkbcommon pixman-1 libdrm xcb
export PKG_CONFIG_PATH := $(PREFIX)/lib/i386-linux-gnu/pkgconfig:$(PREFIX)/lib/pkgconfig
CC ?= cc
CFLAGS ?= -O2 -g -march=atom
CFLAGS += -std=gnu11 -Wall -Wno-unused-parameter -DWLR_USE_UNSTABLE -DLV_CONF_INCLUDE_SIMPLE \
	-Isrc -Ithird_party -Ithird_party/lvgl -Ibuild/gen $(shell pkg-config --cflags $(PKGS))
LDLIBS = $(shell pkg-config --libs $(PKGS)) -lpthread -lm
LDFLAGS += -Wl,-rpath,$(PREFIX)/lib/i386-linux-gnu

SRC = src/main.c src/shell.c src/apps.c src/sysinfo.c
LVGL_SRC = $(shell find third_party/lvgl/src -name '*.c' 2>/dev/null)
OBJ = $(SRC:%.c=out/obj/%.o)
LVGL_OBJ = $(LVGL_SRC:%.c=out/obj/%.o)

all: out/sprite

out/sprite: $(OBJ) $(LVGL_OBJ)
	@echo link $@
	@$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

out/obj/src/%.o: src/%.c src/sprite.h src/lv_conf.h build/gen/xdg-shell-protocol.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

out/obj/third_party/%.o: third_party/%.c src/lv_conf.h
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -w -c $< -o $@

# wlroots' xdg-shell header includes the server protocol header, which we generate.
WP = $(shell pkg-config --variable=pkgdatadir wayland-protocols)
build/gen/xdg-shell-protocol.h:
	@mkdir -p build/gen
	wayland-scanner server-header $(WP)/stable/xdg-shell/xdg-shell.xml $@

clean:
	rm -rf out build/gen

.PHONY: all clean
