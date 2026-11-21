#!/bin/sh
# Runs inside an i386 debian:trixie container. Builds wlroots (pixman only), cage, Xwayland (no glamor) into /opt/sprite.
set -ex
export DEBIAN_FRONTEND=noninteractive
sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/debian.sources
apt-get update -q
apt-get install -y -q --no-install-recommends build-essential meson ninja-build pkg-config dpkg-dev ca-certificates \
  libwayland-dev wayland-protocols libdrm-dev libinput-dev libseat-dev libudev-dev libpixman-1-dev libxkbcommon-dev \
  libdisplay-info-dev hwdata libliftoff-dev libxcb1-dev libxcb-composite0-dev libxcb-icccm4-dev libxcb-render0-dev \
  libxcb-res0-dev libxcb-xfixes0-dev libxcb-ewmh-dev libxcb-errors-dev glslang-tools \
  xutils-dev libxfont-dev libxkbfile-dev libxshmfence-dev libxcvt-dev libtirpc-dev libxau-dev libxdmcp-dev \
  x11proto-dev xtrans-dev libbsd-dev libmd-dev nettle-dev libssl-dev libdrm-dev libsystemd-dev libpciaccess-dev
cd /src
[ -d wlroots-0.18* ] || apt-get source -q wlroots
[ -d cage-* ] || apt-get source -q cage
[ -d xwayland-* ] || apt-get source -q xwayland
P=/opt/sprite
export PKG_CONFIG_PATH=$P/lib/i386-linux-gnu/pkgconfig:$P/lib/pkgconfig
cd /src/xwayland-*/ && rm -rf b && meson setup b --prefix=$P --buildtype=release -Dglamor=false -Dglx=false -Dlibdecor=false -Dsecure-rpc=false -Dxvfb=false -Dxwayland_ei=false -Ddocs=false -Ddevel-docs=false -Dxselinux=false -Dxkb_dir=/usr/share/X11/xkb -Dxkb_output_dir=/var/lib/xkb -Dxkb_bin_dir=/usr/bin -Ddefault_font_path=/usr/share/fonts/X11/misc && ninja -C b install || exit 1
cd /src/wlroots-0.18*/ && rm -rf b && meson setup b --prefix=$P --buildtype=release -Drenderers= -Dallocators= -Dbackends=drm,libinput -Dxwayland=enabled -Dexamples=false -Dsession=enabled -Dcolor-management=disabled && ninja -C b install || exit 1
cd /src/cage-*/ && rm -rf b && meson setup b --prefix=$P --buildtype=release && ninja -C b install || exit 1
cd $P && tar czf /out/sprite-stack.tgz . && ls -la /out
