# sprite: build inside the i386 Trixie build image (scripts/build.sh), against the
# Mesa-free wlroots in /opt/sprite.
PREFIX ?= /opt/sprite
PKGS = wlroots-0.18 wayland-server xkbcommon pixman-1 libdrm xcb alsa
export PKG_CONFIG_PATH := $(PREFIX)/lib/i386-linux-gnu/pkgconfig:$(PREFIX)/lib/pkgconfig
CC ?= cc
CFLAGS ?= -O2 -g -march=atom
VERSION ?= dev
# -Werror=format: a message in src/text whose placeholders don't match the code stops the build.
CFLAGS += -DSPRITE_VERSION=\"$(VERSION)\" -std=gnu11 -Wall -Werror=format -Wno-unused-parameter -DWLR_USE_UNSTABLE -DLV_CONF_INCLUDE_SIMPLE \
	-Isrc -Ithird_party -Ithird_party/lvgl -Ibuild/gen $(shell pkg-config --cflags $(PKGS))
LDLIBS = $(shell pkg-config --libs $(PKGS)) -lpthread -lm
LDFLAGS += -Wl,-rpath,$(PREFIX)/lib/i386-linux-gnu

# The on-screen text: src/text/en_gb.txt, and another language's changes to it with
# TEXT_LANG=<its file's name> (src/text/en_us.txt, say).
TEXT_LANG ?= en_gb
TEXT_SRC = src/text/en_gb.txt $(if $(filter-out en_gb,$(TEXT_LANG)),src/text/$(TEXT_LANG).txt)

SRC = src/main.c src/shell.c src/apps.c src/sysinfo.c src/settings.c src/wifi.c src/audio.c src/icons_40.c src/ui_icons.c
LVGL_SRC = $(shell find third_party/lvgl/src -name '*.c' 2>/dev/null)
OBJ = $(SRC:%.c=out/obj/%.o)
LVGL_OBJ = $(LVGL_SRC:%.c=out/obj/%.o)

all: out/sprite

out/sprite: $(OBJ) $(LVGL_OBJ)
	@echo link $@
	@$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

out/obj/src/%.o: src/%.c src/sprite.h src/lv_conf.h build/gen/xdg-shell-protocol.h build/gen/text.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

out/obj/third_party/%.o: third_party/%.c src/lv_conf.h
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -w -c $< -o $@

build/gen/text.h: $(TEXT_SRC) scripts/make-text.awk scripts/check-text.sh system/sprite-helper
	@mkdir -p build/gen
	sh scripts/check-text.sh
	awk -f scripts/make-text.awk $(TEXT_SRC) > $@.new && mv $@.new $@

# wlroots' xdg-shell header includes the server protocol header, which we generate.
WP = $(shell pkg-config --variable=pkgdatadir wayland-protocols)
build/gen/xdg-shell-protocol.h:
	@mkdir -p build/gen
	wayland-scanner server-header $(WP)/stable/xdg-shell/xdg-shell.xml $@

clean:
	rm -rf out build/gen

.PHONY: all clean
