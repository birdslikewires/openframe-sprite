# sprite: build inside the i386 Trixie build image (scripts/build.sh), against the
# Mesa-free wlroots in /opt/sprite.
PREFIX ?= /opt/sprite
PKGS = wlroots-0.18 wayland-server xkbcommon pixman-1 libdrm xcb alsa libjpeg libpng16
export PKG_CONFIG_PATH := $(PREFIX)/lib/i386-linux-gnu/pkgconfig:$(PREFIX)/lib/pkgconfig
CC ?= cc
CFLAGS ?= -O2 -g -march=atom
VERSION ?= dev
# -Werror=format: a message in src/strings whose placeholders don't match the code stops the build
# (one cut short to fit, a longer translation say, is only warned of).
CFLAGS += -DSPRITE_VERSION=\"$(VERSION)\" -std=gnu11 -Wall -Werror=format -Wno-error=format-truncation -Wno-unused-parameter -DWLR_USE_UNSTABLE -DLV_CONF_INCLUDE_SIMPLE \
	-Isrc -Ithird_party -Ithird_party/lvgl -Ibuild/gen $(shell pkg-config --cflags $(PKGS))
LDLIBS = $(shell pkg-config --libs $(PKGS)) -lpthread -lm
LDFLAGS += -Wl,-rpath,$(PREFIX)/lib/i386-linux-gnu

# The on-screen text: src/strings/en_gb.txt, and another language's changes to it with
# STRINGS_LANG=<its file's name> (src/strings/en_us.txt, say).
STRINGS_LANG ?= en_gb
STRINGS_SRC = src/strings/en_gb.txt $(if $(filter-out en_gb,$(STRINGS_LANG)),src/strings/$(STRINGS_LANG).txt)

SRC = src/main.c src/shell.c src/apps.c src/sysinfo.c src/settings.c src/wifi.c src/audio.c src/icons_40.c src/ui_icons.c \
	src/screensaver.c src/shop.c src/font_clock_180.c src/font_clock_56.c src/bird.c \
	src/nowplaying.c src/font_text_32.c src/font_text_24.c src/font_text_20.c src/font_text_14.c src/ui_icons_16.c
LVGL_SRC = $(shell find third_party/lvgl/src -name '*.c' 2>/dev/null)
OBJ = $(SRC:%.c=out/obj/%.o)
LVGL_OBJ = $(LVGL_SRC:%.c=out/obj/%.o)

all: out/sprite

out/sprite: $(OBJ) $(LVGL_OBJ)
	@echo link $@
	@$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

out/obj/src/%.o: src/%.c src/sprite.h src/lv_conf.h build/gen/xdg-shell-protocol.h build/gen/sprite_strings.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

out/obj/third_party/%.o: third_party/%.c src/lv_conf.h
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -w -c $< -o $@

build/gen/sprite_strings.h: $(STRINGS_SRC) scripts/make-strings.awk scripts/check-strings.sh system/sprite-helper
	@mkdir -p build/gen
	sh scripts/check-strings.sh
	awk -f scripts/make-strings.awk $(STRINGS_SRC) > $@.new && mv $@.new $@

# wlroots' xdg-shell header includes the server protocol header, which we generate.
WP = $(shell pkg-config --variable=pkgdatadir wayland-protocols)
build/gen/xdg-shell-protocol.h:
	@mkdir -p build/gen
	wayland-scanner server-header $(WP)/stable/xdg-shell/xdg-shell.xml $@

clean:
	rm -rf out build/gen

.PHONY: all clean
